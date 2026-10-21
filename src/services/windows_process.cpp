#include "x64arm/windows_services.hpp"
#include "x64arm/windows_cpu_time.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_default_environment.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/process_debugger.hpp"
#include "x64arm/process_identity.hpp"
#include "x64arm/startup_profile.hpp"
#include "process_manifest.hpp"
#include "crt_lowio_abi.hpp"
#include "crt_os_error.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <thread>
#include <sstream>
#include <set>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <libproc.h>
#include <sys/proc_info.h>
#endif
namespace x64arm {
bool WindowsServices::executable_needs_current_directory(std::u16string_view image)const{
    return image.find(u'\\')!=image.npos || !runtime_.environment.contains(u"NODEFAULTCURRENTDIRECTORYINEXEPATH");
}
namespace {
struct Packet {
    std::uint64_t magic{0x4352504d52413458ULL};
    std::uint32_t kind{}, value{};
};
// 程序身分與完整 DWORD 結束狀態隨 handle 共用；PID 重用不能把舊 handle 指向新程序。
#if !defined(_WIN32)
struct ProcessRecord {
    struct Header {
        std::uint64_t magic{UINT64_C(0x3252504852413458)}, birth{};
        std::uint32_t pid{}, exited{}, code{}, idle{}, information_valid{}, bits{};
        std::array<std::uint64_t, 5> information{}, accounting{};
        std::uint32_t accounting_valid{};
        std::uint32_t thread_id{}, object_kind{}, public_thread_id{};
        std::uint64_t console_identity{};std::uint32_t console_group{};
    };
    int descriptor{-1};
    Header *data{};
    std::shared_ptr<process_identity::Lifetime> lifetime;
    std::string path;
    std::string identifier_path;
    bool owner{};
    ~ProcessRecord() {
        if (owner && !identifier_path.empty())
            (void)::unlink(identifier_path.c_str());
        if (owner && !path.empty())
            (void)::unlink(path.c_str());
        if (data)
            ::munmap(data, 4096);
        if (descriptor >= 0)
            ::close(descriptor);
    }
    static std::shared_ptr<ProcessRecord> inherit(int descriptor) {
        auto record = std::make_shared<ProcessRecord>();
        record->descriptor = descriptor;
        (void)::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
        struct stat info{};
        Header header;
        if (::fstat(descriptor, &info) || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
            (info.st_mode & 077) || info.st_size != 4096 ||
            ::pread(descriptor, &header, sizeof(header), 0) != sizeof(header) || header.magic != Header{}.magic ||
            !header.pid || !header.birth || header.object_kind>2 || (header.object_kind==2 && !header.public_thread_id) ||
            ((header.object_kind!=0)!=(header.thread_id!=0)))
            throw HostCallFault("程序 handle 身分紀錄無效");
        const auto mapped = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        if (mapped == MAP_FAILED)
            throw HostCallFault("無法映射程序 handle 身分紀錄");
        record->data = static_cast<Header *>(mapped);
        record->lifetime = process_identity::observe({record->data->pid, record->data->birth});
        return record;
    }
    static std::shared_ptr<ProcessRecord> open(const std::string &session, std::uint32_t pid, bool create,
                                              std::uint32_t thread_id=0,std::uint32_t public_thread_id=0) {
        const auto status = process_identity::query(pid);
        if (status.error || status.exited || !status.identity)
            return {};
        const auto birth = status.identity.birth;
        const auto path = session + (thread_id?"/thread-status-":"/process-status-") + std::to_string(pid) + "-" + std::to_string(birth)+
                          (thread_id?"-"+std::to_string(thread_id):std::string{});
        int descriptor = -1;
        bool created = false;
        if (create) {
            descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            created = descriptor >= 0;
            if (!created && errno != EEXIST)
                return {};
        }
        if (descriptor < 0)
            descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0)
            return {};
        if (created) {
            Header header;
            header.pid = pid;
            header.birth = birth;
            header.thread_id=thread_id;header.object_kind=thread_id?2U:0U;
            header.public_thread_id=thread_id?(public_thread_id?public_thread_id:thread_id==1?pid:thread_id):0U;
            if (::ftruncate(descriptor, 4096) || ::pwrite(descriptor, &header, sizeof(header), 0) != sizeof(header)) {
                ::close(descriptor);
                (void)::unlink(path.c_str());
                return {};
            }
        }
        auto record = inherit(descriptor);
        record->path = path;
        if (record->data->pid != pid || record->data->birth != birth || record->data->thread_id!=thread_id)
            throw HostCallFault("程序身分紀錄與宿主不符");
        return record;
    }
    std::uint32_t thread_identifier()const{return data->object_kind==2?data->public_thread_id:data->thread_id;}
    void publish_identifier(const std::string& session){
        if(!data->object_kind || thread_identifier()<0x80000000U)return;
        const auto index_path=session+"/thread-id-"+std::to_string(thread_identifier());
        if(::link(path.c_str(),index_path.c_str()) && errno!=EEXIST)throw HostCallFault("無法發布 Windows 執行緒識別碼");
        struct stat original{},index{};
        if(::fstat(descriptor,&original) || ::lstat(index_path.c_str(),&index) || original.st_dev!=index.st_dev || original.st_ino!=index.st_ino)
            throw HostCallFault("Windows 執行緒識別碼已屬於其他物件");
        identifier_path=index_path;
    }
    static std::shared_ptr<ProcessRecord> find_thread(const std::string& session,std::uint32_t identifier){
        if(!identifier)return {};
        if(identifier<0x80000000U)return open(session,identifier,false,1);
        const auto path=session+"/thread-id-"+std::to_string(identifier);
        const auto descriptor=::open(path.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);if(descriptor<0)return {};
        auto record=inherit(descriptor);
        if(!record->data->object_kind || record->thread_identifier()!=identifier || !process_identity::query(record->data->pid).live({record->data->pid,record->data->birth}))return {};
        return record;
    }
    static std::uint32_t allocate_thread_identifier(const std::string& session){
        // POSIX PID 為正的有號整數；次要執行緒使用 DWORD 高半部的共用單調序號，避免跨程序碰撞。
        struct Sequence {std::uint64_t magic{UINT64_C(0x3251495441523458)},next{0x80000000U};};
        const auto path=session+"/thread-id-sequence";
        struct Descriptor {int value;~Descriptor(){if(value>=0)::close(value);}} descriptor{::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600)};
        if(descriptor.value<0)throw HostCallFault("無法開啟 Windows 執行緒識別碼序號");
        SharedEventLock lock(descriptor.value);struct stat info{};
        if(::fstat(descriptor.value,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077))throw HostCallFault("Windows 執行緒識別碼序號歸屬無效");
        Sequence sequence;
        if(info.st_size && (info.st_size!=sizeof(sequence) || ::pread(descriptor.value,&sequence,sizeof(sequence),0)!=sizeof(sequence) || sequence.magic!=Sequence{}.magic || sequence.next<0x80000000U || sequence.next>UINT64_C(0x100000000)))throw HostCallFault("Windows 執行緒識別碼序號損毀");
        if(sequence.next>UINT32_MAX)throw HostCallFault("Windows 執行緒識別碼已耗盡");
        const auto identifier=static_cast<std::uint32_t>(sequence.next++);
        if(::pwrite(descriptor.value,&sequence,sizeof(sequence),0)!=sizeof(sequence))throw HostCallFault("無法保存 Windows 執行緒識別碼序號");
        return identifier;
    }
    bool exited() const { return std::atomic_ref(data->exited).load(std::memory_order_acquire) != 0; }
    bool alive() const { return !exited() && lifetime && lifetime->alive(); }
    void finish(std::uint32_t code) {
        if(data->object_kind && exited())return;
        data->code = code;
        std::atomic_ref(data->exited).store(1, std::memory_order_release);
    }
};
std::weak_ptr<ProcessRecord> local_process_record;
std::map<std::uint32_t,std::weak_ptr<ProcessRecord>> local_thread_records;
#endif
int result_descriptor = -1;
bool reported_idle = false;
bool trace_process_protocol() {
    static const bool enabled = std::getenv("X64ARM_TRACE_PROCESS_PROTOCOL") != nullptr;
    return enabled;
}
void send_packet(std::uint32_t kind, std::uint32_t value) {
#if !defined(_WIN32)
    if (result_descriptor < 0)
        return;
    Packet packet;
    packet.kind = kind;
    packet.value = value;
    const auto *bytes = reinterpret_cast<const char *>(&packet);
    std::size_t at = 0;
    while (at < sizeof(packet)) {
        const auto count = ::write(result_descriptor, bytes + at, sizeof(packet) - at);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (trace_process_protocol())
                std::fprintf(stderr, "process_protocol_send pid=%d fd=%d kind=%u value=%u written=%zu errno=%d\n",
                             ::getpid(), result_descriptor, kind, value, at, errno);
            break;
        }
        at += static_cast<std::size_t>(count);
    }
    if (trace_process_protocol() && at == sizeof(packet))
        std::fprintf(stderr, "process_protocol_send pid=%d fd=%d kind=%u value=%u written=%zu\n", ::getpid(),
                     result_descriptor, kind, value, at);
#else
    (void)kind;
    (void)value;
#endif
}
#if !defined(_WIN32)
std::filesystem::path executable_path(std::filesystem::path hint) {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)::_NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (!::_NSGetExecutablePath(path.data(), &size))
        return std::filesystem::canonical(path.data());
#elif defined(__linux__)
    std::array<char, 4096> path{};
    const auto count = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (count > 0)
        return std::filesystem::path(std::string(path.data(), static_cast<std::size_t>(count)));
#endif
    return std::filesystem::canonical(hint);
}
struct Child {
    pid_t pid{-1};
    int pipe{-1}, host_status{};
    std::uint32_t exit_code{259};
    bool gui{}, idle{}, exited{}, have_result{}, success{}, terminated{}, job_started{}, startup_ready{}, independent{};
    std::uint32_t job_error{}, startup_error{};
    bool host_clock{};
    std::array<std::uint64_t, 3> baseline{};
    std::uint32_t baseline_parts{}, accounting_parts{};
    std::array<std::uint64_t, 5> accounting{};
    std::vector<std::shared_ptr<void>> inherited;
    bool diagnostics_saved{},failure_reported{};
    std::array<std::uint8_t, sizeof(Packet)> partial{};
    std::size_t received{};
    std::filesystem::path diagnostics;
    ~Child() {
        if (pid > 0 && !exited && !independent) {
            (void)::kill(pid, SIGKILL);
            int status{};
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
        if (pipe >= 0)
            ::close(pipe);
        std::error_code ignored;
        std::filesystem::remove(diagnostics, ignored);
    }
    void poll() {
        if (pipe >= 0)
            for (;;) {
                const auto count = ::read(pipe, partial.data() + received, partial.size() - received);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                if (count <= 0) {
                    ::close(pipe);
                    pipe = -1;
                    break;
                }
                received += static_cast<std::size_t>(count);
                if (received == partial.size()) {
                    Packet packet;
                    std::memcpy(&packet, partial.data(), sizeof(packet));
                    received = 0;
                    if (trace_process_protocol())
                        std::fprintf(stderr, "process_protocol_receive parent=%d child=%d kind=%u value=%u\n",
                                     ::getpid(), pid, packet.kind, packet.value);
                    if (packet.magic != Packet{}.magic)
                        throw HostCallFault("子程序回傳協議無效");
                    if (packet.kind >= 6 && packet.kind <= 11) {
                        const auto part = packet.kind - 6;
                        auto &value = baseline[part / 2];
                        value = (value & ~(UINT64_C(0xffffffff) << (32 * (part % 2)))) |
                                (static_cast<std::uint64_t>(packet.value) << (32 * (part % 2)));
                        baseline_parts |= 1U << part;
                    } else if (packet.kind >= 12 && packet.kind <= 21) {
                        const auto part = packet.kind - 12;
                        auto &value = accounting[part / 2];
                        value = (value & ~(UINT64_C(0xffffffff) << (32 * (part % 2)))) |
                                (static_cast<std::uint64_t>(packet.value) << (32 * (part % 2)));
                        accounting_parts |= 1U << part;
                    } else if (packet.kind == 5) {
                        job_started = true;
                        job_error = packet.value;
                    } else if (packet.kind == 22) {
                        if (packet.value)
                            startup_error = packet.value;
                        else
                            startup_ready = true;
                    } else if (packet.kind == 4)
                        inherited.clear();
                    else if (packet.kind == 1)
                        idle = true;
                    else if (packet.kind == 2 || packet.kind == 3) {
                        have_result = true;
                        success = packet.kind == 2;
                        exit_code = packet.value;
                    } else
                        throw HostCallFault("子程序回傳種類無效");
                }
            }
        if (!exited) {
            int status{};
            rusage usage{};
            const auto done = ::wait4(pid, &status, WNOHANG, &usage);
            if (done == pid) {
                host_status = status;
                exited = true;
                poll();
                if (host_clock && accounting_parts != 0x3ffU && baseline_parts == 0x3fU) {
                    const auto ticks = [](const timeval &value) {
                        return static_cast<std::uint64_t>(value.tv_sec) * 10000000 +
                               static_cast<std::uint64_t>(value.tv_usec) * 10;
                    };
                    const auto user = ticks(usage.ru_utime), kernel = ticks(usage.ru_stime);
                    accounting[0] = baseline[2];
                    accounting[1] = UINT64_C(116444736000000000) +
                                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                   std::chrono::system_clock::now().time_since_epoch())
                                                                   .count() /
                                                               100);
                    accounting[2] = kernel >= baseline[1] ? kernel - baseline[1] : 0;
                    accounting[3] = user >= baseline[0] ? user - baseline[0] : 0;
                    accounting_parts = 0xffU;
                }
            } else if (done < 0 && errno != EINTR)
                throw HostCallFault("無法查詢客體子程序狀態");
        }
        if (exited && !diagnostics_saved) {
            if (const auto *directory = std::getenv("X64ARM_PROCESS_DIAGNOSTICS_DIR"); directory && *directory) {
                std::error_code ignored;
                std::filesystem::create_directories(directory, ignored);
                diagnostics_saved = std::filesystem::copy_file(
                    diagnostics, std::filesystem::path(directory) / ("child-" + std::to_string(pid) + ".txt"),
                    std::filesystem::copy_options::overwrite_existing, ignored);
            }
        }
        if (exited && !terminated && (!have_result || !success) && !failure_reported) {
            if (trace_process_protocol())
                std::fprintf(stderr,
                             "process_protocol_failure parent=%d child=%d host_status=%d have_result=%d success=%d "
                             "partial=%zu\n",
                             ::getpid(), pid, host_status, have_result, success, received);
            std::ifstream input(diagnostics, std::ios::binary);
            std::string details((std::istreambuf_iterator<char>(input)), {});
            // 停止原因位於模組清單之前；保留真正的 fault，避免大量可選匯入淹沒根因。
            if (const auto position = details.find("reason="); position != std::string::npos)
                details = details.substr(position, 4096);
            else if (details.size() > 4096)
                details.resize(4096);
            // Windows 的 process handle 只公開該子程序的結束狀態；不能把其 fault
            // 當成父程序 Wait／GetExitCode 的 HostCallFault。原始診斷仍完整保存。
            if(!have_result || !exit_code)exit_code=0xc0000001U;
            failure_reported=true;
            std::fprintf(stderr,"process_child_failure parent=%d child=%d exit_code=%u; %s\n",::getpid(),pid,exit_code,details.c_str());
        }
    }
};
struct DebugHandleLife {
    bool exited{};
    std::uint32_t code{259};
};
struct Handle {
    std::shared_ptr<Child> child;
    bool thread{};
    std::uint32_t access{0x1fffff}, remote_pid{};
    std::shared_ptr<ProcessRecord> record{};
    std::uint64_t identity{};
    bool exited{};
    std::uint32_t remote_tid{};
    std::shared_ptr<DebugHandleLife> debug_life{};
    std::shared_ptr<ProcessRecord> parent_record{};
};
void refresh_thread_exit(Handle& object){
    if(object.thread && object.record && object.record->data->object_kind && !object.record->exited() &&
       object.parent_record && object.parent_record->exited())object.record->finish(object.parent_record->data->code);
}
void resolve_thread_record(Handle& object,const std::string& session){
    if(!object.thread)return;
    if(!object.record || !object.record->data->object_kind){
        const auto pid=object.child?static_cast<std::uint32_t>(object.child->pid):object.remote_pid;
        if(auto record=ProcessRecord::open(session,pid,false,object.remote_tid?object.remote_tid:1)){
            object.parent_record=object.record?object.record:ProcessRecord::open(session,pid,false);
            object.record=std::move(record);object.remote_tid=object.record->data->thread_id;
        }
    }
    refresh_thread_exit(object);
}
#endif
} // namespace
void set_child_result_descriptor(int descriptor) {
    if (descriptor < 0)
        throw std::invalid_argument("子程序結果描述符無效");
#if !defined(_WIN32)
    // 父程序可以先退出；內部結果管道失去讀端時不可把合法子程序終止。
    std::signal(SIGPIPE, SIG_IGN);
#endif
    result_descriptor = descriptor;
    reported_idle = false;
}
void report_child_job_status(std::uint32_t error) { send_packet(5, error); }
void report_child_startup_ready() { send_packet(22, 0); }
void report_child_startup_failure(std::uint32_t error) { send_packet(22, error); }
void report_child_input_idle() {
    if (!reported_idle) {
        reported_idle = true;
#if !defined(_WIN32)
        if (const auto record = local_process_record.lock())
            std::atomic_ref(record->data->idle).store(1, std::memory_order_release);
#endif
        send_packet(1, 0);
    }
}
void report_child_result(std::uint32_t code, bool success) {
    if(!success && !code)code=0xc0000001U;
#if !defined(_WIN32)
    if (const auto record = local_process_record.lock())
        record->finish(code);
    for(auto it=local_thread_records.begin();it!=local_thread_records.end();){
        if(const auto record=it->second.lock()){record->finish(code);++it;}else it=local_thread_records.erase(it);
    }
#endif
    send_packet(success ? 2 : 3, code);
}
std::uint32_t host_process_id() noexcept {
#if !defined(_WIN32)
    return static_cast<std::uint32_t>(::getpid());
#else
    return 1;
#endif
}
struct ProcessState {
    ProcessOptions options;
    std::uint32_t identity{1}, parent{};
    struct CrtWait {std::uint64_t handle{},status{};bool cwait{};};
    std::map<std::uint64_t,CrtWait> crt_waits;
    struct PssSnapshot {std::vector<std::uint8_t> process;std::array<std::uint64_t,12> performance{};std::uint64_t storage{};std::uint32_t flags{};bool vm{};};
    std::map<std::uint64_t,PssSnapshot> pss_snapshots;
#if !defined(_WIN32)
    std::map<std::uint64_t, Handle> handles;
    std::shared_ptr<ProcessRecord> record;
    std::map<std::uint32_t,std::shared_ptr<ProcessRecord>> thread_records;
    std::vector<std::shared_ptr<Child>> children;
    ~ProcessState() {
        // Windows 的程序／執行緒 handles 與建立者生命週期不擁有子程序。
        // 長期存活的宿主需要回收 waitpid；CLI 宿主退出後由作業系統接管。
        std::erase_if(children, [](const auto &child) { return child->exited; });
        if (children.empty())
            return;
        try {
            std::thread([remaining = std::move(children)]() mutable {
                std::set<pid_t> reported;
                while (!remaining.empty()) {
                    for (auto &child : remaining) {
                        try {
                            child->poll();
                        } catch (const std::exception &error) {
                            if (reported.insert(child->pid).second)
                                std::fprintf(stderr, "子程序回收診斷：%s\n", error.what());
                        }
                    }
                    std::erase_if(remaining, [](const auto &child) { return child->exited; });
                    if (!remaining.empty())
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }).detach();
        } catch (const std::system_error &) {
            // 無法建立回收執行緒時，仍保留已成功建立的獨立程序。
        }
    }
#endif
};
std::uint32_t WindowsServices::thread_access(std::uint64_t handle)const{
    if(thread_id(handle)){
        if(handle==(x86_?UINT32_MAX-1:UINT64_MAX-1))return kernel_pseudo_access(true);
        const auto found=runtime_.thread_handle_access.find(handle);return found==runtime_.thread_handle_access.end()?0x1fffffU:found->second;
    }
#if !defined(_WIN32)
    if(processes_)if(const auto found=processes_->handles.find(handle);found!=processes_->handles.end() && found->second.thread)return found->second.access;
#endif
    return 0;
}
void WindowsServices::configure_process_identity() {
#if !defined(_WIN32)
    if (!processes_ || processes_->record)
        return;
    processes_->record = ProcessRecord::open(mapping_session(), process_id(), true);
    if (!processes_->record)
        throw HostCallFault("無法建立共用程序身分紀錄");
    initialize_tokens();
    {
        SharedEventLock lock(processes_->record->descriptor);
        if (shared_object_security(processes_->record->descriptor).empty())
            set_shared_object_security(processes_->record->descriptor, runtime_.process_security_descriptor);
        if (shared_object_security(processes_->record->descriptor, true).empty())
            set_shared_object_security(processes_->record->descriptor, runtime_.initial_thread_security_descriptor,
                                       true);
    }
    processes_->record->owner = true;
    local_process_record = processes_->record;
    publish_process_information();
    publish_console_identity(console_identity_!=0);
    configure_thread_record(1);
#endif
}
void WindowsServices::configure_thread_record(std::uint64_t id){
#if !defined(_WIN32)
    if(!processes_ || !processes_->record || !threads_.items.contains(id) || processes_->thread_records.contains(static_cast<std::uint32_t>(id)))return;
    if(id>UINT32_MAX)throw HostCallFault("執行緒身分超出 Windows DWORD 範圍");
    auto& thread=threads_.items.at(id);auto security=thread.security_descriptor;
    if(id==1){SharedEventLock lock(processes_->record->descriptor);security=shared_object_security(processes_->record->descriptor,true);}
    if(security.empty())security=synchronization_descriptor(GuestMemory{},0).bytes;
    auto record=ProcessRecord::open(mapping_session(),process_id(),true,static_cast<std::uint32_t>(id),thread_identifier(id));
    if(!record)throw HostCallFault("無法建立共用執行緒身分紀錄");
    {SharedEventLock lock(record->descriptor);if(shared_object_security(record->descriptor,true).empty())set_shared_object_security(record->descriptor,security,true);}
    record->owner=true;record->publish_identifier(mapping_session());if(thread.status==ThreadStatus::Exited)record->finish(thread.exit_code);
    processes_->thread_records.emplace(static_cast<std::uint32_t>(id),record);local_thread_records[static_cast<std::uint32_t>(id)]=record;
#else
    (void)id;
#endif
}
std::uint32_t WindowsServices::allocate_thread_identifier(std::uint64_t id){
#if !defined(_WIN32)
    if(processes_ && processes_->record)return ProcessRecord::allocate_thread_identifier(mapping_session());
#endif
    return static_cast<std::uint32_t>(id);
}
std::uint32_t WindowsServices::remote_thread_identifier(std::uint32_t pid,std::uint64_t birth,std::uint32_t id){
#if !defined(_WIN32)
    if(pid==process_id())return thread_identifier(id);
    if(processes_)for(const auto& [handle,object]:processes_->handles){(void)handle;if(object.thread && object.record && object.record->data->object_kind && object.record->data->pid==pid && object.record->data->birth==birth && object.record->data->thread_id==id)return object.record->thread_identifier();}
    if(const auto record=ProcessRecord::open(mapping_session(),pid,false,id);record && record->data->birth==birth)return record->thread_identifier();
#endif
    return id==1?pid:id;
}
void WindowsServices::publish_thread_lifetime(std::uint64_t id,std::uint32_t code){
#if !defined(_WIN32)
    if(processes_)if(const auto found=processes_->thread_records.find(static_cast<std::uint32_t>(id));found!=processes_->thread_records.end())found->second->finish(code);
#else
    (void)id;(void)code;
#endif
}
std::uint32_t WindowsServices::process_security_location(std::uint64_t handle, std::uint32_t needed,
                                                         std::vector<std::uint8_t> *&local, int &native, int &lock,
                                                         bool &thread) {
    initialize_tokens();
    const auto id = thread_id(handle);
    if (id) {
        thread = true;
        if ((thread_access(handle) & needed) != needed)
            return 5;
        auto &object = threads_.items.at(id);
        if (object.security_descriptor.empty())
            object.security_descriptor = id == 1 ? runtime_.initial_thread_security_descriptor
                                                 : synchronization_descriptor(GuestMemory{}, 0).bytes;
#if !defined(_WIN32)
        if (processes_ && processes_->record) {
            configure_thread_record(id);
            native = lock = processes_->thread_records.at(static_cast<std::uint32_t>(id))->descriptor;
            return 0;
        }
#endif
        local = &object.security_descriptor;
        return 0;
    }
#if !defined(_WIN32)
    const bool pseudo = handle == (x86_ ? UINT32_MAX : UINT64_MAX);
    const auto found = processes_ ? processes_->handles.find(handle) : std::map<std::uint64_t, Handle>::iterator{};
    if (!pseudo && (!processes_ || found == processes_->handles.end()))
        return 6;
    thread = !pseudo && found->second.thread;
    if(thread)resolve_thread_record(found->second,mapping_session());
    if (!pseudo && (found->second.access & needed) != needed)
        return 5;
    auto record = pseudo ? processes_ ? processes_->record : nullptr : found->second.record;
    if (record) {
        native = lock = record->descriptor;
        if (pseudo && (kernel_pseudo_access(false) & needed) != needed)
            return 5;
        return 0;
    }
    if (!pseudo && (found->second.child || found->second.remote_pid))
        return 1338;
    if (pseudo && (kernel_pseudo_access(false) & needed) != needed)
        return 5;
    local = thread ? &runtime_.initial_thread_security_descriptor : &runtime_.process_security_descriptor;
    return 0;
#else
    if (handle != (x86_ ? UINT32_MAX : UINT64_MAX))
        return 6;
    if ((kernel_pseudo_access(false) & needed) != needed)
        return 5;
    local = &runtime_.process_security_descriptor;
    return 0;
#endif
}
bool WindowsServices::export_process_handle(std::uint64_t handle, InheritedHandle &item) {
#if !defined(_WIN32)
    if (!processes_)
        return false;
    if(const auto id=thread_id(handle)){
        configure_process_identity();configure_thread_record(id);
        const auto found=processes_->thread_records.find(static_cast<std::uint32_t>(id));if(found==processes_->thread_records.end())return false;
        item.type=7;item.mode=2;item.access=thread_access(handle);item.auxiliary=process_id();item.descriptor=found->second->descriptor;item.lifetime=found->second;return true;
    }
    const bool pseudo = handle == (x86_ ? UINT32_MAX : UINT64_MAX);
    const auto found = processes_->handles.find(handle);
    if (!pseudo && found == processes_->handles.end())
        return false;
    const auto source = pseudo ? Handle{{}, false, kernel_pseudo_access(false), 0, processes_->record} : found->second;
    auto record = source.record;
    if(source.thread && (!record || !record->data->object_kind)){
        const auto pid=source.child?static_cast<std::uint32_t>(source.child->pid):source.remote_pid;
        if(auto actual=ProcessRecord::open(mapping_session(),pid,false,source.remote_tid?source.remote_tid:1))record=std::move(actual);
    }
    if (!record) {
        const auto pid = source.child ? static_cast<std::uint32_t>(source.child->pid) : source.remote_pid;
        record = pid ? ProcessRecord::open(mapping_session(), pid, false) : processes_->record;
    }
    if (!record)
        return false;
    item.type = 7;
    item.mode = source.thread ? record->data->object_kind?2U:1U : 0U;
    item.access = source.access;
    item.auxiliary = record->data->pid;
    item.descriptor = record->descriptor;
    item.lifetime = std::move(record);
    return true;
#else
    (void)handle;
    (void)item;
    return false;
#endif
}
void WindowsServices::import_process_handle(const InheritedHandle &item) {
#if !defined(_WIN32)
    auto record = ProcessRecord::inherit(item.descriptor);
    if (!processes_ || record->data->pid != item.auxiliary || item.mode > 2 || item.access & ~0x1fffffU ||
        ((item.mode==2)!=(record->data->object_kind!=0)))
        throw HostCallFault("繼承程序／主執行緒 handle 內容無效");
    const auto pid = record->data->pid;
    if(item.mode==2 && pid==process_id() && processes_->record && processes_->record->data->birth==record->data->birth &&
       threads_.items.contains(record->data->thread_id) && thread_identifier(record->data->thread_id)==record->thread_identifier()){
        runtime_.process_thread_handles[item.handle]={record->data->thread_id,false};runtime_.thread_handle_access[item.handle]=item.access;return;
    }
    const auto tid=record->data->thread_id;const auto birth=record->data->birth;
    processes_->handles.emplace(
        item.handle, Handle{{}, item.mode != 0, item.access, item.mode==2?pid:pid == process_id() ? 0U : pid, std::move(record)});
    auto& imported=processes_->handles.at(item.handle);imported.remote_tid=tid;imported.identity=birth;
    if(item.mode==2)imported.parent_record=ProcessRecord::open(mapping_session(),pid,false);
#else
    (void)item;
    throw HostCallFault("此平台不支援程序 handle 傳輸");
#endif
}
void WindowsServices::configure_processes(ProcessOptions options, GuestMemory &memory) {
    if (processes_)
        throw std::invalid_argument("子程序執行環境已設定");
#if !defined(_WIN32)
    options.engine = executable_path(std::move(options.engine));
#endif
    for (unsigned i = 0; i < 3; ++i) {
        const auto value = i == 0   ? runtime_.process_start_user
                           : i == 1 ? runtime_.process_start_kernel
                                    : runtime_.process_creation_time;
        send_packet(6 + 2 * i, static_cast<std::uint32_t>(value));
        send_packet(7 + 2 * i, static_cast<std::uint32_t>(value >> 32));
    }
    runtime_.startup_show = options.startup_show;
    runtime_.startup_show_used = false;
    runtime_.startup_handles = options.startup_handles;
    detached_console_=options.startup_detached;
    if(detached_console_ && !options.startup_handles){runtime_.standard_handles={0,0,0};standard_open_={false,false,false};}
    runtime_.startup_reserved2=options.startup_data.reserved2;
    runtime_.startup_reserved2_pointer_bytes=options.startup_data.pointer_bytes;
    runtime_.startup_reserved2_address=0;
    if(runtime_.startup_reserved2.size()>65535 || (!runtime_.startup_reserved2.empty() && runtime_.startup_reserved2_pointer_bytes!=4 && runtime_.startup_reserved2_pointer_bytes!=8))throw std::invalid_argument("程序啟動保留資料的大小或來源 ABI 無效");
    if(!runtime_.startup_reserved2.empty())runtime_.startup_reserved2_address=runtime_allocate(memory,runtime_.startup_reserved2);
    if (options.startup_handles)
        runtime_.standard_handles = *options.startup_handles;
    if(detached_console_)for(unsigned i=0;i<3;++i)if(!runtime_.standard_handles[i] || runtime_.standard_handles[i]==(x86_?UINT64_C(0xffffffff):UINT64_MAX))standard_open_[i]=false;
    if (options.startup_suspended) {
        threads_.items.at(1).suspended = 1;
        threads_.yield = true;
    }
    processes_ = std::make_shared<ProcessState>();
    processes_->options = std::move(options);
    processes_->identity = host_process_id();
    threads_.items.at(1).public_id=process_id();
    const auto pid_offset = x86_ ? 0x20U : 0x40U;
    const std::uint8_t pid_bits = x86_ ? 32 : 64;
    if (memory.mapped(GuestAddress{teb_base_.value + pid_offset}))
        memory.store(GuestAddress{teb_base_.value + pid_offset}, pid_bits, process_id());
    if(memory.mapped(GuestAddress{teb_base_.value+pid_offset+(x86_?4U:8U)}))
        memory.store(GuestAddress{teb_base_.value+pid_offset+(x86_?4U:8U)},pid_bits,thread_identifier(1));
    for (const auto &[id, thread] : threads_.items) {
        (void)id;
        if (thread.teb && thread.teb != teb_base_.value)
            memory.store(GuestAddress{thread.teb + pid_offset}, pid_bits, process_id());
    }
#if !defined(_WIN32)
    processes_->parent = static_cast<std::uint32_t>(::getppid());
    console_identity_=processes_->options.console_identity;console_group_=processes_->options.console_group==UINT32_MAX?process_id():processes_->options.console_group;
    runtime_.ignore_ctrl_c=processes_->options.ignore_ctrl_c;
    publish_console_identity(console_identity_!=0);
#endif
}
void WindowsServices::publish_console_identity(bool attached,bool fresh){
    if(!attached)console_identity_=0;
    else {
        if(fresh || !console_identity_)console_identity_=process_identity_birth();
        if(!console_identity_)console_identity_=process_id();
        if(!console_group_)console_group_=process_id();
    }
#if !defined(_WIN32)
    if(processes_ && processes_->record){
        SharedEventLock lock(processes_->record->descriptor);
        processes_->record->data->console_identity=console_identity_;
        processes_->record->data->console_group=console_group_;
    }
#endif
}
std::uint32_t WindowsServices::generate_console_control(GuestMemory& memory,std::uint32_t event,std::uint32_t group){
    if(event>1)return 87;
    if(!console_identity_)return 6;
    // CTRL_C 的非零群組契約是成功但不傳送，不改成 CTRL_BREAK 的群組選擇。
    if(!event && group)return 0;
    std::vector<std::uint32_t> ids;if(const auto error=session_process_ids(ids))return error;
    bool matched=false;
    for(const auto pid:ids){
        if(pid==process_id()){
            if(group && group!=console_group_)continue;
            matched=true;if(const auto error=queue_console_control(memory,event))return error;continue;
        }
#if !defined(_WIN32)
        const auto record=ProcessRecord::open(mapping_session(),pid,false);if(!record || !record->alive())continue;
        {SharedEventLock lock(record->descriptor);if(record->data->console_identity!=console_identity_ || (group && record->data->console_group!=group))continue;}
        matched=true;std::array<std::uint64_t,5> values{console_identity_,group};std::uint32_t mode{};
        const auto error=remote_process_request(memory,pid,record->data->birth,20,event,values,mode);
        if(error && record->alive())return error;
#endif
    }
    return group && !matched?87U:0U;
}
std::uint32_t WindowsServices::receive_console_control(GuestMemory& memory,std::uint32_t sender,std::uint64_t birth,std::uint64_t identity,std::uint32_t group,std::uint32_t event){
    if(event>1 || !birth || birth!=process_identity_birth() || !identity || identity!=console_identity_ || (group && group!=console_group_))return 87;
#if !defined(_WIN32)
    const auto record=ProcessRecord::open(mapping_session(),sender,false);if(!record || !record->alive())return 87;
    {SharedEventLock lock(record->descriptor);if(record->data->console_identity!=identity)return 5;}
#else
    (void)sender;return 50;
#endif
    return queue_console_control(memory,event);
}
std::uint32_t WindowsServices::process_parent_id() const { return processes_ ? processes_->parent : 0; }
std::uint32_t WindowsServices::capture_process_snapshot(GuestMemory &memory,
                                                        std::vector<ProcessSnapshot::Process> &entries) {
    std::vector<std::uint32_t> ids;
    const auto status = session_process_ids(ids);
    if (status)
        return status;
    std::vector<ProcessSnapshot::Process> captured;
    for (const auto pid : ids) {
        ProcessSnapshot::Process entry;
        entry.id = pid;
        if (pid == process_id()) {
            std::array<std::uint64_t, 5> values{};
            std::uint32_t bits{};
            const auto error = local_process_information(0, values, bits, -1);
            if (error)
                return error;
            entry.parent = process_parent_id();
            entry.priority = static_cast<std::uint32_t>(values[3]);
            entry.threads = active_thread_count();
            entry.executable = runtime_.image.path;
            if (entry.executable.empty()) {
                const auto argv = memory.load(GuestAddress{runtime_.wargv_cell}, x86_ ? 32 : 64);
                const auto first = memory.load(GuestAddress{argv}, x86_ ? 32 : 64);
                if (first)
                    entry.executable = read_utf16(memory, GuestAddress{first});
            }
            const auto slash = entry.executable.find_last_of(u"\\/");
            if (slash != entry.executable.npos)
                entry.executable.erase(0, slash + 1);
        } else {
#if !defined(_WIN32)
            const auto record = ProcessRecord::open(mapping_session(), pid, false);
            if (!record || !record->alive())
                continue;
            auto pattern = (std::filesystem::temp_directory_path() / "x64arm-snapshot-XXXXXX").string();
            std::vector<char> name(pattern.begin(), pattern.end());
            name.push_back(0);
            const auto fd = ::mkstemp(name.data());
            if (fd < 0)
                return 8;
            struct Close {
                int fd;
                ~Close() { ::close(fd); }
            } cleanup{fd};
            (void)::unlink(name.data());
            (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
            if (::fchmod(fd, 0600) || ::ftruncate(fd, 65534))
                return 8;
            std::array<std::uint64_t, 5> values{};
            std::uint32_t bits{};
            const auto error = remote_process_request(memory, pid, record->data->birth, 14, 8, values, bits, fd);
            if (error) {
                if (!record->alive() || !remote_process_available(pid))
                    continue;
                return error;
            }
            if (values[0] < 2 || values[0] > 520 || values[0] % 2 || values[1] > UINT32_MAX || values[2] > UINT32_MAX ||
                values[3] > 31)
                return 13;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(values[0]));
            std::size_t done = 0;
            while (done < bytes.size()) {
                const auto count = ::pread(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(done));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return 13;
                done += static_cast<std::size_t>(count);
            }
            if (bytes[bytes.size() - 2] || bytes.back())
                return 13;
            for (std::size_t i = 0; i + 2 < bytes.size(); i += 2)
                entry.executable += static_cast<char16_t>(bytes[i] | (static_cast<unsigned>(bytes[i + 1]) << 8));
            entry.parent = static_cast<std::uint32_t>(values[1]);
            entry.threads = static_cast<std::uint32_t>(values[2]);
            entry.priority = static_cast<std::uint32_t>(values[3]);
#else
            return 50;
#endif
        }
        captured.push_back(std::move(entry));
    }
    entries = std::move(captured);
    return 0;
}
std::uint32_t WindowsServices::local_process_accounting(std::array<std::uint64_t, 5> &values) const {
    const auto cpu = runtime_.host_clock ? host_process_cpu_time()
                                         : std::optional<ProcessCpuTime>{ProcessCpuTime{runtime_.clock_ticks, 0}};
    if (!cpu)
        return 50;
    const auto kernel_start = runtime_.host_clock ? runtime_.process_start_kernel : 0,
               user_start = runtime_.host_clock ? runtime_.process_start_user : 0;
    std::uint64_t cycles = 0;
    for (const auto &[id, thread] : threads_.items) {
        (void)id;
        cycles += thread.cycles;
    }
    values = {runtime_.process_creation_time, 0, cpu->kernel >= kernel_start ? cpu->kernel - kernel_start : 0,
              cpu->user >= user_start ? cpu->user - user_start : 0, cycles};
    return 0;
}
void WindowsServices::report_process_accounting() {
    publish_process_information();
    std::array<std::uint64_t, 5> values{};
    if (local_process_accounting(values))
        return;
    values[1] = runtime_.system_filetime();
#if !defined(_WIN32)
    if (processes_ && processes_->record) {
        auto &record = *processes_->record;
        SharedEventLock lock(record.descriptor);
        record.data->accounting = values;
        std::atomic_ref(record.data->accounting_valid).store(1, std::memory_order_release);
    }
#endif
    for (unsigned i = 0; i < values.size(); ++i) {
        send_packet(12 + 2 * i, static_cast<std::uint32_t>(values[i]));
        send_packet(13 + 2 * i, static_cast<std::uint32_t>(values[i] >> 32));
    }
}
std::uint32_t WindowsServices::query_process_accounting(GuestMemory &memory, std::uint64_t handle,
                                                        std::array<std::uint64_t, 5> &values, bool cycles) {
    if (current_process_handle(handle))
        return local_process_accounting(values);
#if !defined(_WIN32)
    if (!processes_)
        return 6;
    const auto found = processes_->handles.find(handle);
    if (found == processes_->handles.end() || found->second.thread)
        return 6;
    const auto child = found->second.child;
    const auto record = found->second.record;
    const auto required = cycles ? 0x3ffU : 0xffU;
    // 客體已完成，但宿主仍在寫診斷／釋放資源時，IPC 可能已關閉；以已發布的完整終值查詢。
    const auto final_snapshot = [&] {
        if (record && std::atomic_ref(record->data->accounting_valid).load(std::memory_order_acquire)) {
            SharedEventLock lock(record->descriptor);
            values = record->data->accounting;
            return true;
        }
        if (child && (child->exited || (child->have_result && child->success)) &&
            (child->accounting_parts & required) == required) {
            values = child->accounting;
            return true;
        }
        return false;
    };
    if (child)
        child->poll();
    if (final_snapshot())
        return 0;
    if (child && child->exited)
        return 50;
    const auto pid = child ? static_cast<std::uint32_t>(child->pid) : found->second.remote_pid;
    const auto error = remote_process_accounting(memory, pid, values);
    if (child)
        child->poll();
    if (final_snapshot())
        return 0;
    return error;
#else
    (void)memory;
    (void)values;
    (void)cycles;
    return 50;
#endif
}
void WindowsServices::publish_process_information() {
#if !defined(_WIN32)
    if (!processes_ || !processes_->record)
        return;
    std::array<std::uint64_t, 5> values{};
    std::uint32_t bits{};
    if (local_process_information(0, values, bits, -1))
        return;
    auto &record = *processes_->record;
    SharedEventLock lock(record.descriptor);
    record.data->information = values;
    record.data->bits = bits;
    std::atomic_ref(record.data->information_valid).store(1, std::memory_order_release);
#endif
}
std::uint32_t WindowsServices::collect_pss_process_data(GuestMemory& memory,std::vector<std::uint8_t>& bytes){
    std::array<std::uint64_t,5> basic{},times{};std::uint32_t bits{};
    if(const auto error=local_process_information(0,basic,bits,-1))return error;
    if(const auto error=local_process_accounting(times))return error;
    const auto usage=memory.usage();std::array<std::uint64_t,26> values{};
    values[0]=basic[0];values[1]=basic[1];values[2]=basic[2];values[3]=basic[3];values[4]=process_id();values[5]=basic[4];values[6]=x86_?2U:0U;
    for(unsigned i=0;i<4;++i)values[7+i]=times[i];values[11]=runtime_.process_priority_class;
    values[12]=usage.peak_virtual_size;values[13]=usage.virtual_size;values[15]=usage.peak_committed;values[16]=usage.committed;
    // 客體沒有 Windows kernel pool 或分頁檔 fault；沿用 GetProcessMemoryInfo 的同一份 backing 記帳。
    values[21]=usage.private_commit;values[22]=usage.peak_private_commit;values[23]=usage.private_commit;values[25]=bits;
    bytes.resize(values.size()*8);
    for(unsigned i=0;i<values.size();++i)for(unsigned b=0;b<8;++b)bytes[i*8+b]=static_cast<std::uint8_t>(values[i]>>(b*8));
    const auto path=utf16_bytes(runtime_.image.path.substr(0,259));bytes.insert(bytes.end(),path.begin(),path.end());return 0;
}
std::uint32_t WindowsServices::process_memory_target(GuestMemory &memory, std::uint64_t handle, std::uint32_t needed,
                                                     std::uint32_t &pid, std::uint64_t &birth) {
    if (current_process_handle(handle, needed)) {
        pid = process_id();
        birth = process_identity_birth();
        return 0;
    }
    if (current_process_handle(handle))
        return 5;
#if !defined(_WIN32)
    if (!processes_)
        return 6;
    const auto found = processes_->handles.find(handle);
    if (found == processes_->handles.end() || found->second.thread)
        return 6;
    const auto target = found->second;
    if ((target.access & needed) != needed)
        return 5;
    if (!runtime_.network.external_io)
        throw HostCallFault("比較模式不允許操作跨程序位址空間");
    if (target.exited || (target.debug_life && target.debug_life->exited))
        return 87;
    if (target.child) {
        target.child->poll();
        if (target.child->exited)
            return 87;
    }
    if (!target.record || !target.record->alive())
        return 87;
    pid = target.record->data->pid;
    birth = target.record->data->birth;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!remote_process_available(pid)) {
        if (!target.record->alive())
            return 87;
        if (std::chrono::steady_clock::now() >= deadline)
            return 121;
        poll_handle_transfers(memory, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
#else
    (void)memory;
    (void)handle;
    (void)needed;
    (void)pid;
    (void)birth;
    return 50;
#endif
}
std::uint32_t WindowsServices::process_basic_information(GuestMemory &memory, std::uint64_t handle,
                                                         std::array<std::uint64_t, 5> &values, std::uint32_t &pid,
                                                         std::uint32_t &bits) {
    if (current_process_handle(handle, 0x400) || current_process_handle(handle, 0x1000)) {
        pid = process_id();
        return local_process_information(0, values, bits, -1);
    }
    if (current_process_handle(handle))
        return 5;
#if !defined(_WIN32)
    if (!processes_)
        return 6;
    const auto found = processes_->handles.find(handle);
    if (found == processes_->handles.end() || found->second.thread)
        return 6;
    const auto target = found->second;
    if (!(target.access & 0x1400U))
        return 5;
    if (!target.record)
        return 6;
    if (target.child)
        target.child->poll();
    pid = target.record->data->pid;
    if (target.record->exited()) {
        SharedEventLock lock(target.record->descriptor);
        if (!std::atomic_ref(target.record->data->information_valid).load(std::memory_order_acquire))
            return 50;
        values = target.record->data->information;
        values[0] = target.record->data->code;
        bits = target.record->data->bits;
        return 0;
    }
    std::uint32_t endpoint_pid{};
    std::uint64_t birth{};
    auto error = process_memory_target(memory, handle, target.access & 0x400U ? 0x400U : 0x1000U, endpoint_pid, birth);
    return error ? error : remote_process_request(memory, pid, birth, 14, 0, values, bits);
#else
    (void)memory;
    (void)handle;
    (void)values;
    (void)pid;
    (void)bits;
    return 50;
#endif
}
std::optional<std::uint32_t> WindowsServices::process_thread_target(GuestMemory &memory, std::uint64_t handle,
                                                                    std::uint32_t needed, std::uint32_t &pid,
                                                                    std::uint64_t *birth) {
#if !defined(_WIN32)
    if (!processes_)
        return {};
    const auto found = processes_->handles.find(handle);
    if (found == processes_->handles.end() || !found->second.thread)
        return {};
    if ((found->second.access & needed) != needed)
        return 5;
    // 等待 IPC 時可能接收關閉／回滾請求；本次操作持有自己的物件引用。
    const auto target = found->second;
    const auto child = target.child;
    if (child) {
        child->poll();
        if (child->exited)
            return 87;
    }
    if (!target.record || !target.record->alive())
        return 87;
    pid = target.record->data->pid;
    if (birth)
        *birth = target.record->data->birth;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!remote_process_available(pid)) {
        if (child) {
            child->poll();
            if (child->exited)
                return 87;
        }
        if (!target.record->alive())
            return 87;
        if (std::chrono::steady_clock::now() >= deadline)
            return 121;
        poll_handle_transfers(memory, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
#else
    (void)memory;
    (void)handle;
    (void)needed;
    (void)pid;
    (void)birth;
    return {};
#endif
}
std::uint64_t WindowsServices::process_identity_birth() const {
#if !defined(_WIN32)
    return processes_ && processes_->record ? processes_->record->data->birth : 0;
#else
    return 0;
#endif
}
std::optional<std::uint32_t> WindowsServices::process_token_target(GuestMemory &memory, std::uint64_t handle,
                                                                   std::uint32_t operation, std::uint32_t &pid,
                                                                   std::uint64_t &birth) {
#if !defined(_WIN32)
    if (!processes_)
        return {};
    const auto found = processes_->handles.find(handle);
    if (found == processes_->handles.end())
        return {};
    const auto target = found->second;
    if (operation == 1) {
        if (target.thread)
            return 6;
        if (current_process_handle(handle))
            return {};
        if (!(target.access & 0x1400U))
            return 5;
    } else {
        if (!target.thread)
            return 6;
        if (!(target.access & (operation == 0 ? 0x80U : 0x840U)))
            return 5;
    }
    if (target.child) {
        target.child->poll();
        if (target.child->exited)
            return 87;
    }
    if (!target.record || !target.record->alive())
        return 87;
    pid = target.record->data->pid;
    birth = target.record->data->birth;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!remote_process_available(pid)) {
        if (!target.record->alive())
            return 87;
        if (std::chrono::steady_clock::now() >= deadline)
            return 121;
        poll_handle_transfers(memory, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
#else
    (void)memory;
    (void)handle;
    (void)operation;
    (void)pid;
    (void)birth;
    return {};
#endif
}
std::optional<std::uint32_t> WindowsServices::process_thread_control(GuestMemory &memory, std::uint64_t handle,
                                                                     std::uint32_t operation, std::uint32_t &value) {
    std::uint32_t pid{};
    const auto target = process_thread_target(memory, handle, 2U, pid);
    if (!target || *target)
        return target;
    return remote_process_thread_control(memory, pid, operation, value);
}
std::optional<std::uint32_t> WindowsServices::process_thread_context(GuestMemory &memory, std::uint64_t handle,
                                                                     std::uint64_t address, bool setting) {
    std::uint32_t pid{};
    const auto target = process_thread_target(memory, handle, setting ? 16U : 8U, pid);
    if (!target || *target)
        return target;
    if (address % (x86_ ? 4U : 16U))
        return 87;
    try {
        const auto size = x86_ ? 716U : 1232U;
        std::vector<std::uint8_t> bytes(size);
        memory.read(GuestAddress{address}, bytes);
        if (!setting)
            memory.check(GuestAddress{address}, size, Access::Write);
        const auto error = remote_thread_context(memory, pid, bytes, setting);
        if (!error && !setting)
            memory.write(GuestAddress{address}, bytes);
        return error;
    } catch (const MemoryFault &) {
        return 998;
    } catch (const std::bad_alloc &) {
        return 8;
    }
}
std::uint32_t WindowsServices::local_process_thread_control(std::uint32_t operation, std::uint32_t &value) {
    const auto found = threads_.items.find(1);
    if (found == threads_.items.end())
        return 6;
    auto &thread = found->second;
    if (thread.status == ThreadStatus::Exited)
        return 87;
    if (operation != 2 && operation != 3)
        return 87;
    if (operation == 2 && thread.suspended >= 127)
        return 87;
    value = thread.suspended;
    if (operation == 2) {
        ++thread.suspended;
        if (threads_.current == 1)
            threads_.yield = true;
    } else if (thread.suspended)
        --thread.suspended;
    notify_concurrency_changed();
    return 0;
}
std::uint32_t WindowsServices::local_process_handle_count() const {
    std::size_t count = power_requests_.size() + files_.size() + job_handle_count() + mapping_handle_count() +
                        runtime_.wait_objects.size() + runtime_.completion_ports.size() +
                        runtime_.process_thread_handles.size() + runtime_.token_handles.size() +
                        runtime_.process_snapshots.size() + runtime_.network.sockets.size() +
                        runtime_.registry.handles.size() + runtime_.directory_searches.size();
    for (const auto open : standard_open_)
        count += open ? 1 : 0;
    // CRT 尚未物化成 Win32 handle 的 descriptor 仍代表一個開啟項目；已物化者由 files_ 計數。
    for (const auto &[pointer, file] : runtime_.streams) {
        (void)pointer;
        if (!file.standard_handle && !file.os_handle)
            ++count;
    }
#if !defined(_WIN32)
    if (processes_)
        count += processes_->handles.size();
#endif
    return static_cast<std::uint32_t>(std::min<std::size_t>(count, UINT32_MAX));
}
std::vector<std::uint64_t> WindowsServices::local_process_handles() const {
    std::vector<std::uint64_t> result;
    const auto append = [&](const auto &entries) {
        for (const auto &[handle, item] : entries) {
            (void)item;
            result.push_back(handle);
        }
    };
    append(power_requests_);
    append(files_);
    append_job_handles(result);
    append_mapping_handles(result);
    append(runtime_.wait_objects);
    append(runtime_.completion_ports);
    append(runtime_.process_thread_handles);
    append(runtime_.token_handles);
    append(runtime_.process_snapshots);
    append(runtime_.network.sockets);
    append(runtime_.registry.handles);
    append(runtime_.directory_searches);
    for (unsigned n = 0; n < 3; ++n)
        if (standard_open_[n])
            result.push_back(0x100 + n);
#if !defined(_WIN32)
    if (processes_)
        append(processes_->handles);
#endif
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
std::uint32_t WindowsServices::process_id() const noexcept { return processes_ ? processes_->identity : 1; }
std::string WindowsServices::process_diagnostics() const {
    std::string result;
#if !defined(_WIN32)
    if (processes_)
        for (const auto &child : processes_->children) {
            result += "child_process=" + std::to_string(child->pid) + " exited=" + (child->exited ? "1" : "0") +
                      " terminated=" + (child->terminated ? "1" : "0") +
                      " exit_code=" + std::to_string(child->exit_code);
            std::string details;
            if (child->exited && !child->terminated) {
                std::ifstream input(child->diagnostics, std::ios::binary);
                std::string line;
                char character{};
                bool oversized = false;
                for (std::size_t scanned = 0; scanned < 1024U * 1024U && input.get(character); ++scanned) {
                    if (character == '\n') {
                        if (!oversized && line.starts_with("gpu_memory_limit_bytes=")) {
                            result += ' ';
                            result += line;
                        } else if (!oversized && (line.starts_with("reason=") || line.starts_with("RAX=") ||
                                                  line.starts_with("診斷：") || line.starts_with("fault_address=") ||
                                                  line.starts_with("bytes=") || line.starts_with("instruction=") ||
                                                  line.starts_with("exit_code="))) {
                            details += "child_diagnostic=" + std::to_string(child->pid) + " " + line + '\n';
                        }
                        line.clear();
                        oversized = false;
                    } else if (line.size() < 1024)
                        line.push_back(character);
                    else
                        oversized = true;
                }
            }
            result += '\n';
            result += details;
        }
#endif
    return result;
}
void WindowsServices::import_process_handles(const std::string &encoded) {
#if !defined(_WIN32)
    if (encoded.size() > 1024 * 1024)
        throw HostCallFault("繼承清單過大");
    std::istringstream stream(encoded);
    std::string row;
    std::size_t count = 0;
    while (std::getline(stream, row, ';')) {
        std::replace(row.begin(), row.end(), ',', ' ');
        std::istringstream entry(row);
        InheritedHandle item;
        std::string extra;
        if (++count > 4096 ||
            !(entry >> item.handle >> item.type >> item.access >> item.flags >> item.mode >> item.pipe_mode >>
              item.auxiliary >> item.descriptor) ||
            (entry >> extra) ||
            (item.type == 8 ? (item.handle < 0x100 || item.handle > 0x102) : item.handle < 0x1000) ||
            item.handle > 0x7fffffff || item.descriptor < 200 || item.descriptor > 65535 || (item.flags & ~3U) ||
            files_.contains(item.handle) || mapping_handle(item.handle) ||
            runtime_.wait_objects.contains(item.handle) || !process_object_type(item.handle).empty())
            throw HostCallFault("繼承清單內容無效");
        if (item.type == 1) {
            auto opened =
                NamedPipe::inherit(mapping_session(), item.descriptor, item.auxiliary, (item.mode & 0x80000000U) != 0);
            if (opened.error)
                throw HostCallFault("無法匯入繼承管道：" + std::to_string(opened.error));
            FileHandle file;
            file.bytes = std::make_shared<const std::vector<std::uint8_t>>();
            file.pipe = std::move(opened.pipe);
            file.access = item.access;
            file.mode = item.mode & 0x7fffffffU;
            file.pipe_mode = item.pipe_mode;
            file.object = item.handle;
            files_.emplace(item.handle, std::move(file));
            runtime_.win_file_positions[item.handle] = 0;
        } else if (item.type == 2)
            import_mapping_handle(item);
        else if (item.type == 4)
            import_job_handle(item);
        else if (item.type == 3 || item.type == 5 || item.type == 6 || item.type == 7 || item.type == 8 ||
                 item.type == 9) {
            import_transfer_handle(item);
            continue;
        } else
            throw HostCallFault("未知的繼承物件種類");
        runtime_.handle_flags[item.handle] = item.flags;
        runtime_.next_wait_handle = std::max(runtime_.next_wait_handle, (item.handle + 4) & ~UINT64_C(3));
    }
    send_packet(4, 0);
#else
    (void)encoded;
    throw HostCallFault("此平台不支援繼承 handle");
#endif
}
void WindowsServices::set_process_directory(std::u16string directory) {
    if (directory != u"C:\\" && filesystem_.set_current_directory(utf16_to_utf8(directory)))
        throw std::invalid_argument("客體子程序工作目錄無效");
    runtime_.current_directory = std::move(directory);
}
std::uint32_t WindowsServices::kernel_pseudo_access(bool thread) const {
    // Vista 之後的自身 pseudo handle 是既有完整能力；重新 Open 才依物件 DACL 授權。
    return thread ? threads_.items.at(threads_.current).pseudo_access : 0x1fffffU;
}
bool WindowsServices::current_process_handle(std::uint64_t handle, std::uint32_t access) const {
    if (handle == (x86_ ? UINT32_MAX : UINT64_MAX)) {
        if (!access)
            return true;
        return (kernel_pseudo_access(false) & access) == access;
    }
#if !defined(_WIN32)
    if (processes_) {
        const auto found = processes_->handles.find(handle);
        return found != processes_->handles.end() && !found->second.thread && !found->second.child &&
               !found->second.remote_pid && (found->second.access & access) == access;
    }
#else
    (void)access;
#endif
    return false;
}
std::uint32_t WindowsServices::process_handle_pid(std::uint64_t handle, std::uint32_t rights) const {
    if (current_process_handle(handle, rights))
        return process_id();
#if !defined(_WIN32)
    if (processes_) {
        const auto found = processes_->handles.find(handle);
        if (found != processes_->handles.end() && !found->second.thread && (found->second.access & rights) == rights)
            return found->second.child ? static_cast<std::uint32_t>(found->second.child->pid)
                                       : found->second.remote_pid;
    }
#endif
    return 0;
}
void WindowsServices::configure_process_io() {
#if defined(__APPLE__)
    if (io_publisher_)
        return;
    const auto birth = process_debugger::query(host_process_id());
    if (birth.error || !birth.identity)
        throw HostCallFault("無法識別程序 I/O 計數的原生出生身分");
    io_publisher_ =
        std::make_shared<process_io::Publisher>(mapping_session(), process_id(), birth.identity, runtime_.io_counters);
#endif
}
std::uint32_t WindowsServices::query_process_io(std::uint64_t handle, process_io::Counters &counters) {
    if (process_object_type(handle) != u"Process")
        return 6;
    auto pid = process_handle_pid(handle, 0x400);
    if (!pid)
        pid = process_handle_pid(handle, 0x1000);
    if (!pid)
        return 5;
    if (pid == process_id()) {
        counters = runtime_.io_counters;
        return 0;
    }
    if (!runtime_.network.external_io)
        throw HostCallFault("比較模式不允許查詢跨程序 I/O 計數");
#if defined(__APPLE__)
    auto &object = processes_->handles.at(handle);
    if (!object.identity) {
        const auto birth = process_debugger::query(pid);
        if (birth.error)
            return birth.error;
        if (!birth.identity)
            return 31;
        object.identity = birth.identity;
    }
    // 檔名與內容均繫結出生身分；程序退出後仍保留最後已完成的 I/O。
    return process_io::Publisher::read(mapping_session(), pid, object.identity, counters);
#else
    return 50;
#endif
}
std::uint32_t WindowsServices::process_thread_target(std::uint64_t handle, std::uint32_t &pid, std::uint32_t &rights,
                                                     std::uint64_t *birth, std::uint32_t *tid) {
#if !defined(_WIN32)
    if (!processes_)
        return 6;
    const auto found = processes_->handles.find(handle);
    if (found == processes_->handles.end() || !found->second.thread)
        return 6;
    if (!runtime_.network.external_io)
        throw HostCallFault("比較模式不允許查詢跨程序執行緒");
    auto &object = found->second;
    resolve_thread_record(object,mapping_session());
    rights = object.access;
    if (tid)
        *tid = object.remote_tid ? object.remote_tid : 1;
    if ((object.debug_life && object.debug_life->exited) || (object.record && object.record->data->object_kind && object.record->exited()))
        return 87;
    pid = object.child ? static_cast<std::uint32_t>(object.child->pid) : object.remote_pid;
    if (!pid || object.exited)
        return 87;
    const auto current = process_debugger::query(pid);
    if (current.error || current.exited || (object.identity && current.identity != object.identity)) {
        object.exited = true;
        return 87;
    }
    if (!current.identity)
        return 31;
    object.identity = current.identity;
    if (birth)
        *birth = object.identity;
    return 0;
#else
    (void)handle;
    (void)pid;
    (void)rights;
    (void)birth;
    (void)tid;
    return 6;
#endif
}
std::uint32_t WindowsServices::process_remote_target(std::uint64_t handle, std::uint32_t rights, std::uint32_t &pid,
                                                     std::uint64_t &birth) {
#if !defined(_WIN32)
    if (!processes_)
        return 6;
    const auto found = processes_->handles.find(handle);
    if (found == processes_->handles.end() || found->second.thread)
        return 6;
    auto &object = found->second;
    if ((object.access & rights) != rights)
        return 5;
    if (!runtime_.network.external_io)
        throw HostCallFault("比較模式不允許操作跨程序位址空間");
    pid = object.child ? static_cast<std::uint32_t>(object.child->pid) : object.remote_pid;
    if (!object.identity && object.record)
        object.identity = process_debugger::query(pid).identity;
    if (!pid || object.exited || !object.identity || (object.debug_life && object.debug_life->exited))
        return 87;
    const auto current = process_debugger::query(pid);
    if (current.error || current.exited || current.identity != object.identity) {
        object.exited = true;
        return 87;
    }
    birth = object.identity;
    return 0;
#else
    (void)handle;
    (void)rights;
    (void)pid;
    (void)birth;
    return 50;
#endif
}
std::uint64_t WindowsServices::open_process_thread(GuestMemory &memory, std::uint32_t identifier, std::uint32_t rights,
                                                   bool inherit) {
#if !defined(_WIN32)
    if (!runtime_.network.external_io)
        throw HostCallFault("比較模式不允許開啟跨程序執行緒");
    const auto fail = [&](std::uint32_t error) {
        last_error_ = error;
        return UINT64_C(0);
    };
    auto record=ProcessRecord::find_thread(mapping_session(),identifier);
    const auto pid=record?record->data->pid:identifier;
    const auto tid=record?record->data->thread_id:1U;
    if (!identifier || (!record && identifier>=0x80000000U) || !remote_process_available(pid))
        return fail(87);
    if (processes_ && processes_->handles.size() >= 1024)
        return fail(8);
    const auto birth = process_debugger::query(pid);
    if (birth.error || birth.exited || !birth.identity)
        return fail(87);
    std::uint32_t value = 0;
    std::vector<std::uint8_t> context;
    if(!record || !record->exited())if (const auto error = transfer_thread_control(memory, pid, 0, ExecutionMode::X86, 0, value, context,birth.identity,tid))return fail(error);
    const auto current = process_debugger::query(pid);
    if (current.error || current.exited || current.identity != birth.identity)
        return fail(87);
    if (!processes_)
        processes_ = std::make_shared<ProcessState>();
    if(!record)record=ProcessRecord::open(mapping_session(),pid,false);
    auto granted = rights;
    if (record) {
        SharedEventLock lock(record->descriptor);
        granted = kernel_security_access(shared_object_security(record->descriptor, true), rights, true);
    }
    const auto handle = runtime_.allocate_wait_handle();
    Handle object{{},true,granted,pid,std::move(record),birth.identity,false,tid};object.parent_record=ProcessRecord::open(mapping_session(),pid,false);
    processes_->handles.emplace(handle,std::move(object));
    runtime_.handle_flags[handle] = inherit ? 1U : 0U;
    return handle;
#else
    (void)memory;
    (void)identifier;
    (void)rights;
    (void)inherit;
    last_error_ = 50;
    return 0;
#endif
}
std::uint64_t WindowsServices::make_debug_handle(std::uint32_t pid, std::uint64_t birth, std::uint32_t tid) {
#if !defined(_WIN32)
    if (!processes_)
        processes_ = std::make_shared<ProcessState>();
    if (processes_->handles.size() >= 1024) {
        last_error_ = 8;
        return 0;
    }
    const auto identity = process_debugger::query(pid);
    if (identity.error || identity.exited || !birth || birth != identity.identity) {
        last_error_ = 87;
        return 0;
    }
    Handle object;
    object.thread = tid != 0;
    object.remote_pid = pid;
    object.remote_tid = tid;
    object.identity = birth;
    object.record = ProcessRecord::open(mapping_session(), pid, false,tid);
    if(tid)object.parent_record=ProcessRecord::open(mapping_session(),pid,false);
    if(!object.record)object.record=ProcessRecord::open(mapping_session(),pid,false);
    object.access = tid ? 0x10005aU : 0x1fffffU;
    object.debug_life = std::make_shared<DebugHandleLife>();
    const auto handle = runtime_.allocate_wait_handle();
    processes_->handles.emplace(handle, std::move(object));
    return handle;
#else
    (void)pid;
    (void)birth;
    (void)tid;
    last_error_ = 50;
    return 0;
#endif
}
void WindowsServices::finish_debug_handles(std::uint32_t pid, std::uint64_t birth, std::uint32_t tid,
                                           std::uint32_t code) {
#if !defined(_WIN32)
    if (processes_)
        for (auto &[handle, object] : processes_->handles) {
            (void)handle;
            if (object.remote_pid == pid && object.identity == birth && object.debug_life &&
                (!tid || object.remote_tid == tid)) {
                object.debug_life->exited = true;
                object.debug_life->code = code;
                // 已確認的 guest 偵錯退出事件也提交到共用生命週期，複製／繼承的 handle 讀取同一退出碼。
                if(object.record && (!tid || object.record->data->object_kind))object.record->finish(code);
            }
        }
#else
    (void)pid;
    (void)birth;
    (void)tid;
    (void)code;
#endif
}
std::optional<WindowsServices::ObjectMetadata> WindowsServices::process_object_metadata(std::uint64_t handle) const {
    const auto type = process_object_type(handle);
    if (type.empty())
        return {};
    ObjectMetadata out;
    out.access = 0x1fffffU;
    out.identity = process_id();
#if !defined(_WIN32)
    const auto identity = [&](const auto &item) {
        return item.child        ? static_cast<std::uint64_t>(item.child->pid)
               : item.remote_pid ? static_cast<std::uint64_t>(item.remote_pid)
                                 : static_cast<std::uint64_t>(process_id());
    };
    if (processes_) {
        const auto found = processes_->handles.find(handle);
        if (found != processes_->handles.end()) {
            out.access = found->second.access;
            out.identity = identity(found->second);
        }
        for (const auto &[h, item] : processes_->handles) {
            (void)h;
            if (item.thread == (type == u"Thread") && identity(item) == out.identity)
                ++out.handles;
        }
    }
#endif
    out.pointers = out.handles + 1;
    return out;
}
std::u16string_view WindowsServices::process_object_type(std::uint64_t handle) const {
    if (handle == (x86_ ? UINT32_MAX : UINT64_MAX))
        return u"Process";
#if !defined(_WIN32)
    if (processes_) {
        const auto found = processes_->handles.find(handle);
        if (found != processes_->handles.end())
            return found->second.thread ? u"Thread" : u"Process";
    }
#endif
    return {};
}
bool WindowsServices::close_process(std::uint64_t handle) {
#if !defined(_WIN32)
    return processes_ && processes_->handles.erase(handle);
#else
    (void)handle;
    return false;
#endif
}
std::optional<bool> WindowsServices::process_signaled(std::uint64_t handle, bool input_idle) {
#if !defined(_WIN32)
    if (!processes_)
        return {};
    const auto found = processes_->handles.find(handle);
    if (found == processes_->handles.end())
        return {};
    resolve_thread_record(found->second,mapping_session());
    if(found->second.thread && found->second.record && found->second.record->data->object_kind)
        return found->second.record->exited() || !found->second.record->alive();
    if (found->second.debug_life && found->second.debug_life->exited)
        return true;
    if (found->second.remote_pid) {
        const auto &record = found->second.record;
        return record ? (record->exited() || !record->alive() ||
                         (input_idle && std::atomic_ref(record->data->idle).load(std::memory_order_acquire)))
                      : !remote_process_available(found->second.remote_pid);
    }
    if (!found->second.child)
        return false;
    found->second.child->poll();
    return found->second.child->exited || (input_idle && found->second.child->idle);
#else
    (void)handle;
    (void)input_idle;
    return {};
#endif
}
std::vector<int> WindowsServices::child_pipe_descriptors() const {
    std::vector<int> result;
#if !defined(_WIN32)
    if (processes_)
        for (const auto &child : processes_->children)
            if (!child->exited && child->pipe >= 0)
                result.push_back(child->pipe);
#endif
    return result;
}
bool WindowsServices::processes_waiting() {
#if !defined(_WIN32)
    if (!processes_)
        return false;
    bool pending = false;
    for(auto it=processes_->crt_waits.begin();it!=processes_->crt_waits.end();){
        const auto owner=threads_.items.find(it->first);
        if(owner==threads_.items.end() || owner->second.status==ThreadStatus::Exited){
            if(!it->second.cwait){(void)close_process(it->second.handle);runtime_.handle_flags.erase(it->second.handle);}
            it=processes_->crt_waits.erase(it);
        }else ++it;
    }
    // 管線仍開啟且由監看執行緒等待的子程序不需輪詢；EOF 後到 wait4 回收的短暫空窗仍以輪詢補上。
    const bool watched=handle_transfers_ && !handle_transfer_polling();
    for (auto &child : processes_->children) {
        child->poll();
        pending |= !child->exited && !(watched && child->pipe >= 0);
    }
    return pending || handle_transfer_polling();
#else
    return false;
#endif
}
std::span<const WindowsServiceDescriptor> process_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","PssCaptureSnapshot",4,0,0,0x6},{"kernel32.dll","PssQuerySnapshot",4,0,0,0xa},
        {"kernel32.dll","PssFreeSnapshot",2},{"kernel32.dll","PssDuplicateSnapshot",5,0,0,0x10},
        {"crt","_spawnv",3},{"crt","_spawnve",4},{"crt","_spawnvp",3},{"crt","_spawnvpe",4},
        {"crt","_wspawnv",3},{"crt","_wspawnve",4},{"crt","_wspawnvp",3},{"crt","_wspawnvpe",4},
        {"crt","_execv",2},{"crt","_execve",3},{"crt","_execvp",2},{"crt","_execvpe",3},
        {"crt","_wexecv",2},{"crt","_wexecve",3},{"crt","_wexecvp",2},{"crt","_wexecvpe",3},
        {"crt","_cwait",3},{"crt","system",1},{"crt","_wsystem",1},
        {"kernel32.dll", "DebugActiveProcess", 1},
        {"kernel32.dll", "DebugActiveProcessStop", 1},
        {"kernel32.dll", "WaitForDebugEvent", 2},
        {"kernel32.dll", "WaitForDebugEventEx", 2},
        {"kernel32.dll", "ContinueDebugEvent", 3},
        {"kernel32.dll", "InitializeProcThreadAttributeList", 4, 0, 0, 0x6},
        {"kernel32.dll", "UpdateProcThreadAttribute", 7, 0, 0, 0x2},
        {"kernel32.dll", "DeleteProcThreadAttributeList", 1},
        {"kernel32.dll", "CreateProcessW", 10, 0, 0, 0x30},
        {"kernel32.dll", "CreateProcessA", 10, 0, 0, 0x30},
        {"advapi32.dll", "CreateProcessAsUserW", 11, 0, 0, 0x60},
        {"kernel32.dll", "CreateProcessAsUserW", 11, 0, 0, 0x60},
        {"advapi32.dll", "CreateProcessAsUserA", 11, 0, 0, 0x60},
        {"kernel32.dll", "OpenProcess", 3, 0, 0, 0x7},
        {"kernel32.dll", "GetExitCodeProcess", 2},
        {"kernel32.dll", "GetProcessId", 1},
        {"kernel32.dll", "CheckRemoteDebuggerPresent", 2},
        {"kernel32.dll", "GetProcessIoCounters", 2},
        {"kernel32.dll", "SetProcessAffinityMask", 2},
        {"kernel32.dll", "GetProcessAffinityMask", 3},
        {"kernel32.dll", "SetProcessShutdownParameters", 2, 0, 0, 0x3},
        {"kernel32.dll", "GetProcessShutdownParameters", 2},
        {"kernel32.dll", "ProcessIdToSessionId", 2, 0, 0, 0x1},
        {"kernel32.dll", "GetProcessHandleCount", 2},
        {"kernel32.dll", "GetProcessMitigationPolicy", 4, 0, 0, 0x2},
        {"kernel32.dll", "SetProcessMitigationPolicy", 3, 0, 0, 0x1},
        {"kernel32.dll", "SetProcessInformation", 4}, {"kernel32.dll", "GetProcessInformation", 4},
        {"kernel32.dll", "GetProcessDEPPolicy", 3},
        {"kernel32.dll", "SetProcessDEPPolicy", 1, 0, 0, 0x1},
        {"kernel32.dll", "GetSystemDEPPolicy", 0},
        {"kernel32.dll", "CreateJobObjectA", 2},
        {"kernel32.dll", "CreateJobObjectW", 2},
        {"kernel32.dll", "OpenJobObjectA", 3, 0, 0, 0x3},
        {"kernel32.dll", "OpenJobObjectW", 3, 0, 0, 0x3},
        {"kernel32.dll", "SetInformationJobObject", 4, 0, 0, 0xa},
        {"kernel32.dll", "QueryInformationJobObject", 5, 0, 0, 0xa},
        {"kernel32.dll", "AssignProcessToJobObject", 2},
        {"kernel32.dll", "TerminateJobObject", 2, 0, 0, 0x2},
        {"kernel32.dll", "IsProcessInJob", 3},
        {"kernel32.dll", "GetPriorityClass", 1},
        {"kernel32.dll", "SetPriorityClass", 2, 0, 0, 0x2},
        {"kernel32.dll", "GetProcessPriorityBoost", 2},
        {"kernel32.dll", "SetProcessPriorityBoost", 2, 0, 0, 0x2},
        {"kernel32.dll", "TerminateProcess", 2, 0, 0, 0x2},
        {"user32.dll", "WaitForInputIdle", 2, 0, 0, 0x2}};
    return table;
}
bool WindowsServices::create_guest_process(GuestMemory &memory, const std::array<std::uint64_t, 10> &call_arguments,
                                           bool wide, bool as_user, std::uint64_t token_handle, unsigned privilege) {
    startup_profile::ProcessLaunch launch;
    initialize_tokens();
    if ((runtime_.child_process_policy & 1U) || runtime_.token_objects.at(1).child_process_restricted) {
        last_error_ = 367;
        launch.result(last_error_);
        return false;
    }
#if defined(_WIN32)
    (void)memory;
    (void)call_arguments;
    (void)wide;
    (void)as_user;
    (void)token_handle;
    (void)privilege;
    last_error_ = 50;
    launch.result(last_error_);
    return false;
#else
    const auto arg = [&](unsigned i) { return call_arguments.at(i); };
    std::uint64_t result = 0;
    SynchronizationDescriptor process_security, thread_security;
    const auto fail = [&](std::uint32_t error) {
        last_error_ = error;
        launch.result(error);
        result = 0;
        if (trace_process_protocol()) {
            std::fprintf(stderr, "process_creation_failure pid=%u error=%u flags=%x as_user=%u\n", process_id(), error,
                         static_cast<unsigned>(arg(5)), as_user ? 1U : 0U);
            try {
                if (arg(8)) {
                    const auto startup = arg(8), size = memory.load(GuestAddress{startup}, 32),
                               startup_flags = memory.load(GuestAddress{startup + (x86_ ? 44U : 60U)}, 32);
                    std::fprintf(stderr, "process_startup size=%llu flags=%llx", static_cast<unsigned long long>(size),
                                 static_cast<unsigned long long>(startup_flags));
                    if (startup_flags & 0x100U)
                        for (unsigned i = 0; i < 3; ++i)
                            std::fprintf(stderr, " std%u=%llx", i,
                                         static_cast<unsigned long long>(memory.load(
                                             GuestAddress{startup + (x86_ ? 56U : 80U) + i * (x86_ ? 4U : 8U)},
                                             x86_ ? 32 : 64)));
                    std::fputc('\n', stderr);
                }
            } catch (const MemoryFault &) {
            }
        }
    };
    if (!runtime_.network.external_io)
        throw HostCallFault("子程序不支援 deterministic compare");
    const auto text = [&](std::uint64_t pointer) {
        return !pointer ? std::u16string{}
               : wide   ? read_utf16(memory, GuestAddress{pointer})
                        : utf8_to_utf16(read_ansi(memory, pointer));
    };
    const auto application = text(arg(0));
    auto command = text(arg(1));
    const auto arguments = parse_windows_arguments(command);
    auto image = application.empty() ? (arguments.empty() ? std::u16string{} : arguments.front()) : application;
    const auto flags = static_cast<std::uint32_t>(arg(5));
    auto [token_bytes, token_error] = process_creation_token(as_user ? token_handle : 0, privilege);
    std::uint32_t job_error = 0;
    auto job_descriptor = inherited_job_descriptor(flags, job_error);
    if (as_user && !token_handle)
        fail(6);
    else if (token_error)
        fail(token_error);
    else if (job_error)
        fail(job_error);
    else if (!processes_ || processes_->options.engine.empty())
        fail(50);
    else if (image.empty() || !arg(8) || !arg(9))
        fail(87);
    else if (flags & ~0x0908063cU)
        fail(50);
    else if (memory.load(GuestAddress{arg(8)}, 32) < (x86_ ? 68U : 104U))
        fail(87);
    else if (processes_->children.size() >= 1024)
        fail(8);
    else {
        std::vector<std::uint8_t> environment_bytes;
        ProcessStartupData startup_data;startup_data.pointer_bytes=x86_?4U:8U;
        try {
            const auto reserved_size=memory.load(GuestAddress{arg(8)+(x86_?50U:66U)},16);
            const auto reserved_pointer=memory.load(GuestAddress{arg(8)+(x86_?52U:72U)},x86_?32:64);
            if(reserved_size){if(!reserved_pointer){fail(87);return false;}startup_data.reserved2.resize(static_cast<std::size_t>(reserved_size));memory.read(GuestAddress{reserved_pointer},startup_data.reserved2);}
            if (arg(6)) {
                const bool unicode = flags & 0x400;
                const unsigned character_size = unicode ? 2 : 1;
                std::vector<std::uint8_t> bytes;
                bool zero = false, terminated = false;
                for (std::uint64_t offset = 0; offset < output_limit; offset += character_size) {
                    if (arg(6) > UINT64_MAX - offset)
                        throw std::invalid_argument("環境區塊位址溢位");
                    const auto c = memory.load(GuestAddress{arg(6) + offset}, unicode ? 16 : 8);
                    bytes.push_back(static_cast<std::uint8_t>(c));
                    if (unicode)
                        bytes.push_back(static_cast<std::uint8_t>(c >> 8));
                    if (!c && zero) {
                        terminated = true;
                        break;
                    }
                    zero = c == 0;
                }
                const auto parsed = terminated ? decode_guest_environment(bytes, unicode) : std::nullopt;
                if (!parsed)
                    throw std::invalid_argument("環境區塊格式無效");
                environment_bytes = encode_guest_environment(*parsed);
            } else
                environment_bytes = encode_guest_environment(runtime_.environment);
        } catch (const std::invalid_argument &) {
            fail(87);
            return false;
        } catch (const std::length_error &) {
            fail(203);
            return false;
        } catch (const std::bad_alloc &) {
            fail(8);
            return false;
        }
        memory.check(GuestAddress{arg(9)}, x86_ ? 16 : 24, Access::Write);
        std::vector<std::uint64_t> selected, startup_jobs;
        bool explicit_list = false, explicit_policy = false, explicit_jobs = false;
        std::uint32_t inherit_error = 0, child_policy = 0;
        const std::uint8_t width = x86_ ? 4 : 8, bits = x86_ ? 32 : 64;
        if (flags & 0x80000U) {
            if (memory.load(GuestAddress{arg(8)}, 32) < (x86_ ? 72U : 112U))
                inherit_error = 87;
            else if (const auto list = memory.load(GuestAddress{arg(8) + (x86_ ? 68U : 104U)}, bits)) {
                const auto count = memory.load(GuestAddress{list + 4}, 32),
                           used = memory.load(GuestAddress{list + 8}, 32);
                if (memory.load(GuestAddress{list}, 32) != 0x54544158 || !count || count > 64 || used > count)
                    inherit_error = 87;
                else
                    for (std::uint64_t i = 0; i < used; ++i) {
                        const auto entry = list + 16 + i * 3 * width,
                                   attribute = memory.load(GuestAddress{entry}, bits),
                                   pointer = memory.load(GuestAddress{entry + width}, bits),
                                   size = memory.load(GuestAddress{entry + 2 * width}, bits);
                        if (!pointer) {
                            inherit_error = 87;
                            break;
                        }
                        if (attribute == 0x2000e) {
                            if (explicit_policy || size != 4) {
                                inherit_error = 87;
                                break;
                            }
                            explicit_policy = true;
                            child_policy = static_cast<std::uint32_t>(memory.load(GuestAddress{pointer}, 32));
                            if (child_policy & ~7U) {
                                inherit_error = 87;
                                break;
                            }
                            if (child_policy & 4U) {
                                inherit_error = 50;
                                break;
                            }
                        } else if (attribute == 0x20002 || attribute == 0x2000d) {
                            auto &seen = attribute == 0x20002 ? explicit_list : explicit_jobs;
                            if (seen || (attribute == 0x20002 && !arg(4)) || !size || size % width ||
                                size / width > 4096) {
                                inherit_error = 87;
                                break;
                            }
                            seen = true;
                            auto &handles = attribute == 0x20002 ? selected : startup_jobs;
                            for (std::uint64_t j = 0; j < size / width; ++j)
                                handles.push_back(memory.load(GuestAddress{pointer + j * width}, bits));
                        } else {
                            inherit_error = 50;
                            break;
                        }
                    }
            }
        }
        if (inherit_error) {
            fail(inherit_error);
            return false;
        }
        auto creation_token = decode_virtual_token(token_bytes);
        if (!creation_token) {
            fail(13);
            return false;
        }
        if (creation_token->child_process_restricted && !(child_policy & 2U)) {
            fail(367);
            return false;
        }
        // 建立者的限制已在共用入口檢查；只調整新程序的權杖副本。
        if (child_policy & 2U)
            creation_token->child_process_restricted = false;
        if (child_policy & 1U)
            creation_token->child_process_restricted = true;
        token_bytes = encode_virtual_token(*creation_token);
        try {
            const auto creation_security = [&](std::uint64_t address) {
                if (!as_user)
                    return synchronization_descriptor(memory, address);
                auto attributes = read_security_attributes(memory, address, x86_);
                auto &sd = attributes.descriptor;
                if (sd.data[0].empty())
                    sd.data[0] = creation_token->owner;
                if (sd.data[1].empty())
                    sd.data[1] = creation_token->primary_group;
                if (!(sd.control & 4U)) {
                    sd.control |= 4;
                    sd.data[3] = creation_token->default_dacl;
                }
                return SynchronizationDescriptor{attributes.inherit,
                                                 object_security_descriptor(std::move(sd), true, false)};
            };
            process_security = creation_security(arg(2));
            thread_security = creation_security(arg(3));
            const auto label = [&](auto &object) {
                auto sd = security::parse_relative_descriptor(object.bytes);
                if (sd.data[2].empty() && creation_token->integrity != 0x2000) {
                    sd.control |= 16;
                    sd.data[2] = security::integrity_label(creation_token->integrity);
                    object.bytes = security::relative_descriptor(sd);
                }
            };
            label(process_security);
            label(thread_security);
            token_bytes = process_startup_blob(token_bytes, process_security.bytes, thread_security.bytes);
        } catch (const SecurityDescriptorError &error) {
            fail(error.code);
            return false;
        } catch (const MemoryFault &) {
            fail(87);
            return false;
        }
        if (explicit_jobs) {
            job_descriptor = startup_job_descriptor(startup_jobs, job_descriptor, job_error);
            if (job_error) {
                fail(job_error);
                return false;
            }
        }
        if (arg(4) && !explicit_list)
            for (const auto &[handle, attributes] : runtime_.handle_flags)
                if (attributes & 1)
                    selected.push_back(handle);
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        std::vector<InheritedHandle> inherited;
        for (const auto handle : selected) {
            if((flags&8U) && !explicit_list && handle>=0x100 && handle<=0x102){
                bool requested=false;
                if(memory.load(GuestAddress{arg(8)+(x86_?44U:60U)},32)&0x100U)
                    for(unsigned i=0;i<3;++i)requested|=memory.load(GuestAddress{arg(8)+(x86_?56U:80U)+i*width},bits)==handle;
                if(!requested || console_handle(handle))continue;
            }
            const auto attributes = runtime_.handle_flags.find(handle);
            if (attributes == runtime_.handle_flags.end() || !(attributes->second & 1)) {
                inherit_error = 6;
                break;
            }
            InheritedHandle item;
            item.handle = handle;
            item.flags = attributes->second;
            if (const auto file = files_.find(handle); file != files_.end() && file->second.pipe) {
                item.type = 1;
                item.pipe_mode = file->second.pipe_mode;
                item.access = file->second.access;
                item.mode = file->second.mode | (file->second.pipe->server() ? 0x80000000U : 0);
                item.auxiliary = file->second.pipe->identity();
                item.descriptor = file->second.pipe->descriptor();
                item.lifetime = file->second.pipe;
            } else {
                const auto error = export_transfer_handle(handle, item);
                if (error) {
                    if (trace_process_protocol()) {
                        const auto type = object_type(handle);
                        std::array<char, 64> type_name{};
                        for (std::size_t index = 0; index < std::min(type.size(), type_name.size() - 1); ++index)
                            type_name[index] = type[index] <= 0x7f ? static_cast<char>(type[index]) : '?';
                        std::fprintf(stderr, "process_inherit_failure pid=%u handle=%llx error=%u type=%s\n", process_id(),
                                     static_cast<unsigned long long>(handle), error, type.empty() ? "unknown" : type_name.data());
                    }
                    inherit_error = error;
                    break;
                }
            }
            inherited.push_back(std::move(item));
        }
        if (inherit_error) {
            fail(inherit_error);
            return false;
        }
        std::string full;
        try {
            if(application.empty() && image.find_first_of(u"\\/:")==image.npos){
                auto leaf=image;if(leaf.find(u'.')==leaf.npos)leaf+=u".exe";
                std::vector<std::u16string> directories{runtime_.application_directory};
                if(executable_needs_current_directory(image))directories.push_back(runtime_.current_directory);
                const auto root=runtime_.environment.find(u"SYSTEMROOT");const auto system=root==runtime_.environment.end()?u"C:\\Windows":root->second.second;
                directories.push_back(std::u16string(system)+u"\\System32");directories.push_back(system);
                if(const auto path_environment=runtime_.environment.find(u"PATH");path_environment!=runtime_.environment.end()){
                    const auto& value=path_environment->second.second;std::size_t begin=0;
                    do{const auto end=value.find(u';',begin);auto directory=value.substr(begin,end==value.npos?value.size()-begin:end-begin);
                        if(directory.size()>=2 && directory.front()==u'"' && directory.back()==u'"')directory=directory.substr(1,directory.size()-2);
                        if(!directory.empty())directories.push_back(std::move(directory));if(end==value.npos)break;begin=end+1;
                    }while(begin<value.size());
                }
                std::uint32_t search_error=2;
                for(const auto& directory:directories){if(directory.empty())continue;const auto candidate=normalized_windows_path(directory+u"\\"+leaf,runtime_.current_directory);
                    const auto executable_result=filesystem_.executable_path(utf16_to_utf8(candidate),runtime_.application_source,utf16_to_utf8(runtime_.application_directory));
                    if(!executable_result.second){image=candidate;search_error=0;break;}if(executable_result.second!=2 && executable_result.second!=3){search_error=executable_result.second;break;}}
                if(search_error){fail(search_error);return false;}
            }
            full = utf16_to_utf8(normalized_windows_path(image, runtime_.current_directory));
        } catch (const std::invalid_argument &) {
            fail(123);
            return false;
        }
        const auto [host, error] = filesystem_.executable_path(full, runtime_.application_source,
                                                               utf16_to_utf8(runtime_.application_directory));
        if (error)
            fail(error);
        else {
            try {
                const auto level=process_manifest::executable(host);
                const auto administrators=security::account_sid(*security::find_account("Administrators"));
                const bool admin=security::token_sid_matches(*creation_token,administrators,false) &&
                    (!creation_token->restriction_check || security::token_sid_matches(*creation_token,administrators,false,true));
                if((level==process_manifest::Level::Administrator && (!admin || creation_token->integrity<0x3000U)) ||
                   (level==process_manifest::Level::HighestAvailable && admin && creation_token->integrity<0x3000U)){
                    fail(740);return false;
                }
            }catch(const SecurityDescriptorError& manifest_error){fail(manifest_error.code);return false;}
            catch(const std::invalid_argument&){fail(193);return false;}
            catch(const std::bad_alloc&){fail(8);return false;}
            std::u16string current_directory;
            try {
                current_directory = arg(7) ? normalized_windows_path(text(arg(7)), runtime_.current_directory)
                                           : runtime_.current_directory;
            } catch (const std::invalid_argument &) {
                fail(267);
                return false;
            }
            const auto directory = filesystem_.information(utf16_to_utf8(current_directory));
            if (arg(7) && (directory.error || !(directory.metadata.attributes & 0x10)))
                fail(267);
            else {
                if (command.empty())
                    command = u"\"" + image + u"\"";
                auto child = std::make_shared<Child>();
                child->host_clock = uses_host_clock();
                PeFileBytes executable_source;
                try {executable_source=read_pe_snapshot(host,512U*1024U*1024U);}
                catch(const std::system_error&){fail(30);return false;}
                catch(const std::length_error&){fail(193);return false;}
                const auto& header=*executable_source;
                const auto count=header.size();
                const auto pe=count>=64?std::uint32_t{header[60]} | (std::uint32_t{header[61]} << 8) |
                                (std::uint32_t{header[62]} << 16) | (std::uint32_t{header[63]} << 24):0U;
                if (count < 94 || static_cast<std::uint64_t>(pe) + 94U > count ||
                    header[0] != 'M' || header[1] != 'Z')
                    fail(193);
                else {
                    child->gui = header[pe + 24 + 68] == 2;
                    // 這三個值是父引擎自己的 UI 控制通道，並非 Windows 環境區塊。
                    // 其他宿主選項照常承襲，避免改變共用效能／圖形設定。
                    std::vector<char *> environment;
                    for (auto item = environ; item && *item; ++item) {
                        const std::string_view value(*item);
                        if (value.starts_with("X64ARM_GUI_AUTOMATION=") ||
                            value.starts_with("X64ARM_GUI_AUTOMATION_FD=") || value.starts_with("X64ARM_GUI_STATE=") ||
                            value.starts_with("X64ARM_GUI_CHILD_AUTOMATION_DIR="))
                            continue;
                        environment.push_back(*item);
                    }
                    // 測試可為每個子程序明確提供獨立腳本；不重播父程序的控制通道。
                    std::vector<std::string> automation_environment;
                    if (const char *directory_name = std::getenv("X64ARM_GUI_CHILD_AUTOMATION_DIR");
                        directory_name && *directory_name) {
                        const auto child_directory =
                            std::filesystem::path(directory_name) / std::to_string(processes_->children.size());
                        std::error_code probe_error;
                        if (std::filesystem::is_regular_file(child_directory / "actions.json", probe_error)) {
                            automation_environment.push_back("X64ARM_GUI_AUTOMATION=" +
                                                             (child_directory / "actions.json").string());
                            automation_environment.push_back("X64ARM_GUI_STATE=" +
                                                             (child_directory / "state.json").string());
                            automation_environment.push_back("X64ARM_GUI_CHILD_AUTOMATION_DIR=" +
                                                             (child_directory / "children").string());
                            for (auto &value : automation_environment)
                                environment.push_back(value.data());
                        }
                    }
                    environment.push_back(nullptr);
                    int pipes[2]{};
                    if (::pipe(pipes))
                        fail(8);
                    else {
                        child->pipe = pipes[0];
                        watch_descriptor(child->pipe); // 子程序回報與結束（EOF）由監看執行緒喚醒，父程序不必輪詢。
                        (void)::fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
                        (void)::fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
                        (void)::fcntl(pipes[0], F_SETFL, O_NONBLOCK);
                        auto pattern = (std::filesystem::temp_directory_path() / "x64arm-child-XXXXXX").string();
                        std::vector<char> name(pattern.begin(), pattern.end());
                        name.push_back(0);
                        const auto diagnostic_fd = ::mkstemp(name.data());
                        if (diagnostic_fd < 0) {
                            ::close(pipes[1]);
                            fail(8);
                        } else {
                            ::close(diagnostic_fd);
                            child->diagnostics = name.data();
                            const auto &settings = processes_->options;
                            struct StartupDescriptor {int fd{-1};~StartupDescriptor(){if(fd>=0)::close(fd);}} passed_startup;
                            if(!startup_data.reserved2.empty()){
                                passed_startup.fd=create_process_startup_descriptor(startup_data,static_cast<int>(200+inherited.size()));
                                if(passed_startup.fd<0){::close(pipes[1]);fail(8);return false;}
                            }
                            std::vector<std::string> values{settings.engine.string(),
                                                            "run-pe",
                                                            host.string(),
                                                            "--backend",
                                                            settings.jit ? "jit" : "interpreter",
                                                            "--gui",
                                                            settings.native_gui ? "native" : "headless",
                                                            "--clock",
                                                            uses_host_clock() ? "host" : "virtual",
                                                            "--max-instructions",
                                                            std::to_string(settings.max_instructions),
                                                            "--memory-limit-mib",
                                                            std::to_string(settings.memory_limit_mib),
                                                            "--gpu-memory-mib",
                                                            std::to_string(settings.gpu_memory_mib),
                                                            "--unresolved-imports",
                                                            settings.trap_unresolved ? "trap" : "reject",
                                                            "--fs-mode",
                                                            settings.writable_files ? "read-write" : "read-only",
                                                            "--command-line",
                                                            utf16_to_utf8(command),
                                                            "--mapping-session",
                                                            mapping_session(),
                                                            "--process-result-fd",
                                                            "198",
                                                            "--process-directory",
                                                            utf16_to_utf8(current_directory),
                                                            "--diagnostics-file",
                                                            child->diagnostics.string()};
                            if (memory.load(GuestAddress{arg(8) + (x86_ ? 44U : 60U)}, 32) & 1U) {
                                values.push_back("--process-startup-show");
                                values.push_back(
                                    std::to_string(memory.load(GuestAddress{arg(8) + (x86_ ? 48U : 64U)}, 16)));
                            }
                            if(passed_startup.fd>=0){values.push_back("--process-startup-data-fd");values.push_back("193");}
                            if(flags&8U){values.push_back("--process-detached");values.push_back("1");}
                            if(!(flags&0x18U) && console_identity_){values.push_back("--process-console-id");values.push_back(std::to_string(console_identity_));}
                            values.push_back("--process-console-group");values.push_back(std::to_string((flags&0x200U) && !(flags&0x10U)?UINT32_MAX:console_group_));
                            if(runtime_.ignore_ctrl_c || ((flags&0x200U) && !(flags&0x10U))){values.push_back("--process-ignore-ctrl-c");values.push_back("1");}
                            // 客體進入點先暫停；完成身分紀錄與啟動握手後才依 CREATE_SUSPENDED 決定放行。
                            values.push_back("--process-startup-suspended");
                            values.push_back("1");
                            if (memory.load(GuestAddress{arg(8) + (x86_ ? 44U : 60U)}, 32) & 0x100U) {
                                std::string handles;
                                for (unsigned i = 0; i < 3; ++i) {
                                    if (i)
                                        handles += ',';
                                    handles += std::to_string(
                                        memory.load(GuestAddress{arg(8) + (x86_ ? 56U : 80U) + i * (x86_ ? 4U : 8U)},
                                                    x86_ ? 32 : 64));
                                }
                                values.push_back("--process-startup-handles");
                                values.push_back(std::move(handles));
                            }
                            const bool child_x64 = header[pe + 4] == 0x64 && header[pe + 5] == 0x86;
                            const auto child_cores = std::min(settings.processor_count, child_x64 ? 64U : 32U);
                            const auto child_system_mask =
                                child_cores == 64 ? UINT64_MAX : (UINT64_C(1) << child_cores) - 1;
                            const auto child_affinity = runtime_.process_affinity & child_system_mask;
                            values.push_back("--cpu-count");
                            values.push_back(std::to_string(child_cores));
                            // 保留相同架構的核心與 affinity；跨架構時縮至目標遮罩可表示的核心。
                            values.push_back("--process-affinity");
                            values.push_back(std::to_string(child_affinity ? child_affinity : child_system_mask));
                            if (!settings.filesystem_root.empty()) {
                                values.push_back("--fs-root");
                                values.push_back(settings.filesystem_root.string());
                            }
                            if (!settings.registry_file.empty()) {
                                values.push_back("--registry-file");
                                values.push_back(settings.registry_file.string());
                            }
                            if (!settings.c_drive.empty()) {
                                values.push_back("--c-drive");
                                values.push_back(settings.c_drive.string());
                            }
                            if (!settings.certificate_roots_file.empty()) {
                                values.push_back("--certificate-roots-file");
                                values.push_back(settings.certificate_roots_file);
                            }
                            for (const auto &[letter, path] : settings.drive_mappings) {
                                values.push_back("--drive");
                                values.push_back(std::string(1, letter) + "=" + path.string());
                            }
                            if (!settings.install_root.empty()) {
                                values.push_back("--install-root");
                                values.push_back(settings.install_root.string());
                            }
                            // 先複製到高位描述符，避免 dup2 的目標覆寫其他來源。
                            std::vector<int> passed;
                            std::string encoded;
                            int transfer_error = 0;
                            for (const auto &item : inherited) {
                                const int source =
                                    ::fcntl(item.descriptor, F_DUPFD_CLOEXEC, static_cast<int>(200 + inherited.size()));
                                if (source < 0) {
                                    transfer_error = 8;
                                    break;
                                }
                                passed.push_back(source);
                                const auto target = 199 + passed.size();
                                if (!encoded.empty())
                                    encoded += ';';
                                encoded += std::to_string(item.handle) + "," + std::to_string(item.type) + "," +
                                           std::to_string(item.access) + "," + std::to_string(item.flags) + "," +
                                           std::to_string(item.mode) + "," + std::to_string(item.pipe_mode) + "," +
                                           std::to_string(item.auxiliary) + "," + std::to_string(target);
                                child->inherited.push_back(item.lifetime);
                            }
                            if (transfer_error) {
                                for (auto fd : passed)
                                    ::close(fd);
                                ::close(pipes[1]);
                                fail(static_cast<std::uint32_t>(transfer_error));
                                return false;
                            }
                            if (!encoded.empty()) {
                                values.push_back("--inherit-handles");
                                values.push_back(std::move(encoded));
                            }
                            int passed_job = -1;
                            if (job_descriptor >= 0) {
                                passed_job =
                                    ::fcntl(job_descriptor, F_DUPFD_CLOEXEC, static_cast<int>(200 + inherited.size()));
                                if (passed_job < 0) {
                                    for (auto fd : passed)
                                        ::close(fd);
                                    ::close(pipes[1]);
                                    fail(8);
                                    return false;
                                }
                                values.push_back("--process-job-fd");
                                values.push_back("197");
                            }
                            auto token_pattern =
                                (std::filesystem::temp_directory_path() / "x64arm-token-XXXXXX").string();
                            std::vector<char> token_name(token_pattern.begin(), token_pattern.end());
                            token_name.push_back(0);
                            const auto temporary_token = ::mkstemp(token_name.data());
                            int passed_token = -1;
                            bool token_written = false;
                            if (temporary_token >= 0) {
                                (void)::unlink(token_name.data());
                                std::size_t offset = 0;
                                while (offset < token_bytes.size()) {
                                    const auto written = ::write(temporary_token, token_bytes.data() + offset,
                                                                 token_bytes.size() - offset);
                                    if (written < 0 && errno == EINTR)
                                        continue;
                                    if (written <= 0)
                                        break;
                                    offset += static_cast<std::size_t>(written);
                                }
                                token_written = offset == token_bytes.size();
                                if (token_written)
                                    passed_token = ::fcntl(temporary_token, F_DUPFD_CLOEXEC,
                                                           static_cast<int>(200 + inherited.size()));
                                ::close(temporary_token);
                            }
                            if (passed_token < 0 || !token_written) {
                                if (passed_token >= 0)
                                    ::close(passed_token);
                                if (passed_job >= 0)
                                    ::close(passed_job);
                                for (auto fd : passed)
                                    ::close(fd);
                                ::close(pipes[1]);
                                fail(8);
                                return false;
                            }
                            auto environment_pattern =
                                (std::filesystem::temp_directory_path() / "x64arm-environment-XXXXXX").string();
                            std::vector<char> environment_name(environment_pattern.begin(), environment_pattern.end());
                            environment_name.push_back(0);
                            const auto temporary_environment = ::mkstemp(environment_name.data());
                            int passed_environment = -1;
                            if (temporary_environment >= 0) {
                                (void)::unlink(environment_name.data());
                                std::size_t offset = 0;
                                while (offset < environment_bytes.size()) {
                                    const auto written =
                                        ::write(temporary_environment, environment_bytes.data() + offset,
                                                environment_bytes.size() - offset);
                                    if (written < 0 && errno == EINTR)
                                        continue;
                                    if (written <= 0)
                                        break;
                                    offset += static_cast<std::size_t>(written);
                                }
                                if (offset == environment_bytes.size())
                                    passed_environment = ::fcntl(temporary_environment, F_DUPFD_CLOEXEC,
                                                                 static_cast<int>(200 + inherited.size()));
                                ::close(temporary_environment);
                            }
                            if (passed_environment < 0) {
                                ::close(passed_token);
                                if (passed_job >= 0)
                                    ::close(passed_job);
                                for (auto fd : passed)
                                    ::close(fd);
                                ::close(pipes[1]);
                                fail(8);
                                return false;
                            }
                            values.push_back("--process-environment-fd");
                            values.push_back("195");
                            values.push_back("--process-token-fd");
                            values.push_back("196");
                            // macOS 的 adddup2(fd,fd) 不會清除 FD_CLOEXEC；結果管道也先移到目標範圍之外。
                            const auto passed_result =
                                ::fcntl(pipes[1], F_DUPFD_CLOEXEC, static_cast<int>(200 + inherited.size()));
                            if (passed_result < 0) {
                                for (auto fd : passed)
                                    ::close(fd);
                                if (passed_job >= 0)
                                    ::close(passed_job);
                                ::close(passed_token);
                                ::close(passed_environment);
                                ::close(pipes[1]);
                                fail(8);
                                return false;
                            }
                            // 子程序載入 PE 之前也必須保有命名空間；父程序可立即退出。
                            const auto passed_session = ::fcntl(mapping_session_descriptor(), F_DUPFD_CLOEXEC,
                                                                static_cast<int>(200 + inherited.size()));
                            if (passed_session < 0) {
                                ::close(passed_result);
                                for (auto fd : passed)
                                    ::close(fd);
                                if (passed_job >= 0)
                                    ::close(passed_job);
                                ::close(passed_token);
                                ::close(passed_environment);
                                ::close(pipes[1]);
                                fail(8);
                                return false;
                            }
                            values.push_back("--mapping-session-fd");
                            values.push_back("194");
                            std::vector<char *> argv;
                            for (auto &item : values)
                                argv.push_back(item.data());
                            argv.push_back(nullptr);
                            posix_spawn_file_actions_t actions;
                            ::posix_spawn_file_actions_init(&actions);
                            if(passed_startup.fd>=0){::posix_spawn_file_actions_adddup2(&actions,passed_startup.fd,193);::posix_spawn_file_actions_addclose(&actions,passed_startup.fd);}
                            ::posix_spawn_file_actions_adddup2(&actions, passed_result, 198);
                            ::posix_spawn_file_actions_addclose(&actions, passed_result);
                            if (pipes[0] != 198)
                                ::posix_spawn_file_actions_addclose(&actions, pipes[0]);
                            if (pipes[1] != 198)
                                ::posix_spawn_file_actions_addclose(&actions, pipes[1]);
                            for (std::size_t i = 0; i < passed.size(); ++i) {
                                ::posix_spawn_file_actions_adddup2(&actions, passed[i], static_cast<int>(200 + i));
                                ::posix_spawn_file_actions_addclose(&actions, passed[i]);
                            }
                            ::posix_spawn_file_actions_adddup2(&actions, passed_environment, 195);
                            ::posix_spawn_file_actions_addclose(&actions, passed_environment);
                            ::posix_spawn_file_actions_adddup2(&actions, passed_session, 194);
                            ::posix_spawn_file_actions_addclose(&actions, passed_session);
                            ::posix_spawn_file_actions_adddup2(&actions, passed_token, 196);
                            ::posix_spawn_file_actions_addclose(&actions, passed_token);
                            if (passed_job >= 0) {
                                ::posix_spawn_file_actions_adddup2(&actions, passed_job, 197);
                                ::posix_spawn_file_actions_addclose(&actions, passed_job);
                            }
                            // 子引擎與本程序是同一個已授權引擎。本程序已在 4 KiB 頁面時直接以相同頁面大小啟動子程序，
                            // 省去子程序啟動後再以 4 KiB 屬性重新 exec 一次（每個子程序約 15–25 ms）。
                            // 屬性不可用或帶屬性的啟動失敗時，退回原本的啟動方式，由子程序自行切換。
                            auto spawned = -1;
                            launch.event("spawn_begin");
#if defined(__APPLE__) && defined(__aarch64__)
                            if (::getpagesize() == 4096) {
                                using SetPages = int (*)(posix_spawnattr_t *);
                                static const auto set_pages = reinterpret_cast<SetPages>(
                                    ::dlsym(RTLD_DEFAULT, "posix_spawnattr_set_4k_page_size_np"));
                                posix_spawnattr_t attributes;
                                if (set_pages && ::posix_spawnattr_init(&attributes) == 0) {
                                    if (set_pages(&attributes) == 0)
                                        spawned = ::posix_spawn(&child->pid, settings.engine.c_str(), &actions, &attributes,
                                                                argv.data(), environment.data());
                                    ::posix_spawnattr_destroy(&attributes);
                                }
                            }
#endif
                            if (spawned)
                                spawned = ::posix_spawn(&child->pid, settings.engine.c_str(), &actions, nullptr,
                                                        argv.data(), environment.data());
                            if (!spawned)
                                launch.peer(static_cast<std::uint32_t>(child->pid));
                            launch.event("spawn_end");
                            if (trace_process_protocol())
                                std::fprintf(stderr,
                                             "process_protocol_spawn parent=%d child=%d result_original=%d "
                                             "result_source=%d target=198 error=%d diagnostics=%s mapping_session=%s\n",
                                             ::getpid(), child->pid, pipes[1], passed_result, spawned,
                                             child->diagnostics.c_str(), mapping_session().c_str());
                            ::posix_spawn_file_actions_destroy(&actions);
                            ::close(passed_session);
                            ::close(passed_result);
                            ::close(pipes[1]);
                            for (auto fd : passed)
                                ::close(fd);
                            if (passed_job >= 0)
                                ::close(passed_job);
                            ::close(passed_token);
                            ::close(passed_environment);
                            if (spawned) {
                                child->pid = -1;
                                fail(spawned == ENOENT ? 2 : 5);
                            } else {
                                auto record = ProcessRecord::open(mapping_session(),
                                                                  static_cast<std::uint32_t>(child->pid), true);
                                if (!record)
                                    throw HostCallFault("無法保存子程序身分");
                                {
                                    SharedEventLock lock(record->descriptor);
                                    if (shared_object_security(record->descriptor).empty())
                                        set_shared_object_security(record->descriptor, process_security.bytes);
                                    if (shared_object_security(record->descriptor, true).empty())
                                        set_shared_object_security(record->descriptor, thread_security.bytes, true);
                                }
                                launch.event("startup_wait_begin");
                                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                                while (!child->startup_ready && !child->startup_error && !child->job_error &&
                                       !child->exited && std::chrono::steady_clock::now() < deadline) {
                                    child->poll();
                                    poll_handle_transfers(memory, true);
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                                launch.event("startup_wait_end");
                                if (!child->startup_ready || (job_descriptor >= 0 && !child->job_started) ||
                                    child->job_error) {
                                    fail(child->startup_error ? child->startup_error
                                         : child->job_error   ? child->job_error
                                                              : 1460U);
                                    return false;
                                }
                                if (!(flags & 4U)) {
                                    std::uint32_t suspended = 0;
                                    launch.event("resume_begin");
                                    const auto resume_error = remote_process_thread_control(
                                        memory, static_cast<std::uint32_t>(child->pid), 3, suspended);
                                    launch.event("resume_end");
                                    if (resume_error) {
                                        fail(resume_error);
                                        return false;
                                    }
                                }
                                const auto process = runtime_.allocate_wait_handle(), thread = runtime_.allocate_wait_handle();
                                const auto birth =
                                    process_debugger::query(static_cast<std::uint32_t>(child->pid)).identity;
                                processes_->children.push_back(child);
                                processes_->handles.emplace(process, Handle{child, false, 0x1fffffU, 0, record, birth});
                                processes_->handles.emplace(
                                    thread, Handle{child, true, 0x1fffffU, 0, std::move(record), birth});
                                if (process_security.inherit)
                                    runtime_.handle_flags[process] = 1;
                                if (thread_security.inherit)
                                    runtime_.handle_flags[thread] = 1;
                                memory.store(GuestAddress{arg(9)}, x86_ ? 32 : 64, process);
                                memory.store(GuestAddress{arg(9) + (x86_ ? 4U : 8U)}, x86_ ? 32 : 64, thread);
                                memory.store(GuestAddress{arg(9) + (x86_ ? 8U : 16U)}, 32,
                                             static_cast<std::uint32_t>(child->pid));
                                memory.store(GuestAddress{arg(9) + (x86_ ? 12U : 20U)}, 32,
                                             static_cast<std::uint32_t>(child->pid));
                                child->independent = true;
                                result = 1;
                            }
                        }
                    }
                }
            }
        }
    }
    launch.result(result ? 0U : last_error_);
    return result != 0;
#endif
}
std::uint32_t WindowsServices::local_process_priority_operation(std::uint32_t operation, std::uint32_t &value) {
    if (operation == 0) {
        value = runtime_.process_priority_class;
        return 0;
    }
    if (operation == 2) {
        value = runtime_.priority_boost_disabled ? 1U : 0U;
        return 0;
    }
    if (operation == 3) {
        runtime_.priority_boost_disabled = value != 0;
        if (value)
            for (auto &[id, thread] : threads_.items) {
                (void)id;
                thread.dynamic_priority_boost = 0;
            }
        return 0;
    }
    if (operation != 1)
        return 87;
    auto priority = runtime_.process_priority_class;
    bool background = runtime_.process_background;
    if (value == 0x100000) {
        if (background)
            return 402;
        background = true;
    } else if (value == 0x200000) {
        if (!background)
            return 403;
        background = false;
    } else if (value == 0x20 || value == 0x40 || value == 0x80 || value == 0x4000 || value == 0x8000)
        priority = value;
    else if (value == 0x100)
        return 5; // 尚未提供客體即時排程權限，不冒稱即時成功。
    else
        return 87;
#if defined(__APPLE__)
    if (runtime_.network.external_io) {
        const auto qos = background || priority == 0x40 ? QOS_CLASS_BACKGROUND
                         : priority == 0x4000           ? QOS_CLASS_UTILITY
                         : priority == 0x80             ? QOS_CLASS_USER_INTERACTIVE
                         : priority == 0x8000           ? QOS_CLASS_USER_INITIATED
                                                        : QOS_CLASS_DEFAULT;
        HostExecutionScope probe;
        const auto policy=qos==QOS_CLASS_BACKGROUND?ExecutionPolicy::Efficient:
            qos==QOS_CLASS_UTILITY?ExecutionPolicy::Utility:
            qos==QOS_CLASS_USER_INTERACTIVE?ExecutionPolicy::Interactive:
            qos==QOS_CLASS_USER_INITIATED?ExecutionPolicy::Responsive:ExecutionPolicy::Automatic;
        if (!probe.apply(policy))return 5;
        if (background != runtime_.process_background &&
            ::setpriority(PRIO_DARWIN_PROCESS, 0, background ? PRIO_DARWIN_BG : 0)) {
            return 5;
        }
    }
#elif !defined(_WIN32)
    if (runtime_.network.external_io)
        return 50;
#endif
    runtime_.process_priority_class = priority;
    runtime_.process_background = background;
    notify_concurrency_changed();
    return 0;
}
std::optional<std::uint32_t> WindowsServices::invoke_process(GuestState &state, GuestMemory &memory,
                                                             const WindowsServiceDescriptor &service) {
    if(service.symbol.starts_with("Pss")){
#include "windows_pss_snapshot.inc"
    }
    if(service.family=="crt"){
#include "windows_crt_process.inc"
    }
    if (service.symbol == "DebugActiveProcess" || service.symbol == "DebugActiveProcessStop" ||
        service.symbol == "WaitForDebugEvent" || service.symbol == "WaitForDebugEventEx" ||
        service.symbol == "ContinueDebugEvent")
        return invoke_debugger(state, memory, service);
    if (service.symbol == "GetProcessMitigationPolicy" || service.symbol == "SetProcessMitigationPolicy" ||
        service.symbol == "SetProcessInformation" || service.symbol == "GetProcessInformation" ||
        service.symbol == "GetProcessDEPPolicy" || service.symbol == "SetProcessDEPPolicy" ||
        service.symbol == "GetSystemDEPPolicy")
        return invoke_process_policy(state, memory, service);
    for (const auto name :
         {"CreateJobObjectA", "CreateJobObjectW", "OpenJobObjectA", "OpenJobObjectW", "SetInformationJobObject",
          "QueryInformationJobObject", "AssignProcessToJobObject", "TerminateJobObject", "IsProcessInJob"})
        if (service.symbol == name)
            return invoke_job(state, memory, service);
    const WindowsCall call(state, memory, service);
    const auto symbol = service.symbol;
    const bool as_user = symbol.starts_with("CreateProcessAsUser");
    const auto arg = [&](unsigned index) { return call(index + (as_user ? 1U : 0U)); };
    std::uint64_t result = 0;
    const auto fail = [&](std::uint32_t error) {
        last_error_ = error;
        result = 0;
    };
    if (symbol == "GetProcessIoCounters") {
        if (!arg(1))
            fail(87);
        else
            try {
                memory.check(GuestAddress{arg(1)}, 48, Access::Write);
                process_io::Counters counters;
                const auto error = query_process_io(arg(0), counters);
                if (error)
                    fail(error);
                else {
                    for (unsigned i = 0; i < 6; ++i)
                        memory.store(GuestAddress{arg(1) + 8ULL * i}, 64, counters.values[i]);
                    result = 1;
                }
            } catch (const MemoryFault &) {
                fail(998);
            }
        call.return_to(state, result);
        return std::nullopt;
    }
    if (symbol == "CheckRemoteDebuggerPresent") {
        const auto pid = process_handle_pid(arg(0), 0x400);
        if (process_object_type(arg(0)) != u"Process")
            fail(6);
        else if (!pid)
            fail(5);
        else if (!arg(1))
            fail(87);
        else
            try {
                memory.check(GuestAddress{arg(1)}, 4, Access::Write);
                process_debugger::Result debug;
                if (pid == process_id())
                    debug = debugger_attached()
                                ? process_debugger::Result{0, true, 0, false}
                                : process_debugger::self(memory, peb_base_, runtime_.network.external_io);
                else {
                    if (!runtime_.network.external_io)
                        throw HostCallFault("比較模式不查詢外部程序的偵錯狀態");
#if !defined(_WIN32)
                    const auto handle = processes_->handles.find(arg(0));
                    auto &object = handle->second;
                    if (object.exited || (object.child && object.child->exited) ||
                        (object.debug_life && object.debug_life->exited))
                        debug = {0, false, object.identity, true};
                    else {
                        debug = process_debugger::query(pid);
                        // handle 保有原程序身分；死亡／PID 重用不會變成另一個程序的旗標。
                        if (debug.error == 87 || (!debug.error && object.identity && debug.identity != object.identity))
                            debug = {0, false, object.identity, true};
                        std::error_code endpoint_error;
                        const bool guest_endpoint =
                            debug_memory_ && std::filesystem::exists(std::filesystem::path(mapping_session()) /
                                                                         ("debug-" + std::to_string(pid)),
                                                                     endpoint_error);
                        // P_TRACED 已是肯定答案；原生 debugger 可能停止整個 host，不能等待它服務 IPC。
                        if (!debug.error && !debug.exited && !debug.present && guest_endpoint) {
                            std::vector<std::uint8_t> reply;
                            debug.error = transfer_debug_memory(memory, pid, object.identity, 8, 0, 0, reply);
                            if (!debug.error) {
                                if (reply.size() != 8 || reply[0] > 1 ||
                                    std::any_of(reply.begin() + 1, reply.end(), [](auto byte) { return byte != 0; }))
                                    debug.error = 13;
                                else
                                    debug.present = reply[0] != 0;
                            }
                        }
                    }
                    if (debug.exited)
                        object.exited = true;
#else
                    debug = process_debugger::query(pid);
#endif
                }
                if (debug.error)
                    fail(debug.error);
                else {
                    memory.store(GuestAddress{arg(1)}, 32, debug.present);
                    result = 1;
                }
            } catch (const MemoryFault &) {
                fail(998);
            }
        call.return_to(state, result);
        return std::nullopt;
    }
#if defined(_WIN32)
    fail(50);
#else
    if (symbol == "GetPriorityClass" || symbol == "SetPriorityClass" || symbol == "GetProcessPriorityBoost" ||
        symbol == "SetProcessPriorityBoost") {
        const bool setting = symbol.starts_with("Set"), boost = symbol.ends_with("PriorityBoost");
        auto pid = process_handle_pid(arg(0), setting ? 0x200U : 0x400U);
        if (!pid && !setting)
            pid = process_handle_pid(arg(0), 0x1000);
        if (process_object_type(arg(0)) != u"Process")
            fail(6);
        else if (!pid)
            fail(5);
        else if (!setting && boost && !arg(1))
            fail(87);
        else if (setting && !boost && (arg(1) == 0x100000 || arg(1) == 0x200000) && !current_process_handle(arg(0)))
            fail(87);
        else {
            if (!setting && boost)
                memory.check(GuestAddress{arg(1)}, 4, Access::Write);
            std::uint32_t value = static_cast<std::uint32_t>(arg(1));
            const auto error =
                process_priority_operation(memory, pid, boost ? (setting ? 3U : 2U) : (setting ? 1U : 0U), value);
            if (error)
                fail(error);
            else if (setting)
                result = 1;
            else if (boost) {
                memory.store(GuestAddress{arg(1)}, 32, value);
                result = 1;
            } else
                result = value;
        }
    } else if (symbol == "GetProcessHandleCount") {
        auto pid = process_handle_pid(arg(0), 0x400);
        if (!pid)
            pid = process_handle_pid(arg(0), 0x1000);
        if (!pid)
            fail(process_object_type(arg(0)) == u"Process" ? 5U : 6U);
        else if (!arg(1))
            fail(87);
        else {
            memory.check(GuestAddress{arg(1)}, 4, Access::Write);
            std::uint32_t count = 0;
            const auto error = query_process_handle_count(memory, pid, count);
            if (error)
                fail(error);
            else {
                memory.store(GuestAddress{arg(1)}, 32, count);
                result = 1;
            }
        }
    } else if (symbol == "GetProcessAffinityMask" || symbol == "SetProcessAffinityMask") {
        const bool setting = symbol.starts_with("Set");
        if (process_object_type(arg(0)) != u"Process")
            fail(6);
        else if (!(setting ? process_handle_pid(arg(0), 0x200) != 0
                           : (process_handle_pid(arg(0), 0x400) != 0 || process_handle_pid(arg(0), 0x1000) != 0)))
            fail(5);
        else if (!current_process_handle(arg(0)))
            fail(50);
        else if (setting) {
            if (!set_process_affinity(state, arg(1)))
                fail(87);
            else
                result = 1;
        } else if (!arg(1) || !arg(2))
            fail(87);
        else {
            const unsigned unit = x86_ ? 4 : 8;
            const std::uint8_t width = x86_ ? 32 : 64;
            memory.check(GuestAddress{arg(1)}, unit, Access::Write);
            memory.check(GuestAddress{arg(2)}, unit, Access::Write);
            memory.store(GuestAddress{arg(1)}, width, runtime_.process_affinity);
            memory.store(GuestAddress{arg(2)}, width,
                         runtime_.processor_count == 64 ? UINT64_MAX : (UINT64_C(1) << runtime_.processor_count) - 1);
            result = 1;
        }
    } else if (symbol == "SetProcessShutdownParameters") {
        const auto level = static_cast<std::uint32_t>(arg(0)), flags = static_cast<std::uint32_t>(arg(1));
        if (level > 0x4ff || (flags & ~1U))
            fail(87);
        else {
            runtime_.shutdown_level = level;
            runtime_.shutdown_flags = flags;
            result = 1;
        }
    } else if (symbol == "GetProcessShutdownParameters") {
        if (!arg(0) || !arg(1))
            fail(87);
        else {
            memory.check(GuestAddress{arg(0)}, 4, Access::Write);
            memory.check(GuestAddress{arg(1)}, 4, Access::Write);
            memory.store(GuestAddress{arg(0)}, 32, runtime_.shutdown_level);
            memory.store(GuestAddress{arg(1)}, 32, runtime_.shutdown_flags);
            result = 1;
        }
    } else if (symbol == "ProcessIdToSessionId") {
        const auto pid = static_cast<std::uint32_t>(arg(0));
        if (!arg(1) || !pid)
            fail(87);
        else {
            bool known = pid == process_id();
            if (!known && processes_)
                for (const auto &child : processes_->children) {
                    child->poll();
                    if (static_cast<std::uint32_t>(child->pid) == pid && !child->exited) {
                        known = true;
                        break;
                    }
                }
            if (!known)
                known = remote_process_available(pid);
            if (!known)
                fail(87);
            else {
                memory.store(GuestAddress{arg(1)}, 32, 1);
                result = 1;
            }
        }
    } else if (symbol == "InitializeProcThreadAttributeList") {
        const auto count = static_cast<std::uint32_t>(arg(1));
        const unsigned width = x86_ ? 4 : 8;
        if (!arg(3) || arg(2) || !count || count > 64)
            fail(87);
        else {
            const auto capacity = memory.load(GuestAddress{arg(3)}, static_cast<std::uint8_t>(width * 8)),
                       needed = 16 + std::uint64_t{count} * 3 * width;
            memory.store(GuestAddress{arg(3)}, static_cast<std::uint8_t>(width * 8), needed);
            if (!arg(0) || capacity < needed)
                fail(122);
            else {
                memory.write(GuestAddress{arg(0)}, std::vector<std::uint8_t>(static_cast<std::size_t>(needed)));
                memory.store(GuestAddress{arg(0)}, 32, 0x54544158);
                memory.store(GuestAddress{arg(0) + 4}, 32, count);
                result = 1;
            }
        }
    } else if (symbol == "DeleteProcThreadAttributeList") {
        if (arg(0))
            memory.store(GuestAddress{arg(0)}, 32, 0);
    } else if (symbol == "UpdateProcThreadAttribute") {
        const unsigned width = x86_ ? 4 : 8;
        const auto attribute = arg(2), size = arg(4);
        if (!arg(0) || arg(1) || !arg(3) || arg(5) || arg(6))
            fail(87);
        else if (memory.load(GuestAddress{arg(0)}, 32) != 0x54544158)
            fail(87);
        else {
            const auto count = memory.load(GuestAddress{arg(0) + 4}, 32),
                       used = memory.load(GuestAddress{arg(0) + 8}, 32);
            if (!count || count > 64 || used > count)
                fail(87);
            else if (attribute != 0x20002 && attribute != 0x2000d && attribute != 0x2000e)
                fail(50);
            else if (attribute == 0x2000e ? size != 4 : (!size || size % width || size / width > 4096))
                fail(24);
            else if (attribute == 0x2000e && (memory.load(GuestAddress{arg(3)}, 32) & ~7U))
                fail(87);
            else if (attribute == 0x2000e && (memory.load(GuestAddress{arg(3)}, 32) & 4U))
                fail(50);
            else {
                bool duplicate = false;
                for (std::uint64_t i = 0; i < used; ++i)
                    duplicate |= memory.load(GuestAddress{arg(0) + 16 + i * 3 * width},
                                             static_cast<std::uint8_t>(width * 8)) == attribute;
                if (duplicate)
                    fail(698);
                else if (used == count)
                    fail(122);
                else {
                    memory.check(GuestAddress{arg(3)}, static_cast<std::size_t>(size), Access::Read);
                    const auto entry = arg(0) + 16 + used * 3 * width;
                    memory.check(GuestAddress{entry}, 3 * width, Access::Write);
                    memory.store(GuestAddress{entry}, static_cast<std::uint8_t>(width * 8), attribute);
                    memory.store(GuestAddress{entry + width}, static_cast<std::uint8_t>(width * 8), arg(3));
                    memory.store(GuestAddress{entry + 2 * width}, static_cast<std::uint8_t>(width * 8), size);
                    memory.store(GuestAddress{arg(0) + 8}, 32, used + 1);
                    result = 1;
                }
            }
        }
    } else if (symbol == "DuplicateHandle") {
        const bool pseudo = arg(1) == (x86_ ? UINT32_MAX : UINT64_MAX);
        if (pseudo && !processes_)
            processes_ = std::make_shared<ProcessState>();
        const auto source = processes_ ? processes_->handles.find(arg(1)) : std::map<std::uint64_t, Handle>::iterator{};
        if (!processes_ || (!pseudo && source == processes_->handles.end()) || !current_process_handle(arg(0), 0x40) ||
            !current_process_handle(arg(2), 0x40))
            fail(6);
        else if (!arg(3) || (arg(6) & ~3U))
            fail(87);
        else if (!(arg(6) & 2) && (arg(4) & ~(pseudo ? kernel_pseudo_access(false) : source->second.access)))
            fail(5);
        else if ((arg(6) & 1) && (runtime_.handle_flags[arg(1)] & 2U))
            fail(5);
        else {
            memory.check(GuestAddress{arg(3)}, x86_ ? 4 : 8, Access::Write);
            const auto handle = runtime_.allocate_wait_handle();
            auto copy = pseudo ? Handle{{}, false, kernel_pseudo_access(false), 0, processes_->record} : source->second;
            if (!(arg(6) & 2))
                copy.access = static_cast<std::uint32_t>(arg(4));
            processes_->handles.emplace(handle, std::move(copy));
            runtime_.handle_flags[handle] = arg(5) ? 1 : 0;
            memory.store(GuestAddress{arg(3)}, x86_ ? 32 : 64, handle);
            result = 1;
            if ((arg(6) & 1) && !pseudo) {
                processes_->handles.erase(source);
                runtime_.handle_flags.erase(arg(1));
            }
        }
    } else if (symbol == "OpenProcess") {
        try {
            const auto pid = static_cast<std::uint32_t>(arg(2)), rights = static_cast<std::uint32_t>(arg(0));
            if (!pid)
                fail(87);
            else if (rights & 0x1000000U)
                fail(1314);
            else if (rights & ~0xf21fffffU)
                fail(50);
            else if (processes_ && processes_->handles.size() >= 1024)
                fail(8);
            else if (!processes_) {
                if (pid != process_id())
                    fail(5);
                else {
                    const auto granted = kernel_security_access(runtime_.process_security_descriptor, rights, false);
                    processes_ = std::make_shared<ProcessState>();
                    result = runtime_.allocate_wait_handle();
                    processes_->handles.emplace(result, Handle{{}, false, granted});
                    runtime_.handle_flags[result] = arg(1) ? 1 : 0;
                }
            } else {
                const auto child =
                    std::find_if(processes_->children.begin(), processes_->children.end(),
                                 [&](const auto &item) { return static_cast<std::uint32_t>(item->pid) == pid; });
                if (pid != process_id() && child == processes_->children.end() && !remote_process_available(pid))
                    fail(5);
                else {
                    result = runtime_.allocate_wait_handle();
                    const auto birth = pid == process_id() ? 0 : process_debugger::query(pid).identity;
                    auto record =
                        pid == process_id() ? processes_->record : ProcessRecord::open(mapping_session(), pid, false);
                    std::uint32_t granted{};
                    if (record) {
                        SharedEventLock lock(record->descriptor);
                        granted = kernel_security_access(shared_object_security(record->descriptor), rights, false);
                    } else if (pid == process_id())
                        granted = kernel_security_access(runtime_.process_security_descriptor, rights, false);
                    else
                        throw SecurityDescriptorError{1338};
                    processes_->handles.emplace(
                        result,
                        Handle{pid == process_id() || child == processes_->children.end() ? std::shared_ptr<Child>{}
                                                                                          : *child,
                               false, granted, pid != process_id() && child == processes_->children.end() ? pid : 0U,
                               std::move(record), birth});
                    runtime_.handle_flags[result] = arg(1) ? 1 : 0;
                }
            }
        } catch (const SecurityDescriptorError &error) {
            fail(error.code);
            result = 0;
        } catch (const std::bad_alloc &) {
            fail(8);
            result = 0;
        }
    } else if (symbol.starts_with("CreateProcess")) {
        std::array<std::uint64_t, 10> arguments{};
        for (unsigned i = 0; i < arguments.size(); ++i)
            arguments[i] = arg(i);
        result = create_guest_process(memory, arguments, symbol.ends_with('W'), as_user, as_user ? call(0) : 0);
    } else if ((symbol == "GetExitCodeProcess" || symbol == "GetProcessId") && current_process_handle(arg(0))) {
        if (!current_process_handle(arg(0), 0x400) && !current_process_handle(arg(0), 0x1000))
            fail(5);
        else if (symbol == "GetProcessId")
            result = process_id();
        else {
            memory.store(GuestAddress{arg(1)}, 32, 259);
            result = 1;
        }
    }
    // 強制終止不執行 FLS、TLS 或 DLL_PROCESS_DETACH；ExitProcess 才走有序退出。
    else if (symbol == "TerminateProcess" && current_process_handle(arg(0), 1)) {
        record_job_exit(static_cast<std::uint32_t>(arg(1)));
        return static_cast<std::uint32_t>(arg(1));
    } else {
        auto found = processes_ ? processes_->handles.find(arg(0)) : std::map<std::uint64_t, Handle>::iterator{};
        if (!processes_ || found == processes_->handles.end())
            fail(6);
        else {
            if(symbol=="GetThreadId" || symbol=="GetExitCodeThread"){
                auto& object=found->second;
                if(!object.thread)fail(6);
                else if(!(object.access&0x840U))fail(5);
                else{
                    resolve_thread_record(object,mapping_session());
                    if(object.record && object.record->data->object_kind){
                        if(symbol=="GetThreadId")result=object.record->thread_identifier();
                        else{
                            if(!arg(1))fail(87);
                            else if(!object.record->exited() && !object.record->alive())throw HostCallFault("遠端執行緒未回報 DWORD 結束狀態");
                            else{memory.store(GuestAddress{arg(1)},32,object.record->exited()?object.record->data->code:259U);result=1;}
                        }
                        call.return_to(state,result);return std::nullopt;
                    }
                }
                if(last_error_ && !result && (!object.thread || !(object.access&0x840U))){call.return_to(state,0);return std::nullopt;}
            }
            if (found->second.remote_pid) {
                const auto &record = found->second.record;
                if (symbol == "TerminateProcess") {
                    if (!(found->second.access & 1U))
                        fail(5);
                    else if (!record || !record->alive())
                        fail(5);
                    else if (::kill(static_cast<pid_t>(record->data->pid), SIGKILL))
                        fail(5);
                    else {
                        record->finish(static_cast<std::uint32_t>(arg(1)));
                        result = 1;
                    }
                } else if (!(found->second.access & (found->second.thread ? 0x840U : 0x1400U)))
                    fail(5);
                else if (symbol == (found->second.thread ? "GetThreadId" : "GetProcessId"))
                    result = found->second.thread && found->second.remote_tid ? found->second.remote_tid
                                                                              : found->second.remote_pid;
                else if (found->second.debug_life &&
                         symbol == (found->second.thread ? "GetExitCodeThread" : "GetExitCodeProcess")) {
                    memory.store(GuestAddress{arg(1)}, 32,
                                 found->second.debug_life->exited ? found->second.debug_life->code : 259);
                    result = 1;
                } else if (symbol == (found->second.thread ? "GetExitCodeThread" : "GetExitCodeProcess") && record) {
                    if (!record->exited() && !record->alive())
                        throw HostCallFault("遠端程序未回報 DWORD 結束狀態");
                    memory.store(GuestAddress{arg(1)}, 32, record->exited() ? record->data->code : 259U);
                    result = 1;
                } else
                    fail(50);
                call.return_to(state, result);
                return std::nullopt;
            }
            if (!found->second.child) {
                fail(5);
                call.return_to(state, result);
                return std::nullopt;
            }
            const auto rights = found->second.access;
            if ((symbol == "TerminateProcess" && !(rights & 1)) ||
                ((symbol == "GetProcessId" || symbol == "GetExitCodeProcess" || symbol == "GetPriorityClass") &&
                 !(rights & 0x1400))) {
                fail(5);
                call.return_to(state, result);
                return std::nullopt;
            }
            auto &child = *found->second.child;
            child.poll();
            if (symbol == "GetExitCodeProcess" || symbol == "GetExitCodeThread") {
                memory.store(GuestAddress{arg(1)}, 32, child.exited ? child.exit_code : 259);
                result = 1;
            } else if (symbol == "GetProcessId" || symbol == "GetThreadId")
                result = static_cast<std::uint32_t>(child.pid);
            else if (symbol == "TerminateProcess") {
                if (child.exited)
                    fail(5);
                else if (::kill(child.pid, SIGKILL))
                    fail(5);
                else {
                    child.terminated = true;
                    child.have_result = true;
                    child.success = true;
                    child.exit_code = static_cast<std::uint32_t>(arg(1));
                    if (found->second.record)
                        found->second.record->finish(child.exit_code);
                    result = 1;
                }
            } else if (symbol == "WaitForInputIdle") {
                if (!child.gui) {
                    last_error_ = 1471;
                    result = UINT32_MAX;
                } else if (child.idle || child.exited)
                    result = 0;
                else if (!arg(1))
                    result = 258;
                else {
                    auto &thread = threads_.items.at(threads_.current);
                    thread.status = ThreadStatus::Waiting;
                    thread.wait = {};
                    thread.wait.kind = ThreadWaitKind::ProcessInputIdle;
                    thread.wait.handles = {arg(0)};
                    thread.wait.deadline =
                        static_cast<std::uint32_t>(arg(1)) == UINT32_MAX
                            ? UINT64_MAX
                            : runtime_.clock_ticks +
                                  static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(1))) * 10000;
                    schedule(state, memory);
                    return std::nullopt;
                }
            } else
                throw HostCallFault("未知子程序服務");
        }
    }
#endif
    call.return_to(state, result);
    return std::nullopt;
}
} // namespace x64arm
