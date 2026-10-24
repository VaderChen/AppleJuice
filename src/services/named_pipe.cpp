#include "x64arm/named_pipe.hpp"
#include "x64arm/process_identity.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include "x64arm/security_access.hpp"
#include "io_wait_policy.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm {
#if !defined(_WIN32)
namespace {
// 第 7 版：擁有者加上程序出生識別，標頭由 256 KiB 擴為 512 KiB；舊版檔案以簽章拒絕，不混用。
constexpr std::uint64_t signature=UINT64_C(0x375049504d524134);
constexpr std::size_t header_size=524288, limit=16*1024*1024;
// 固定的稀疏檔案區段讓各程序不必重映射；實際佇列容量仍從初始配額按需成長。
constexpr std::size_t queue_extent=limit+65536;
// PID 會被系統重用；只靠 pid 判斷存活時，對端結束後若 pid 被其他程序取得，管道永遠不會回報斷線。
// birth 為 0 代表登記時無法取得出生識別，退回只比對 pid。
struct Owner {std::uint32_t pid{},references{};std::uint64_t birth{};};
// fork 後子程序會沿用父程序的快取；以 pid 為鍵，pid 改變就重新查詢。thread_local 避免多執行緒讀到不成對的值。
std::uint64_t self_birth(){
    thread_local std::uint32_t cached_pid{};thread_local std::uint64_t cached_birth{};
    const auto pid=static_cast<std::uint32_t>(::getpid());
    if(cached_pid!=pid){cached_birth=process_identity::query(pid).identity.birth;cached_pid=pid;}
    return cached_birth;
}
struct Slot { std::uint32_t server{}, client{}, generation{}, connected{};std::array<Owner,32> servers{},clients{};std::uint32_t server_process{},client_process{}; };
bool retain(std::array<Owner,32>& owners) {const auto pid=static_cast<std::uint32_t>(::getpid());for(auto& o:owners)if(o.pid==pid){++o.references;return true;}for(auto& o:owners)if(!o.pid){o={pid,1,self_birth()};return true;}return false;}
void release(std::array<Owner,32>& owners) {for(auto& o:owners)if(o.pid==static_cast<std::uint32_t>(::getpid())){if(o.references && !--o.references)o={};return;}}
struct Header {
    std::uint64_t magic{signature};
    std::uint32_t access{},mode{},maximum{},output{},input{},timeout{},capacity{},characters{},integrity{},policy{},access_mask{UINT32_MAX};
    std::array<char16_t,256> name{};
    std::array<Slot,255> slots{};
};
static_assert(sizeof(Header)<=header_size);
struct Queue { std::uint32_t head{},used{},bytes{},messages{},remaining{},capacity{};std::uint64_t produced{},consumed{}; };
struct ReplySlot {std::array<char,32> name{};std::uint32_t direction{};std::uint64_t sequence{};};
struct Data { std::array<Queue,2> queues{};std::array<ReplySlot,32> replies{};std::uint64_t reply_sequence{};std::uint32_t context_size{},context_read{};std::array<std::uint8_t,262144> context{}; };
std::uint32_t error() {return errno==ENOENT?2:errno==EACCES || errno==EPERM?5:errno==ENOSPC || errno==ENOMEM?8:87;}
struct Lock {
    int fd;
    explicit Lock(int value):fd(value) {while(::flock(fd,LOCK_EX)<0)if(errno!=EINTR)throw std::runtime_error("無法鎖定管道 session");}
    ~Lock(){(void)::flock(fd,LOCK_UN);}
};
std::string key(std::u16string& name) {
    for(auto& c:name)if(c>=u'A' && c<=u'Z')c+=u'a'-u'A';
    if(name.size()>256 || !name.starts_with(u"\\\\.\\pipe\\") || name.size()==9 || name.find(u'\\',9)!=name.npos || name.find(u'/')!=name.npos)return {};
    std::uint64_t hash=14695981039346656037ULL;
    for(auto c:name)hash=(hash^c)*1099511628211ULL;
    std::ostringstream out;out<<"pipe-"<<std::hex<<hash;return out.str();
}
// 只有確定不存在、已成為 zombie 或出生識別不符時才判定結束；權限或暫時性查詢錯誤保守視為存活，
// 與原本 kill 回傳 EPERM 時的處理一致。
// 讀寫資料的慢路徑每次都會檢查 slot 內每個擁有者；出生識別的 sysctl 約 8 µs，比 kill 慢約 24 倍。
// 先以 kill 排除已結束程序；存在時才查出生識別，並對同一 (pid, birth) 快取 identity_recheck。
// 因此 zombie 與 PID 重用最多延遲 identity_recheck 才偵測到，與待命讀取的存活複查間隔相同。
// 快取以 pid＋birth 為鍵且放在 thread_local：fork 後沿用的只是時間有界的既有判定，不會套到別的程序。
// 自身程序正在執行時只需核對出生識別；其他程序仍每次 kill。固定 16 格以完整鍵查找，
// 避免不同擁有者的雜湊碰撞反覆淘汰尚未過期的結果；滿載時替換最早查詢的一格。
constexpr auto identity_recheck=std::chrono::milliseconds(50);
bool alive(const Owner& owner){
    if(!owner.pid)return false;
    if(owner.pid==static_cast<std::uint32_t>(::getpid())){
        if(!owner.birth)return true;
        if(const auto birth=self_birth())return owner.birth==birth;
    }
    if(::kill(static_cast<pid_t>(owner.pid),0)!=0 && errno==ESRCH)return false;
    struct Verdict {std::uint32_t pid{};std::uint64_t birth{};bool live{};std::chrono::steady_clock::time_point checked{};};
    thread_local std::array<Verdict,16> verdicts{};
    const auto now=std::chrono::steady_clock::now();
    auto* cached=&verdicts.front();
    for(auto& verdict:verdicts){
        if(verdict.pid==owner.pid && verdict.birth==owner.birth){
            if(now-verdict.checked<identity_recheck)return verdict.live;
            cached=&verdict;break;
        }
        if(verdict.checked<cached->checked)cached=&verdict;
    }
    const auto status=process_identity::query(owner.pid);
    const bool live=status.error?status.error!=87:!status.exited && (!owner.birth || status.identity.birth==owner.birth);
    *cached={owner.pid,owner.birth,live,now};
    return live;
}
// 清除已結束程序的擁有者，並依剩餘參考重算該 slot 的 server／client 代表 pid。
void reap_slot(Slot& s){s.server=s.client=0;for(auto& o:s.servers){if(o.pid && !alive(o))o={};if(o.references)s.server=o.pid;}for(auto& o:s.clients){if(o.pid && !alive(o))o={};if(o.references)s.client=o.pid;}}
// 全表 255 slot × 64 擁有者；只用於需要跨 slot 判斷的建立、開啟、查詢資訊與最後關閉。
// 單一 instance 的讀寫／flush／connect 只看自己的 slot，改用 reap_slot，結果相同。
void reap(Header& h){for(auto& s:h.slots)reap_slot(s);}
// 跨程序共享頁上的欄位由持有 session 鎖的一方更新；無鎖快路徑只拿來偵測「是否有變」，變了就回到鎖內完整判定。
std::uint32_t observe(const std::uint32_t& value){return __atomic_load_n(&value,__ATOMIC_ACQUIRE);}
// 對端當掉（未經關閉而消失）時的最大偵測延遲；正常關閉會改變 slot 欄位，仍立即偵測。
constexpr auto idle_interval=std::chrono::milliseconds(50);
// 只在程序正常收尾時輸出作用計數，不把 pending 呼叫次數當成節省的 CPU 時間。
struct ConnectWaitProfile {
    std::uint64_t cached{},locked{};
    ~ConnectWaitProfile(){if(std::getenv("X64ARM_IO_WAIT_PROFILE"))
        std::fprintf(stderr,"[pipe_connect_wait] pid=%d cached=%llu locked=%llu\n",::getpid(),
                     static_cast<unsigned long long>(cached),static_cast<unsigned long long>(locked));}
};
ConnectWaitProfile& connect_wait_profile(){thread_local ConnectWaitProfile profile;return profile;}
}
struct NamedPipe::Transaction {
    struct Header {std::uint64_t magic{UINT64_C(0x3159504c52413458)};std::uint32_t capacity{},bytes{},error{},complete{};};
    int descriptor{-1},directory{-1};Header* mapped{};std::size_t extent{};std::array<char,32> name{};std::function<void()> cleanup;
    ~Transaction(){if(cleanup)cleanup();if(directory>=0 && name[0])::unlinkat(directory,name.data(),0);if(mapped)::munmap(mapped,extent);if(descriptor>=0)::close(descriptor);if(directory>=0)::close(directory);}
    static std::shared_ptr<Transaction> create(int directory,const std::string& session,std::size_t capacity){
        if(capacity>limit)throw std::bad_alloc();auto reply=std::make_shared<Transaction>();reply->directory=::dup(directory);
        auto path=session+"/reply-XXXXXX";std::vector<char> text(path.begin(),path.end());text.push_back(0);reply->descriptor=::mkstemp(text.data());
        if(reply->directory<0 || reply->descriptor<0)throw std::bad_alloc();
        const auto leaf=std::strrchr(text.data(),'/')+1;std::copy_n(leaf,std::strlen(leaf)+1,reply->name.data());
        (void)::fcntl(reply->directory,F_SETFD,FD_CLOEXEC);(void)::fcntl(reply->descriptor,F_SETFD,FD_CLOEXEC);
        reply->extent=sizeof(Header)+capacity;if(::ftruncate(reply->descriptor,static_cast<off_t>(reply->extent)))throw std::bad_alloc();
        const auto mapped=::mmap(nullptr,reply->extent,PROT_READ|PROT_WRITE,MAP_SHARED,reply->descriptor,0);if(mapped==MAP_FAILED)throw std::bad_alloc();
        reply->mapped=static_cast<Header*>(mapped);*reply->mapped=Header{};reply->mapped->capacity=static_cast<std::uint32_t>(capacity);return reply;
    }
    PipeResult result(std::span<std::uint8_t> output)const {
        if(!std::atomic_ref(mapped->complete).load(std::memory_order_acquire))return {0,0,true};
        if(mapped->bytes>output.size() || mapped->bytes>mapped->capacity)return {0,6};
        std::copy_n(reinterpret_cast<const std::uint8_t*>(mapped+1),mapped->bytes,output.begin());return {mapped->bytes,mapped->error};
    }
};
struct NamedPipe::Impl {
    std::shared_ptr<SharedWake> wake;
    int directory{-1},fd{-1};Header* header{};Data* data{};std::size_t extent{},slot{};std::uint32_t generation{};bool is_server{},counted{},created_file{};std::string name,session_path;
    ~Impl(){
        if(counted || created_file)try {
            Lock lock(directory);
            if(header){auto& s=header->slots[slot];
                if(counted && (is_server || s.generation==generation)){release(is_server?s.servers:s.clients);}
                reap(*header);
            }
            if(!header || std::none_of(header->slots.begin(),header->slots.end(),[](auto s){return s.server || s.client;}))(void)::unlinkat(directory,name.c_str(),0);
        }catch(...){}
        if(wake)wake->notify();
        if(data)::munmap(data,extent);if(header)::munmap(header,header_size);if(fd>=0)::close(fd);if(directory>=0)::close(directory);
    }
    std::uint32_t setup(const std::string& session,std::u16string& pipe_name,bool create,bool& created){
        wake=SharedWake::open(session);session_path=session;name=key(pipe_name);if(name.empty())return 123;
        directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        struct stat st{};
        if(directory<0 || ::fstat(directory,&st) || st.st_uid!=::geteuid() || (st.st_mode&077))return 5;
        // 呼叫端持有 session lock 後才開檔，避免最後 close / unlink 競態。
        (void)create;(void)created;return 0;
    }
    std::uint32_t map_header(std::u16string_view pipe_name,bool create,bool& created){
        if(create){fd=::openat(directory,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);created=created_file=fd>=0;if(fd<0 && errno!=EEXIST)return error();}
        if(fd<0)fd=::openat(directory,name.c_str(),O_RDWR|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0)return error();
        struct stat st{};if(::fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=::geteuid() || (st.st_mode&077))return 5;
        if(created){if(::ftruncate(fd,header_size))return error();}
        else if(st.st_size<static_cast<off_t>(header_size))return 6;
        auto* mapped=::mmap(nullptr,header_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(mapped==MAP_FAILED)return error();header=static_cast<Header*>(mapped);
        // 標頭約 268 KB，原地建構，不在堆疊上產生暫存物件。
        if(created){::new(static_cast<void*>(header)) Header{};header->characters=static_cast<std::uint32_t>(pipe_name.size());std::copy(pipe_name.begin(),pipe_name.end(),header->name.begin());}
        else if(header->magic!=signature || header->characters!=pipe_name.size() || !std::equal(pipe_name.begin(),pipe_name.end(),header->name.begin()) || !header->maximum || header->maximum>255 || header->capacity<65536 || header->capacity>limit)return 6;
        reap(*header);return 0;
    }
    std::uint32_t map_data(){
        extent=(sizeof(Data)+2*queue_extent+65535)&~std::size_t{65535};
        const auto offset=header_size+slot*extent;struct stat st{};
        if(::fstat(fd,&st))return error();
        if(static_cast<std::uint64_t>(st.st_size)<offset+extent && ::ftruncate(fd,static_cast<off_t>(offset+extent)))return error();
        auto* mapped=::mmap(nullptr,extent,PROT_READ|PROT_WRITE,MAP_SHARED,fd,static_cast<off_t>(offset));if(mapped==MAP_FAILED)return error();data=static_cast<Data*>(mapped);return 0;
    }
    std::uint32_t capacity(unsigned q)const{return data->queues[q].capacity?data->queues[q].capacity:header->capacity;}
    std::uint8_t* buffer(unsigned q){return reinterpret_cast<std::uint8_t*>(data)+sizeof(Data)+q*queue_extent;}
    void transfer(unsigned q,std::uint32_t pos,std::span<std::uint8_t> bytes,bool writing){
        if(bytes.empty())return;
        const auto first=std::min<std::size_t>(bytes.size(),capacity(q)-pos);
        if(writing){std::memcpy(buffer(q)+pos,bytes.data(),first);std::memcpy(buffer(q),bytes.data()+first,bytes.size()-first);}
        else {std::memcpy(bytes.data(),buffer(q)+pos,first);std::memcpy(bytes.data()+first,buffer(q),bytes.size()-first);}
    }
    PipeResult reserve(unsigned n,std::size_t bytes){
        if(bytes>limit){if(std::getenv("X64ARM_TRACE_PIPE_FAILURES"))std::fprintf(stderr,"[pipe] pid=%d capacity-limit size=%zu pipe=%s\n",::getpid(),bytes,name.c_str());return {0,8};}auto& q=data->queues[n];const auto required=bytes+4;
        // 一般要求保留既有背壓；大筆要求只在前方資料排空後入列，避免無界累積。
        if(q.used+required>std::max<std::size_t>(header->capacity,required))return {0,0,true};
        if(required>capacity(n))try{
            std::vector<std::uint8_t> previous(q.used);transfer(n,q.head,previous,false);
            q.capacity=static_cast<std::uint32_t>((required+65535)&~std::size_t{65535});q.head=0;
            if(!previous.empty())std::memcpy(buffer(n),previous.data(),previous.size());
            if(std::getenv("X64ARM_TRACE_PIPE_FAILURES"))std::fprintf(stderr,"[pipe] pid=%d grow size=%zu capacity=%u pipe=%s server=%u\n",::getpid(),bytes,q.capacity,name.c_str(),is_server);
        }catch(const std::bad_alloc&){return {0,8};}
        return {};
    }
    std::uint32_t status(){auto& s=header->slots[slot];if(!is_server && s.generation!=generation)return 233;if(!s.connected)return 233;return (is_server?s.client:s.server)?0:109;}
    // 待命讀取的無鎖快路徑。上次在鎖內判定為 pending 時記下佇列與 slot 狀態；之後只要狀態未變、
    // 且距上次存活檢查未超過 idle_interval，結果必然仍是 pending，不必取 session 鎖、檢查存活或配置緩衝區。
    bool idle{};std::array<std::uint32_t,5> idle_state{};std::chrono::steady_clock::time_point idle_checked{};
    std::array<std::uint32_t,5> observed(unsigned queue) const {
        const auto& s=header->slots[slot];
        return {observe(data->queues[queue].messages),observe(s.generation),observe(s.connected),observe(s.server),observe(s.client)};
    }
    bool would_block(unsigned queue) const {
        return idle && std::chrono::steady_clock::now()-idle_checked<idle_interval && observed(queue)==idle_state;
    }
    void remember_idle(unsigned queue){idle=true;idle_state=observed(queue);idle_checked=std::chrono::steady_clock::now();}
    // 連線與讀取各自保存負面提示；只重用鎖內確認的 pending，變更或逾時一律重新取鎖。
    bool connect_idle{};std::array<std::uint32_t,4> connect_state{};
    std::chrono::steady_clock::time_point connect_checked{};
    std::array<std::uint32_t,4> observed_connection() const {
        const auto& s=header->slots[slot];
        return {observe(s.generation),observe(s.connected),observe(s.server),observe(s.client)};
    }
};
NamedPipe::NamedPipe(std::shared_ptr<Impl> value):impl_(std::move(value)){}
NamedPipe::~NamedPipe()=default;
NamedPipe::Opened NamedPipe::create(const std::string& session,std::u16string name,std::uint32_t access,std::uint32_t mode,std::uint32_t maximum,std::uint32_t output,std::uint32_t input,std::uint32_t timeout,bool first,std::uint32_t integrity,std::uint32_t policy,std::uint32_t access_mask,std::span<const std::uint8_t> descriptor,const PipeAccessCheck& authorize)try{
    if(access<1 || access>3 || (mode&~15U) || ((mode&2) && !(mode&4)) || !maximum || maximum>255)return {{},87};
    if(input>limit-4 || output>limit-4)return {{},8};
    auto p=std::make_shared<Impl>();bool created=false;if(auto e=p->setup(session,name,true,created))return {{},e};
    Lock lock(p->directory);if(auto e=p->map_header(name,true,created))return {{},e};auto& h=*p->header;
    if(!created && std::none_of(h.slots.begin(),h.slots.end(),[](auto s){return s.server || s.client;}))created=p->created_file=true;
    if(created){
        h.access_mask=access_mask;h.integrity=integrity;h.policy=policy;h.access=access;h.mode=mode&4;h.maximum=maximum;h.output=output;h.input=input;h.timeout=timeout?timeout:50;h.capacity=std::max<std::uint32_t>(65536,std::max(output,input)+4);
        security::Bytes bytes(descriptor.begin(),descriptor.end());
        if(bytes.empty()) {
            security::SecurityDescriptor sd;sd.control=4|16;sd.data[2]=security::integrity_label(integrity,0,policy);
            if(access_mask!=UINT32_MAX)sd.data[3]=security::encode_acl({security::encode_ace({0,0,access_mask,security::account_sid(*security::find_account("Everyone"))})});
            bytes=security::relative_descriptor(sd);
        }else (void)security::parse_relative_descriptor(bytes);
        set_shared_object_security(p->fd,bytes);
    }
    else if(first)return {{},5};
    else if(h.access!=access || h.mode!=(mode&4) || h.maximum!=maximum || h.timeout!=(timeout?timeout:50))return {{},5};
    if(!created && authorize){const auto stored=shared_object_security(p->fd);if(stored.empty())throw SecurityDescriptorError{1338};(void)authorize(stored);}
    const auto count=std::count_if(h.slots.begin(),h.slots.end(),[](auto s){return s.server!=0;});if(static_cast<std::uint32_t>(count)>=maximum)return {{},231};
    const auto free=std::find_if(h.slots.begin(),h.slots.end(),[](auto s){return !s.server && !s.client;});if(free==h.slots.end())return {{},8};
    p->slot=static_cast<std::size_t>(free-h.slots.begin());if(auto e=p->map_data())return {{},e};*p->data=Data{};
    ++free->generation;if(!free->generation)++free->generation;free->servers={};free->clients={};(void)retain(free->servers);free->server=static_cast<std::uint32_t>(::getpid());free->client=free->connected=0;
    free->server_process=static_cast<std::uint32_t>(::getpid());free->client_process=0;
    p->generation=free->generation;p->is_server=p->counted=true;if(p->wake)p->wake->notify();return {std::shared_ptr<NamedPipe>(new NamedPipe(p)),0};
}catch(const SecurityDescriptorError& e){return {{},e.code};}catch(const std::bad_alloc&){return {{},8};}
NamedPipe::Opened NamedPipe::open(const std::string& session,std::u16string name,std::uint32_t access,std::span<const std::uint8_t> context,std::uint32_t integrity,const PipeAccessCheck& authorize)try{
    if(context.size()>262144)return {{},8};
    auto p=std::make_shared<Impl>();bool created=false;if(auto e=p->setup(session,name,false,created))return {{},e};
    Lock lock(p->directory);if(auto e=p->map_header(name,false,created))return {{},e};auto& h=*p->header;
    std::uint32_t granted{};
    if(authorize){const auto stored=shared_object_security(p->fd);if(stored.empty())throw SecurityDescriptorError{1338};granted=authorize(stored);access=(granted&1U)|((granted&2U)?2U:0U);}
    else {
        if((access&~h.access_mask&3U)!=0)return {{},5};
        if(h.integrity>integrity && (((access&1) && (h.policy&2)) || ((access&2) && (h.policy&1))))return {{},5};
    }
    if(((access&1) && !(h.access&2)) || ((access&2) && !(h.access&1)))return {{},5};
    auto s=std::find_if(h.slots.begin(),h.slots.end(),[](auto s){return s.server && !s.connected;});
    if(s==h.slots.end())return {{},std::any_of(h.slots.begin(),h.slots.end(),[](auto s){return s.server;})?231U:2U};
    p->slot=static_cast<std::size_t>(s-h.slots.begin());if(auto e=p->map_data())return {{},e};
    p->data->context_size=static_cast<std::uint32_t>(context.size());p->data->context_read=0;std::copy(context.begin(),context.end(),p->data->context.begin());
    if(!retain(s->clients))return {{},8};s->client_process=s->client=static_cast<std::uint32_t>(::getpid());s->connected=1;p->generation=s->generation;p->counted=true;if(p->wake)p->wake->notify();
    return {std::shared_ptr<NamedPipe>(new NamedPipe(p)),0,granted};
}catch(const SecurityDescriptorError& e){return {{},e.code};}catch(const std::bad_alloc&){return {{},8};}
NamedPipe::Pair NamedPipe::anonymous(const std::string& session,std::uint32_t size,std::span<const std::uint8_t> security){
    static std::atomic<std::uint64_t> sequence{};
    const auto text=std::string("\\\\.\\pipe\\anonymous-")+std::to_string(::getpid())+"-"+std::to_string(sequence.fetch_add(1,std::memory_order_relaxed));
    const std::u16string name(text.begin(),text.end());
    // nSize 是緩衝建議值；沿用有界、按需成長的共用管線佇列。
    const auto capacity=size?std::min<std::uint32_t>(size,limit-4):4096U;
    auto reader=create(session,name,1,0,1,capacity,capacity,0,true,0x2000,1,UINT32_MAX,security);
    if(reader.error)return {{},{},reader.error};
    // 建立時已取得兩端能力，不以新物件的 DACL 再次拒絕建立者。
    auto writer=open(session,name,2);
    if(writer.error)return {{},{},writer.error};
    return {std::move(reader.pipe),std::move(writer.pipe),0};
}
std::uint32_t NamedPipe::available(const std::string& session,std::u16string name,std::uint32_t& timeout){
    auto p=std::make_shared<Impl>();bool created=false;if(auto e=p->setup(session,name,false,created))return e;Lock lock(p->directory);
    if(auto e=p->map_header(name,false,created))return e;timeout=p->header->timeout;
    bool exists=false;for(auto s:p->header->slots)if(s.server){exists=true;if(!s.connected)return 0;}return exists?231:2;
}
std::uint32_t NamedPipe::security(const std::string& session,std::u16string name,const std::function<void(int)>& operation)try {
    auto p=std::make_shared<Impl>();bool created=false;if(const auto e=p->setup(session,name,false,created))return e;
    Lock lock(p->directory);if(const auto e=p->map_header(name,false,created))return e;
    if(std::none_of(p->header->slots.begin(),p->header->slots.end(),[](const auto& slot){return slot.server || slot.client;}))return 2;
    operation(p->fd);return 0;
}catch(const SecurityDescriptorError& e){return e.code;}catch(const std::bad_alloc&){return 8;}
PipeResult NamedPipe::connect(){
    auto& p=*impl_;
    if(!p.is_server)return {0,6};
    const bool fast = detail::io_wait_fastpath_enabled();
    auto& profile = connect_wait_profile();
    if(fast && p.connect_idle &&
       std::chrono::steady_clock::now()-p.connect_checked<idle_interval &&
       p.observed_connection()==p.connect_state){++profile.cached;return {0,0,true};}
    ++profile.locked;
    Lock lock(p.directory);reap_slot(p.header->slots[p.slot]);const auto& s=p.header->slots[p.slot];
    p.connect_idle=!s.connected;
    if(fast && p.connect_idle){p.connect_state=p.observed_connection();p.connect_checked=std::chrono::steady_clock::now();}
    return s.connected?PipeResult{0,s.client?0U:232U}:PipeResult{0,0,true};
}
std::uint32_t NamedPipe::disconnect(){auto& p=*impl_;Lock lock(p.directory);auto& s=p.header->slots[p.slot];if(!p.is_server)return 6;if(!s.connected)return 233;s.clients={};s.client=s.connected=0;++s.generation;if(!s.generation)++s.generation;p.generation=s.generation;*p.data=Data{};if(p.wake)p.wake->notify();return 0;}
PipeResult NamedPipe::write(std::span<const std::uint8_t> input,bool nonblocking,WriteCompletion* completion){
    auto& p=*impl_;Lock lock(p.directory);reap_slot(p.header->slots[p.slot]);
    const unsigned n=p.is_server?1:0;auto& q=p.data->queues[n];
    if(completion && completion->end){
        if(completion->generation!=p.header->slots[p.slot].generation)return {0,233};
        if(q.consumed>=completion->end)return {completion->bytes,0};
        if(auto e=p.status())return {0,e};return {0,0,true};
    }
    if(auto e=p.status())return {0,e};
    for(;;){
        auto selected=p.data->replies.end();
        for(auto it=p.data->replies.begin();it!=p.data->replies.end();++it)if(it->name[0] && it->direction==n && (selected==p.data->replies.end() || it->sequence<selected->sequence))selected=it;
        if(selected==p.data->replies.end())break;auto& slot=*selected;
        const auto valid=slot.name.back()==0 && std::string_view(slot.name.data()).starts_with("reply-") && std::string_view(slot.name.data()).find('/')==std::string_view::npos;
        if(!valid){slot={};return {0,6};}
        const int fd=::openat(p.directory,slot.name.data(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0){if(errno==ENOENT){slot={};continue;}return {0,error()};}
        struct Close {int fd;~Close(){::close(fd);}} close{fd};struct stat st{};Transaction::Header h;
        if(::fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=::geteuid() || (st.st_mode&077) || ::pread(fd,&h,sizeof(h),0)!=sizeof(h) || h.magic!=Transaction::Header{}.magic || h.capacity>limit || st.st_size!=sizeof(h)+h.capacity || h.complete)return {0,6};
        const auto count=std::min<std::size_t>(input.size(),h.capacity),remaining=input.size()-count;
        if(remaining && nonblocking && q.used+remaining+4>p.header->capacity)return {};
        if(remaining){const auto reserved=p.reserve(n,remaining);if(reserved.error || reserved.pending)return reserved;}
        const auto extent=sizeof(h)+h.capacity;const auto mapping=::mmap(nullptr,extent,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(mapping==MAP_FAILED)return {0,error()};
        auto* reply=static_cast<Transaction::Header*>(mapping);std::copy_n(input.begin(),count,reinterpret_cast<std::uint8_t*>(reply+1));reply->bytes=static_cast<std::uint32_t>(count);reply->error=remaining?234U:0U;
        if(remaining){const auto size=static_cast<std::uint32_t>(remaining);auto at=(q.head+q.used)%p.capacity(n);p.transfer(n,at,{reinterpret_cast<std::uint8_t*>(const_cast<std::uint32_t*>(&size)),4},true);at=(at+4)%p.capacity(n);p.transfer(n,at,{const_cast<std::uint8_t*>(input.data()+count),remaining},true);q.used+=size+4;q.produced+=size+4;q.bytes+=size;++q.messages;}
        std::atomic_ref(reply->complete).store(1,std::memory_order_release);::munmap(mapping,extent);slot={};if(p.wake)p.wake->notify();
        if(completion && remaining+4>p.header->capacity){*completion={q.produced,p.generation,static_cast<std::uint32_t>(input.size())};return {0,0,true};}
        return {static_cast<std::uint32_t>(input.size()),0};
    }

    if(nonblocking && q.used+input.size()+4>p.header->capacity){
        if((p.header->mode&4) || q.used+4>=p.header->capacity)return {};
        input=input.first(p.header->capacity-q.used-4);
    }
    const auto count=static_cast<std::uint32_t>(input.size());
    const auto reserved=p.reserve(n,input.size());if(reserved.error || reserved.pending)return reserved;
    auto pos=(q.head+q.used)%p.capacity(n);auto bytes=std::span(reinterpret_cast<std::uint8_t*>(const_cast<std::uint32_t*>(&count)),4);p.transfer(n,pos,bytes,true);pos=(pos+4)%p.capacity(n);
    p.transfer(n,pos,{const_cast<std::uint8_t*>(input.data()),input.size()},true);q.used+=count+4;q.produced+=count+4;q.bytes+=count;++q.messages;if(p.wake)p.wake->notify();
    if(completion && input.size()+4>p.header->capacity){*completion={q.produced,p.generation,count};return {0,0,true};}
    return {count,0};
}
PipeResult NamedPipe::read(std::span<std::uint8_t> output,bool message,bool peek,std::uint32_t* available,std::uint32_t* remaining){
    auto& p=*impl_;const unsigned n=p.is_server?0:1;
    if(!peek && !available && !remaining && p.would_block(n))return {0,0,true};
    Lock lock(p.directory);reap_slot(p.header->slots[p.slot]);p.idle=false;auto q=p.data->queues[n];
    if(available)*available=q.bytes;if(remaining)*remaining=0;const auto status=p.status();
    if(status && status!=109)return {0,status};
    if(!q.messages){
        if(status)return {0,status};if(peek)return {};
        p.remember_idle(n);return {0,0,true};
    }
    std::uint32_t copied=0;
    do {
        if(!q.remaining){std::uint32_t length{};p.transfer(n,q.head,{reinterpret_cast<std::uint8_t*>(&length),4},false);q.head=(q.head+4)%p.capacity(n);q.used-=4;q.consumed+=4;q.remaining=length;}
        const auto count=std::min<std::uint32_t>(q.remaining,static_cast<std::uint32_t>(output.size())-copied);
        p.transfer(n,q.head,output.subspan(copied,count),false);q.head=(q.head+count)%p.capacity(n);q.used-=count;q.consumed+=count;q.bytes-=count;q.remaining-=count;copied+=count;
        if(remaining)*remaining=q.remaining;if(!q.remaining)--q.messages;
        if(message || !q.messages || copied==output.size())break;
    }while(true);
    if(!peek){p.data->queues[n]=q;if(p.is_server)p.data->context_read=1;if(p.wake)p.wake->notify();}return {copied,message && q.remaining && !peek?234U:0U};
}
PipeResult NamedPipe::transact(std::span<const std::uint8_t> input,std::span<std::uint8_t> output,std::shared_ptr<Transaction>& operation){
    auto& p=*impl_;
    if(operation){auto result=operation->result(output);if(!result.pending)return result;}
    Lock lock(p.directory);reap_slot(p.header->slots[p.slot]);
    if(operation){auto result=operation->result(output);if(!result.pending)return result;if(auto e=p.status())return {0,e};return {0,0,true};}
    if(auto e=p.status())return {0,e};const unsigned writing=p.is_server?1:0,reading=1-writing;auto& q=p.data->queues[writing];
    const auto reserved=p.reserve(writing,input.size());if(reserved.error || reserved.pending)return reserved;
    const auto slot=std::find_if(p.data->replies.begin(),p.data->replies.end(),[](const auto& item){return !item.name[0];});if(slot==p.data->replies.end())return {0,8};
    try {operation=Transaction::create(p.directory,p.session_path,output.size());}catch(const std::bad_alloc&){return {0,8};}
    // 在要求可被 server 讀取前，先登錄回覆目的地，避免 Write→Disconnect 搶在 client 輪詢前丟失回覆。
    slot->name=operation->name;slot->direction=reading;slot->sequence=++p.data->reply_sequence;
    const auto owner=impl_;const auto reply_name=operation->name;
    operation->cleanup=[owner,reply_name]{try{Lock lock(owner->directory);for(auto& item:owner->data->replies)if(item.name==reply_name)item={};}catch(...) {}};
    const auto count=static_cast<std::uint32_t>(input.size());auto at=(q.head+q.used)%p.capacity(writing);
    p.transfer(writing,at,{reinterpret_cast<std::uint8_t*>(const_cast<std::uint32_t*>(&count)),4},true);at=(at+4)%p.capacity(writing);
    p.transfer(writing,at,{const_cast<std::uint8_t*>(input.data()),input.size()},true);q.used+=count+4;q.produced+=count+4;q.bytes+=count;++q.messages;if(p.wake)p.wake->notify();
    // 先前已排入的回覆也按 FIFO 交給這次讀取；交易只消耗下一則訊息。
    auto& queued=p.data->queues[reading];
    if(queued.messages){
        if(!queued.remaining){p.transfer(reading,queued.head,{reinterpret_cast<std::uint8_t*>(&queued.remaining),4},false);queued.head=(queued.head+4)%p.capacity(reading);queued.used-=4;queued.consumed+=4;}
        const auto copied=std::min<std::uint32_t>(queued.remaining,static_cast<std::uint32_t>(output.size()));p.transfer(reading,queued.head,output.first(copied),false);
        queued.head=(queued.head+copied)%p.capacity(reading);queued.used-=copied;queued.consumed+=copied;queued.bytes-=copied;queued.remaining-=copied;if(!queued.remaining)--queued.messages;
        auto& header=*operation->mapped;header.bytes=copied;header.error=queued.remaining?234U:0U;std::copy_n(output.begin(),copied,reinterpret_cast<std::uint8_t*>(&header+1));
        std::atomic_ref(header.complete).store(1,std::memory_order_release);*slot={};return {copied,header.error};
    }
    return {0,0,true};
}
PipeResult NamedPipe::flush(){auto& p=*impl_;Lock lock(p.directory);reap_slot(p.header->slots[p.slot]);if(auto e=p.status())return {0,e};return {0,0,p.data->queues[p.is_server?1:0].used!=0};}
PipeInfo NamedPipe::information()const{auto& p=*impl_;Lock lock(p.directory);reap(*p.header);const auto& h=*p.header;return {h.mode,h.maximum,h.output,h.input,h.timeout,static_cast<std::uint32_t>(std::count_if(h.slots.begin(),h.slots.end(),[](auto s){return s.server;})),h.integrity,h.policy};}
std::u16string NamedPipe::file_name()const{
    // 名稱於建立時固定，繼承／複製的 descriptor 映射同一份 header。
    const auto& h=*impl_->header;return {h.name.data()+8,h.characters-8};
}
std::uint32_t NamedPipe::client_context(std::vector<std::uint8_t>& output)const{
    auto& p=*impl_;Lock lock(p.directory);if(!p.is_server)return 6;
    if(!p.data->context_read || !p.data->context_size || p.data->context_size>p.data->context.size())return 1368;
    output.assign(p.data->context.data(),p.data->context.data()+p.data->context_size);return 0;
}
bool NamedPipe::server()const{return impl_->is_server;}
std::uint32_t NamedPipe::process_id(bool client,std::uint32_t& output)const{
    auto& p=*impl_;Lock lock(p.directory);auto& s=p.header->slots[p.slot];reap_slot(s);
    if(client && !p.is_server)return 6;
    if(!p.is_server && s.generation!=p.generation)return 233;
    if(client && !s.connected)return 233;
    const auto value=client?s.client_process:s.server_process;if(!value)return 233;
    output=value;return 0;
}
bool NamedPipe::read_would_block()const{return impl_->would_block(impl_->is_server?0:1);}
int NamedPipe::descriptor()const{return impl_->fd;}
bool NamedPipe::uses_wake(const SharedWake* wake)const noexcept{return wake && impl_->wake.get()==wake;}
int NamedPipe::lock_descriptor()const{return impl_->directory;}
std::uint64_t NamedPipe::identity()const{return (std::uint64_t{impl_->generation}<<32)|impl_->slot;}
NamedPipe::Opened NamedPipe::inherit(const std::string& session,int fd,std::uint64_t identity,bool server){
    auto p=std::make_shared<Impl>();p->wake=SharedWake::open(session);p->session_path=session;p->fd=fd;(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
    p->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat st{};
    if(p->directory<0 || ::fstat(p->directory,&st) || st.st_uid!=::geteuid() || (st.st_mode&077))return {{},5};
    // 標頭約 268 KB，放在 heap；次要執行緒的預設堆疊只有 512 KB。
    Lock lock(p->directory);const auto header=std::make_unique<Header>();
    if(::fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=::geteuid() || (st.st_mode&077) || ::pread(fd,header.get(),sizeof(Header),0)!=sizeof(Header) || header->magic!=signature || header->characters>256)return {{},6};
    std::u16string name(header->name.data(),header->characters);bool created=false;
    p->name=key(name);if(p->name.empty())return {{},6};if(auto e=p->map_header(name,false,created))return {{},e};
    p->slot=static_cast<std::uint32_t>(identity);p->generation=static_cast<std::uint32_t>(identity>>32);p->is_server=server;
    if(p->slot>=p->header->maximum || p->header->slots[p->slot].generation!=p->generation)return {{},6};
    if(auto e=p->map_data())return {{},e};auto& slot=p->header->slots[p->slot];
    if(!(server?slot.server:slot.client))return {{},6};
    if(!retain(server?slot.servers:slot.clients))return {{},8};p->counted=true;reap(*p->header);
    return {std::shared_ptr<NamedPipe>(new NamedPipe(p)),0};
}

namespace named_pipe_detail {
bool owner_alive(std::uint32_t pid,std::uint64_t birth){return alive(Owner{pid,1,birth});}
}
#else
struct NamedPipe::Impl{};struct NamedPipe::Transaction{};
namespace named_pipe_detail {
bool owner_alive(std::uint32_t,std::uint64_t){return false;}
}
NamedPipe::NamedPipe(std::shared_ptr<Impl> value):impl_(std::move(value)){}
NamedPipe::~NamedPipe()=default;
NamedPipe::Opened NamedPipe::create(const std::string&,std::u16string,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,bool,std::uint32_t,std::uint32_t,std::uint32_t,std::span<const std::uint8_t>,const PipeAccessCheck&){return {{},50};}
NamedPipe::Opened NamedPipe::open(const std::string&,std::u16string,std::uint32_t,std::span<const std::uint8_t>,std::uint32_t,const PipeAccessCheck&){return {{},50};}
NamedPipe::Pair NamedPipe::anonymous(const std::string&,std::uint32_t,std::span<const std::uint8_t>){return {{},{},50};}
std::uint32_t NamedPipe::process_id(bool,std::uint32_t&)const{return 50;}
std::uint32_t NamedPipe::available(const std::string&,std::u16string,std::uint32_t&){return 50;}
std::uint32_t NamedPipe::security(const std::string&,std::u16string,const std::function<void(int)>&){return 50;}
PipeResult NamedPipe::connect(){return {0,50};}std::uint32_t NamedPipe::disconnect(){return 50;}
PipeResult NamedPipe::read(std::span<std::uint8_t>,bool,bool,std::uint32_t*,std::uint32_t*){return {0,50};}
PipeResult NamedPipe::transact(std::span<const std::uint8_t>,std::span<std::uint8_t>,std::shared_ptr<Transaction>&){return {0,50};}
PipeResult NamedPipe::write(std::span<const std::uint8_t>,bool,WriteCompletion*){return {0,50};}PipeResult NamedPipe::flush(){return {0,50};}
int NamedPipe::descriptor()const{return -1;}std::uint64_t NamedPipe::identity()const{return 0;}NamedPipe::Opened NamedPipe::inherit(const std::string&,int,std::uint64_t,bool){return {{},50};}
int NamedPipe::lock_descriptor()const{return -1;}
bool NamedPipe::uses_wake(const SharedWake*)const noexcept{return false;}
std::uint32_t NamedPipe::client_context(std::vector<std::uint8_t>&)const{return 50;}
std::u16string NamedPipe::file_name()const{return {};}
PipeInfo NamedPipe::information()const{return {};}bool NamedPipe::server()const{return false;}bool NamedPipe::read_would_block()const{return false;}
#endif
}
