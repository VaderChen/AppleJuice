#include "x64arm/windows_services.hpp"
#include "x64arm/startup_profile.hpp"
#include "x64arm/shared_wake.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/thread_context.hpp"
#include "x64arm/process_debugger.hpp"
#include "x64arm/process_identity.hpp"
#include "socket_state_access.hpp"
#include "windows_debug_state.hpp"
#include "monotonic_clock.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <cstring>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <sys/socket.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/event.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
std::uint32_t transfer_access(std::uint32_t type,std::uint32_t access){
    return type==1 || type==6?security::mapped_access(access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU}):access;
}
struct TransferMessage {
    std::uint64_t magic{UINT64_C(0x3154464841523458)},sequence{},handle{},auxiliary{};
    std::uint32_t kind{},sender{},error{},type{},access{},flags{},mode{},pipe_mode{},options{},reserved{};
    std::array<std::uint64_t,5> accounting{};
};
using TransferKey=std::pair<std::uint32_t,std::uint64_t>;
constexpr std::uint32_t family_request_kind=21,family_cancel_kind=22,family_protocol=0x31435052U;
constexpr std::uint32_t duplicate_protocol=0x31505544U;
struct TransferReply {TransferMessage message;int descriptor{-1};std::vector<std::uint8_t> context{};};
struct FamilyRequest {
    TransferMessage packet;
    std::uint64_t birth{};std::chrono::steady_clock::time_point deadline;
    int descriptor{-1},state_descriptor{-1};bool sent{};
    ~FamilyRequest(){
#if !defined(_WIN32)
        if(descriptor>=0)::close(descriptor);if(state_descriptor>=0)::close(state_descriptor);
#endif
    }
};
struct FamilyCancel {TransferMessage packet;std::chrono::steady_clock::time_point deadline,next_attempt{};};
// 跨程序複製保留原本的 export/import/ack 契約，但只掛起呼叫的客體執行緒。
// 描述元與 lifetime 都由作業持有，其他客體關閉原 handle 不會破壞已開始的轉移。
struct DuplicateRequest {
    std::array<std::uint64_t,7> arguments{};
    std::uint32_t source{},target{},peer{},error{};
    std::uint64_t result{},export_sequence{};
    InheritedHandle item;
    TransferMessage packet;
    bool exporting{},sent{};
    int descriptor{-1};
    std::chrono::steady_clock::time_point deadline,started;
    ~DuplicateRequest(){
#if !defined(_WIN32)
        if(descriptor>=0)::close(descriptor);
#endif
    }
};
std::string endpoint(const std::string& directory,std::uint32_t pid){return directory+"/process-"+std::to_string(pid);}
#if !defined(_WIN32)
int send_transfer(int socket,const std::string& path,const TransferMessage& packet,int descriptor=-1,int state_descriptor=-1,std::span<const std::uint8_t> context={}){
    sockaddr_un address{};address.sun_family=AF_UNIX;if(path.size()>=sizeof(address.sun_path))return ENAMETOOLONG;std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    std::array<iovec,2> data{{{const_cast<TransferMessage*>(&packet),sizeof(packet)},{const_cast<std::uint8_t*>(context.data()),context.size()}}};msghdr message{};message.msg_name=&address;message.msg_namelen=sizeof(address);message.msg_iov=data.data();message.msg_iovlen=context.empty()?1:2;
    alignas(cmsghdr) std::array<char,CMSG_SPACE(2*sizeof(int))> control{};
    if(descriptor>=0){const std::array descriptors{descriptor,state_descriptor};const auto bytes=(state_descriptor>=0?2U:1U)*sizeof(int);message.msg_control=control.data();message.msg_controllen=static_cast<decltype(message.msg_controllen)>(CMSG_SPACE(bytes));auto* header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=static_cast<decltype(header->cmsg_len)>(CMSG_LEN(bytes));std::memcpy(CMSG_DATA(header),descriptors.data(),bytes);}
    for(;;){
        const auto count=::sendmsg(socket,&message,0);if(count<0 && errno==EINTR)continue;
        if(count==static_cast<ssize_t>(sizeof(packet)+context.size())){
            if(auto wake=SharedWake::open(std::filesystem::path(path).parent_path().string()))wake->notify();
            return 0;
        }
        return count<0?errno:EIO;
    }
}
#endif
}
// 描述元監看：獨立執行緒以 kqueue 在核心等待描述元可讀（傳輸通道、偵錯監聽 socket、子程序回報管線），
// 可讀時遞增共用喚醒並暫停，待協調執行緒排空後再重新等待。閒置程序因此不必每毫秒輪詢，
// 與原生程序在系統呼叫中睡眠相同；事件到達仍立即喚醒。描述元可隨時加入；關閉的描述元由 kqueue 自動移除。
struct DescriptorWatch {
    int queue{-1};std::shared_ptr<SharedWake> wake;
    std::atomic<bool> ready{false},stop{false};bool paused{};
    // 第一個描述元是本程序的 handle 轉移通道；它可讀時另設旗標，讓服務邊界只在真的有轉移請求時才排空，
    // 不因子程序管線等其他描述元可讀而每個邊界都輪詢。
    int primary{-1};std::atomic<bool> primary_ready{false};
    std::mutex mutex;std::condition_variable resumed;std::thread thread;
    mutable std::mutex registry_mutex;std::set<int> registered;
    static constexpr std::uintptr_t stop_ident=1;
    DescriptorWatch(std::vector<int> fds,std::shared_ptr<SharedWake> shared):wake(std::move(shared)){
        queue=::kqueue();if(queue<0)return;
        (void)::fcntl(queue,F_SETFD,FD_CLOEXEC);
        struct kevent control{};EV_SET(&control,stop_ident,EVFILT_USER,EV_ADD|EV_CLEAR,0,0,nullptr);
        if(::kevent(queue,&control,1,nullptr,0,nullptr)<0){::close(queue);queue=-1;return;}
        if(!fds.empty())primary=fds.front();
        for(const auto fd:fds)add(fd);
        try{thread=std::thread([this]{run();});}catch(const std::system_error&){}
    }
    DescriptorWatch(const DescriptorWatch&)=delete;DescriptorWatch& operator=(const DescriptorWatch&)=delete;
    ~DescriptorWatch(){
        stop.store(true,std::memory_order_seq_cst);
        {std::lock_guard<std::mutex> lock(mutex);paused=false;}resumed.notify_all();
        if(queue>=0){struct kevent trigger{};EV_SET(&trigger,stop_ident,EVFILT_USER,0,NOTE_TRIGGER,0,nullptr);(void)::kevent(queue,&trigger,1,nullptr,0,nullptr);}
        if(thread.joinable())thread.join();
        if(queue>=0)::close(queue);
    }
    bool active()const noexcept{return thread.joinable();}
    bool signalled()const noexcept{return ready.load(std::memory_order_acquire);}
    bool primary_signalled()const noexcept{return primary_ready.load(std::memory_order_acquire);}
    // 任一執行緒可加入；kqueue 的註冊與等待可同時進行。
    void add(int fd)noexcept{
        if(queue<0 || fd<0)return;
        struct kevent change{};EV_SET(&change,static_cast<std::uintptr_t>(fd),EVFILT_READ,EV_ADD,0,0,nullptr);
        if(::kevent(queue,&change,1,nullptr,0,nullptr)==0){std::lock_guard<std::mutex> lock(registry_mutex);registered.insert(fd);}
    }
    // 該描述元是否已交給監看執行緒等待（呼叫端據此略過自己的輪詢）。
    bool watches(int fd)const noexcept{std::lock_guard<std::mutex> lock(registry_mutex);return registered.contains(fd);}
    // 協調執行緒排空後呼叫。ready／paused 只在同一把鎖內一起轉換：監看執行緒已暫停就一定解除，
    // 不依賴先前讀到的 ready。舊寫法在「ready 已設、paused 未設」之間被呼叫時會清掉 ready 卻沒有
    // 解除後續的暫停，監看執行緒從此永遠停住，跨程序 handle 請求不再被處理（請求方卡在同步等待）。
    // 解除後 kqueue 為 level-triggered，仍可讀的描述元會立即再次通知，不會遺失事件。
    void resume()noexcept{
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready.store(false,std::memory_order_release);primary_ready.store(false,std::memory_order_release);
            if(!paused)return;
            paused=false;
        }
        resumed.notify_one();
    }
    void run(){
        std::array<struct kevent,16> events{};
        while(!stop.load(std::memory_order_acquire)){
            const int count=::kevent(queue,nullptr,0,events.data(),static_cast<int>(events.size()),nullptr);
            if(count<0){if(errno==EINTR)continue;break;}
            if(stop.load(std::memory_order_acquire))break;
            bool readable=false,transfer=false;
            for(int i=0;i<count;++i){
                const auto& event=events[static_cast<std::size_t>(i)];
                if(event.filter==EVFILT_USER)continue;
                readable=true;if(primary>=0 && event.ident==static_cast<std::uintptr_t>(primary))transfer=true;
            }
            if(!readable)continue;
            {std::lock_guard<std::mutex> lock(mutex);ready.store(true,std::memory_order_release);
                if(transfer)primary_ready.store(true,std::memory_order_release);paused=true;}
            if(wake)wake->notify();
            std::unique_lock<std::mutex> lock(mutex);resumed.wait(lock,[&]{return !paused || stop.load(std::memory_order_acquire);});
        }
    }
};
struct HandleTransferState {
    std::string directory,path;int descriptor{-1};std::uint64_t next_sequence{1};
    std::chrono::steady_clock::time_point next_idle_poll{};
    std::unique_ptr<DescriptorWatch> watch;bool watch_attempted{};
    std::map<TransferKey,TransferReply> replies;
    std::map<TransferKey,std::shared_ptr<FamilyRequest>> family_requests;
    std::map<TransferKey,FamilyCancel> family_cancellations;
    std::map<TransferKey,std::shared_ptr<void>> exports;
    std::map<TransferKey,std::uint64_t> imports;
    std::map<std::uint64_t,std::shared_ptr<DuplicateRequest>> duplicates;
    std::set<TransferKey> duplicate_requests;
    ~HandleTransferState(){
#if !defined(_WIN32)
        watch.reset(); // 先停止監看執行緒，再關閉它等待的描述元。
        for(const auto& [key,reply]:replies){(void)key;if(reply.descriptor>=0)::close(reply.descriptor);}
        if(descriptor>=0)::close(descriptor);if(!path.empty())(void)::unlink(path.c_str());
#endif
    }
};
void WindowsServices::configure_handle_transfer(){
#if !defined(_WIN32)
    if(handle_transfers_)return;
    auto transfer=std::make_shared<HandleTransferState>();transfer->directory=mapping_session();transfer->path=endpoint(transfer->directory,process_id());
    transfer->descriptor=::socket(AF_UNIX,SOCK_DGRAM,0);if(transfer->descriptor<0)throw HostCallFault("無法建立程序 handle 傳輸通道");
    (void)::fcntl(transfer->descriptor,F_SETFD,FD_CLOEXEC);(void)::fcntl(transfer->descriptor,F_SETFL,O_NONBLOCK);
    sockaddr_un address{};address.sun_family=AF_UNIX;if(transfer->path.size()>=sizeof(address.sun_path))throw HostCallFault("程序 handle 通道路徑過長");
    std::memcpy(address.sun_path,transfer->path.c_str(),transfer->path.size()+1);
    if(::bind(transfer->descriptor,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || ::chmod(transfer->path.c_str(),0600))throw HostCallFault("無法綁定程序 handle 傳輸通道");
    configure_process_io();
    handle_transfers_=std::move(transfer);
    configure_process_identity();
    configure_debug_memory();
#endif
}
bool WindowsServices::remote_process_available(std::uint32_t pid) const {
#if !defined(_WIN32)
    if(!handle_transfers_)return false;struct stat info{};const auto path=endpoint(handle_transfers_->directory,pid);
    return !::lstat(path.c_str(),&info) && S_ISSOCK(info.st_mode) && info.st_uid==::geteuid() && !(info.st_mode&077) && (!::kill(static_cast<pid_t>(pid),0) || errno==EPERM);
#else
    (void)pid;return false;
#endif
}
std::uint32_t WindowsServices::export_transfer_handle(std::uint64_t handle,InheritedHandle& item){
    item.handle=handle;item.flags=runtime_.handle_flags[handle];
    if(handle>=0x100 && handle<=0x102 && standard_open_[handle-0x100]){
        const auto stream=static_cast<unsigned>(handle-0x100);
        if(live_stdio_[stream]<0)return 50;
        item.type=8;item.auxiliary=stream;item.access=stream?0x40000000U:0x80000000U;
        item.descriptor=live_stdio_[stream];item.lifetime=inherited_stdio_[stream];return 0;
    }
    if(const auto file=files_.find(handle);file!=files_.end()){
        if(!file->second.pipe){
            auto& source=file->second;if(!source.native || source.serial || source.hid || !source.native->transferable()){
                if(std::getenv("X64ARM_TRACE_HANDLE_TRANSFER"))std::fprintf(stderr,"[handle_export_unsupported] pid=%u handle=%llx type=file native=%u serial=%u hid=%u access=%x path=%s\n",process_id(),static_cast<unsigned long long>(handle),source.native?1U:0U,source.serial?1U:0U,source.hid?1U:0U,source.access,source.path.c_str());return 50;
            }
            const auto details=source.native->information();if(details.error)return details.error;
            auto& position=runtime_.win_file_positions.at(source.object);
            const auto session=mapping_session();if(const auto error=filesystem_.publish_shared_file(*source.native))return error;
            const FileTransferMetadata metadata{details.metadata.device,details.metadata.id,source.access,source.sharing,source.mode,!source.native->security_writable(),source.path,source.native->transfer_identity(),source.manage_volume_at_open};
            const auto identity=(static_cast<std::uint64_t>(process_id())<<32)|source.object;
            if(const auto error=position.publish(session,identity,metadata))return error;
            item.type=6;item.access=source.access;item.mode=source.mode;item.pipe_mode=source.sharing;item.auxiliary=position.identity();item.descriptor=source.native->transfer_descriptor();
            item.lifetime=std::make_shared<std::pair<std::shared_ptr<HostFile>,FilePosition>>(source.native,position);return 0;
        }
        item.type=1;item.pipe_mode=file->second.pipe_mode;item.access=file->second.access;item.mode=file->second.mode|(file->second.pipe->server()?0x80000000U:0U);
        item.auxiliary=file->second.pipe->identity();item.descriptor=file->second.pipe->descriptor();item.lifetime=file->second.pipe;return 0;
    }
    if(runtime_.token_handles.contains(handle)){
        try{const auto token=share_token(runtime_.token_handle_objects.at(handle));item.type=9;item.access=runtime_.token_handles.at(handle);item.descriptor=token->descriptor();item.lifetime=token;return 0;}
        catch(const SecurityDescriptorError& error){return error.code;}
    }
    if(export_mapping_handle(handle,item) || export_job_handle(handle,item) || export_process_handle(handle,item))return 0;
    if(const auto found=runtime_.wait_objects.find(handle);found!=runtime_.wait_objects.end()){
        auto& event=found->second;
        if(event.kind==WaitObject::Kind::Mutex){
            auto& mutex=runtime_.mutex_objects.at(event.mutex);
            if(!mutex.shared){const auto text=std::string("transfer-mutex-")+std::to_string(host_process_id())+"-"+std::to_string(event.mutex)+"-"+std::to_string(runtime_.allocate_wait_handle());const auto opened=open_shared_mutex(mapping_session(),std::u16string(text.begin(),text.end()),true,mutex.owner,mutex.recursion,[&]{return mutex.security_descriptor;});if(opened.error)return opened.error;mutex.shared=opened.mutex;}
            item.type=5;item.access=event.access;item.descriptor=mutex.shared->descriptor();item.lifetime=mutex.shared;return 0;
        }
        if(event.kind!=WaitObject::Kind::Event && event.kind!=WaitObject::Kind::MemoryResource){
            if(std::getenv("X64ARM_TRACE_HANDLE_TRANSFER"))std::fprintf(stderr,"[handle_export_unsupported] pid=%u handle=%llx type=wait kind=%u access=%x\n",process_id(),static_cast<unsigned long long>(handle),static_cast<unsigned>(event.kind),event.access);
            return 50;
        }
        if(!event.shared_event){
            const auto text=std::string("transfer-event-")+std::to_string(host_process_id())+"-"+std::to_string(handle)+"-"+std::to_string(runtime_.allocate_wait_handle());
            const auto opened=open_shared_event(mapping_session(),std::u16string(text.begin(),text.end()),true,event.manual_reset,event.signaled(),[&]{return event.security_descriptor;});
            if(opened.error)return opened.error;event.shared_event=opened.event;
        }
        item.type=3;item.access=event.access;item.auxiliary=event.kind==WaitObject::Kind::MemoryResource?0x100U|event.memory_notification_type:0;
        item.descriptor=event.shared_event->descriptor();item.lifetime=event.shared_event;return 0;
    }
    return power_requests_.contains(handle) || mapping_handle(handle) || !process_object_type(handle).empty() || thread_id(handle)?50U:6U;
}
void WindowsServices::import_transfer_handle(const InheritedHandle& item){
    if(item.type==8){import_standard_handle(item);return;}
    if(item.type==9){
#if !defined(_WIN32)
        struct CloseDescriptor {int value;~CloseDescriptor(){if(value>=0)::close(value);}} owned{item.descriptor};
#endif
        try{if(item.access&~0xf01ffU)throw SecurityDescriptorError{5};const auto id=attach_shared_token(SharedToken::inherit(item.descriptor));runtime_.token_handles.emplace(item.handle,item.access);runtime_.token_handle_objects.emplace(item.handle,id);}
        catch(const SecurityDescriptorError&){throw HostCallFault("傳輸權杖物件無效");}
    }
    else if(item.type==7){import_process_handle(item);}
    else if(item.type==6){
#if !defined(_WIN32)
        struct CloseDescriptor {int value;~CloseDescriptor(){if(value>=0)::close(value);}} owned{item.descriptor};
#endif
        FileTransferMetadata metadata;auto [position,error]=FilePosition::inherit(mapping_session(),item.auxiliary,item.descriptor,metadata);
        if(error || (transfer_access(item.type,item.access)&~transfer_access(item.type,metadata.access)) || item.mode!=metadata.mode || item.pipe_mode!=metadata.sharing)throw HostCallFault("繼承檔案能力紀錄無效");
        auto opened=filesystem_.inherit_host(item.descriptor,item.access,metadata.sharing,metadata.readonly,metadata.object);
        if(opened.error)throw HostCallFault("無法匯入原生檔案："+std::to_string(opened.error));
        FileHandle file;file.object=item.handle;file.native=std::move(opened.file);file.metadata=opened.metadata;file.access=item.access;file.sharing=metadata.sharing;file.mode=item.mode;file.path=std::move(metadata.path);file.key=file.path;file.bytes=std::make_shared<const std::vector<std::uint8_t>>();
        file.manage_volume_at_open=metadata.manage_volume_at_open;
        runtime_.win_file_positions.emplace(item.handle,std::move(position));try{files_.emplace(item.handle,std::move(file));}catch(...){runtime_.win_file_positions.erase(item.handle);throw;}
    }else if(item.type==1){
        auto opened=NamedPipe::inherit(mapping_session(),item.descriptor,item.auxiliary,(item.mode&0x80000000U)!=0);
        if(opened.error)throw HostCallFault("無法接收傳輸管道："+std::to_string(opened.error));
        FileHandle file;file.bytes=std::make_shared<const std::vector<std::uint8_t>>();file.pipe=std::move(opened.pipe);file.access=item.access;file.mode=item.mode&0x7fffffffU;file.pipe_mode=item.pipe_mode;file.object=item.handle;
        files_.emplace(item.handle,std::move(file));runtime_.win_file_positions[item.handle]=0;
    } else if(item.type==2)import_mapping_handle(item);
    else if(item.type==3){
        const auto opened=inherit_shared_event(mapping_session(),item.descriptor);if(opened.error)throw HostCallFault("無法接收傳輸事件");
        if(item.auxiliary && (item.auxiliary<0x100 || item.auxiliary>0x101 || !opened.event->manual_reset() || (item.access&~0x100001U)))throw HostCallFault("記憶體通知傳輸參數無效");
        WaitObject event;event.kind=item.auxiliary?WaitObject::Kind::MemoryResource:WaitObject::Kind::Event;event.maximum=1;
        event.memory_notification_type=static_cast<std::uint32_t>(item.auxiliary&1U);event.active=item.auxiliary!=0;
        event.shared_event=opened.event;event.manual_reset=opened.event->manual_reset();event.access=item.access;
        if(event.kind==WaitObject::Kind::MemoryResource)memory_resource_poll_deadline_=0;
        runtime_.wait_objects.emplace(item.handle,std::move(event));
    }else if(item.type==4)import_job_handle(item);
    else if(item.type==5){
        const auto opened=inherit_shared_mutex(mapping_session(),item.descriptor);if(opened.error)throw HostCallFault("無法接收傳輸 mutex");
        std::uint64_t identity{};for(const auto& [id,mutex]:runtime_.mutex_objects)if(mutex.shared && mutex.shared->descriptor()==opened.mutex->descriptor()){identity=id;break;}
        if(!identity){identity=runtime_.allocate_wait_handle();MutexObject mutex;mutex.shared=opened.mutex;runtime_.mutex_objects.emplace(identity,std::move(mutex));}
        WaitObject object;object.kind=WaitObject::Kind::Mutex;object.mutex=identity;object.access=item.access;runtime_.wait_objects.emplace(item.handle,std::move(object));
    }
    else throw HostCallFault("未知的傳輸物件類型");
    runtime_.handle_flags[item.handle]=item.flags;
    runtime_.next_wait_handle=std::max(runtime_.next_wait_handle,(item.handle+4)&~UINT64_C(3));
}
void WindowsServices::close_transfer_handle(GuestMemory& memory,std::uint64_t handle){
    if(const auto file=files_.find(handle);file!=files_.end()){
        cancel_file_io(memory,handle);const auto object=file->second.object;files_.erase(file);
        // CRT 擁有的客體 HANDLE 被直接關閉時，也要釋放其宿主參照。
        // 保留 descriptor 的 opened bit；後續 I/O 由共用 HANDLE 驗證回報 EBADF。
        const auto release=[&](CrtFile& stream){if(stream.os_handle==handle){stream.native.reset();stream.bytes.clear();}};
        release(runtime_.standard_input);for(auto& stream:runtime_.standard_output)release(stream);
        for(auto& [pointer,stream]:runtime_.streams){(void)pointer;release(stream);}
        if(std::none_of(files_.begin(),files_.end(),[&](const auto& entry){return entry.second.object==object;}))runtime_.win_file_positions.erase(object);
    }else if(close_bluetooth(handle)){}
    else if(runtime_.token_handles.erase(handle)){runtime_.token_handle_objects.erase(handle);collect_tokens();}
    else if(!close_mapping(handle) && !close_job(handle) && !close_process(handle))(void)close_wait_object(handle);
    runtime_.handle_flags.erase(handle);
}
void WindowsServices::watch_descriptor(int descriptor) noexcept {
    if(handle_transfers_ && handle_transfers_->watch)handle_transfers_->watch->add(descriptor);
}
bool WindowsServices::handle_transfer_polling() const noexcept {
    // 監看執行緒會以共用喚醒通知通道事件；沒有監看（非 host clock、無共用喚醒或建立失敗）時沿用排程器輪詢。
    return handle_transfers_ && !(handle_transfers_->watch && handle_transfers_->watch->active());
}
bool WindowsServices::handle_transfer_requested() const noexcept {
#if !defined(_WIN32)
    // X64ARM_HANDLE_TRANSFER_PROMPT=0 保留只在排程時排空的舊行為，供同成品對照。
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_HANDLE_TRANSFER_PROMPT");
        return !setting || std::string_view(setting)!="0";}();
    return enabled && handle_transfers_ && handle_transfers_->watch && handle_transfers_->watch->primary_signalled();
#else
    return false;
#endif
}
void WindowsServices::poll_handle_transfers(GuestMemory& memory,bool force){
#if !defined(_WIN32)
    bool signalled=false;
    if(handle_transfers_){
        auto& transfer=*handle_transfers_;
        static const bool watch_enabled=[] {const auto* setting=std::getenv("X64ARM_HOST_WAIT_WATCH");return !setting || std::string_view(setting)!="0";}();
        if(!transfer.watch_attempted && runtime_.host_clock && watch_enabled){
            transfer.watch_attempted=true;
            if(auto wake=mapping_wake()){
                std::vector<int> descriptors{transfer.descriptor};
#if defined(__APPLE__)
                if(debug_memory_ && debug_memory_->owner==this){const auto listener=debug_memory_->transport.listener_descriptor();if(listener>=0)descriptors.push_back(listener);}
#endif
                for(const auto fd:child_pipe_descriptors())descriptors.push_back(fd);
                auto watch=std::make_unique<DescriptorWatch>(std::move(descriptors),std::move(wake));
                if(watch->active())transfer.watch=std::move(watch);
            }
        }
        signalled=transfer.watch && transfer.watch->signalled();
        // 子程序回報管線也由同一監看等待；先讀空再於收尾恢復監看，否則它保持可讀而立即再通知。
        if(signalled)drain_child_pipes();
    }
    bool listener_watched=false;
#if defined(__APPLE__)
    if(!force && !signalled && handle_transfers_ && handle_transfers_->watch &&
       handle_transfers_->watch->active() && debug_memory_ && debug_memory_->owner==this &&
       debug_memory_->pending.empty() && debug_memory_->transport.listener_only())
        listener_watched=handle_transfers_->watch->watches(debug_memory_->transport.listener_descriptor());
#endif
    if(!listener_watched)poll_debug_memory(memory,0,force || signalled);
    if(!handle_transfers_)return;auto& transfer=*handle_transfers_;
    // 排空後讓監看執行緒重新等待；之後的可讀事件再次通知共用喚醒。
    struct ResumeWatch {DescriptorWatch* watch;~ResumeWatch(){if(watch)watch->resume();}} resume_watch{transfer.watch.get()};
    // 完整監看中的空通道依通知接收；取消重試仍按原期限處理。
    // 沒有監看或監看已停止時沿用每毫秒退避；同步請求與通知都立即排空。
    const auto now=std::chrono::steady_clock::now();
    const bool watched=transfer.watch && transfer.watch->active();
    if(!force && !signalled && watched && transfer.family_cancellations.empty())return;
    if(!force && !signalled && now<transfer.next_idle_poll)return;
    transfer.next_idle_poll={};
    for(auto it=transfer.family_cancellations.begin();it!=transfer.family_cancellations.end();){
        auto& cancel=it->second;const auto& packet=cancel.packet;
        const auto status=process_identity::query(it->first.first);
        if(now>=cancel.deadline || status.gone({it->first.first,packet.auxiliary})){
            it=transfer.family_cancellations.erase(it);continue;
        }
        if(!status.live({it->first.first,packet.auxiliary})){cancel.next_attempt=now+std::chrono::milliseconds(1);++it;continue;}
        if(now<cancel.next_attempt){++it;continue;}
        int error;
        try{error=send_transfer(transfer.descriptor,endpoint(transfer.directory,it->first.first),packet);}
        catch(const std::bad_alloc&){error=ENOBUFS;}
        if(!error){it=transfer.family_cancellations.erase(it);continue;}
        cancel.next_attempt=now+std::chrono::milliseconds(1);++it;
    }
    if(!force && !signalled && watched){transfer.next_idle_poll=now+std::chrono::milliseconds(1);return;}
    for(unsigned received=0;received<64;++received){
        if(registry_io_busy_){
            // 外部程序也可能暫停／改寫目前保留原生框架的 owner。請求仍留在
            // 有界 socket 佇列，交易完成後依原順序處理，不回傳假的 busy 成功。
            // peek 同樣可能複製 SCM_RIGHTS；沿完整 control buffer 契約接收並
            // 立即關閉 peek 的副本，不能讓無 control buffer 的 recv 洩漏 fd。
            TransferMessage next{};iovec peek_data{&next,sizeof(next)};msghdr peek_message{};
            alignas(cmsghdr) std::array<char,8192> peek_control{};
            peek_message.msg_iov=&peek_data;peek_message.msg_iovlen=1;
            peek_message.msg_control=peek_control.data();peek_message.msg_controllen=peek_control.size();
            const auto peek=socket_detail::SocketStateAccess::receive_rights(transfer.descriptor,peek_message,MSG_PEEK|MSG_DONTWAIT);
            if(peek<0){
                if(errno==EINTR)continue;
                if(errno==EINPROGRESS || errno==EAGAIN || errno==EWOULDBLOCK)break;
                throw HostCallFault("程序控制通道預覽失敗");
            }
            if(peek>=0)for(auto* header=CMSG_FIRSTHDR(&peek_message);header;header=CMSG_NXTHDR(&peek_message,header))
                if(header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS && header->cmsg_len>=CMSG_LEN(sizeof(int))){
                    const auto bytes=header->cmsg_len-CMSG_LEN(0);
                    for(std::size_t at=0;at+sizeof(int)<=bytes;at+=sizeof(int)){int fd=-1;std::memcpy(&fd,CMSG_DATA(header)+at,sizeof(fd));(void)socket_detail::SocketStateAccess::close_descriptor(fd);}
                }
            if(peek>=static_cast<ssize_t>(sizeof(next)) &&
               (next.kind==6 || next.kind==7 || next.kind==11 || next.kind==12 ||
                next.kind==14 || next.kind==15 || next.kind==18 || next.kind==family_request_kind))break;
        }
        TransferMessage packet;sockaddr_un sender{};std::array<std::uint8_t,1232> context{};std::array<iovec,2> data{{{&packet,sizeof(packet)},{context.data(),context.size()}}};msghdr message{};
        alignas(cmsghdr) std::array<char,CMSG_SPACE(2*sizeof(int))> control{};message.msg_name=&sender;message.msg_namelen=sizeof(sender);message.msg_iov=data.data();message.msg_iovlen=data.size();message.msg_control=control.data();message.msg_controllen=control.size();
        ssize_t count;{startup_profile::Scope measurement("scheduler",force?"handle_recvmsg_forced":"handle_recvmsg_idle");count=socket_detail::SocketStateAccess::receive_rights(transfer.descriptor,message,MSG_DONTWAIT);}if(count<0){if(errno==EINTR)continue;if(errno==EINPROGRESS)break;if(errno==EAGAIN || errno==EWOULDBLOCK){transfer.next_idle_poll=now+std::chrono::milliseconds(1);break;}throw HostCallFault("程序 handle 通道接收失敗");}
        int descriptor=-1,state_descriptor=-1;unsigned descriptor_count=0;
        for(auto* header=CMSG_FIRSTHDR(&message);header;header=CMSG_NXTHDR(&message,header))if(header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS && header->cmsg_len>=CMSG_LEN(sizeof(int))){
            const auto bytes=header->cmsg_len-CMSG_LEN(0);for(std::size_t offset=0;offset+sizeof(int)<=bytes;offset+=sizeof(int)){int fd=-1;std::memcpy(&fd,CMSG_DATA(header)+offset,sizeof(fd));if(!descriptor_count)descriptor=fd;else if(descriptor_count==1)state_descriptor=fd;else (void)socket_detail::SocketStateAccess::close_descriptor(fd);++descriptor_count;}}
        struct CloseFd {int& descriptor;~CloseFd(){if(descriptor>=0)(void)socket_detail::SocketStateAccess::close_descriptor(descriptor);}} cleanup{descriptor},state_cleanup{state_descriptor};
        const auto path=endpoint(transfer.directory,packet.sender);
        const auto context_size=count>=static_cast<ssize_t>(sizeof(packet))?static_cast<std::size_t>(count)-sizeof(packet):0;
        const bool context_packet=packet.kind==3 || (packet.kind==18 && packet.options==4 && ((packet.mode==32 && context_size==716) || (packet.mode==64 && context_size==1232)));
        if(count<static_cast<ssize_t>(sizeof(packet)) || (context_size && (!context_packet || packet.reserved!=0x31585443U || (context_size!=716 && context_size!=1232) || descriptor_count)) || (message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) || packet.magic!=TransferMessage{}.magic || sender.sun_family!=AF_UNIX || std::string(sender.sun_path,strnlen(sender.sun_path,sizeof(sender.sun_path)))!=path)continue;
        if(descriptor>=0)(void)::fcntl(descriptor,F_SETFD,FD_CLOEXEC);if(state_descriptor>=0)(void)::fcntl(state_descriptor,F_SETFD,FD_CLOEXEC);
        if(descriptor_count>2 || (state_descriptor>=0 && packet.kind!=16 && packet.kind!=family_request_kind))continue;
        const TransferKey key{packet.sender,packet.sequence};
        if(packet.kind==17){std::erase_if(runtime_.network.duplications,[&](const auto& item){return item.second.sender==packet.sender && item.second.sequence==packet.sequence;});continue;}
        if(packet.kind==3){
            // 已取消或完成的家族 RPC 回覆直接忽略，不佔用其他程序控制的回覆表。
            if(packet.reserved==family_protocol && !transfer.family_requests.contains(key))continue;
            if(packet.reserved==duplicate_protocol && !transfer.duplicate_requests.contains(key))continue;
            if(transfer.replies.size()<1024){
                const auto [position,inserted]=transfer.replies.emplace(key,TransferReply{packet,descriptor,std::vector<std::uint8_t>(context.begin(),context.begin()+context_size)});
                (void)position;if(inserted)descriptor=-1;
            }continue;
        }
        if(packet.kind==4 || packet.kind==5){transfer.exports.erase(key);if(const auto found=transfer.imports.find(key);found!=transfer.imports.end()){if(packet.kind==5)close_transfer_handle(memory,found->second);transfer.imports.erase(found);}continue;}
        TransferMessage reply;reply.kind=3;reply.sequence=packet.sequence;reply.sender=process_id();reply.reserved=packet.reserved;InheritedHandle item;
        if(packet.kind==6){reply.handle=local_process_handle_count();reply.type=active_thread_count();(void)send_transfer(transfer.descriptor,path,reply);}
        else if(packet.kind==7 || packet.kind==8){reply.error=packet.kind==7?(descriptor<0?6U:import_process_job(descriptor)):0U;reply.handle=current_job_identity();(void)send_transfer(transfer.descriptor,path,reply);}
        else if(packet.kind==10){reply.error=local_process_accounting(reply.accounting);(void)send_transfer(transfer.descriptor,path,reply);}
        else if(packet.kind==11){
            auto value=static_cast<std::uint32_t>(packet.handle);
            reply.auxiliary=process_identity_birth();
            reply.error=packet.reserved==family_protocol && packet.auxiliary!=reply.auxiliary?87U:local_process_thread_control(packet.options,value);
            reply.handle=value;reply.accounting[0]=value;(void)send_transfer(transfer.descriptor,path,reply);
        }
        else if(packet.kind==12){
            struct stat info{};const auto size=x86_?716U:1232U;
            if(descriptor<0 || packet.options>1 || packet.mode!=(x86_?32U:64U))reply.error=50;
            else if(::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size!=size)reply.error=13;
            else {
                std::vector<std::uint8_t> bytes(size);ssize_t bytes_read{};do{bytes_read=::pread(descriptor,bytes.data(),size,0);}while(bytes_read<0 && errno==EINTR);
                if(bytes_read!=size)reply.error=13;
                else {reply.error=thread_context_bytes(1,bytes,packet.options!=0,true);
                    if(!reply.error && !packet.options){do{bytes_read=::pwrite(descriptor,bytes.data(),size,0);}while(bytes_read<0 && errno==EINTR);if(bytes_read!=size)reply.error=13;}}
            }
            (void)send_transfer(transfer.descriptor,path,reply);
        }
        else if(packet.kind==13){
            int output=-1;
            reply.error=packet.auxiliary!=process_identity_birth()?87U:local_token_operation(packet.options,descriptor,packet.access,reply.access,output);
            (void)send_transfer(transfer.descriptor,path,reply,reply.error?-1:output);
        }
        else if(packet.kind==20){
            reply.error=descriptor_count?87U:receive_console_control(memory,packet.sender,packet.auxiliary,packet.accounting[0],static_cast<std::uint32_t>(packet.accounting[1]),packet.options);
            (void)send_transfer(transfer.descriptor,path,reply);
        }
        else if(packet.kind==family_request_kind){
            reply.reserved=family_protocol;reply.auxiliary=process_identity_birth();
            const auto caller=process_identity::query(packet.sender);
            reply.error=packet.reserved!=family_protocol || !packet.sequence || !packet.options || !packet.flags || packet.flags>60000 ||
                !packet.auxiliary || packet.auxiliary!=reply.auxiliary || !caller.live({packet.sender,packet.handle})?87U:0U;
            if(!reply.error)reply.error=packet.options==1?
                queue_perflib_remote(packet.sender,packet.handle,packet.sequence,packet.type,packet.flags,packet.accounting,packet.mode,descriptor,state_descriptor):50U;
            // 已接受的請求由客體 worker 執行及回覆，不能在排入工作時回傳 API 成功。
            if(reply.error)(void)send_transfer(transfer.descriptor,path,reply);
        }
        else if(packet.kind==family_cancel_kind){
            if(packet.reserved==family_protocol && packet.options==1 && packet.sequence && !descriptor_count &&
               packet.auxiliary==process_identity_birth() && process_identity::query(packet.sender).live({packet.sender,packet.handle}))
                cancel_perflib_remote(packet.sender,packet.handle,packet.sequence);
        }
        else if(packet.kind==14 || packet.kind==15 || packet.kind==19){
            reply.accounting=packet.accounting;reply.mode=packet.mode;
            // 每次請求仍核對建立時間，避免舊 handle 因 PID 重用操作另一程序。
            if(!packet.auxiliary || packet.auxiliary!=process_identity_birth())reply.error=87;
            else {
                const bool needs_file=packet.kind==14?(packet.options==6 || packet.options==7 || packet.options==8 || packet.options==9):packet.kind==15 && (packet.options<=1 || packet.options==9);
                struct stat info{};const auto size=packet.kind==14?UINT64_C(65534):packet.options==9?UINT64_C(24):packet.accounting[1];
                if(needs_file && (descriptor<0 || size>static_cast<std::uint64_t>(INT64_MAX) || ::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink || info.st_size<0 || static_cast<std::uint64_t>(info.st_size)!=size))reply.error=13;
                else if(packet.kind==19){
                    if(packet.options&~0x1ffffU)reply.error=87;
                    else reply.error=local_process_api_information(memory,packet.options&0xffffU,(packet.options&0x10000U)!=0,reply.accounting);
                }else if(packet.kind==14 && packet.options==9){
                    std::vector<std::uint8_t> snapshot_data;reply.error=collect_pss_process_data(memory,snapshot_data);
                    if(!reply.error){std::size_t done=0;while(done<snapshot_data.size()){const auto written=::pwrite(descriptor,snapshot_data.data()+done,snapshot_data.size()-done,static_cast<off_t>(done));if(written<0 && errno==EINTR)continue;if(written<=0){reply.error=8;break;}done+=static_cast<std::size_t>(written);}
                        reply.accounting[0]=snapshot_data.size();reply.mode=x86_?32U:64U;}
                }else reply.error=packet.kind==14?local_process_information(packet.options,reply.accounting,reply.mode,descriptor):local_memory_operation(memory,packet.options,reply.accounting,descriptor);
            }
            (void)send_transfer(transfer.descriptor,path,reply);
        }
        else if(packet.kind==9){auto value=static_cast<std::uint32_t>(packet.handle);reply.error=local_process_priority_operation(packet.options,value);reply.handle=value;(void)send_transfer(transfer.descriptor,path,reply);}
        else if(packet.kind==18){
            std::uint32_t value=0;std::vector<std::uint8_t> result;if(packet.options==4)result.assign(context.begin(),context.begin()+static_cast<std::ptrdiff_t>(context_size));reply.reserved=0x31585443U;reply.options=packet.options;reply.mode=packet.mode;
            const auto identity=process_debugger::query(process_id());reply.auxiliary=identity.identity;
            reply.error=(descriptor_count || !packet.handle || packet.handle>UINT32_MAX || (packet.mode!=32 && packet.mode!=64) || identity.error || identity.exited || !identity.identity || (packet.auxiliary && packet.auxiliary!=identity.identity) || (packet.options==4 && !packet.auxiliary))?87U:local_primary_thread_control(packet.options,packet.mode==32?ExecutionMode::X86:ExecutionMode::X64,packet.flags,value,result,static_cast<std::uint32_t>(packet.handle));
            if(packet.options==4)result.clear();
            reply.handle=value;(void)send_transfer(transfer.descriptor,path,reply,-1,-1,result);
        }
        else if(packet.kind==16){
            reply.error=10022;
            if(descriptor>=0 && state_descriptor>=0 && runtime_.network.duplications.size()<1024){
                sockaddr_storage address{};socklen_t length=sizeof(address);int type=0;socklen_t type_size=sizeof(type);
                if(!::getsockname(descriptor,reinterpret_cast<sockaddr*>(&address),&length) && !::getsockopt(descriptor,SOL_SOCKET,SO_TYPE,&type,&type_size) &&
                   static_cast<std::uint32_t>(address.ss_family)==packet.type && static_cast<std::uint32_t>(type)==packet.access){
                    try{auto shared=std::make_shared<SharedSocketState>(state_descriptor);
                        auto socket=std::make_shared<NativeSocket>(descriptor,static_cast<int>(packet.type),type,static_cast<int>(packet.mode),std::move(shared));descriptor=-1;
                        reply.handle=reserve_socket_duplication(std::move(socket),packet.sender,packet.sequence);reply.error=reply.handle?0U:10055U;
                    }catch(const std::system_error&){reply.error=10055;}catch(const std::bad_alloc&){reply.error=10055;}
                }
            }
            (void)send_transfer(transfer.descriptor,path,reply);
        }
        else if(packet.kind==1){
            if(descriptor<0 || packet.type<1 || (packet.type>7 && packet.type!=9) || packet.flags&~1U)reply.error=87;
            else if(transfer.imports.size()>=1024 || files_.size()+runtime_.wait_objects.size()>=process_handle_limit)reply.error=8;
            else {
                item.handle=runtime_.allocate_wait_handle();item.type=packet.type;item.access=packet.access;item.flags=packet.flags;item.mode=packet.mode;item.pipe_mode=packet.pipe_mode;item.auxiliary=packet.auxiliary;item.descriptor=descriptor;descriptor=-1;
                try {import_transfer_handle(item);reply.handle=item.handle;transfer.imports.emplace(key,item.handle);}catch(const HostCallFault& failure){reply.error=50;if(std::getenv("X64ARM_TRACE_HANDLE_TRANSFER"))std::fprintf(stderr,"[handle_import_error] pid=%u sender=%u type=%u handle=%llx access=%x detail=%s\n",process_id(),packet.sender,item.type,static_cast<unsigned long long>(item.handle),item.access,failure.what());}
            }
            (void)send_transfer(transfer.descriptor,path,reply);
        }else if(packet.kind==2){
            reply.error=export_transfer_handle(packet.handle,item);
            if(!reply.error && (packet.options&~3U))reply.error=87;
            if(!reply.error && ((packet.options&1U) && (item.flags&2U)))reply.error=5;
            if(!reply.error && !(packet.options&2U)){
                if(item.type==3 || item.type==5){try{initialize_tokens();const auto all=item.type==5?0x1f0001U:0x1f0003U;const security::AccessMapping mapping{0x20001U,item.type==5?0x20001U:0x20002U,0x120000U,all};item.access=security::duplicate_object_access(runtime_.token_objects.at(effective_token_id()),security::parse_relative_descriptor(shared_object_security(item.descriptor)),item.access,packet.access,mapping);}catch(const SecurityDescriptorError& failure){reply.error=failure.code;}}
                else {const auto requested=transfer_access(item.type,packet.access);if(requested&~transfer_access(item.type,item.access))reply.error=5;else item.access=requested;}
            }
            if(!reply.error && item.type==3 && item.auxiliary && (item.access&~0x100001U))reply.error=5;
            if(!reply.error && transfer.exports.size()>=1024)reply.error=8;
            if(!reply.error){
                reply.type=item.type;reply.access=item.access;reply.flags=packet.flags;reply.mode=item.mode;reply.pipe_mode=item.pipe_mode;reply.auxiliary=item.auxiliary;
                transfer.exports.emplace(key,item.lifetime);
                if(packet.options&1U)close_transfer_handle(memory,packet.handle);
            }
            if(reply.error && std::getenv("X64ARM_TRACE_HANDLE_TRANSFER"))std::fprintf(stderr,"[handle_export_error] pid=%u requester=%u handle=%llx requested_access=%x options=%x error=%u\n",process_id(),packet.sender,static_cast<unsigned long long>(packet.handle),packet.access,packet.options,reply.error);
            (void)send_transfer(transfer.descriptor,path,reply,reply.error?-1:item.descriptor);
        }
    }
#else
    poll_debug_memory(memory,0,force);(void)memory;
#endif
}
std::uint32_t WindowsServices::remote_process_accounting(GuestMemory& memory,std::uint32_t pid,std::array<std::uint64_t,5>& values){
#if !defined(_WIN32)
    if(!handle_transfers_ || !pid)return 50;auto& transfer=*handle_transfers_;
    TransferMessage request;request.kind=10;request.sender=process_id();request.sequence=transfer.next_sequence++;
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request);if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 50;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        if(found!=transfer.replies.end()){auto reply=found->second;transfer.replies.erase(found);if(reply.descriptor>=0)::close(reply.descriptor);if(!reply.message.error)values=reply.message.accounting;return reply.message.error;}
        if(!remote_process_available(pid))return 6;if(std::chrono::steady_clock::now()>=deadline)return 121;
        pollfd waiting{transfer.descriptor,POLLIN,0};(void)::poll(&waiting,1,1);
    }
#else
    (void)memory;(void)pid;(void)values;return 50;
#endif
}
std::uint32_t WindowsServices::remote_process_thread_control(GuestMemory& memory,std::uint32_t pid,std::uint32_t operation,std::uint32_t& value){
#if !defined(_WIN32)
    if(!handle_transfers_ || !pid)return 50;auto& transfer=*handle_transfers_;
    TransferMessage request;request.kind=11;request.sender=process_id();request.sequence=transfer.next_sequence++;request.options=operation;
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request);if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 50;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        if(found!=transfer.replies.end()){auto reply=found->second;transfer.replies.erase(found);if(reply.descriptor>=0)::close(reply.descriptor);if(!reply.message.error)value=static_cast<std::uint32_t>(reply.message.handle);return reply.message.error;}
        if(!remote_process_available(pid))return 6;if(std::chrono::steady_clock::now()>=deadline)return 121;
        pollfd waiting{transfer.descriptor,POLLIN,0};(void)::poll(&waiting,1,1);
    }
#else
    (void)memory;(void)pid;(void)operation;(void)value;return 50;
#endif
}
std::uint32_t WindowsServices::remote_process_request(GuestMemory& memory,std::uint32_t pid,std::uint64_t birth,std::uint32_t kind,std::uint32_t operation,std::array<std::uint64_t,5>& values,std::uint32_t& mode,int descriptor){
#if !defined(_WIN32)
    if(!runtime_.network.external_io || !handle_transfers_ || !pid || !birth)return 50;auto& transfer=*handle_transfers_;
    TransferMessage request;request.kind=kind;request.sender=process_id();request.sequence=transfer.next_sequence++;request.options=operation;request.auxiliary=birth;request.accounting=values;request.mode=mode;
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request,descriptor);if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 50;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        if(found!=transfer.replies.end()){auto reply=found->second;transfer.replies.erase(found);if(reply.descriptor>=0)::close(reply.descriptor);values=reply.message.accounting;mode=reply.message.mode;return reply.message.error;}
        if(!remote_process_available(pid))return 87;if(std::chrono::steady_clock::now()>=deadline)return 121;
        pollfd waiting{transfer.descriptor,POLLIN,0};(void)::poll(&waiting,1,1);
    }
#else
    (void)memory;(void)pid;(void)birth;(void)kind;(void)operation;(void)values;(void)mode;(void)descriptor;return 50;
#endif
}
std::uint32_t WindowsServices::begin_remote_process_thread_control(std::uint32_t pid,std::uint64_t birth,std::uint32_t operation,std::uint64_t& sequence){
    sequence=0;
#if !defined(_WIN32)
    if(!handle_transfers_ || !pid || !birth)return 50;
    if(!process_identity::query(pid).live({pid,birth}))return 87;
    auto& transfer=*handle_transfers_;
    if(transfer.family_requests.size()>=1024 || !transfer.next_sequence || transfer.next_sequence==UINT64_MAX)return 8;
    try{
        auto request=std::make_shared<FamilyRequest>();request->birth=birth;
        request->deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        auto& packet=request->packet;packet.kind=11;packet.sender=process_id();packet.sequence=transfer.next_sequence++;
        packet.options=operation;packet.auxiliary=birth;packet.reserved=family_protocol;
        transfer.family_requests.emplace(TransferKey{pid,packet.sequence},request);sequence=packet.sequence;return 0;
    }catch(const std::bad_alloc&){return 8;}
#else
    (void)pid;(void)birth;(void)operation;return 50;
#endif
}
std::uint32_t WindowsServices::begin_remote_family_request(std::uint32_t pid,std::uint64_t birth,std::uint32_t family,std::uint32_t operation,
    const std::array<std::uint64_t,5>& values,std::uint32_t mode,int descriptor,int state_descriptor,std::uint64_t& sequence,std::uint32_t timeout){
    sequence=0;
#if !defined(_WIN32)
    if(!runtime_.network.external_io || !handle_transfers_ || !process_identity_birth())return 50;
    if(!pid || !birth || !family || !timeout || timeout>60000 || (state_descriptor>=0 && descriptor<0))return 87;
    if(!process_identity::query(pid).live({pid,birth}))return 87;
    auto& transfer=*handle_transfers_;if(transfer.family_requests.size()>=1024 || !transfer.next_sequence || transfer.next_sequence==UINT64_MAX)return 8;
    try{
        auto request=std::make_shared<FamilyRequest>();request->birth=birth;request->deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        const auto duplicate=[](int input){if(input<0)return -1;int output;do{output=::fcntl(input,F_DUPFD_CLOEXEC,0);}while(output<0 && errno==EINTR);return output;};
        if(descriptor>=0){request->descriptor=duplicate(descriptor);if(request->descriptor<0)return 6;}
        if(state_descriptor>=0){request->state_descriptor=duplicate(state_descriptor);if(request->state_descriptor<0)return 6;}
        auto& packet=request->packet;packet.kind=family_request_kind;packet.sender=process_id();packet.handle=process_identity_birth();
        packet.sequence=transfer.next_sequence++;packet.auxiliary=birth;packet.reserved=family_protocol;
        packet.options=family;packet.type=operation;packet.accounting=values;packet.mode=mode;packet.flags=timeout;
        // 送出與重試都由 poll 進行；呼叫者可先保存 continuation，再讓出客體執行緒。
        if(!transfer.family_requests.emplace(TransferKey{pid,packet.sequence},request).second)return 8;
        sequence=packet.sequence;return 0;
    }catch(const std::bad_alloc&){return 8;}
#else
    (void)pid;(void)birth;(void)family;(void)operation;(void)values;(void)mode;(void)descriptor;(void)state_descriptor;(void)timeout;return 50;
#endif
}
WindowsServices::RemoteFamilyResult WindowsServices::poll_remote_family_request(GuestMemory& memory,std::uint32_t pid,std::uint64_t sequence){
#if !defined(_WIN32)
    if(!handle_transfers_)return {true,50,{},0};
    auto& transfer=*handle_transfers_;const TransferKey key{pid,sequence};const auto found=transfer.family_requests.find(key);
    if(found==transfer.family_requests.end())return {true,6,{},0};const auto request=found->second;
    const auto finish=[&](RemoteFamilyResult result){
        if(const auto reply=transfer.replies.find(key);reply!=transfer.replies.end()){
            if(reply->second.descriptor>=0)::close(reply->second.descriptor);transfer.replies.erase(reply);
        }transfer.family_requests.erase(key);return result;
    };
    if(!runtime_.network.external_io && request->packet.kind!=11)return finish({true,50,{},0});
    if(request->packet.kind==11){
        // Resume 的回覆可能先於子程序快速退出；已確認的操作不可因隨後
        // 的退出被改判失敗。回覆仍核對請求的 birth，排除 PID 重用。
        poll_handle_transfers(memory,true);
        if(const auto reply=transfer.replies.find(key);reply!=transfer.replies.end()){
            const auto& value=reply->second;
            if(value.message.reserved!=family_protocol || value.message.auxiliary!=request->birth || value.descriptor>=0 || !value.context.empty())return finish({true,13,{},0});
            return finish({true,value.message.error,value.message.accounting,value.message.mode});
        }
    }
    if(!request->sent && std::chrono::steady_clock::now()>=request->deadline){cancel_remote_family_request(pid,sequence);return {true,121,{},0};}
    if(!process_identity::query(pid).live({pid,request->birth}))return finish({true,87,{},0});
    if(!request->sent){
        const auto error=send_transfer(transfer.descriptor,endpoint(transfer.directory,pid),request->packet,request->descriptor,request->state_descriptor);
        if(!error)request->sent=true;
        else if(error!=EAGAIN && error!=EWOULDBLOCK && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)
            return finish({true,error==EACCES || error==EPERM?5U:31U,{},0});
    }
    poll_handle_transfers(memory,true);
    if(const auto reply=transfer.replies.find(key);reply!=transfer.replies.end()){
        const auto& value=reply->second;
        if(value.message.reserved!=family_protocol || value.message.auxiliary!=request->birth || value.descriptor>=0 || !value.context.empty())return finish({true,13,{},0});
        return finish({true,value.message.error,value.message.accounting,value.message.mode});
    }
    if(std::chrono::steady_clock::now()>=request->deadline){cancel_remote_family_request(pid,sequence);return {true,121,{},0};}
    return {};
#else
    (void)memory;(void)pid;(void)sequence;return {true,50,{},0};
#endif
}
void WindowsServices::cancel_remote_family_request(std::uint32_t pid,std::uint64_t sequence){
#if !defined(_WIN32)
    if(!handle_transfers_)return;
    auto& transfer=*handle_transfers_;const TransferKey key{pid,sequence};const auto found=transfer.family_requests.find(key);
    if(found==transfer.family_requests.end())return;
    const auto request=found->second;
    if(const auto reply=transfer.replies.find(key);reply!=transfer.replies.end()){
        if(reply->second.descriptor>=0)::close(reply->second.descriptor);transfer.replies.erase(reply);
    }transfer.family_requests.erase(key);
    if(request->sent && request->packet.kind==family_request_kind)(void)queue_remote_family_cancel(pid,request->birth,request->packet.options,sequence);
#else
    (void)pid;(void)sequence;
#endif
}
std::uint32_t WindowsServices::queue_remote_family_cancel(std::uint32_t pid,std::uint64_t birth,std::uint32_t family,std::uint64_t sequence){
#if !defined(_WIN32)
    if(!runtime_.network.external_io || !handle_transfers_ || !process_identity_birth())return 50;
    if(!pid || !birth || !family || !sequence)return 87;
    const auto status=process_identity::query(pid);if(status.gone({pid,birth}))return 0;
    // 查詢失敗不等於對方已結束；保留指定出生識別，交由送出輪次重新確認。
    auto& transfer=*handle_transfers_;const TransferKey key{pid,sequence};
    if(const auto found=transfer.family_cancellations.find(key);found!=transfer.family_cancellations.end())
        return found->second.packet.auxiliary==birth && found->second.packet.options==family?0U:87U;
    if(transfer.family_cancellations.size()>=1024)return 8;
    try{
        TransferMessage packet;packet.kind=family_cancel_kind;packet.sender=process_id();packet.sequence=sequence;packet.handle=process_identity_birth();
        packet.auxiliary=birth;packet.options=family;packet.reserved=family_protocol;
        transfer.family_cancellations.emplace(key,FamilyCancel{packet,std::chrono::steady_clock::now()+std::chrono::seconds(60),{}});
        transfer.next_idle_poll={};return 0; // 接受送出工作；不代表遠端 Remove 回呼已完成。
    }catch(const std::bad_alloc&){return 8;}
#else
    (void)pid;(void)birth;(void)family;(void)sequence;return 50;
#endif
}
std::uint32_t WindowsServices::complete_remote_family_request(std::uint32_t pid,std::uint64_t sequence,std::uint64_t birth,std::uint32_t error,
    const std::array<std::uint64_t,5>& values,std::uint32_t mode){
#if !defined(_WIN32)
    if(!runtime_.network.external_io || !handle_transfers_ || !process_identity_birth())return 50;
    if(!sequence || !process_identity::query(pid).live({pid,birth}))return 87;
    auto& transfer=*handle_transfers_;TransferMessage reply;reply.kind=3;reply.sequence=sequence;reply.sender=process_id();reply.auxiliary=process_identity_birth();
    reply.reserved=family_protocol;reply.error=error;reply.accounting=values;reply.mode=mode;
    return send_transfer(transfer.descriptor,endpoint(transfer.directory,pid),reply)?31U:0U;
#else
    (void)pid;(void)sequence;(void)birth;(void)error;(void)values;(void)mode;return 50;
#endif
}
bool WindowsServices::remote_family_waiting()const{
    return runtime_.network.external_io && handle_transfers_ && (!handle_transfers_->family_requests.empty() || !handle_transfers_->family_cancellations.empty());
}
std::uint32_t WindowsServices::remote_thread_context(GuestMemory& memory,std::uint32_t pid,std::vector<std::uint8_t>& bytes,bool setting){
#if !defined(_WIN32)
    if(!handle_transfers_ || !pid)return 50;auto& transfer=*handle_transfers_;
    auto pattern=(std::filesystem::temp_directory_path()/"x64arm-context-XXXXXX").string();std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    const auto descriptor=::mkstemp(name.data());if(descriptor<0)return 8;
    struct Close {int fd;~Close(){::close(fd);}} cleanup{descriptor};(void)::unlink(name.data());(void)::fcntl(descriptor,F_SETFD,FD_CLOEXEC);
    if(::fchmod(descriptor,0600))return 5;
    ssize_t count{};do{count=::pwrite(descriptor,bytes.data(),bytes.size(),0);}while(count<0 && errno==EINTR);if(count!=static_cast<ssize_t>(bytes.size()))return 8;
    TransferMessage request;request.kind=12;request.sender=process_id();request.sequence=transfer.next_sequence++;request.options=setting?1U:0U;request.mode=x86_?32U:64U;
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request,descriptor);if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 50;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        if(found!=transfer.replies.end()){const auto reply=found->second;transfer.replies.erase(found);if(reply.descriptor>=0)::close(reply.descriptor);if(reply.message.error)return reply.message.error;
            if(!setting){do{count=::pread(descriptor,bytes.data(),bytes.size(),0);}while(count<0 && errno==EINTR);if(count!=static_cast<ssize_t>(bytes.size()))return 13;}return 0;}
        if(!remote_process_available(pid))return 6;if(std::chrono::steady_clock::now()>=deadline)return 121;
        pollfd waiting{transfer.descriptor,POLLIN,0};(void)::poll(&waiting,1,1);
    }
#else
    (void)memory;(void)pid;(void)bytes;(void)setting;return 50;
#endif
}
std::uint32_t WindowsServices::remote_token_operation(GuestMemory& memory,std::uint32_t pid,std::uint64_t birth,std::uint32_t operation,int descriptor,std::uint32_t desired,std::uint32_t& granted,std::shared_ptr<SharedToken>& token){
#if !defined(_WIN32)
    if(!runtime_.network.external_io || !handle_transfers_ || !pid)return 50;auto& transfer=*handle_transfers_;
    TransferMessage request;request.kind=13;request.sender=process_id();request.sequence=transfer.next_sequence++;request.options=operation;request.access=desired;request.auxiliary=birth;
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request,descriptor);if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 50;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        if(found!=transfer.replies.end()){auto reply=found->second;transfer.replies.erase(found);struct Close {int fd;~Close(){if(fd>=0)::close(fd);}} cleanup{reply.descriptor};
            if(reply.message.error)return reply.message.error;
            if(operation){try{token=SharedToken::inherit(reply.descriptor);granted=reply.message.access;}catch(const SecurityDescriptorError& error){return error.code;}}
            return 0;
        }
        if(!remote_process_available(pid))return 87;if(std::chrono::steady_clock::now()>=deadline)return 121;
        pollfd waiting{transfer.descriptor,POLLIN,0};(void)::poll(&waiting,1,1);
    }
#else
    (void)memory;(void)pid;(void)birth;(void)operation;(void)descriptor;(void)desired;(void)granted;(void)token;return 50;
#endif
}
std::uint32_t WindowsServices::reserve_socket_duplication(std::shared_ptr<NativeSocket> socket,std::uint32_t sender,std::uint64_t sequence){
    auto& pending=runtime_.network.duplications;if(pending.size()>=1024)return 0;
    std::uint32_t token=0;do{token=::arc4random();}while(!token || pending.contains(token));
    pending.emplace(token,SocketDuplication{std::move(socket),sender,sequence});return token;
}
std::uint32_t WindowsServices::transfer_thread_control(GuestMemory& memory,std::uint32_t pid,std::uint32_t operation,ExecutionMode mode,std::uint32_t flags,std::uint32_t& value,std::vector<std::uint8_t>& context,std::uint64_t expected_birth,std::uint32_t target_thread) {
#if !defined(_WIN32)
    if(!handle_transfers_ || !remote_process_available(pid))return 87;
    const auto identity=process_debugger::query(pid);if(identity.error || identity.exited || !identity.identity || (expected_birth && identity.identity!=expected_birth))return 87;
    auto& transfer=*handle_transfers_;TransferMessage request;
    request.kind=18;request.sender=process_id();request.sequence=transfer.next_sequence++;request.handle=target_thread;
    request.options=operation;request.mode=mode==ExecutionMode::X86?32:64;request.flags=flags;
    request.auxiliary=identity.identity;
    if(operation==4){if(context.size()!=thread_context::size(mode))return 87;request.reserved=0x31585443U;}
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;) {
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request,-1,-1,operation==4?std::span<const std::uint8_t>(context):std::span<const std::uint8_t>{});if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 87;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        const auto current=process_debugger::query(pid);if(current.error || current.exited || current.identity!=identity.identity)return 87;
        if(found!=transfer.replies.end()) {
            auto reply=std::move(found->second);transfer.replies.erase(found);
            if(reply.descriptor>=0)::close(reply.descriptor);
            if(reply.message.reserved!=0x31585443U || reply.message.options!=operation || reply.message.mode!=request.mode || reply.descriptor>=0 || reply.message.auxiliary!=identity.identity)return 13;
            if(reply.message.error)return reply.message.error;
            if((operation==1 && reply.context.size()!=thread_context::size(mode)) || (operation!=1 && !reply.context.empty()))return 13;
            if(operation==1){const auto offset=thread_context::flags_offset(mode);std::uint32_t returned=0;for(unsigned i=0;i<4;++i)returned|=std::uint32_t{reply.context[offset+i]}<<(8*i);if(returned!=flags)return 13;}
            value=static_cast<std::uint32_t>(reply.message.handle);context=std::move(reply.context);return 0;
        }
        if(std::chrono::steady_clock::now()>=deadline)return 1460;
        if(!remote_process_available(pid))return 87;
        pollfd wait{transfer.descriptor,POLLIN,0};(void)::poll(&wait,1,1);
    }
#else
    (void)memory;(void)pid;(void)operation;(void)mode;(void)flags;(void)value;(void)context;(void)expected_birth;(void)target_thread;return 50;
#endif
}
std::uint32_t WindowsServices::duplicate_socket_to_process(GuestMemory& memory,std::uint64_t handle,std::uint32_t pid,std::uint32_t& token){
    const auto found=runtime_.network.sockets.find(handle);if(found==runtime_.network.sockets.end())return 10038;
    if(pid==process_id()){token=reserve_socket_duplication(found->second);return token?0U:10055U;}
#if !defined(_WIN32)
    if(!pid || !remote_process_available(pid))return 10022;auto& transfer=*handle_transfers_;const auto socket=found->second;
    TransferMessage request;request.kind=16;request.sender=process_id();request.sequence=transfer.next_sequence++;
    request.type=static_cast<std::uint32_t>(socket->family);request.access=static_cast<std::uint32_t>(socket->type);request.mode=static_cast<std::uint32_t>(socket->protocol);
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    const auto cancel=[&]{auto packet=request;packet.kind=17;(void)send_transfer(transfer.descriptor,path,packet);};
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request,socket->descriptor,socket->shared->descriptor());if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS){cancel();return 10022;}}
        poll_handle_transfers(memory,true);const auto reply=transfer.replies.find({pid,request.sequence});
        if(reply!=transfer.replies.end()){const auto packet=reply->second;transfer.replies.erase(reply);if(packet.descriptor>=0)::close(packet.descriptor);if(packet.message.error)return packet.message.error;token=static_cast<std::uint32_t>(packet.message.handle);return token?0U:10022U;}
        if(!remote_process_available(pid) || std::chrono::steady_clock::now()>=deadline){cancel();return 10022;}
        pollfd wait{transfer.descriptor,POLLIN,0};(void)::poll(&wait,1,2);
    }
#else
    (void)memory;return 10022;
#endif
}
std::uint32_t WindowsServices::active_thread_count()const{return static_cast<std::uint32_t>(std::count_if(threads_.items.begin(),threads_.items.end(),[](const auto& item){return item.second.status!=ThreadStatus::Exited;}));}
std::uint32_t WindowsServices::query_process_handle_count(GuestMemory& memory,std::uint32_t pid,std::uint32_t& count){std::uint32_t threads=0;return query_process_counters(memory,pid,count,threads);}
std::uint32_t WindowsServices::process_priority_operation(GuestMemory& memory,std::uint32_t pid,std::uint32_t operation,std::uint32_t& value){
    if(pid==process_id())return local_process_priority_operation(operation,value);
#if !defined(_WIN32)
    if(!handle_transfers_)return 50;auto& transfer=*handle_transfers_;
    TransferMessage request;request.kind=9;request.sender=process_id();request.sequence=transfer.next_sequence++;request.options=operation;request.handle=value;
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request);if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 50;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        if(found!=transfer.replies.end()){const auto reply=found->second;transfer.replies.erase(found);if(reply.descriptor>=0)::close(reply.descriptor);if(reply.message.error)return reply.message.error;value=static_cast<std::uint32_t>(reply.message.handle);return 0;}
        if(::kill(static_cast<pid_t>(pid),0)<0 && errno==ESRCH)return 87;
        if(std::chrono::steady_clock::now()>=deadline)return 1460;
        pollfd wait{transfer.descriptor,POLLIN,0};(void)::poll(&wait,1,2);
    }
#else
    (void)memory;(void)operation;(void)value;return 50;
#endif
}
std::uint32_t WindowsServices::query_process_counters(GuestMemory& memory,std::uint32_t pid,std::uint32_t& count,std::uint32_t& threads){
    if(pid==process_id()){count=local_process_handle_count();threads=active_thread_count();return 0;}
#if !defined(_WIN32)
    if(!handle_transfers_)return 50;auto& transfer=*handle_transfers_;
    TransferMessage request;request.kind=6;request.sender=process_id();request.sequence=transfer.next_sequence++;
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request);if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 50;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        if(found!=transfer.replies.end()){const auto reply=found->second;transfer.replies.erase(found);if(reply.descriptor>=0)::close(reply.descriptor);if(reply.message.error)return reply.message.error;count=static_cast<std::uint32_t>(reply.message.handle);threads=reply.message.type;return 0;}
        if(::kill(static_cast<pid_t>(pid),0)<0 && errno==ESRCH)return 87;
        if(std::chrono::steady_clock::now()>=deadline)return 1460;
        pollfd wait{transfer.descriptor,POLLIN,0};(void)::poll(&wait,1,2);
    }
#else
    (void)memory;return 50;
#endif
}
std::uint32_t WindowsServices::session_process_ids(std::vector<std::uint32_t>& ids) const {
    ids={process_id()};
#if !defined(_WIN32)
    if(handle_transfers_){
        std::error_code error;std::filesystem::directory_iterator iterator(handle_transfers_->directory,error);if(error)return 5;
        for(const auto& entry:iterator){
            const auto name=entry.path().filename().string();if(!name.starts_with("process-"))continue;
            std::uint32_t pid=0;const auto parsed=std::from_chars(name.data()+8,name.data()+name.size(),pid);
            if(parsed.ec!=std::errc{} || parsed.ptr!=name.data()+name.size() || !pid || pid==process_id() || !remote_process_available(pid))continue;
            if(ids.size()>=4096)return 8;ids.push_back(pid);
        }
    }
#endif
    std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());return 0;
}
std::uint32_t WindowsServices::session_process_counters(GuestMemory& memory,std::uint32_t& handles,std::uint32_t& processes,std::uint32_t& threads){
    handles=local_process_handle_count();processes=1;threads=active_thread_count();
#if !defined(_WIN32)
    if(!handle_transfers_)return 0;
    std::error_code error;std::filesystem::directory_iterator iterator(handle_transfers_->directory,error);if(error)return 5;
    for(const auto& entry:iterator){const auto name=entry.path().filename().string();if(!name.starts_with("process-"))continue;
        std::uint32_t pid=0;const auto parsed=std::from_chars(name.data()+8,name.data()+name.size(),pid);if(parsed.ec!=std::errc{} || parsed.ptr!=name.data()+name.size() || !pid || pid==process_id() || !remote_process_available(pid))continue;
        if(processes>=4096)return 8;std::uint32_t remote_handles=0,remote_threads=0;const auto failure=query_process_counters(memory,pid,remote_handles,remote_threads);
        if(failure){if(!remote_process_available(pid))continue;return failure;}
        if(remote_handles>UINT32_MAX-handles || remote_threads>UINT32_MAX-threads)return 534;
        handles+=remote_handles;threads+=remote_threads;++processes;
    }
#else
    (void)memory;
#endif
    return 0;
}
std::uint32_t WindowsServices::remote_process_job(GuestMemory& memory,std::uint32_t pid,int descriptor,std::uint64_t& identity){
    if(pid==process_id()){const auto error=descriptor<0?0U:import_process_job(descriptor);identity=current_job_identity();return error;}
#if !defined(_WIN32)
    if(!handle_transfers_)return 50;auto& transfer=*handle_transfers_;
    TransferMessage request;request.kind=descriptor<0?8U:7U;request.sender=process_id();request.sequence=transfer.next_sequence++;
    const auto path=endpoint(transfer.directory,pid);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool sent=false;
    for(;;){
        if(!sent){const auto error=send_transfer(transfer.descriptor,path,request,descriptor);if(!error)sent=true;else if(error!=EAGAIN && error!=ENOBUFS && error!=ENOENT && error!=ECONNREFUSED)return 50;}
        poll_handle_transfers(memory,true);const auto found=transfer.replies.find({pid,request.sequence});
        if(found!=transfer.replies.end()){const auto reply=found->second;transfer.replies.erase(found);if(reply.descriptor>=0)::close(reply.descriptor);if(reply.message.error)return reply.message.error;identity=reply.message.handle;return 0;}
        if(::kill(static_cast<pid_t>(pid),0)<0 && errno==ESRCH)return 87;
        if(std::chrono::steady_clock::now()>=deadline)return 1460;
        pollfd wait{transfer.descriptor,POLLIN,0};(void)::poll(&wait,1,2);
    }
#else
    (void)memory;(void)descriptor;return 50;
#endif
}
void WindowsServices::cancel_duplicate_remote_handle(GuestMemory& memory,std::uint64_t thread){
#if !defined(_WIN32)
    (void)memory;if(!handle_transfers_)return;auto& transfer=*handle_transfers_;
    const auto found=transfer.duplicates.find(thread);if(found==transfer.duplicates.end())return;
    const auto& op=*found->second;
    const auto cancel=[&](std::uint32_t pid,std::uint64_t sequence){
        if(!sequence)return;TransferMessage packet;packet.kind=5;packet.sender=process_id();packet.sequence=sequence;
        (void)send_transfer(transfer.descriptor,endpoint(transfer.directory,pid),packet);
        transfer.duplicate_requests.erase({pid,sequence});
        if(const auto reply=transfer.replies.find({pid,sequence});reply!=transfer.replies.end()){
            if(reply->second.descriptor>=0)::close(reply->second.descriptor);transfer.replies.erase(reply);
        }
    };
    cancel(op.peer,op.packet.sequence);cancel(op.source,op.export_sequence);
    transfer.duplicates.erase(found);
#else
    (void)memory;(void)thread;
#endif
}
bool WindowsServices::finish_duplicate_remote_handle(GuestState& state,GuestMemory& memory,std::uint64_t thread){
#if !defined(_WIN32)
    if(!handle_transfers_)return true;auto& transfer=*handle_transfers_;
    const auto found=transfer.duplicates.find(thread);if(found==transfer.duplicates.end())return true;
    auto& op=*found->second;const auto self=process_id();
    const auto acknowledge=[&](std::uint32_t pid,std::uint64_t sequence,bool cancel){
        if(!sequence)return;TransferMessage packet;packet.kind=cancel?5U:4U;packet.sender=self;packet.sequence=sequence;
        (void)send_transfer(transfer.descriptor,endpoint(transfer.directory,pid),packet);
        transfer.duplicate_requests.erase({pid,sequence});
    };
    for(;;){
        if(op.error || op.result)break;
        if(!op.packet.sequence){
            op.packet={};op.packet.sender=self;op.packet.reserved=duplicate_protocol;
            op.packet.sequence=transfer.next_sequence++;op.sent=false;
            op.deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            if(op.exporting){
                op.peer=op.source;op.packet.kind=2;op.packet.handle=op.arguments[1];
                op.packet.access=static_cast<std::uint32_t>(op.arguments[4]);op.packet.options=static_cast<std::uint32_t>(op.arguments[6]);op.packet.flags=op.arguments[5]?1U:0U;
            }else{
                op.item.flags=op.arguments[5]?1U:0U;
                if(op.target==self){
                    op.item.handle=runtime_.allocate_wait_handle();
                    // import_transfer_handle 接收描述元的所有權；失敗也由該 provider 回收。
                    op.descriptor=-1;
                    try{import_transfer_handle(op.item);op.result=op.item.handle;}catch(const HostCallFault&){op.error=50;}
                    op.packet.sequence=0;break;
                }
                op.peer=op.target;op.packet.kind=1;op.packet.type=op.item.type;op.packet.access=op.item.access;
                op.packet.flags=op.item.flags;op.packet.mode=op.item.mode;op.packet.pipe_mode=op.item.pipe_mode;op.packet.auxiliary=op.item.auxiliary;
            }
            transfer.duplicate_requests.emplace(op.peer,op.packet.sequence);
        }
        if(!op.sent){
            const auto failure=send_transfer(transfer.descriptor,endpoint(transfer.directory,op.peer),op.packet,op.exporting?-1:op.item.descriptor);
            if(!failure)op.sent=true;
            else if(failure!=EAGAIN && failure!=ENOBUFS && failure!=ENOENT && failure!=ECONNREFUSED){op.error=50;break;}
        }
        const auto reply=transfer.replies.find({op.peer,op.packet.sequence});
        if(reply!=transfer.replies.end()){
            auto received=std::move(reply->second);transfer.replies.erase(reply);
            transfer.duplicate_requests.erase({op.peer,op.packet.sequence});op.error=received.message.error;
            if(op.exporting){
                op.export_sequence=op.packet.sequence;op.descriptor=received.descriptor;
                op.item.type=received.message.type;op.item.access=received.message.access;op.item.mode=received.message.mode;
                op.item.pipe_mode=received.message.pipe_mode;op.item.auxiliary=received.message.auxiliary;op.item.descriptor=received.descriptor;
                if(!op.error && op.item.descriptor<0)op.error=6;
                if(!op.error && op.item.type==3 && op.item.auxiliary && (op.item.access&~0x100001U))op.error=5;
                op.exporting=false;op.packet.sequence=0;continue;
            }
            if(received.descriptor>=0)::close(received.descriptor);
            op.result=received.message.handle;break;
        }
        if(std::chrono::steady_clock::now()>=op.deadline || (::kill(static_cast<pid_t>(op.peer),0)<0 && errno==ESRCH)){
            op.error=1460;break;
        }
        // 其他客體持續運算時，也須喚醒逾時或尚未送出的請求；不能只
        // 依外部回覆，否則死亡／滿佇列的對端會讓這個等待永久失效。
        const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(op.deadline-std::chrono::steady_clock::now()).count();
        const auto delay=op.sent?remaining:std::min<std::int64_t>(remaining,1000000);
        threads_.items.at(thread).wait.deadline=(runtime_.host_clock?detail::monotonic_ticks():runtime_.clock_ticks)+static_cast<std::uint64_t>(std::max<std::int64_t>(delay,100))/100;
        return false;
    }
    if(!op.error){
        try{memory.store(GuestAddress{op.arguments[3]},x86_?32:64,op.result);}
        catch(const MemoryFault&){op.error=998;if(op.target==self && op.result)close_transfer_handle(memory,op.result);}
    }
    acknowledge(op.peer,op.packet.sequence,op.error!=0);
    acknowledge(op.source,op.export_sequence,op.error!=0);
    if(op.error){
        memory.store(GuestAddress{threads_.items.at(thread).teb+(x86_?0x34:0x68)},32,op.error);
        if(thread==threads_.current)last_error_=op.error;
    }
    if(std::getenv("X64ARM_TRACE_HANDLE_TRANSFER"))std::fprintf(stderr,
        "[duplicate_remote_complete] pid=%u thread=%llu source_pid=%u target_pid=%u error=%u elapsed_us=%lld\n",self,
        static_cast<unsigned long long>(thread),op.source,op.target,op.error,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-op.started).count()));
    WindowsCall(state,memory,7).return_to(state,op.error?0:1);
    transfer.duplicates.erase(found);return true;
#else
    (void)state;(void)memory;(void)thread;return true;
#endif
}
std::optional<std::uint32_t> WindowsServices::duplicate_remote_handle(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);std::uint32_t error=0;
#if !defined(_WIN32)
    const auto source=process_handle_pid(arg(0),0x40),target=process_handle_pid(arg(2),0x40),self=process_id();
    if(!source || !target)error=process_object_type(arg(0))==u"Process" && process_object_type(arg(2))==u"Process"?5:6;
    else if(!arg(3) || arg(6)&~3U)error=87;
    else if(!handle_transfers_)error=50;
    else {
        memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);auto& transfer=*handle_transfers_;
        if(transfer.duplicates.size()>=1024 || transfer.next_sequence>=UINT64_MAX-1)error=8;
        else {
            auto op=std::make_shared<DuplicateRequest>();op->source=source;op->target=target;op->exporting=source!=self;
            for(unsigned i=0;i<op->arguments.size();++i)op->arguments[i]=arg(i);
            op->started=std::chrono::steady_clock::now();
            if(source==self){
                auto& item=op->item;error=export_transfer_handle(arg(1),item);
                if(!error && (arg(6)&1U) && (item.flags&2U))error=5;
                if(!error && !(arg(6)&2U)){
                    if(item.type==3 || item.type==5){try{initialize_tokens();const auto all=item.type==5?0x1f0001U:0x1f0003U;const security::AccessMapping mapping{0x20001U,item.type==5?0x20001U:0x20002U,0x120000U,all};item.access=security::duplicate_object_access(runtime_.token_objects.at(effective_token_id()),security::parse_relative_descriptor(shared_object_security(item.descriptor)),item.access,static_cast<std::uint32_t>(arg(4)),mapping);}catch(const SecurityDescriptorError& failure){error=failure.code;}}
                    else {const auto requested=transfer_access(item.type,static_cast<std::uint32_t>(arg(4)));if(requested&~transfer_access(item.type,item.access))error=5;else item.access=requested;}
                }
                if(!error && item.type==3 && item.auxiliary && (item.access&~0x100001U))error=5;
                if(!error){do{op->descriptor=::fcntl(item.descriptor,F_DUPFD_CLOEXEC,0);}while(op->descriptor<0 && errno==EINTR);
                    if(op->descriptor<0)error=6;else item.descriptor=op->descriptor;}
                // CLOSE_SOURCE 不依結果而保留原 handle；先取得作業私有描述元，再只關閉一次。
                if((arg(6)&1U) && !(runtime_.handle_flags[arg(1)]&2U))close_transfer_handle(memory,arg(1));
            }
            if(!error){
                transfer.duplicates.emplace(threads_.current,std::move(op));poll_handle_transfers(memory,true);
                auto& thread=threads_.items.at(threads_.current);
                thread.wait={};thread.wait.kind=ThreadWaitKind::HandleTransfer;
                if(finish_duplicate_remote_handle(state,memory,threads_.current)){thread.wait={};return std::nullopt;}
                thread.status=ThreadStatus::Waiting;
                schedule(state,memory);return std::nullopt;
            }
        }
    }
#else
    error=50;
#endif
    last_error_=error;arg.return_to(state,0);return std::nullopt;
}
}
