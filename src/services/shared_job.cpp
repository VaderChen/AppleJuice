#include "x64arm/shared_job.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <iomanip>
#include <sstream>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace x64arm {
namespace {
constexpr std::uint64_t magic=UINT64_C(0x33424f4a52413458);
constexpr std::size_t storage_size=65536;
constexpr std::uint64_t atom_magic=UINT64_C(0x314d544152413458);
struct JobAtom {std::uint32_t references{},characters{};std::array<char16_t,255> name{};};
struct JobAtoms {std::uint64_t signature{atom_magic},identity{};std::array<JobAtom,0x4000> atoms{};};
struct JobUserGrant {std::uint64_t handle{},endpoint{};std::uint32_t owner{},reserved{};};
struct JobOwner {std::uint32_t pid{},references{};std::uint64_t endpoint{};};
struct JobMember {std::uint32_t pid{},stopped{},code{},reserved{};std::uint64_t endpoint{},memory{};};
struct JobHeader {
    std::uint64_t signature{magic},identity{};
    std::uint32_t flags{},active_limit{},total{},terminated{},characters{};
    std::uint32_t ui_restrictions{};
    std::uint64_t process_memory_limit{},peak_process_memory{},peak_job_memory{};
    std::array<char16_t,260> name{};
    std::array<JobOwner,256> owners{};
    std::array<JobMember,256> members{};
    std::array<JobUserGrant,256> user_grants{};
    std::uint64_t notification_sequence{},completion_generation{},completion_endpoint{},completion_port{},completion_key{};
    std::uint32_t completion_process{},completion_reserved{};
    std::array<JobNotification,1024> notifications{};
    // 控制狀態（限制、終止、成員、通知、完成埠）每次變更遞增；輪詢端不加鎖即可判斷是否需要完整掃描。
    std::uint64_t revision{};
};
static_assert(sizeof(JobHeader)<storage_size);
std::string object_name(std::u16string_view name){
    std::uint64_t a=14695981039346656037ULL,b=0x84222325cbf29ce4ULL;
    for(auto c:name){a=(a^c)*1099511628211ULL;b=(b^(c+1U))*0x100000001b3ULL;}
    std::ostringstream out;out<<"section-"<<std::hex<<std::setw(16)<<std::setfill('0')<<a<<std::setw(16)<<b;return out.str();
}
std::string atom_object_name(std::uint64_t identity){return "job-atoms-"+std::to_string(identity);}
#if !defined(_WIN32)
std::uint64_t endpoint_identity(int directory,std::uint32_t pid){struct stat info{};const auto name="process-"+std::to_string(pid);return !::fstatat(directory,name.c_str(),&info,AT_SYMLINK_NOFOLLOW) && S_ISSOCK(info.st_mode) && info.st_uid==::geteuid()?static_cast<std::uint64_t>(info.st_ino):0;}
bool alive(int directory,std::uint32_t pid,std::uint64_t endpoint){return pid && endpoint && endpoint_identity(directory,pid)==endpoint && (!::kill(static_cast<pid_t>(pid),0) || errno==EPERM);}
void touch(JobHeader& data){std::atomic_ref(data.revision).fetch_add(1,std::memory_order_release);}
void notify(JobHeader& data,std::uint32_t message,std::uint32_t pid){touch(data);const auto sequence=++data.notification_sequence;data.notifications[(sequence-1)%data.notifications.size()]={sequence,message,pid};}
bool abnormal_exit(std::uint32_t code){
    switch(code){
    case 0x80000001U:case 0x80000002U:case 0x80000003U:case 0x80000004U:
    case 0xc0000005U:case 0xc0000006U:case 0xc000001dU:case 0xc0000025U:case 0xc0000026U:
    case 0xc000008cU:case 0xc000008dU:case 0xc000008eU:case 0xc000008fU:case 0xc0000090U:
    case 0xc0000091U:case 0xc0000092U:case 0xc0000093U:case 0xc0000094U:case 0xc0000095U:
    case 0xc0000096U:case 0xc00000fdU:case 0xc000013aU:case 0xc00002b4U:case 0xc00002b5U:case 0xc00002c9U:return true;
    default:return false;
    }
}
void remove_member(JobHeader& data,JobMember& member){
    if((member.reserved || member.stopped) && abnormal_exit(member.code))notify(data,8,member.pid);
    notify(data,7,member.pid);member={};
    if(std::none_of(data.members.begin(),data.members.end(),[](const auto& item){return item.pid!=0;}))notify(data,4,0);
}
// 只有成員實際轉為停止才遞增版本：KILL_ON_JOB_CLOSE 的 Job 在最後一個 handle 關閉後，
// 每次 sweep 都會再進入這裡；無變更也遞增會讓所有觀察者的節流失效而持續完整輪詢。
void terminate_locked(JobHeader& data,std::uint32_t code){bool changed=false;for(auto& member:data.members)if(member.pid && !member.stopped){member.code=code;member.stopped=1;++data.terminated;changed=true;}if(changed)touch(data);}
std::optional<std::uint32_t> record_member_memory(JobHeader& data,std::uint64_t current,std::uint64_t peak){
    const auto pid=static_cast<std::uint32_t>(::getpid());std::uint64_t total=0;std::optional<std::uint32_t> termination;
    for(auto& member:data.members)if(member.pid){
        if(member.pid==pid){member.memory=current;if(member.stopped)termination=member.code;}
        total+=member.memory;
    }
    data.peak_process_memory=std::max(data.peak_process_memory,peak);
    data.peak_job_memory=std::max(data.peak_job_memory,total);return termination;
}
void sweep(int directory,JobHeader& data){
    // 同一程序可同時是 handle 擁有者、Job 成員與完成埠接收者。
    // 僅在這次持鎖掃描重用 (PID, endpoint) 結果；下一次仍向核心查詢，
    // 不延後異常退出或端點替換的偵測。容量用盡時直接查詢，不配置熱路徑記憶體。
    struct Check {std::uint32_t pid;std::uint64_t endpoint;bool result;};
    std::array<Check,32> checks;std::size_t count=0;
    const auto present=[&](std::uint32_t pid,std::uint64_t endpoint){
        for(std::size_t i=0;i<count;++i)if(checks[i].pid==pid && checks[i].endpoint==endpoint)return checks[i].result;
        const auto result=alive(directory,pid,endpoint);
        if(count<checks.size())checks[count++]={pid,endpoint,result};
        return result;
    };
    for(auto& owner:data.owners)if(owner.pid && !present(owner.pid,owner.endpoint))owner={};
    for(auto& member:data.members)if(member.pid && !present(member.pid,member.endpoint))remove_member(data,member);
    if(data.completion_process && !present(data.completion_process,data.completion_endpoint)){data.completion_process=0;data.completion_port=0;++data.completion_generation;touch(data);}
    if((data.flags&0x2000U) && std::none_of(data.owners.begin(),data.owners.end(),[](const auto& owner){return owner.references!=0;}))terminate_locked(data,0);
}
std::uint32_t io_error(){return errno==ENOENT?2U:errno==EACCES || errno==EPERM?5U:errno==ENOMEM || errno==ENOSPC?8U:87U;}
#endif
}
struct SharedJobStorage {
    int directory{-1},file{-1};JobHeader* data{};std::string name;bool counted{},member{};
    int atom_file{-1};JobAtoms* atoms{};
    ~SharedJobStorage(){
#if !defined(_WIN32)
        if(data){try{SharedEventLock lock(directory);const auto pid=static_cast<std::uint32_t>(::getpid());
            if(counted)for(auto& owner:data->owners)if(owner.pid==pid && owner.references){if(!--owner.references)owner={};break;}
            if(member)for(auto& item:data->members)if(item.pid==pid){remove_member(*data,item);break;}
            sweep(directory,*data);
            if(std::none_of(data->owners.begin(),data->owners.end(),[](const auto& item){return item.references!=0;}) && std::none_of(data->members.begin(),data->members.end(),[](const auto& item){return item.pid!=0;})){
                (void)::unlinkat(directory,name.c_str(),0);(void)::unlinkat(directory,atom_object_name(data->identity).c_str(),0);
            }
        }catch(...){}::munmap(data,storage_size);}
        if(atoms)::munmap(atoms,sizeof(JobAtoms));if(atom_file>=0)::close(atom_file);
        if(file>=0)::close(file);if(directory>=0)::close(directory);
#endif
    }
};
namespace {
#if !defined(_WIN32)
std::uint32_t map_atoms(SharedJobStorage& storage){
    // 呼叫端持有 session 鎖；同 Job 不同程序共享表，其他 Job 與非受限表互不混用。
    if(storage.atoms)return 0;
    const auto name=atom_object_name(storage.data->identity);
    auto file=::openat(storage.directory,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    const bool created=file>=0;if(!created && errno!=EEXIST)return io_error();
    if(!created)file=::openat(storage.directory,name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(file<0)return io_error();
    if(created){const std::array<std::uint64_t,2> header{atom_magic,storage.data->identity};
        if(::ftruncate(file,sizeof(JobAtoms)) || ::pwrite(file,header.data(),sizeof(header),0)!=sizeof(header)){const auto error=io_error();::close(file);(void)::unlinkat(storage.directory,name.c_str(),0);return error;}}
    struct stat info{};if(::fstat(file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1 || info.st_size!=sizeof(JobAtoms)){::close(file);return 6;}
    const auto mapped=::mmap(nullptr,sizeof(JobAtoms),PROT_READ|PROT_WRITE,MAP_SHARED,file,0);if(mapped==MAP_FAILED){::close(file);return 8;}
    auto* atoms=static_cast<JobAtoms*>(mapped);if(atoms->signature!=atom_magic || atoms->identity!=storage.data->identity){::munmap(mapped,sizeof(JobAtoms));::close(file);return 6;}
    storage.atom_file=file;storage.atoms=atoms;return 0;
}
std::uint32_t count_handle(SharedJobStorage& storage){
    const auto pid=static_cast<std::uint32_t>(::getpid());const auto endpoint=endpoint_identity(storage.directory,pid);if(!endpoint)return 50;
    for(auto& owner:storage.data->owners)if(owner.pid==pid){++owner.references;storage.counted=true;return 0;}
    for(auto& owner:storage.data->owners)if(!owner.pid){owner={pid,1,endpoint};storage.counted=true;return 0;}return 8;
}
std::uint32_t map_storage(SharedJobStorage& storage){
    struct stat info{};if(::fstat(storage.file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size!=storage_size)return 6;
    const auto mapped=::mmap(nullptr,storage_size,PROT_READ|PROT_WRITE,MAP_SHARED,storage.file,0);if(mapped==MAP_FAILED)return 8;
    const auto& header=*static_cast<JobHeader*>(mapped);
    if(header.signature!=magic || !header.identity || !header.characters || header.characters>=260){::munmap(mapped,storage_size);return 6;}
    storage.data=static_cast<JobHeader*>(mapped);
    storage.name=object_name(std::u16string_view(header.name.data(),header.characters));return 0;
}
#endif
}
OpenSharedJob open_shared_job(const std::string& session,std::u16string name,bool create,const std::function<std::vector<std::uint8_t>()>& security){
#if defined(_WIN32)
    (void)session;(void)name;(void)create;(void)security;return {{},50};
#else
    if(name.starts_with(u"Local\\"))name.erase(0,6);
    if(name.empty() || name.size()>=260 || name.find(u'\\',name.starts_with(u"Global\\")?7:0)!=name.npos)return {{},123};
    auto storage=std::make_shared<SharedJobStorage>();storage->name=object_name(name);
    storage->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage->directory<0 || ::fstat(storage->directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {{},5};
    SharedEventLock lock(storage->directory);bool created=false;
    if(create){storage->file=::openat(storage->directory,storage->name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);created=storage->file>=0;if(!created && errno!=EEXIST)return {{},io_error()};}
    if(storage->file<0)storage->file=::openat(storage->directory,storage->name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(storage->file<0)return {{},io_error()};
    if(created){JobHeader header;header.identity=static_cast<std::uint64_t>(::getpid())<<32;static std::uint32_t sequence=0;header.identity|=++sequence;header.characters=static_cast<std::uint32_t>(name.size());std::copy(name.begin(),name.end(),header.name.begin());
        if(::ftruncate(storage->file,storage_size) || ::pwrite(storage->file,&header,sizeof(header),0)!=sizeof(header)){const auto error=io_error();(void)::unlinkat(storage->directory,storage->name.c_str(),0);return {{},error};}
        try{if(security)set_shared_object_security(storage->file,security());}catch(...){(void)::unlinkat(storage->directory,storage->name.c_str(),0);throw;}
    }
    if(const auto error=map_storage(*storage))return {{},error};
    if(storage->data->characters!=name.size() || !std::equal(name.begin(),name.end(),storage->data->name.begin())){::munmap(storage->data,storage_size);storage->data=nullptr;return {{},6};}
    sweep(storage->directory,*storage->data);if(const auto error=count_handle(*storage))return {{},error};
    return {std::make_shared<SharedJob>(std::move(storage)),0,!created};
#endif
}
OpenSharedJob import_shared_job(const std::string& session,int descriptor,bool handle){
#if defined(_WIN32)
    (void)session;(void)descriptor;(void)handle;return {{},50};
#else
    auto storage=std::make_shared<SharedJobStorage>();storage->file=::fcntl(descriptor,F_DUPFD_CLOEXEC,3);if(storage->file<0)return {{},6};
    storage->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage->directory<0 || ::fstat(storage->directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {{},5};
    SharedEventLock lock(storage->directory);if(const auto error=map_storage(*storage))return {{},error};
    struct stat linked{},opened{};if(::fstatat(storage->directory,storage->name.c_str(),&linked,AT_SYMLINK_NOFOLLOW) || ::fstat(storage->file,&opened) || linked.st_ino!=opened.st_ino || linked.st_dev!=opened.st_dev){::munmap(storage->data,storage_size);storage->data=nullptr;return {{},6};}
    sweep(storage->directory,*storage->data);if(handle)if(const auto error=count_handle(*storage))return {{},error};
    return {std::make_shared<SharedJob>(std::move(storage)),0,true};
#endif
}
int SharedJob::descriptor()const{return storage_->file;}
std::u16string SharedJob::object_name()const{
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);const auto& header=*storage_->data;return {header.name.data(),header.characters};
#else
    return {};
#endif
}
int SharedJob::lock_descriptor()const{return storage_->directory;}
JobSnapshot SharedJob::snapshot()const{
    JobSnapshot result;
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);auto& data=*storage_->data;sweep(storage_->directory,data);
    result.ui_restrictions=data.ui_restrictions;result.process_memory_limit=data.process_memory_limit;result.peak_process_memory=data.peak_process_memory;result.peak_job_memory=data.peak_job_memory;
    result.identity=data.identity;result.flags=data.flags;result.active_limit=data.active_limit;result.total=data.total;result.terminated=data.terminated;
    result.notification_sequence=data.notification_sequence;result.completion_generation=data.completion_generation;
    for(const auto& member:data.members)if(member.pid)result.processes.push_back(member.pid);
#endif
    return result;
}
std::uint32_t SharedJob::associate_completion_port(std::uint64_t port,std::uint64_t key,JobSnapshot& result){
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);auto& data=*storage_->data;sweep(storage_->directory,data);
    if(port && data.completion_process)return 87;
    const auto pid=static_cast<std::uint32_t>(::getpid());const auto endpoint=endpoint_identity(storage_->directory,pid);if(port && !endpoint)return 50;
    data.completion_process=port?pid:0;data.completion_endpoint=port?endpoint:0;data.completion_port=port;data.completion_key=key;
    touch(data);result.identity=data.identity;result.completion_generation=++data.completion_generation;result.notification_sequence=data.notification_sequence;
    for(const auto& member:data.members)if(member.pid)result.processes.push_back(member.pid);
    return 0;
#else
    (void)port;(void)key;(void)result;return 50;
#endif
}
std::optional<std::vector<JobNotification>> SharedJob::notifications_since(std::uint64_t& cursor,std::uint64_t generation) const{
    std::vector<JobNotification> result;
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);auto& data=*storage_->data;sweep(storage_->directory,data);
    if(data.completion_generation!=generation || data.completion_process!=static_cast<std::uint32_t>(::getpid()))return std::nullopt;
    // Job 通知沒有保證投遞；有界共享佇列保留最近事件，仍能捕捉兩次 poll 間的快速退出。
    const auto first=std::max(cursor+1,data.notification_sequence>=data.notifications.size()?data.notification_sequence-data.notifications.size()+1:1);
    for(auto sequence=first;sequence<=data.notification_sequence;++sequence){const auto& item=data.notifications[(sequence-1)%data.notifications.size()];if(item.sequence==sequence)result.push_back(item);}
    cursor=data.notification_sequence;
#else
    (void)cursor;(void)generation;
#endif
    return result;
}
void SharedJob::record_exit_code(std::uint32_t code){
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);
    for(auto& member:storage_->data->members)if(member.pid==static_cast<std::uint32_t>(::getpid())){member.code=code;member.reserved=1;break;}
#else
    (void)code;
#endif
}
std::uint32_t SharedJob::set_limits(std::uint32_t flags,std::uint32_t active_limit,std::uint64_t process_memory_limit){
#if !defined(_WIN32)
    // ActiveProcessLimit=0 是合法的封鎖狀態：保留既有成員，join 拒絕任何新增程序。
    // 程序記憶體限制的 0 仍然無效，兩種限制不可共用參數驗證。
    if(flags&~0x3d08U)return 50;if((flags&0x100U) && !process_memory_limit)return 87;
    SharedEventLock lock(storage_->directory);auto& data=*storage_->data;sweep(storage_->directory,data);
    data.flags=flags;data.active_limit=active_limit;data.process_memory_limit=(flags&0x100U)?process_memory_limit:0;touch(data);return 0;
#else
    (void)flags;(void)active_limit;(void)process_memory_limit;return 50;
#endif
}
std::uint32_t SharedJob::set_ui_restrictions(std::uint32_t flags){
#if !defined(_WIN32)
    if(flags&~0xffU)return 87U;
    SharedEventLock lock(storage_->directory);storage_->data->ui_restrictions=flags;touch(*storage_->data);return 0;
#else
    (void)flags;return 50;
#endif
}
std::uint16_t SharedJob::global_atom(std::u16string_view name,bool add,std::uint32_t& error){
    error=0;
#if !defined(_WIN32)
    if(name.empty() || name.size()>255){error=87;return 0;}
    SharedEventLock lock(storage_->directory);if((error=map_atoms(*storage_)))return 0;
    std::size_t empty=0x4000;
    for(std::size_t i=0;i<storage_->atoms->atoms.size();++i){auto& atom=storage_->atoms->atoms[i];
        if(!atom.references){if(empty==0x4000)empty=i;continue;}
        if(atom.characters!=name.size())continue;
        bool equal=true;for(std::size_t j=0;j<name.size();++j)if(unicode_case_unit(atom.name[j],true)!=unicode_case_unit(name[j],true)){equal=false;break;}
        if(!equal)continue;if(add){if(atom.references==UINT32_MAX){error=8;return 0;}++atom.references;}return static_cast<std::uint16_t>(0xc000+i);
    }
    if(!add){error=2;return 0;}if(empty==0x4000){error=8;return 0;}
    auto& atom=storage_->atoms->atoms[empty];atom={};atom.characters=static_cast<std::uint32_t>(name.size());std::copy(name.begin(),name.end(),atom.name.begin());atom.references=1;return static_cast<std::uint16_t>(0xc000+empty);
#else
    (void)name;(void)add;error=50;return 0;
#endif
}
std::u16string SharedJob::global_atom_name(std::uint16_t atom,std::uint32_t& error){
    error=0;
#if !defined(_WIN32)
    if(atom<0xc000){error=6;return {};}
    SharedEventLock lock(storage_->directory);if((error=map_atoms(*storage_)))return {};
    const auto& value=storage_->atoms->atoms[atom-0xc000];if(!value.references || value.characters>255){error=6;return {};}
    return {value.name.data(),value.characters};
#else
    (void)atom;error=50;return {};
#endif
}
std::uint32_t SharedJob::delete_global_atom(std::uint16_t atom){
#if !defined(_WIN32)
    if(atom<0xc000)return atom?0U:87U;
    SharedEventLock lock(storage_->directory);if(const auto error=map_atoms(*storage_))return error;
    auto& value=storage_->atoms->atoms[atom-0xc000];if(!value.references)return 87;
    if(!--value.references)value={};return 0;
#else
    (void)atom;return 50;
#endif
}
std::uint32_t SharedJob::grant_user_handle(std::uint64_t handle,std::uint32_t owner,bool grant){
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);const auto endpoint=endpoint_identity(storage_->directory,owner);if(!endpoint)return 6;
    auto& grants=storage_->data->user_grants;auto empty=grants.end();
    for(auto it=grants.begin();it!=grants.end();++it){if(it->handle==handle && it->owner==owner && it->endpoint==endpoint){if(!grant)*it={};return 0;}
        if(it->handle && !alive(storage_->directory,it->owner,it->endpoint))*it={};if(!it->handle && empty==grants.end())empty=it;}
    if(!grant)return 0;if(empty==grants.end())return 8;*empty={handle,endpoint,owner,0};return 0;
#else
    (void)handle;(void)owner;(void)grant;return 50;
#endif
}
bool SharedJob::user_handle_granted(std::uint64_t handle,std::uint32_t owner)const{
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);const auto endpoint=endpoint_identity(storage_->directory,owner);
    return endpoint && std::any_of(storage_->data->user_grants.begin(),storage_->data->user_grants.end(),[&](const auto& item){return item.handle==handle && item.owner==owner && item.endpoint==endpoint;});
#else
    (void)handle;(void)owner;return false;
#endif
}
void SharedJob::record_memory(std::uint64_t current,std::uint64_t peak){
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);auto& data=*storage_->data;sweep(storage_->directory,data);
    (void)record_member_memory(data,current,peak);
#else
    (void)current;(void)peak;
#endif
}
std::uint64_t SharedJob::revision() const noexcept{
#if !defined(_WIN32)
    return std::atomic_ref(storage_->data->revision).load(std::memory_order_acquire);
#else
    return 0;
#endif
}
JobMemberPoll SharedJob::poll_member(std::uint64_t current,std::uint64_t peak){
    JobMemberPoll result;
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);auto& data=*storage_->data;sweep(storage_->directory,data);
    result.flags=data.flags;result.process_memory_limit=data.process_memory_limit;
    result.termination_code=record_member_memory(data,current,peak);
#else
    (void)current;(void)peak;
#endif
    return result;
}
std::uint32_t SharedJob::join(){
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);auto& data=*storage_->data;sweep(storage_->directory,data);const auto pid=static_cast<std::uint32_t>(::getpid());
    for(const auto& member:data.members)if(member.pid==pid)return 5;
    const auto active=std::count_if(data.members.begin(),data.members.end(),[](const auto& member){return member.pid!=0;});
    if((data.flags&8U) && static_cast<std::uint64_t>(active)>=data.active_limit){notify(data,3,0);return 1816;}
    const auto endpoint=endpoint_identity(storage_->directory,pid);if(!endpoint)return 50;
    for(auto& member:data.members)if(!member.pid){member={pid,0,0,0,endpoint};++data.total;storage_->member=true;notify(data,6,pid);return 0;}return 8;
#else
    return 50;
#endif
}
void SharedJob::terminate(std::uint32_t code){
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);sweep(storage_->directory,*storage_->data);terminate_locked(*storage_->data,code);
#else
    (void)code;
#endif
}
std::optional<std::uint32_t> SharedJob::termination_code()const{
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);sweep(storage_->directory,*storage_->data);const auto pid=static_cast<std::uint32_t>(::getpid());
    for(const auto& member:storage_->data->members)if(member.pid==pid && member.stopped)return member.code;
#endif
    return {};
}
}
