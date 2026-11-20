#include "service_callback_task.hpp"
#include "guest_counted_string.hpp"
#include "x64arm/windows_services.hpp"
#include "windows_loader_lock.hpp"
#include "windows_nt_entry.hpp"
#include "windows_exception_diagnostics.hpp"
#include "windows_object_identity.hpp"
#include "windows_file_stat.hpp"
#include "windows_native_process_parameters.hpp"
#include "x64arm/process_io.hpp"
#include "x64arm/windows_paths.hpp"
#include "windows_globalroot_path.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/pe_loader.hpp"
#include "x64arm/decoder.hpp"
#include "x64arm/shared_job.hpp"
#include "guest_concrt_queue.hpp"
#include "guest_concrt_queue_code.hpp"
#include "resource_directory.hpp"
#include "default_pipe_acl.hpp"
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <charconv>
#include <span>

namespace x64arm {
namespace {
struct NativeError {std::uint32_t status;};
// 原有 1／* 保留完整追蹤；逗號分隔的 NT／Zw 名稱只觀察指定呼叫，
// 不設定 TRACE_SERVICES，避免為了診斷而停用常駐 API 路徑。
bool trace_native_call(std::string_view symbol){
    static const std::string selected=[] {const auto* value=std::getenv("X64ARM_TRACE_SYSCALL");return value?std::string(value):std::string{};}();
    if(selected.empty() || selected=="0")return false;
    if(selected=="1" || selected=="*")return true;
    const auto normalize=[](std::string_view name){return name.starts_with("Nt") || name.starts_with("Zw")?name.substr(2):name;};
    for(std::string_view rest=selected;!rest.empty();){
        const auto comma=rest.find(',');auto token=rest.substr(0,comma);
        while(!token.empty() && token.front()==' ')token.remove_prefix(1);
        while(!token.empty() && token.back()==' ')token.remove_suffix(1);
        if(!token.empty() && normalize(token)==normalize(symbol))return true;
        if(comma==rest.npos)break;rest.remove_prefix(comma+1);
    }
    return false;
}
bool trace_native_thread(std::uint64_t thread){
    // 可選的客體執行緒篩選只影響診斷，避免等待熱路徑產生大量無關快照。
    static const auto selected=[] {
        const auto* setting=std::getenv("X64ARM_TRACE_SYSCALL_THREAD");
        if(!setting || !*setting)return std::uint64_t{0};
        const std::string_view text{setting};std::uint64_t value{};
        const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
        return result.ec==std::errc{} && result.ptr==text.data()+text.size()?value:UINT64_MAX;
    }();
    return !selected || selected==thread;
}

// \Device\NamedPipe\ 目錄 handle 在檔案表中的識別鍵（不對應宿主路徑）。
constexpr std::string_view nt_pipe_root_key="\\Device\\NamedPipe\\";
std::uint32_t nt_status(std::uint32_t error){
    switch(error){case 0:return 0;case 1:return 0xc0000010U;case 2:return 0xc0000034U;case 3:return 0xc000003aU;case 5:return 0xc0000022U;case 6:return 0xc0000008U;
    case 8:return 0xc0000017U;case 19:return 0xc00000a2U;case 24:return 0xc0000004U;case 32:return 0xc0000043U;case 33:return 0xc0000054U;case 38:return 0xc0000011U;
    case 50:return 0xc00000bbU;case 80:case 183:return 0xc0000035U;case 87:return 0xc000000dU;case 109:return 0xc000014bU;
    case 112:return 0xc000007fU;case 122:return 0xc0000023U;case 123:return 0xc0000033U;case 193:return 0xc000007bU;case 234:return 0x80000005U;case 267:return 0xc0000103U;
    case 230:return 0xc00000adU;case 231:return 0xc00000abU;case 232:return 0xc00000b1U;case 233:return 0xc00000b0U;case 535:return 0xc00000b2U;case 536:return 0xc00000b3U;
    case 288:return 0xc0000046U;case 298:return 0xc0000047U;case 995:return 0xc0000120U;case 997:return 0x103U;case 998:return 0xc0000005U;
    case 1006:return 0xc000011eU;case 1008:return 0xc000007cU;case 1018:return 0xc000017cU;case 1117:return 0xc0000185U;case 1168:return 0xc0000225U;case 1237:return 0xc000022dU;case 1305:return 0xc0000058U;case 1306:return 0xc0000059U;
    case 1307:return 0xc000005aU;case 1308:return 0xc000005bU;case 1309:return 0xc00000a8U;case 1310:return 0xc000005dU;case 1314:return 0xc0000061U;case 629:return 0xc00002b3U;case 158:return 0xc000002aU;case 1349:return 0xc00000bcU;
    case 1336:return 0xc0000077U;case 1337:return 0xc0000078U;case 1338:return 0xc0000079U;case 1344:return 0xc0000099U;
    case 1346:return 0xc00000a5U;case 1347:return 0xc00000a4U;case 1360:return 0xc00000e6U;default:return 0xc0000001U;}
}
struct ObjectAttributes {std::u16string name;std::uint64_t root{},security{};std::uint32_t flags{},file_qos{};};
std::optional<std::u16string> win32_object_name(std::u16string name);
std::u16string rooted_object_name(const RuntimeState& runtime,const ObjectAttributes& attributes,bool creating){
    auto name=attributes.name;if(name.find(u'\0')!=name.npos)throw NativeError{0xc0000033U};
    if(!attributes.root)return name;
    const auto directory=runtime.object_directories.find(attributes.root);
    if(directory==runtime.object_directories.end())throw NativeError{0xc0000008U};
    if(directory->second.object->symbolic_link())throw NativeError{0xc0000024U};
    const auto required=creating?6U:2U;if((directory->second.access&required)!=required)throw NativeError{0xc0000022U};
    if(!name.empty() && name.front()==u'\\')throw NativeError{0xc000003bU};
    auto root=std::u16string(directory->second.object->path());if(!name.empty()){if(root.back()!=u'\\')root+=u'\\';root+=name;}return root;
}
std::u16string native_string(const GuestMemory& memory,std::uint64_t address,bool x86){
    const auto length=memory.load(GuestAddress{address},16),maximum=memory.load(GuestAddress{address+2},16),buffer=memory.load(GuestAddress{address+(x86?4U:8U)},x86?32:64);
    if(length%2 || length>maximum || (length && !buffer))throw NativeError{0xc000000dU};
    std::u16string result;for(std::uint64_t n=0;n<length;n+=2)result.push_back(static_cast<char16_t>(memory.load(GuestAddress{buffer+n},16)));return result;
}
ObjectAttributes object_attributes(const GuestMemory& memory,std::uint64_t address,bool x86,bool optional=false,bool file_qos=false,bool exclusive=false){
    if(!address){if(optional)return {};throw NativeError{0xc000000dU};}
    if(std::getenv("X64ARM_TRACE_OBJECT_ATTRIBUTES")){
        const unsigned unit=x86?4U:8U;
        std::fprintf(stderr,"[object_attributes] address=%llx length=%llx",static_cast<unsigned long long>(address),static_cast<unsigned long long>(memory.load(GuestAddress{address},32)));
        for(unsigned i=1;i<6;++i)std::fprintf(stderr," field%u=%llx",i,static_cast<unsigned long long>(memory.load(GuestAddress{address+i*unit},x86?32:64)));
        std::fprintf(stderr,"\n");
    }
    if(memory.load(GuestAddress{address},32)!=(x86?24U:48U))throw NativeError{0xc000000dU};
    const auto unit=x86?4U:8U;const auto pointer=[&](unsigned n){return memory.load(GuestAddress{address+n*unit},x86?32:64);};
    ObjectAttributes result;result.root=pointer(1);result.flags=static_cast<std::uint32_t>(pointer(3));result.security=pointer(4);
    // 檔案 SQOS 沿用共用開檔旗標；其他物件仍維持原有的拒絕契約。
    if(result.flags&~(exclusive?0x1e2U:0x1c2U))throw NativeError{0xc000000dU};
    if(const auto qos=pointer(5)){
        if(!file_qos)throw NativeError{0xc000000dU};
        memory.check(GuestAddress{qos},12,Access::Read);
        const auto level=memory.load(GuestAddress{qos+4},32),tracking=memory.load(GuestAddress{qos+8},8),effective=memory.load(GuestAddress{qos+9},8);
        if(memory.load(GuestAddress{qos},32)!=12 || level>3 || tracking>1 || effective>1)throw NativeError{0xc000000dU};
        result.file_qos=0x100000U|static_cast<std::uint32_t>(level<<16)|(tracking?0x40000U:0U)|(effective?0x80000U:0U);
    }
    if(pointer(2))result.name=native_string(memory,pointer(2),x86);return result;
}
}
// 映射 ConcRT concurrent_queue 基底的客體常式映像；映像沒有匯入表，宿主入口在映射前寫進 guest_concrt_host[]，之後整個映像唯讀。
void concrt_queue::initialize(GuestMemory& memory,std::map<std::uint64_t,RuntimeImage>& images,bool x86,std::uint64_t gateway,
                              const std::array<std::uint64_t,CONCRT_QUEUE_HOST_COUNT>& host){
    const auto bytes=x86?std::span<const std::uint8_t>{image_x86}:std::span<const std::uint8_t>{image_x64};
    const auto base=gateway+image_offset;auto parsed=parse_pe_image(bytes,GuestAddress{base},true,memory.limit());
    if(!parsed.imports.empty() || !parsed.delay_imports.empty() || parsed.metadata.image_size>image_capacity)
        throw std::logic_error("Concrt 佇列常式包含外部相依或超出常駐映像範圍");
    for(const auto& [address,image]:images)if(base<address+image.size && address<base+parsed.metadata.image_size)
        throw std::logic_error("Concrt 佇列常式與既有常駐映像重疊");
    const auto native=[&](std::string_view name){
        const auto found=parsed.runtime.export_names.find(std::string(name));
        if(found==parsed.runtime.export_names.end())throw std::logic_error("Concrt 佇列常式缺少產碼入口："+std::string(name));
        return parsed.runtime.export_ordinals.at(found->second);
    };
    auto image=parsed.runtime;image.private_code=true;image.path=u"[guest-concrt-queue]";image.export_names.clear();image.export_ordinals.clear();
    for(const auto& binding:bindings)image.export_names.emplace(std::string(x86?binding.x86:binding.x64),native(binding.native));
    const std::uint64_t cells=native("guest_concrt_host");const unsigned unit=x86?4U:8U;
    const auto data=std::find_if(parsed.mappings.begin(),parsed.mappings.end(),[&](const PeMapping& mapping){
        return cells>=mapping.rva && cells-mapping.rva+unit*host.size()<=mapping.bytes.size();});
    if(data==parsed.mappings.end())throw std::logic_error("Concrt 佇列常式的宿主入口表不在已初始化資料區");
    for(std::size_t index=0;index<host.size();++index){
        if(!host[index] || (x86 && host[index]>UINT32_MAX))throw std::logic_error("Concrt 佇列常式的宿主入口無效");
        for(unsigned n=0;n<unit;++n)data->bytes[cells-data->rva+index*unit+n]=static_cast<std::uint8_t>(host[index]>>(8*n));
    }
    for(auto& mapping:parsed.mappings)mapping.permissions=(mapping.permissions&permission(Access::Execute))?read_execute:permission(Access::Read);
    map_pe_image(memory,parsed);images.emplace(base,std::move(image));
}
// 物件命名空間（windows_object_namespace.inc）：目錄、符號連結、私有命名空間與邊界描述元。
static bool object_namespace_service(std::string_view symbol){
    static constexpr std::array<std::string_view,23> names{
        "NtCreateDirectoryObject","NtCreateDirectoryObjectEx","NtQueryDirectoryObject","NtCreateSymbolicLinkObject","NtOpenSymbolicLinkObject",
        "NtQuerySymbolicLinkObject","NtMakePermanentObject","NtCreatePrivateNamespace","NtOpenPrivateNamespace","NtDeletePrivateNamespace",
        "RtlCreateBoundaryDescriptor","RtlDeleteBoundaryDescriptor","RtlAddSIDToBoundaryDescriptor","RtlAddIntegrityLabelToBoundaryDescriptor",
        "CreateBoundaryDescriptorA","CreateBoundaryDescriptorW","DeleteBoundaryDescriptor","AddSIDToBoundaryDescriptor","AddIntegrityLabelToBoundaryDescriptor",
        "CreatePrivateNamespaceA","CreatePrivateNamespaceW","OpenPrivateNamespaceA","OpenPrivateNamespaceW"};
    if(symbol=="ClosePrivateNamespace")return true;
    const auto native=symbol.starts_with("Zw")?std::string("Nt")+std::string(symbol.substr(2)):std::string(symbol);
    return std::find(names.begin(),names.end(),native)!=names.end();
}
std::span<const WindowsServiceDescriptor> nt_native_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","RtlDisownModuleHeapAllocation",2},
        {"kernel32.dll","BaseGetNamedObjectDirectory",1},
        // 分散／集中 I/O（windows_file_segments.inc）
        {"kernel32.dll","ReadFileScatter",5,0,0,0x4},{"kernel32.dll","WriteFileGather",5,0,0,0x4},
        {"ntdll.dll","NtReadFileScatter",9,0,0,0x40},{"ntdll.dll","ZwReadFileScatter",9,0,0,0x40},{"ntdll.dll","NtWriteFileGather",9,0,0,0x40},{"ntdll.dll","ZwWriteFileGather",9,0,0,0x40},
        {"ntdll.dll","NtOpenDirectoryObject",3,0,0,2},{"ntdll.dll","ZwOpenDirectoryObject",3,0,0,2},
        {"ntdll.dll","NtCreateDirectoryObject",3,0,0,2},{"ntdll.dll","ZwCreateDirectoryObject",3,0,0,2},
        {"ntdll.dll","NtCreateDirectoryObjectEx",5,0,0,0x12},{"ntdll.dll","ZwCreateDirectoryObjectEx",5,0,0,0x12},
        {"ntdll.dll","NtQueryDirectoryObject",7,0,0,0x1c},{"ntdll.dll","ZwQueryDirectoryObject",7,0,0,0x1c},
        {"ntdll.dll","NtCreateSymbolicLinkObject",4,0,0,2},{"ntdll.dll","ZwCreateSymbolicLinkObject",4,0,0,2},
        {"ntdll.dll","NtOpenSymbolicLinkObject",3,0,0,2},{"ntdll.dll","ZwOpenSymbolicLinkObject",3,0,0,2},
        {"ntdll.dll","NtQuerySymbolicLinkObject",3},{"ntdll.dll","ZwQuerySymbolicLinkObject",3},
        {"ntdll.dll","NtMakePermanentObject",1},{"ntdll.dll","ZwMakePermanentObject",1},
        {"ntdll.dll","NtCreatePrivateNamespace",4,0,0,2},{"ntdll.dll","ZwCreatePrivateNamespace",4,0,0,2},
        {"ntdll.dll","NtOpenPrivateNamespace",4,0,0,2},{"ntdll.dll","ZwOpenPrivateNamespace",4,0,0,2},
        {"ntdll.dll","NtDeletePrivateNamespace",1},{"ntdll.dll","ZwDeletePrivateNamespace",1},
        {"ntdll.dll","RtlCreateBoundaryDescriptor",2,0,0,2},{"ntdll.dll","RtlDeleteBoundaryDescriptor",1},
        {"ntdll.dll","RtlAddSIDToBoundaryDescriptor",2},{"ntdll.dll","RtlAddIntegrityLabelToBoundaryDescriptor",2},
        {"kernel32.dll","CreateBoundaryDescriptorA",2,0,0,2},{"kernel32.dll","CreateBoundaryDescriptorW",2,0,0,2},{"kernel32.dll","DeleteBoundaryDescriptor",1},
        {"kernel32.dll","AddSIDToBoundaryDescriptor",2},{"kernel32.dll","AddIntegrityLabelToBoundaryDescriptor",2},
        {"kernel32.dll","CreatePrivateNamespaceA",3},{"kernel32.dll","CreatePrivateNamespaceW",3},
        {"kernel32.dll","OpenPrivateNamespaceA",2},{"kernel32.dll","OpenPrivateNamespaceW",2},{"kernel32.dll","ClosePrivateNamespace",2,0,0,2},
#include "concrt_sync_descriptors.inc"
#include "concrt_lock_descriptors.inc"
#include "concrt_queue_descriptors.inc"
#include "concrt_scheduler_descriptors.inc"
        {"ntdll.dll","NtCreateFile",11,0,0,0x5e2},{"ntdll.dll","NtOpenFile",6,0,0,0x32},
        {"ntdll.dll","NtQueryAttributesFile",2},{"ntdll.dll","NtQueryFullAttributesFile",2},
        {"ntdll.dll","NtQueryInformationByName",5,0,0,0x18},
        {"ntdll.dll","NtDeleteFile",1},{"ntdll.dll","NtFlushBuffersFile",2},
        {"ntdll.dll","NtQuerySecurityObject",5,0,0,0xa},{"ntdll.dll","NtSetSecurityObject",3,0,0,0x2},
        {"ntdll.dll","NtAccessCheck",8,0,0,0x4},
        {"ntdll.dll","NtReadFile",9,0,0,0x40},{"ntdll.dll","NtWriteFile",9,0,0,0x40},
        {"ntdll.dll","NtCancelIoFile",2},{"ntdll.dll","NtCancelIoFileEx",3},
        {"ntdll.dll","NtLockFile",10,0,0,0x380},{"ntdll.dll","NtUnlockFile",5,0,0,0x10},
        {"internal-nt-event","CreateEventExW",5,0,0,0x1c},{"internal-nt-event","OpenEventW",4,0,0,0xb},
        {"internal-nt-event","DuplicateHandle",8,0,0,0xf0},
        {"internal-nt-file","ReadFile",9,0,0,0x24},{"internal-nt-file","WriteFile",9,0,0,0x24},
        {"internal-nt-file","CreateFileW",7,0,0,0x36},{"internal-nt-file","CloseHandle",1},
        {"internal-nt-file","CancelIo",1},{"internal-nt-file","CancelIoEx",2},
        {"internal-file-apc","Complete",3},
        {"internal-nt-mapping","CreateFileMappingW",7,0,0,0x5c},{"internal-nt-mapping","OpenFileMappingW",3,0,0,0x3},
        {"ntdll.dll","NtOpenProcess",4,0,0,0x2},{"ntdll.dll","NtOpenThread",4,0,0,0x2},
        {"ntdll.dll","NtOpenProcessToken",3,0,0,0x2},{"ntdll.dll","NtOpenProcessTokenEx",4,0,0,0x6},
        {"ntdll.dll","NtOpenThreadToken",4,0,0,0x6},{"ntdll.dll","NtOpenThreadTokenEx",5,0,0,0xe},
        {"ntdll.dll","NtCreateWaitCompletionPacket",3,0,0,0x2},{"ntdll.dll","NtAssociateWaitCompletionPacket",8,0,0,0x20},{"ntdll.dll","NtCancelWaitCompletionPacket",2,0,0,0x2},
        {"ntdll.dll","NtCreateIoCompletion",4,0,0,0xa},{"ntdll.dll","NtSetIoCompletion",5,0,0,0x8},{"ntdll.dll","NtQueryIoCompletion",5,0,0,0xa},
        {"ntdll.dll","NtRemoveIoCompletion",5},{"ntdll.dll","NtRemoveIoCompletionEx",6,0,0,0x24},
        {"ntdll.dll","NtDuplicateObject",7,0,0,0x70},{"ntdll.dll","NtWaitForSingleObject",3,0,0,0x2},{"ntdll.dll","NtQueryMutant",5,0,0,0xa},{"ntdll.dll","NtSignalAndWaitForSingleObject",4,0,0,0x4},
        {"ntdll.dll","NtWaitForMultipleObjects",5,0,0,0xd},{"ntdll.dll","NtWaitForMultipleObjects32",5,0,0,0xd},
        {"ntdll.dll","NtSuspendThread",2},{"ntdll.dll","NtResumeThread",2},
        // NtCreateNamedPipeFile：DesiredAccess、ShareAccess、CreateDisposition、CreateOptions、管道型態／讀取／完成模式、實例數與配額皆為 ULONG。
        {"ntdll.dll","NtCreateNamedPipeFile",14,0,0,0x1ff2},{"ntdll.dll","ZwCreateNamedPipeFile",14,0,0,0x1ff2},
        {"ntdll.dll","NtGetContextThread",2},{"ntdll.dll","ZwGetContextThread",2},{"ntdll.dll","NtSetContextThread",2},{"ntdll.dll","ZwSetContextThread",2},
        {"ntdll.dll","NtCreateSection",7,0,0,0x32},{"ntdll.dll","NtOpenSection",3,0,0,0x2},{"ntdll.dll","NtMapViewOfSection",10,0,0,0x380},
        {"ntdll.dll","NtFlushVirtualMemory",4},{"ntdll.dll","NtExtendSection",2},{"ntdll.dll","NtMapViewOfSectionEx",9,0,0,0x160},
        {"ntdll.dll","RtlCreateHeap",6,0,0,0x1},{"ntdll.dll","RtlDestroyHeap",1},
        {"ntdll.dll","RtlRunOnceExecuteOnce",4},
        // MSVC ABI 包裝沿用此處的非同步服務轉接，底層同步物件仍由 Win32 共用流程持有。
        {"msvcp140.dll","_Mtx_init_in_situ",2,0,0,0,true},
        {"msvcp140.dll","_Mtx_destroy_in_situ",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_lock",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_unlock",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_init_in_situ",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_destroy_in_situ",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_wait",2,0,0,0,true},
        {"msvcp140.dll","_Cnd_signal",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_broadcast",1,0,0,0,true},
        {"msvcp140.dll","_Thrd_yield",0,0,0,0,true},
        {"msvcp140.dll","_Mtx_trylock",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_current_owns",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_clear_owner",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_reset_owner",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_getconcrtcs",1,0,0,0,true},
        {"msvcp140.dll","_Mtx_init",2,0,0,0,true},
        {"msvcp140.dll","_Mtx_destroy",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_init",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_destroy",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_timedwait",3,0,0,0,true},
        {"msvcp140.dll","_Cnd_register_at_thread_exit",3,0,0,0,true},
        {"msvcp140.dll","_Cnd_unregister_at_thread_exit",1,0,0,0,true},
        {"msvcp140.dll","_Cnd_do_broadcast_at_thread_exit",0,0,0,0,true},
        {"msvcp140.dll","_Thrd_id",0,0,0,0,true},
        {"msvcp140.dll","_Thrd_hardware_concurrency",0,0,0,0,true},
        {"msvcp140.dll","_Thrd_sleep",1,0,0,0,true},
        {"msvcp140.dll","_Thrd_detach",1,1,0,0,true},
        {"msvcp140.dll","_Thrd_join",2,1,0,0,true},
        {"msvcp140.dll","_Thrd_start",3,0,0,0,true},
        {"msvcp140.dll","_Query_perf_counter",0,0,0,0,true},
        {"msvcp140.dll","_Query_perf_frequency",0,0,0,0,true},
        {"msvcp140.dll","_Xtime_get_ticks",0,0,0,0,true},
        // VC11／VC12 的 <thread>／<mutex>／<condition_variable> C 匯出：_Mtx_t／_Cnd_t 為 _Mtx_init／_Cnd_init 配置的不透明指標，
        // _Thrd_t｛HANDLE, id｝與 xtime｛__int64 秒, long 奈秒｝和 msvcp140 相同，共用同一套同步實作。
        {"msvcp110.dll","_Mtx_init",2,0,0,0,true},{"msvcp110.dll","_Mtx_destroy",1,0,0,0,true},{"msvcp110.dll","_Mtx_lock",1,0,0,0,true},{"msvcp110.dll","_Mtx_unlock",1,0,0,0,true},{"msvcp110.dll","_Mtx_trylock",1,0,0,0,true},{"msvcp110.dll","_Mtx_current_owns",1,0,0,0,true},{"msvcp110.dll","_Mtx_clear_owner",1,0,0,0,true},{"msvcp110.dll","_Mtx_reset_owner",1,0,0,0,true},{"msvcp110.dll","_Mtx_getconcrtcs",1,0,0,0,true},{"msvcp110.dll","_Cnd_init",1,0,0,0,true},{"msvcp110.dll","_Cnd_destroy",1,0,0,0,true},{"msvcp110.dll","_Cnd_wait",2,0,0,0,true},{"msvcp110.dll","_Cnd_timedwait",3,0,0,0,true},{"msvcp110.dll","_Cnd_signal",1,0,0,0,true},{"msvcp110.dll","_Cnd_broadcast",1,0,0,0,true},{"msvcp110.dll","_Cnd_register_at_thread_exit",3,0,0,0,true},{"msvcp110.dll","_Cnd_unregister_at_thread_exit",1,0,0,0,true},{"msvcp110.dll","_Cnd_do_broadcast_at_thread_exit",0,0,0,0,true},{"msvcp110.dll","_Thrd_detach",1,1,0,0,true},{"msvcp110.dll","_Thrd_join",2,1,0,0,true},{"msvcp110.dll","_Thrd_sleep",1,0,0,0,true},{"msvcp110.dll","_Thrd_yield",0,0,0,0,true},{"msvcp110.dll","_Thrd_start",3,0,0,0,true},{"msvcp110.dll","_Xtime_get_ticks",0,0,0,0,true},{"msvcp110.dll","_Thrd_create",3,0,0,0,true},{"msvcp110.dll","_Thrd_current",1,0,0,0,true},{"msvcp110.dll","_Thrd_equal",2,3,0,0,true},{"msvcp110.dll","_Thrd_lt",2,3,0,0,true},{"msvcp110.dll","_Xtime_diff_to_millis",1,0,0,0,true},{"msvcp110.dll","_Xtime_diff_to_millis2",2,0,0,0,true},{"msvcp110.dll","xtime_get",2,0,0,0,true},
        {"msvcp120.dll","_Mtx_init",2,0,0,0,true},{"msvcp120.dll","_Mtx_destroy",1,0,0,0,true},{"msvcp120.dll","_Mtx_lock",1,0,0,0,true},{"msvcp120.dll","_Mtx_unlock",1,0,0,0,true},{"msvcp120.dll","_Mtx_trylock",1,0,0,0,true},{"msvcp120.dll","_Mtx_current_owns",1,0,0,0,true},{"msvcp120.dll","_Mtx_clear_owner",1,0,0,0,true},{"msvcp120.dll","_Mtx_reset_owner",1,0,0,0,true},{"msvcp120.dll","_Mtx_getconcrtcs",1,0,0,0,true},{"msvcp120.dll","_Cnd_init",1,0,0,0,true},{"msvcp120.dll","_Cnd_destroy",1,0,0,0,true},{"msvcp120.dll","_Cnd_wait",2,0,0,0,true},{"msvcp120.dll","_Cnd_timedwait",3,0,0,0,true},{"msvcp120.dll","_Cnd_signal",1,0,0,0,true},{"msvcp120.dll","_Cnd_broadcast",1,0,0,0,true},{"msvcp120.dll","_Cnd_register_at_thread_exit",3,0,0,0,true},{"msvcp120.dll","_Cnd_unregister_at_thread_exit",1,0,0,0,true},{"msvcp120.dll","_Cnd_do_broadcast_at_thread_exit",0,0,0,0,true},{"msvcp120.dll","_Thrd_detach",1,1,0,0,true},{"msvcp120.dll","_Thrd_join",2,1,0,0,true},{"msvcp120.dll","_Thrd_sleep",1,0,0,0,true},{"msvcp120.dll","_Thrd_yield",0,0,0,0,true},{"msvcp120.dll","_Thrd_start",3,0,0,0,true},{"msvcp120.dll","_Xtime_get_ticks",0,0,0,0,true},{"msvcp120.dll","_Thrd_create",3,0,0,0,true},{"msvcp120.dll","_Thrd_current",1,0,0,0,true},{"msvcp120.dll","_Thrd_equal",2,3,0,0,true},{"msvcp120.dll","_Thrd_lt",2,3,0,0,true},{"msvcp120.dll","_Xtime_diff_to_millis",1,0,0,0,true},{"msvcp120.dll","_Xtime_diff_to_millis2",2,0,0,0,true},{"msvcp120.dll","xtime_get",2,0,0,0,true},
        {"msvcp140.dll","_Thrd_create",3,0,0,0,true},{"msvcp140.dll","_Thrd_current",1,0,0,0,true},{"msvcp140.dll","_Thrd_equal",2,3,0,0,true},{"msvcp140.dll","_Xtime_diff_to_millis",1,0,0,0,true},{"msvcp140.dll","_Xtime_diff_to_millis2",2,0,0,0,true},{"msvcp140.dll","xtime_get",2,0,0,0,true},
        {"internal-msvcp","ThreadRunner",1},
        {"internal-msvcp","OnceComplete",1},{"msvcp110.dll","_Call_once",2,0,0,0,true},{"msvcp110.dll","_Call_onceEx",3,0,0,0,true},{"msvcp120.dll","_Call_once",2,0,0,0,true},{"msvcp120.dll","_Call_onceEx",3,0,0,0,true},
        {"msvcp140.dll","??0_Lockit@std@@QEAA@H@Z",2},
        {"msvcp140.dll","??0_Lockit@std@@QEAA@XZ",1},
        {"msvcp140.dll","??1_Lockit@std@@QEAA@XZ",1},
        {"msvcp140.dll","??0_Lockit@std@@QAE@H@Z",1},
        {"msvcp140.dll","??0_Lockit@std@@QAE@XZ",0},
        {"msvcp140.dll","??1_Lockit@std@@QAE@XZ",0},
        {"internal-msvcp","GlobalLocksInit",3},
        // PPL（Concurrency）在 msvcp140 的匯出：執行緒池工作、內容回呼與工作事件記錄。
        {"internal-msvcp","TaskSchedulerCallback",3},
        {"msvcp140.dll","??0task_continuation_context@Concurrency@@AEAA@XZ",1},{"msvcp140.dll","??0task_continuation_context@Concurrency@@AAE@XZ",0},
        {"msvcp140.dll","?GetCurrentThreadId@platform@details@Concurrency@@YAJXZ",0,0,0,0,true},
        {"msvcp140.dll","?ReportUnhandledError@_ExceptionHolder@details@Concurrency@@AEAAXXZ",1},{"msvcp140.dll","?ReportUnhandledError@_ExceptionHolder@details@Concurrency@@AAEXXZ",0},
        {"msvcp140.dll","?_CallInContext@_ContextCallback@details@Concurrency@@QEBAXV?$function@$$A6AXXZ@std@@_N@Z",3},
        {"msvcp140.dll","?_Capture@_ContextCallback@details@Concurrency@@AEAAXXZ",1},{"msvcp140.dll","?_Capture@_ContextCallback@details@Concurrency@@AAEXXZ",0},
        {"msvcp140.dll","?_Reset@_ContextCallback@details@Concurrency@@AEAAXXZ",1},{"msvcp140.dll","?_Reset@_ContextCallback@details@Concurrency@@AAEXXZ",0},
        {"msvcp140.dll","?_LogCancelTask@_TaskEventLogger@details@Concurrency@@QEAAXXZ",1},{"msvcp140.dll","?_LogScheduleTask@_TaskEventLogger@details@Concurrency@@QEAAX_N@Z",2},
        {"msvcp140.dll","?_LogTaskCompleted@_TaskEventLogger@details@Concurrency@@QEAAXXZ",1},{"msvcp140.dll","?_LogTaskExecutionCompleted@_TaskEventLogger@details@Concurrency@@QEAAXXZ",1},
        {"msvcp140.dll","?_LogWorkItemCompleted@_TaskEventLogger@details@Concurrency@@QEAAXXZ",1},{"msvcp140.dll","?_LogWorkItemStarted@_TaskEventLogger@details@Concurrency@@QEAAXXZ",1},
        {"msvcp140.dll","?_LogCancelTask@_TaskEventLogger@details@Concurrency@@QAEXXZ",0},{"msvcp140.dll","?_LogScheduleTask@_TaskEventLogger@details@Concurrency@@QAEX_N@Z",1},
        {"msvcp140.dll","?_LogTaskCompleted@_TaskEventLogger@details@Concurrency@@QAEXXZ",0},{"msvcp140.dll","?_LogTaskExecutionCompleted@_TaskEventLogger@details@Concurrency@@QAEXXZ",0},
        {"msvcp140.dll","?_LogWorkItemCompleted@_TaskEventLogger@details@Concurrency@@QAEXXZ",0},{"msvcp140.dll","?_LogWorkItemStarted@_TaskEventLogger@details@Concurrency@@QAEXXZ",0},
        {"msvcp140.dll","?_ReportUnobservedException@details@Concurrency@@YAXXZ",0,0,0,0,true},
        {"msvcp140.dll","?_Schedule_chore@details@Concurrency@@YAHPEAU_Threadpool_chore@12@@Z",1,0,0,0,true},{"msvcp140.dll","?_Schedule_chore@details@Concurrency@@YAHPAU_Threadpool_chore@12@@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Release_chore@details@Concurrency@@YAXPEAU_Threadpool_chore@12@@Z",1,0,0,0,true},{"msvcp140.dll","?_Release_chore@details@Concurrency@@YAXPAU_Threadpool_chore@12@@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Execute_once@std@@YAHAEAUonce_flag@1@P6AHPEAX1PEAPEAX@Z1@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Execute_once@std@@YAHAAUonce_flag@1@P6GHPAX1PAPAX@Z1@Z",3,0,0,0,true},
        {"ntdll.dll","RtlAnsiStringToUnicodeString",3,0,0,0x4},{"ntdll.dll","RtlCompareUnicodeString",3,0,0,0x4},{"ntdll.dll","RtlEqualUnicodeString",3,0,0,0x4},
        {"ntdll.dll","RtlFreeUnicodeString",1},{"ntdll.dll","RtlGetLastNtStatus",0}
        ,{"ntdll.dll","RtlDetermineDosPathNameType_U",1},{"ntdll.dll","RtlIsDosDeviceName_U",1}
        ,{"ntdll.dll","RtlGetFullPathName_U",4},{"ntdll.dll","RtlDosPathNameToNtPathName_U",4},{"ntdll.dll","RtlDosPathNameToNtPathName_U_WithStatus",4}
        ,{"ntdll.dll","LdrLockLoaderLock",3,0,0,1},{"ntdll.dll","LdrUnlockLoaderLock",2,0,0,1}
        ,{"ntdll.dll","LdrLoadDll",4},{"ntdll.dll","LdrGetDllHandle",4},{"ntdll.dll","LdrGetProcedureAddress",4,0,0,0x4},{"ntdll.dll","LdrUnloadDll",1}
        ,{"ntdll.dll","LdrRegisterDllNotification",4},{"ntdll.dll","LdrUnregisterDllNotification",1}
        ,{"ntdll.dll","RtlAreLongPathsEnabled",0}
        ,{"ole32.dll","CoWaitForMultipleHandles",5},{"combase.dll","CoWaitForMultipleObjects",5}
        ,{"ntdll.dll","RtlGetUnloadEventTrace",0},{"ntdll.dll","RtlGetUnloadEventTraceEx",3}
        // NT 同步物件、計時、APC（windows_nt_objects.inc）。
        ,{"ntdll.dll","NtCreateEvent",5},{"ntdll.dll","NtOpenEvent",3},{"ntdll.dll","NtSetEvent",2},{"ntdll.dll","NtResetEvent",2},{"ntdll.dll","NtClearEvent",1}
        ,{"ntdll.dll","NtQueryEvent",5},{"ntdll.dll","NtSetEventBoostPriority",1}
        ,{"ntdll.dll","NtCreateMutant",4},{"ntdll.dll","NtOpenMutant",3},{"ntdll.dll","NtReleaseMutant",2}
        ,{"ntdll.dll","NtCreateSemaphore",5},{"ntdll.dll","NtOpenSemaphore",3},{"ntdll.dll","NtReleaseSemaphore",3}
        ,{"ntdll.dll","NtCreateTimer",4},{"ntdll.dll","NtOpenTimer",3},{"ntdll.dll","NtSetTimer",7},{"ntdll.dll","NtCancelTimer",2},{"ntdll.dll","NtQueryTimer",5}
        ,{"ntdll.dll","NtSetTimerResolution",3},{"ntdll.dll","NtQueryTimerResolution",3}
        ,{"ntdll.dll","NtDelayExecution",2},{"ntdll.dll","NtYieldExecution",0},{"ntdll.dll","NtQueryPerformanceCounter",2},{"ntdll.dll","NtQuerySystemTime",1}
        ,{"ntdll.dll","NtSetSystemTime",2},{"ntdll.dll","NtGetTickCount",0},{"ntdll.dll","NtGetCurrentProcessorNumber",0},{"ntdll.dll","NtFlushProcessWriteBuffers",0}
        ,{"ntdll.dll","NtTestAlert",0},{"ntdll.dll","NtQueueApcThread",5},{"ntdll.dll","NtQueueApcThreadEx",6}
        ,{"ntdll.dll","NtAlertThreadByThreadId",1},{"ntdll.dll","NtWaitForAlertByThreadId",2}
        // 以 CONTEXT 續行與例外派送。
        ,{"ntdll.dll","NtContinue",2},{"ntdll.dll","NtContinueEx",2},{"ntdll.dll","NtRaiseException",3}
        ,{"ntdll.dll","KiUserExceptionDispatcher",0},{"ntdll.dll","KiUserApcDispatcher",0}
        ,{"internal-apc-dispatch","Invoke",3}
        ,{"ntdll.dll","RtlRestoreContext",2},{"kernel32.dll","RtlRestoreContext",2},{"ntdll.dll","RtlRaiseException",1},{"kernel32.dll","RtlRaiseException",1},{"ntdll.dll","RtlRaiseStatus",1}
        // 權杖、程序、Job、Atom、地區設定、電源與雜項（windows_nt_families.inc）。
        ,{"ntdll.dll","NtQueryInformationToken",5,0,0,0xa},{"ntdll.dll","NtSetInformationToken",4,0,0,0xa}
        ,{"ntdll.dll","NtAdjustPrivilegesToken",6,0,0,0xa},{"ntdll.dll","NtAdjustGroupsToken",6,0,0,0xa}
        ,{"ntdll.dll","NtDuplicateToken",6,0,0,0x1a},{"ntdll.dll","NtFilterToken",6,0,0,0x2}
        ,{"ntdll.dll","NtCompareTokens",3},{"ntdll.dll","NtPrivilegeCheck",3},{"ntdll.dll","NtImpersonateAnonymousToken",1}
        ,{"ntdll.dll","NtAllocateLocallyUniqueId",1},{"ntdll.dll","NtAllocateUuids",4}
        ,{"ntdll.dll","NtSetInformationProcess",4,0,0,0xa},{"ntdll.dll","NtIsProcessInJob",2}
        ,{"ntdll.dll","NtCreateJobObject",3,0,0,0x2},{"ntdll.dll","NtOpenJobObject",3,0,0,0x2},{"ntdll.dll","NtAssignProcessToJobObject",2}
        ,{"ntdll.dll","NtTerminateJobObject",2,0,0,0x2},{"ntdll.dll","NtQueryInformationJobObject",5,0,0,0xa},{"ntdll.dll","NtSetInformationJobObject",4,0,0,0xa}
        ,{"ntdll.dll","NtAddAtom",3,0,0,0x2},{"ntdll.dll","NtFindAtom",3,0,0,0x2},{"ntdll.dll","NtDeleteAtom",1,0,0,0x1},{"ntdll.dll","NtQueryInformationAtom",5,0,0,0xb}
        ,{"ntdll.dll","NtQueryDefaultLocale",2,0,0,0x1},{"ntdll.dll","NtQueryDefaultUILanguage",1},{"ntdll.dll","NtQueryInstallUILanguage",1}
        ,{"ntdll.dll","NtPowerInformation",5,0,0,0x15},{"ntdll.dll","NtSetThreadExecutionState",2,0,0,0x1}
        ,{"ntdll.dll","NtPulseEvent",2},{"ntdll.dll","NtQuerySemaphore",5,0,0,0xa}
        ,{"ntdll.dll","NtLockVirtualMemory",4,0,0,0x8},{"ntdll.dll","NtUnlockVirtualMemory",4,0,0,0x8}
        ,{"ntdll.dll","NtFlushBuffersFileEx",5,0,0,0xa},{"ntdll.dll","NtCancelSynchronousIoFile",3}
        ,{"ntdll.dll","NtQuerySystemInformationEx",6,0,0,0x15}
        // Rtl 轉接家族（windows_ntdll_rtl.inc）：安全描述元、SID、ACL、時間、FLS、計時器佇列、等待登錄與 heap 資訊。
        ,{"ntdll.dll","RtlCopySid",3,0,0,0x1},{"ntdll.dll","RtlInitializeSid",3},{"ntdll.dll","RtlAllocateAndInitializeSid",11,0,0,0x3fc}
        ,{"ntdll.dll","RtlCreateAcl",3,0,0,0x6},{"ntdll.dll","RtlAddAce",5,0,0,0x16},{"ntdll.dll","RtlGetAce",3,0,0,0x2},{"ntdll.dll","RtlDeleteAce",2,0,0,0x2}
        ,{"ntdll.dll","RtlAddAccessAllowedAce",4,0,0,0x6},{"ntdll.dll","RtlAddAccessAllowedAceEx",5,0,0,0xe},{"ntdll.dll","RtlAddAccessDeniedAce",4,0,0,0x6},{"ntdll.dll","RtlAddAccessDeniedAceEx",5,0,0,0xe}
        ,{"ntdll.dll","RtlAddAuditAccessAce",6,0,0,0x6},{"ntdll.dll","RtlAddAuditAccessAceEx",7,0,0,0xe},{"ntdll.dll","RtlQueryInformationAcl",4,0,0,0xc}
        ,{"ntdll.dll","RtlCreateSecurityDescriptor",2,0,0,0x2},{"ntdll.dll","RtlSetDaclSecurityDescriptor",4},{"ntdll.dll","RtlSetSaclSecurityDescriptor",4}
        ,{"ntdll.dll","RtlSetOwnerSecurityDescriptor",3},{"ntdll.dll","RtlSetGroupSecurityDescriptor",3},{"ntdll.dll","RtlGetOwnerSecurityDescriptor",3},{"ntdll.dll","RtlGetGroupSecurityDescriptor",3}
        ,{"ntdll.dll","RtlGetDaclSecurityDescriptor",4},{"ntdll.dll","RtlGetSaclSecurityDescriptor",4},{"ntdll.dll","RtlGetControlSecurityDescriptor",3},{"ntdll.dll","RtlSetControlSecurityDescriptor",3}
        ,{"ntdll.dll","RtlMakeSelfRelativeSD",3},{"ntdll.dll","RtlAbsoluteToSelfRelativeSD",3},{"ntdll.dll","RtlSelfRelativeToAbsoluteSD",11},{"ntdll.dll","RtlImpersonateSelf",1,0,0,0x1}
        ,{"ntdll.dll","RtlLocalTimeToSystemTime",2},{"ntdll.dll","RtlSystemTimeToLocalTime",2},{"ntdll.dll","RtlFlsFree",1,0,0,0x1},{"ntdll.dll","RtlFlsSetValue",2,0,0,0x1}
        ,{"ntdll.dll","RtlDeleteTimer",3},{"ntdll.dll","RtlDeleteTimerQueueEx",2},{"ntdll.dll","RtlUpdateTimer",4,0,0,0xc},{"ntdll.dll","RtlRegisterWait",6,0,0,0x30}
        ,{"ntdll.dll","RtlDeregisterWait",1},{"ntdll.dll","RtlDeregisterWaitEx",2},{"ntdll.dll","RtlQueueWorkItem",3,0,0,0x4},{"ntdll.dll","RtlSetThreadErrorMode",2,0,0,0x1}
        ,{"ntdll.dll","RtlRunOnceComplete",3,0,0,0x2},{"ntdll.dll","RtlQueryHeapInformation",5,0,0,0xa},{"ntdll.dll","RtlSetHeapInformation",4,0,0,0xa}
        ,{"ntdll.dll","RtlEnterCriticalSection",1},{"ntdll.dll","RtlLeaveCriticalSection",1},{"ntdll.dll","RtlTryEnterCriticalSection",1},{"ntdll.dll","RtlInitializeCriticalSection",1}
        ,{"ntdll.dll","RtlInitializeCriticalSectionAndSpinCount",2,0,0,0x2},{"ntdll.dll","RtlInitializeCriticalSectionEx",3,0,0,0x6}
        ,{"ntdll.dll","RtlIsCriticalSectionLocked",1},{"ntdll.dll","RtlIsCriticalSectionLockedByThread",1}
        ,{"ntdll.dll","RtlSleepConditionVariableCS",3},{"ntdll.dll","RtlSleepConditionVariableSRW",4,0,0,0x8},{"ntdll.dll","RtlRunOnceBeginInitialize",3,0,0,0x2}
        ,{"ntdll.dll","RtlFlsAlloc",2},{"ntdll.dll","RtlFlsGetValue",2,0,0,0x1},{"ntdll.dll","RtlCreateTimerQueue",1},{"ntdll.dll","RtlCreateTimer",7,0,0,0x70}
        ,{"ntdll.dll","RtlAdjustPrivilege",4,0,0,0x1},{"ntdll.dll","RtlAddMandatoryAce",6,0,0,0xe}
        ,{"ntdll.dll","RtlQueryTimeZoneInformation",1},{"ntdll.dll","RtlQueryDynamicTimeZoneInformation",1},{"ntdll.dll","RtlGetSystemTimePrecise",0}
        ,{"ntdll.dll","RtlGetCurrentDirectory_U",2,0,0,0x1},{"ntdll.dll","RtlSetCurrentDirectory_U",1},{"ntdll.dll","RtlDoesFileExists_U",1},{"ntdll.dll","RtlDosSearchPath_U",6,0,0,0x8}
        ,{"ntdll.dll","RtlQueryEnvironmentVariable_U",3},{"ntdll.dll","RtlQueryEnvironmentVariable",6},{"ntdll.dll","RtlSetEnvironmentVariable",3},{"ntdll.dll","RtlExpandEnvironmentStrings_U",4}
        ,{"ntdll.dll","RtlExpandEnvironmentStrings",6},{"ntdll.dll","RtlCreateEnvironment",2},{"ntdll.dll","RtlDestroyEnvironment",1},{"ntdll.dll","RtlSetCurrentEnvironment",2},{"ntdll.dll","RtlDefaultNpAcl",1},{"ntdll.dll","RtlAddProcessTrustLabelAce",6,0,0,0x26},{"ntdll.dll","RtlCopySecurityDescriptor",2}
        ,{"ntdll.dll","RtlAddAccessAllowedObjectAce",7,0,0,0xe},{"ntdll.dll","RtlAddAccessDeniedObjectAce",7,0,0,0xe},{"ntdll.dll","RtlAddAuditAccessObjectAce",9,0,0,0xe}
        ,{"ntdll.dll","RtlNewSecurityObject",6},{"ntdll.dll","RtlNewSecurityObjectEx",8,0,0,0x20},{"ntdll.dll","RtlNewSecurityObjectWithMultipleInheritance",9,0,0,0x50}
        ,{"ntdll.dll","RtlConvertToAutoInheritSecurityObject",6},{"ntdll.dll","RtlDeleteSecurityObject",1},{"ntdll.dll","RtlValidRelativeSecurityDescriptor",3,0,0,0x6}
        // Ldr／Tp／Dbg／地區設定／IDN／正規化／路徑（windows_ntdll_loader.inc）。
        ,{"ntdll.dll","TpAllocCleanupGroup",1},{"ntdll.dll","TpQueryPoolStackInformation",2},{"ntdll.dll","TpSetPoolStackInformation",2},{"ntdll.dll","TpAllocWork",4},{"ntdll.dll","TpAllocTimer",4},{"ntdll.dll","TpAllocWait",4},{"ntdll.dll","TpAllocPool",2},{"ntdll.dll","TpAllocIoCompletion",5},{"ntdll.dll","RtlSetIoCompletionCallback",3,0,0,0x4}
        ,{"ntdll.dll","TpSetPoolMinThreads",2,0,0,0x2},{"ntdll.dll","TpSimpleTryPost",3},{"ntdll.dll","TpCallbackMayRunLong",1}
        ,{"ntdll.dll","LdrGetDllHandleEx",5,0,0,0x1},{"ntdll.dll","LdrAddRefDll",2,0,0,0x1},{"ntdll.dll","LdrDisableThreadCalloutsForDll",1}
        ,{"ntdll.dll","LdrFindEntryForAddress",2},{"ntdll.dll","LdrGetDllFullName",2},{"ntdll.dll","LdrGetDllDirectory",1},{"ntdll.dll","LdrSetDllDirectory",1}
        ,{"ntdll.dll","LdrAddDllDirectory",2},{"ntdll.dll","LdrRemoveDllDirectory",1},{"ntdll.dll","LdrSetDefaultDllDirectories",1,0,0,0x1}
        ,{"ntdll.dll","LdrFindResource_U",4,0,0,0x4},{"ntdll.dll","LdrFindResourceDirectory_U",4,0,0,0x4},{"ntdll.dll","LdrAccessResource",4},{"ntdll.dll","LdrProcessRelocationBlock",4,0,0,0x2}
        ,{"ntdll.dll","LdrEnumerateLoadedModules",3},{"ntdll.dll","LdrGetDllPath",4},{"ntdll.dll","RtlGetSearchPath",1},{"ntdll.dll","RtlGetExePath",2},{"ntdll.dll","RtlReleasePath",1},
        {"ntdll.dll","LdrQueryProcessModuleInformation",3},{"ntdll.dll","RtlQueryProcessPlaceholderCompatibilityMode",0},{"ntdll.dll","RtlSetProcessPlaceholderCompatibilityMode",1},
        {"ntdll.dll","RtlQueryThreadPlaceholderCompatibilityMode",0},{"ntdll.dll","RtlSetThreadPlaceholderCompatibilityMode",1},{"ntdll.dll","LdrQueryImageFileExecutionOptions",6,0,0,0x14}
        ,{"ntdll.dll","DbgBreakPoint",0},{"ntdll.dll","DbgUserBreakPoint",0},{"ntdll.dll","DbgUiRemoteBreakin",1}
        ,{"ntdll.dll","RtlLcidToLocaleName",4,0,0,0x5},{"ntdll.dll","RtlLocaleNameToLcid",3,0,0,0x4},{"ntdll.dll","RtlIsValidLocaleName",2,0,0,0x2}
        ,{"ntdll.dll","RtlGetUserPreferredUILanguages",5,0,0,0x3},{"ntdll.dll","RtlGetSystemPreferredUILanguages",5,0,0,0x3},{"ntdll.dll","RtlGetThreadPreferredUILanguages",4,0,0,0x1}
        ,{"ntdll.dll","RtlGetProcessPreferredUILanguages",4,0,0,0x1},{"ntdll.dll","RtlSetProcessPreferredUILanguages",3,0,0,0x1},{"ntdll.dll","RtlSetThreadPreferredUILanguages",3,0,0,0x1}
        ,{"ntdll.dll","RtlIdnToNameprepUnicode",5,0,0,0x5},{"ntdll.dll","RtlIdnToAscii",5,0,0,0x5},{"ntdll.dll","RtlIdnToUnicode",5,0,0,0x5},{"ntdll.dll","RtlNormalizeString",5,0,0,0x5},{"ntdll.dll","RtlIsNormalizedString",4,0,0,0x5}
        ,{"ntdll.dll","RtlConvertSidToUnicodeString",3},{"ntdll.dll","RtlEncodeSystemPointer",1},{"ntdll.dll","RtlDecodeSystemPointer",1}
        ,{"ntdll.dll","RtlGetFullPathName_UEx",5,0,0,0x2},{"ntdll.dll","RtlGetLongestNtPathLength",0}
        ,{"ntdll.dll","RtlDosPathNameToRelativeNtPathName_U",4},{"ntdll.dll","RtlDosPathNameToRelativeNtPathName_U_WithStatus",4},{"ntdll.dll","RtlReleaseRelativeName",1}
    };return table;
}
const WindowsServices::SystemCallTable& WindowsServices::system_call_table() const {
    // ntdll 的 NT 入口位址在程序內固定：第一次建立後快取。原本每次 syscall 都重新解析全部描述元的位址並排序，
    // Steam 的 GPU／瀏覽器程序取樣中占協調者可見時間。
    if(const auto* table=system_call_cache_.table.load(std::memory_order_acquire))return *table;
    static std::mutex build_lock;const std::lock_guard guard(build_lock);
    if(const auto* table=system_call_cache_.table.load(std::memory_order_acquire))return *table;
    std::map<std::uint64_t,const WindowsServiceDescriptor*> entries;
    for(const auto& descriptor:runtime_service_descriptors())
        if(descriptor.family=="ntdll.dll" && detail::nt_system_service(descriptor.symbol))
            entries.try_emplace(resolve("ntdll.dll",descriptor.symbol).value,&descriptor);
    // 系統呼叫編號＝NT 入口依位址排序的名次（從 0 起）。Windows 的 ntdll 依編號排列 stub，所以讀 stub 內 mov eax 的程式
    // 與自行把 Zw 匯出依位址排序推算編號的程式（防護層常見做法）得到同一個編號；兩種算法在這裡也必須一致。
    auto table=std::make_unique<SystemCallTable>();std::uint32_t rank=0;
    for(const auto& [target,descriptor]:entries){table->numbers.emplace(target,rank++);table->targets.push_back(target);table->descriptors.push_back(descriptor);}
    system_call_cache_.owner=std::move(table);system_call_cache_.table.store(system_call_cache_.owner.get(),std::memory_order_release);
    return *system_call_cache_.owner;
}
void WindowsServices::enter_process_instrumentation(GuestState& state){
    const auto callback=runtime_.instrumentation_callback;if(!callback)return;
    // 這是核心返回的轉向，並非一般函式呼叫：不推入返回位址、不改 RAX／旗標。
    // 客體負責保存暫存器及避免其回呼中的系統呼叫無限遞迴。
    if(x86_)state.reg(Gpr::Rcx)=static_cast<std::uint32_t>(state.rip.value);
    else {state.reg(Gpr::R10)=state.rip.value;state.reg(Gpr::Rcx)=callback;}
    state.rip=GuestAddress{callback};
}
std::optional<std::uint32_t> WindowsServices::finish_system_call(GuestState& state,GuestMemory& memory){
    auto& frames=threads_.items.at(threads_.current).system_calls;if(frames.empty())throw HostCallFault("NT 系統呼叫返回缺少框架");
    const auto& frame=frames.back();if(state.reg(Gpr::Rsp)!=frame.expected_stack)throw HostCallFault("NT 系統呼叫返回堆疊不符");
    if(frame.traced_call!=UINT32_MAX){
        const auto& calls=system_call_table();const auto symbol=frame.traced_call<calls.descriptors.size()?calls.descriptors[frame.traced_call]->symbol:std::string_view{"unknown"};
        std::fprintf(stderr,"[syscall_return] pid=%u thread=%llu name=%.*s pc=%llx status=%llx\n",process_id(),static_cast<unsigned long long>(threads_.current),int(symbol.size()),symbol.data(),static_cast<unsigned long long>(frame.resume),static_cast<unsigned long long>(state.reg(Gpr::Rax)&0xffffffffU));
    }
    if(x86_)memory.write(GuestAddress{frame.stack},frame.saved_memory);else memory.store(GuestAddress{frame.stack},64,frame.caller);
    state.reg(Gpr::Rsp)=frame.resume_stack;state.reg(Gpr::Rcx)=frame.resume_rcx;state.reg(Gpr::Rdx)=frame.resume_rdx;
    state.reg(Gpr::R11)=frame.resume_r11;state.rflags=frame.flags;state.rip=GuestAddress{frame.resume};
    const bool instrumented=frame.instrumented;frames.pop_back();
    if(instrumented)enter_process_instrumentation(state);return std::nullopt;
}
bool WindowsServices::dispatch_system_call(GuestState& state,GuestMemory& memory,SystemCallEntry entry){
    if((entry==SystemCallEntry::Syscall && x86_) || (entry==SystemCallEntry::Sysenter && !x86_))return false;
    GuestThread::SystemCallFrame frame;frame.resume=state.rip.value;frame.flags=state.rflags;
    frame.instrumented=runtime_.instrumentation_callback!=0;
    frame.resume_stack=state.reg(Gpr::Rsp);frame.resume_rcx=state.reg(Gpr::Rcx);
    frame.resume_rdx=state.reg(Gpr::Rdx);frame.resume_r11=state.reg(Gpr::R11);
    if(entry==SystemCallEntry::Sysenter){
        frame.resume=callback_return_.value+0xf0;frame.resume_stack=static_cast<std::uint32_t>(state.reg(Gpr::Rdx));
        frame.resume_rcx=frame.resume_stack;frame.resume_rdx=frame.resume;
    }
    const auto fail=[&](std::uint32_t status){
        state.reg(Gpr::Rax)=status;state.rip=GuestAddress{frame.resume};state.reg(Gpr::Rsp)=frame.resume_stack;
        state.reg(Gpr::Rcx)=frame.resume_rcx;state.reg(Gpr::Rdx)=frame.resume_rdx;
        if(frame.instrumented)enter_process_instrumentation(state);return true;
    };
    const auto id=static_cast<std::uint32_t>(state.reg(Gpr::Rax));const auto& calls=system_call_table();
    frame.call_id=id;
    const std::uint64_t target=id<calls.targets.size()?calls.targets[id]:0;
    const auto symbol=id<calls.descriptors.size()?calls.descriptors[id]->symbol:std::string_view{"unknown"};
    const bool tracing=trace_native_call(symbol) && trace_native_thread(threads_.current);if(tracing && target)frame.traced_call=id;
    if(tracing)std::fprintf(stderr,"[syscall_name] pid=%u thread=%llu name=%.*s\n",process_id(),static_cast<unsigned long long>(threads_.current),int(symbol.size()),symbol.data());
    if(tracing)std::fprintf(stderr,"[syscall] pid=%u pc=%llx id=%x target=%llx known=%u r10=%llx rdx=%llx r8=%llx r9=%llx\n",process_id(),static_cast<unsigned long long>(frame.resume),id,static_cast<unsigned long long>(target),unsigned(calls.numbers.contains(target)),static_cast<unsigned long long>(state.reg(Gpr::R10)),static_cast<unsigned long long>(state.reg(Gpr::Rdx)),static_cast<unsigned long long>(state.reg(Gpr::R8)),static_cast<unsigned long long>(state.reg(Gpr::R9)));
    if(tracing && !x86_)try{const auto sp=frame.resume_stack;std::fprintf(stderr,"[syscall_stack] a5=%llx a6=%llx a7=%llx a8=%llx a9=%llx a10=%llx\n",static_cast<unsigned long long>(memory.load(GuestAddress{sp+0x28},64)),static_cast<unsigned long long>(memory.load(GuestAddress{sp+0x30},64)),static_cast<unsigned long long>(memory.load(GuestAddress{sp+0x38},64)),static_cast<unsigned long long>(memory.load(GuestAddress{sp+0x40},64)),static_cast<unsigned long long>(memory.load(GuestAddress{sp+0x48},64)),static_cast<unsigned long long>(memory.load(GuestAddress{sp+0x50},64)));
        // 指標參數指向的值（基址、大小、偏移等輸入輸出參數），無法讀取時略過。
        const auto deref=[&](const char* name,std::uint64_t pointer){try{if(pointer>=0x10000)std::fprintf(stderr," *%s=%llx",name,static_cast<unsigned long long>(memory.load(GuestAddress{pointer},64)));}catch(const MemoryFault&){}};
        std::fprintf(stderr,"[syscall_deref]");deref("rdx",state.reg(Gpr::Rdx));deref("r8",state.reg(Gpr::R8));deref("r9",state.reg(Gpr::R9));
        deref("a6",memory.load(GuestAddress{sp+0x30},64));deref("a7",memory.load(GuestAddress{sp+0x38},64));std::fprintf(stderr,"\n");}catch(const MemoryFault&){}
    if(tracing){
        // 與一般 API 追蹤相同的指定地址觀測；可選擇再讀一次指標所指的 64 bytes。
        // 僅由使用者明列的診斷地址觸發，不掃描資料區，也不改寫客體內容。
        static const auto observed_address=[]()->std::optional<std::uint64_t>{
            const auto* setting=std::getenv("X64ARM_TRACE_MEMORY_ADDRESS");if(!setting || !*setting)return std::nullopt;
            std::string_view text{setting};if(text.starts_with("0x") || text.starts_with("0X"))text.remove_prefix(2);
            std::uint64_t value{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,16);
            if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())return std::nullopt;
            return value;
        }();
        if(observed_address)try{
            const auto value=memory.load(GuestAddress{*observed_address},x86_?32:64);
            std::fprintf(stderr,"[syscall_observe] pid=%u thread=%llu address=%llx value=%llx\n",process_id(),static_cast<unsigned long long>(threads_.current),static_cast<unsigned long long>(*observed_address),static_cast<unsigned long long>(value));
            const auto* pointer=std::getenv("X64ARM_TRACE_MEMORY_POINTER");
            if(value && pointer && std::string_view(pointer)=="1"){
                std::array<std::uint8_t,64> bytes{};memory.read(GuestAddress{value},bytes);
                std::fprintf(stderr,"[syscall_observe_pointer] pid=%u thread=%llu address=%llx bytes=",process_id(),static_cast<unsigned long long>(threads_.current),static_cast<unsigned long long>(value));
                for(const auto byte:bytes)std::fprintf(stderr,"%02x",byte);
                std::fprintf(stderr,"\n");
            }
        }catch(const MemoryFault&){}
    }
    // 與 API 追蹤共用既有 AMD64 unwind 語義；在暫借 syscall 返回槽之前
    // 讀取原始呼叫鏈，避免診斷只看見 internal callback。僅觀測副本，
    // 不改客體暫存器、堆疊或例外狀態。非標準 syscall stub 無法直接 unwind 時，
    // 另列有限範圍內、緊接 CALL 指令的堆疊候選；候選鏈不是已證實的呼叫鏈。
    if(tracing && !x86_ && std::getenv("X64ARM_TRACE_SERVICE_UNWIND")){
        try {
            const auto low=memory.load(GuestAddress{state.gs_base+16},64);
            const auto high=memory.load(GuestAddress{state.gs_base+8},64);
            const auto* trace_code=std::getenv("X64ARM_TRACE_SERVICE_CODE");
            const auto* register_setting=std::getenv("X64ARM_TRACE_SERVICE_REGISTERS");
            const bool trace_registers=register_setting && *register_setting && std::string_view(register_setting)!="0";
            std::array<std::uint64_t,16> code_ranges{};
            unsigned code_count=0;std::size_t code_bytes=0;
            const auto walk_chain=[&](GuestState walk,unsigned candidate){
                for(unsigned depth=0;depth<24 && walk.rip.value;++depth){
                    const auto sp=walk.reg(Gpr::Rsp);
                    if(sp<low || sp>=high || handles(walk.rip))break;
                    const auto* image=module_image(walk.rip.value,true);
                    if(!image){std::fprintf(stderr,"[syscall_unwind_stop] pid=%u thread=%llu candidate=%u pc=%llx reason=unmapped_image\n",process_id(),static_cast<unsigned long long>(threads_.current),candidate,static_cast<unsigned long long>(walk.rip.value));break;}
                    memory.check(walk.rip,1,Access::Execute);
                    std::fprintf(stderr,"[%s] pid=%u thread=%llu candidate=%u depth=%u pc=%llx sp=%llx module=%s offset=%llx\n",candidate?"syscall_candidate_unwind":"syscall_unwind",process_id(),static_cast<unsigned long long>(threads_.current),candidate,depth,static_cast<unsigned long long>(walk.rip.value),static_cast<unsigned long long>(sp),utf16_to_utf8(image->path).c_str(),static_cast<unsigned long long>(walk.rip.value-image->base));
                    // 只列出 unwind 可恢復的非揮發暫存器；不把揮發暫存器誤稱為上層參數。
                    if(trace_registers)std::fprintf(stderr,"[syscall_unwind_registers] pid=%u thread=%llu candidate=%u depth=%u rbx=%llx rbp=%llx rsi=%llx rdi=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
                        process_id(),static_cast<unsigned long long>(threads_.current),candidate,depth,
                        static_cast<unsigned long long>(walk.reg(Gpr::Rbx)),static_cast<unsigned long long>(walk.reg(Gpr::Rbp)),
                        static_cast<unsigned long long>(walk.reg(Gpr::Rsi)),static_cast<unsigned long long>(walk.reg(Gpr::Rdi)),
                        static_cast<unsigned long long>(walk.reg(Gpr::R12)),static_cast<unsigned long long>(walk.reg(Gpr::R13)),
                        static_cast<unsigned long long>(walk.reg(Gpr::R14)),static_cast<unsigned long long>(walk.reg(Gpr::R15)));
                    // 動態程式碼可能與磁碟映像不同；只讀目前函式的可執行範圍，
                    // 去重且限制單函式 32 KiB、單次 64 KiB／16 函式，不擷取資料區。
                    if(trace_code && *trace_code && std::string_view(trace_code)!="0" && code_count<code_ranges.size()){
                        if(const auto* function=lookup_function(*image,walk.rip.value)){
                            const auto begin=image->base+function->begin;
                            const auto size=std::uint64_t{function->end}-function->begin;
                            if(function->end>function->begin && size<=32768 && code_bytes+size<=65536 &&
                               std::find(code_ranges.begin(),code_ranges.begin()+code_count,begin)==code_ranges.begin()+code_count){
                                try {
                                    memory.check(GuestAddress{begin},static_cast<std::size_t>(size),Access::Execute);
                                    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{begin},bytes);
                                    std::fprintf(stderr,"[syscall_unwind_code] pid=%u thread=%llu begin=%llx size=%llu bytes=",process_id(),static_cast<unsigned long long>(threads_.current),static_cast<unsigned long long>(begin),static_cast<unsigned long long>(size));
                                    for(const auto byte:bytes)std::fprintf(stderr,"%02x",byte);
                                    std::fprintf(stderr,"\n");code_ranges[code_count++]=begin;code_bytes+=size;
                                }catch(const MemoryFault&){}
                            }
                        }
                    }
                    walk=virtual_unwind(memory,*image,walk.rip.value,walk,0).caller;
                    if(walk.reg(Gpr::Rsp)<=sp)break;
                }
            };
            auto walk=state;
            if(walk.reg(Gpr::Rsp)<low || walk.reg(Gpr::Rsp)>=high || high-walk.reg(Gpr::Rsp)<8)throw HostCallFault("診斷堆疊範圍無效");
            walk.rip=GuestAddress{memory.load(GuestAddress{walk.reg(Gpr::Rsp)},64)};
            walk.reg(Gpr::Rsp)+=8;
            const bool direct_image=module_image(walk.rip.value,true)!=nullptr;
            walk_chain(walk,0);
            if(!direct_image){
                unsigned candidates=0;
                const auto stack=frame.resume_stack;
                for(unsigned slot=0;slot<64 && candidates<8 && high-stack>=std::uint64_t{slot+1}*8;++slot){
                    const auto address=stack+std::uint64_t{slot}*8;
                    const auto pc=memory.load(GuestAddress{address},64);
                    const auto* image=module_image(pc,true);
                    if(!image || pc<15)continue;
                    try {
                        memory.check(GuestAddress{pc},1,Access::Execute);
                        bool call=false;
                        for(unsigned length=2;length<=15 && pc-image->base>=length;++length){
                            const auto decoded=Decoder{}.decode_at(memory,GuestAddress{pc-length});
                            if(decoded.status==DecodeStatus::Ok && decoded.instruction.length==length && decoded.instruction.opcode==Opcode::Call){call=true;break;}
                        }
                        if(!call)continue;
                        ++candidates;
                        std::fprintf(stderr,"[syscall_stack_candidate] pid=%u thread=%llu candidate=%u slot=%u address=%llx pc=%llx confidence=call_suffix_only\n",process_id(),static_cast<unsigned long long>(threads_.current),candidates,slot,static_cast<unsigned long long>(address),static_cast<unsigned long long>(pc));
                        auto candidate=state;candidate.rip=GuestAddress{pc};candidate.reg(Gpr::Rsp)=address+8;
                        walk_chain(candidate,candidates);
                    }catch(const MemoryFault&){}catch(const HostCallFault&){}
                }
            }
        }catch(const MemoryFault&){}catch(const HostCallFault&){}
    }
    if(!target)return fail(0xc000001cU);
    auto& frames=threads_.items.at(threads_.current).system_calls;
    if(x86_){
        const WindowsServiceDescriptor* const descriptor=calls.descriptors[id];
        if(!descriptor)throw HostCallFault("NT 系統呼叫描述元缺失");
        const auto slots=descriptor->arguments+static_cast<unsigned>(std::popcount(descriptor->wide_arguments))+
            3U*static_cast<unsigned>(std::popcount(descriptor->aggregate_arguments));
        const auto size=(std::uint64_t{slots}+1)*4;
        // SYSENTER 不保存／使用原 ESP；Windows 由 EDX 提供使用者返回框架。
        const auto sp=static_cast<std::uint32_t>(frame.resume_stack);
        const auto arguments=std::uint64_t{static_cast<std::uint32_t>(state.reg(Gpr::Rdx))}+(entry==SystemCallEntry::Sysenter?8U:0U);
        if(sp<size || arguments>UINT64_C(0x100000000)-slots*4U)return fail(0xc0000005U);
        frame.stack=(sp-size)&~UINT64_C(15);frame.expected_stack=frame.stack+4+(descriptor->caller_cleanup?0:slots*4U);
        try{
            std::vector<std::uint8_t> values(slots*4U);memory.read(GuestAddress{arguments},values);
            frame.saved_memory.resize(size);memory.read(GuestAddress{frame.stack},frame.saved_memory);
            memory.check(GuestAddress{frame.stack},size,Access::Write);
            // 一次複製參數，回呼與等待沿用既有 NT API；返回時恢復暫借的使用者堆疊。
            std::vector<std::uint8_t> outgoing(size);const auto callback=callback_return_.value+0xe0;
            for(unsigned n=0;n<4;++n)outgoing[n]=static_cast<std::uint8_t>(callback>>(n*8));
            std::copy(values.begin(),values.end(),outgoing.begin()+4);
            memory.write(GuestAddress{frame.stack},outgoing);
        }catch(const MemoryFault&){return fail(0xc0000005U);}
        frames.push_back(std::move(frame));
        state.reg(Gpr::Rsp)=frames.back().stack;
    }else{
        frame.stack=state.reg(Gpr::Rsp);frame.expected_stack=frame.stack+8;
        memory.check(GuestAddress{frame.stack},8,Access::Write);frame.caller=memory.load(GuestAddress{frame.stack},64);
        frames.push_back(frame);memory.store(GuestAddress{frame.stack},64,callback_return_.value+0xe0);
        state.reg(Gpr::Rcx)=state.reg(Gpr::R10);
    }
    state.rip=GuestAddress{target};return true;
}
bool WindowsServices::software_interrupt_enabled(std::uint8_t vector) const noexcept {
    return vector==3 || vector==4 || (vector>=0x29 && vector<=0x2e);
}
std::optional<ExecutionResult> WindowsServices::dispatch_software_interrupt(GuestState& state,GuestMemory& memory,std::uint8_t vector,unsigned length){
    if(vector==0x2e){if(dispatch_system_call(state,memory,SystemCallEntry::Interrupt))return std::nullopt;}
    if(vector==0x29){
        // Fast-fail 是第二次機會、不可繼續的程序失敗，不能進入一般 SEH／VEH。
        state.rip.value=(state.rip.value-length)&(x86_?UINT32_MAX:UINT64_MAX);
        // 終止前記錄 fast-fail 代碼（RCX／ECX）與位址：多核平行模式下的 0xC0000409 需要知道是 stack cookie、
        // guard ICall 還是其他檢查失敗；一次性輸出，不影響正常路徑。
        {const auto code=static_cast<std::uint32_t>(state.reg(Gpr::Rcx));const auto* image=module_image(state.rip.value,true);
         std::fprintf(stderr,"fast_fail pid=%u code=%u rip=0x%llx module=%s offset=0x%llx thread=%llu\n",process_id(),code,static_cast<unsigned long long>(state.rip.value),
             image?utf16_to_utf8(image->path).c_str():"?",static_cast<unsigned long long>(image?state.rip.value-image->base:0),static_cast<unsigned long long>(threads_.current));}
        (void)emit_debug_event(state,memory,debugger::Exception{0xc0000409U,1,0,state.rip.value,{static_cast<std::uint32_t>(state.reg(Gpr::Rcx))},false});
        record_job_exit(0xc0000409U);(void)emit_debug_event(state,memory,debugger::ExitProcess{0xc0000409U});
        ExecutionResult result;result.reason=StopReason::Exited;result.exit_code=0xc0000409U;result.rip=state.rip;return result;
    }
    if(vector==0x2c){
        state.rip.value=(state.rip.value-length)&(x86_?UINT32_MAX:UINT64_MAX);
        if(dispatch_cpu_exception(state,memory,0xc0000420U))return std::nullopt;
        ExecutionResult result;result.reason=StopReason::ArithmeticFault;result.rip=state.rip;result.message="未處理的 Windows assertion trap";return result;
    }
    return HostServices::dispatch_software_interrupt(state,memory,vector,length);
}
bool nt_object_service(std::string_view symbol);
bool nt_continuation_service(std::string_view symbol);
bool nt_family_service(std::string_view symbol);
bool rtl_family_service(std::string_view symbol);
bool ntdll_loader_service(std::string_view symbol);
std::optional<std::uint32_t> WindowsServices::invoke_nt_native(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="KiUserExceptionDispatcher")return finish_user_exception_entry(state,memory);
    if(service.symbol.ends_with("ReadFileScatter") || service.symbol.ends_with("WriteFileGather"))return invoke_file_segments(state,memory,service);
    if(service.symbol=="KiUserApcDispatcher")return finish_user_apc_entry(state,memory);
    if(service.family=="internal-apc-dispatch"){
        if(runtime_.callbacks.empty() || !native_apc_continuation(runtime_.callbacks.back().continuation))
            throw HostCallFault("APC 配接入口缺少回呼框架");
        enter_callback(state,memory);return std::nullopt;
    }
    if(service.family=="internal-file-apc"){
        const WindowsCall arg(state,memory,service);const auto target=arg(0),ios=arg(1);const auto saved_error=last_error_;
        const auto status=static_cast<std::uint32_t>(memory.load(GuestAddress{ios},x86_?32:64));
        const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{ios+(x86_?4:8)},x86_?32:64));
        const auto error=nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status});last_error_=saved_error;
        // NT APC 與 Win32 completion 都是三參數 stdcall／Win64 ABI。
        // 尾呼叫保留原回程框架及 alertable 執行緒，不配置 thunk 或新增常駐索引。
        if(x86_){memory.store(GuestAddress{state.reg(Gpr::Rsp)+4},32,error);memory.store(GuestAddress{state.reg(Gpr::Rsp)+8},32,count);memory.store(GuestAddress{state.reg(Gpr::Rsp)+12},32,ios);}
        else{state.reg(Gpr::Rcx)=error;state.reg(Gpr::Rdx)=count;state.reg(Gpr::R8)=ios;}
        state.rip=GuestAddress{target};return std::nullopt;
    }
    if(service.family=="internal-nt-event")return invoke_compatibility(state,memory,service);
    if(service.family=="internal-nt-mapping")return invoke_mapping(state,memory,service);
    if(service.family=="internal-nt-file") {
        if(service.symbol=="CreateFileW")return invoke_file_open(state,memory,service);
        if(service.symbol=="CloseHandle")return invoke_file_close(state,memory,service);
        if(service.symbol=="CancelIo" || service.symbol=="CancelIoEx")return invoke_file_io(state,memory,service);
        return invoke_file_readwrite(state,memory,service);
    }
    if(object_namespace_service(service.symbol))return invoke_object_namespace(state,memory,service);
    if(nt_object_service(service.symbol))return invoke_nt_object(state,memory,service);
    if(nt_continuation_service(service.symbol))return invoke_nt_continuation(state,memory,service);
    if(nt_family_service(service.symbol))return invoke_nt_family(state,memory,service);
    if(rtl_family_service(service.symbol))return invoke_rtl_family(state,memory,service);
    if(ntdll_loader_service(service.symbol))return invoke_ntdll_loader(state,memory,service);
    if(service.symbol=="NtLockFile" || service.symbol=="NtUnlockFile")return invoke_file_lock(state,memory,service);
    if(service.symbol=="NtSuspendThread" || service.symbol=="NtResumeThread" || service.symbol.ends_with("tContextThread"))return invoke_thread_control(state,memory,service);
    const WindowsCall call(state,memory,service);const auto name=service.symbol;const auto saved_error=last_error_;const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t status){if(name.starts_with("Nt"))threads_.items.at(threads_.current).last_native_status=static_cast<std::uint32_t>(status);last_error_=saved_error;call.return_to(state,status);return std::optional<std::uint32_t>{};};
    // Microsoft 明訂：未啟用 Application Verifier 時為 no-op；不可釋放、移轉或改變配置內容。
    if(name=="RtlDisownModuleHeapAllocation")return finish(0);
    const auto failure=[&](std::uint32_t status){return finish(name=="RtlDosPathNameToNtPathName_U" || name=="RtlGetFullPathName_U" || name=="RtlDetermineDosPathNameType_U" || name=="RtlIsDosDeviceName_U"?0:status);};
    NativeOperation op;op.symbol=name;op.saved_error=saved_error;for(unsigned i=0;i<service.arguments;++i)op.arguments[i]=call(i);
    const auto start=[&](std::string_view family,std::string_view symbol,std::vector<std::uint64_t> arguments){
        const auto id=next_native_operation_++;native_operations_.emplace(id,op);start_callbacks(state,memory,{{resolve(family,symbol).value,std::move(arguments)}},Continuation::NativeOperation,id);return std::optional<std::uint32_t>{};
    };
    const auto scratch=[&](std::size_t size){op.scratch=allocate_heap(memory,size);if(!op.scratch)throw NativeError{0xc0000017U};memory.write(GuestAddress{op.scratch},std::vector<std::uint8_t>(size));return op.scratch;};
    const auto security=[&](const ObjectAttributes& attr,std::uint64_t at){memory.store(GuestAddress{at},32,x86_?12U:24U);memory.store(GuestAddress{at+unit},bits,attr.security);memory.store(GuestAddress{at+unit*2},32,(attr.flags&2U)!=0);return at;};
    const auto path_string=[&](std::uint64_t pointer){try{return read_utf16(memory,GuestAddress{pointer},32768);}catch(const std::invalid_argument&){throw NativeError{0xc0000033U};}};
    const auto path=[&](ObjectAttributes attr){
        auto value=std::move(attr.name);if(value.find(u'\0')!=value.npos || value.empty())throw NativeError{0xc0000033U};
        if(attr.root){const auto file=files_.find(attr.root);if(file==files_.end())throw NativeError{object_type(attr.root).empty()?0xc0000008U:0xc0000024U};if(!(file->second.metadata.attributes&0x10U))throw NativeError{0xc0000024U};if(value.front()==u'\\' || value.find(u':')!=value.npos)throw NativeError{0xc0000033U};value=utf8_to_utf16(file->second.path)+u"\\"+value;}
        else if(value.size()==12 && std::equal(value.begin(),value.end(),u"\\Device\\Null",[](auto a,auto b){return windows_path_upper(a)==windows_path_upper(b);}))value=u"\\\\.\\NUL";
        else if(value.starts_with(u"\\??\\") || value.starts_with(u"\\DosDevices\\")){
            value.erase(0,value.starts_with(u"\\??\\")?4:12);
            if(value.size()>=4 && windows_path_upper(value[0])==u'U' && windows_path_upper(value[1])==u'N' && windows_path_upper(value[2])==u'C' && value[3]==u'\\')value=u"\\\\"+value.substr(4);
            else if(windows_dos_path_type(value)!=WindowsDosPathType::DriveAbsolute){
                const auto volume=u"\\\\?\\"+value;
                if(windows_volume_letter(std::u16string_view(volume)) || detail::volume_device_path(volume).volume)value=volume;
                else value=u"\\\\.\\"+value; // DOS 裝置（含管道）交回既有共用 provider 判定。
            }
        }
        else if(auto resolved=detail::globalroot_file_path(std::u16string_view(value),true);!resolved.empty())value=std::move(resolved);
        else if(value.size()>=8 && std::equal(value.begin(),value.begin()+8,u"\\Device\\",[](auto a,auto b){return windows_path_upper(a)==windows_path_upper(b);})){ 
            // 原生裝置名稱與 DOS／GLOBALROOT 入口共用 provider；未登錄裝置由該 provider 回報不存在。
            const auto component=value.substr(8,value.find(u'\\',8)==value.npos?value.npos:value.find(u'\\',8)-8);
            auto folded=component;for(auto& c:folded)c=windows_path_upper(c);
            if(folded==u"NAMEDPIPE" || folded==u"MAILSLOT")value=u"\\\\.\\"+(folded==u"NAMEDPIPE"?std::u16string(u"pipe"):std::u16string(u"mailslot"))+value.substr(8+component.size());
            else value=u"\\\\.\\GLOBALROOT"+value;
        }
        else if(value.size()<3 || value[1]!=u':')throw NativeError{0xc000003bU};
        return value;
    };
    try{
#include "windows_nt_directory_api.inc"
#include "windows_wait_completion_native.inc"
#include "concrt_sync_handlers.inc"
#include "concrt_lock_handlers.inc"
#include "concrt_objects.inc"
#include "concrt_scheduler_handlers.inc"
#include "concrt_task_collection.inc"
#include "msvcp_sync_handlers.inc"
        if(name=="CoWaitForMultipleHandles" || name=="CoWaitForMultipleObjects"){
            // COWAIT_*：WAITALL 1、ALERTABLE 2、INPUTAVAILABLE 4、DISPATCH_CALLS 8、DISPATCH_WINDOW_MESSAGES 0x10；
            // CWMO_*（CoWaitForMultipleObjects）：DISPATCH_CALLS 1、DISPATCH_WINDOW_MESSAGES 2。
            const bool legacy=name=="CoWaitForMultipleHandles";const auto flags=static_cast<std::uint32_t>(call(0)),count=static_cast<std::uint32_t>(call(2));
            if(!call(4) || (count && !call(3)) || (legacy?(flags&~0x1fU):(flags&~3U)))return finish(0x80070057U);
            if(!count)return finish(0x80010120U); // RPC_E_NO_SYNC：沒有可等待的控制代碼。
            memory.check(GuestAddress{call(4)},4,Access::Write);
            // STA 的套間值是擁有執行緒 ID（MTA 為 0、未初始化為空）。STA 等待期間由等待服務處理 SendMessage 與跨套間呼叫；
            // 要求處理視窗訊息時，有輸入就取出並分派一則訊息後以剩餘時間重新等待。WAITALL 改為只等控制代碼（MsgWait 的 WAITALL 還要求輸入）。
            const auto apartment=com_apartment(threads_.current);const bool sta=apartment && *apartment;
            const bool waitall=legacy && (flags&1U),alertable=legacy && (flags&2U),input=legacy && (flags&4U);
            const bool pump=sta && !waitall && (legacy?(flags&0x10U)!=0:(flags&2U)!=0);
            if(count>(sta && !waitall?63U:64U))return finish(0x80070057U);
            op.attributes=(sta && !waitall?1U:0U)|(pump?2U:0U)|(waitall?4U:0U)|(alertable?8U:0U)|(input?16U:0U);
            const auto timeout=static_cast<std::uint32_t>(call(1));op.allocation=timeout==UINT32_MAX?UINT64_MAX:runtime_.clock_ticks+std::uint64_t{timeout}*10000;
            if(pump)scratch(x86_?28U:48U); // MSG
            if(op.attributes&1U)return start("user32.dll","MsgWaitForMultipleObjectsEx",{count,call(3),timeout,pump || input?0x4ffU:0x40U,(alertable?2U:0U)|(input?4U:0U)});
            return start("kernel32.dll","WaitForMultipleObjectsEx",{count,call(3),waitall?1U:0U,timeout,alertable?1U:0U});
        }
        if(name=="RtlDetermineDosPathNameType_U" || name=="RtlIsDosDeviceName_U"){
            if(!call(0))throw NativeError{0xc000000dU};
            const auto input=path_string(call(0));
            return finish(name=="RtlDetermineDosPathNameType_U"?static_cast<std::uint32_t>(windows_dos_path_type(input)):windows_dos_device(input));
        }
        if(name=="RtlGetFullPathName_U" || name=="RtlDosPathNameToNtPathName_U" || name=="RtlDosPathNameToNtPathName_U_WithStatus"){
            const bool full=name=="RtlGetFullPathName_U";
            if(!call(0))throw NativeError{0xc0000033U};
            const auto input=path_string(call(0));
            if(input.empty())throw NativeError{0xc0000033U};
            // Microsoft 公開的 DOS→NT 契約將第四參數保留；不假造目前目錄參照物件。
            if(!full && call(3))throw NativeError{0xc000000dU};
            const auto part_pointer=call(full?3:2);
            if(part_pointer)memory.check(GuestAddress{part_pointer},unit,Access::Write);
            const bool literal=!full && (input.starts_with(u"\\\\?\\") || (input.starts_with(u"\\??\\") && input.size()>4) || (input.starts_with(u"\\\\.\\") && windows_dos_device(input)==0x00080006U));
            std::u16string result;
            bool device=false;
            try{
                if(literal)result=u"\\??\\"+input.substr(4);
                else {
                    const auto dos_device=windows_dos_device(input);device=dos_device!=0;
                    if(!full && (dos_device>>16)){
                        const auto parent=full_windows_environment_path(input.substr(0,(dos_device>>16)/2),runtime_.current_directory,runtime_.environment);
                        const auto info=filesystem_.information(utf16_to_utf8(parent));
                        if(info.error || !(info.metadata.attributes&0x10U))throw NativeError{0xc0000033U};
                    }
                    result=full_windows_environment_path(input,runtime_.current_directory,runtime_.environment);
                    if(!full){
                        const auto type=windows_dos_path_type(result);
                        if(type==WindowsDosPathType::UncAbsolute)result=u"\\??\\UNC\\"+result.substr(2);
                        else if(type==WindowsDosPathType::LocalDevice)result=u"\\??\\"+result.substr(4);
                        else result=u"\\??\\"+result;
                    }
                }
            }catch(const std::invalid_argument&){throw NativeError{0xc0000033U};}
            if(result.size()>32766)throw NativeError{0xc0000033U};
            const auto bytes=utf16_bytes(result);const auto part=windows_path_file_part(result,device);
            if(full){
                if(part_pointer)memory.store(GuestAddress{part_pointer},bits,0);
                if(static_cast<std::uint32_t>(call(1))<bytes.size())return finish(bytes.size());
                memory.write(GuestAddress{call(2)},bytes);
                if(part_pointer && part!=result.npos)memory.store(GuestAddress{part_pointer},bits,call(2)+part*2);
                return finish(bytes.size()-2);
            }
            memory.check(GuestAddress{call(1)},unit*2,Access::Write);
            const auto buffer=scratch(bytes.size());memory.write(GuestAddress{buffer},bytes);
            memory.write(GuestAddress{call(1)},std::vector<std::uint8_t>(unit*2));
            memory.store(GuestAddress{call(1)},16,bytes.size()-2);memory.store(GuestAddress{call(1)+2},16,bytes.size());
            memory.store(GuestAddress{call(1)+unit},bits,buffer);
            if(part_pointer)memory.store(GuestAddress{part_pointer},bits,part==result.npos?0:buffer+part*2);
            op.scratch=0;return finish(name=="RtlDosPathNameToNtPathName_U"?1:0);
        }
        if(name=="NtQueryMutant"){
            // MUTANT_BASIC_INFORMATION：CurrentCount=1-遞迴次數、OwnedByCaller、AbandonedState；MUTANT_OWNER_INFORMATION：擁有者 CLIENT_ID。
            const auto handle=runtime_.wait_objects.find(call(0));
            if(handle==runtime_.wait_objects.end())return finish(object_type(call(0)).empty()?0xc0000008U:0xc0000024U);
            if(handle->second.kind!=WaitObject::Kind::Mutex)return finish(0xc0000024U);
            if(!(handle->second.access&1U))return finish(0xc0000022U);
            const auto info=static_cast<std::uint32_t>(call(1)),length=static_cast<std::uint32_t>(call(3));
            if(info>1)return finish(0xc0000003U);const auto required=info?2U*unit:8U;if(length!=required)return finish(0xc0000004U);
            const auto& mutex=runtime_.mutex_objects.at(handle->second.mutex);
            std::uint64_t process=0,thread=0;std::uint32_t recursion=0;bool abandoned=mutex.abandoned,caller=false;
            if(mutex.shared){SharedEventLock lock(mutex.shared->lock_descriptor());const auto value=mutex.shared->state_locked();
                // 其他程序持有時只能取得該程序的執行緒鍵，與本程序公開 TID 的轉換無法跨程序重建。
                const bool local=mutex.shared->owned();process=value.process;recursion=value.recursion;abandoned=value.abandoned;caller=local && value.thread==threads_.current;thread=local?thread_identifier(value.thread):static_cast<std::uint32_t>(value.thread);}
            else if(mutex.recursion){process=process_id();recursion=mutex.recursion;caller=mutex.owner==threads_.current;thread=thread_identifier(mutex.owner);}
            memory.check(GuestAddress{call(2)},required,Access::Write);
            if(!info){memory.store(GuestAddress{call(2)},32,static_cast<std::uint32_t>(1-static_cast<std::int64_t>(recursion)));memory.store(GuestAddress{call(2)+4},8,caller);memory.store(GuestAddress{call(2)+5},8,abandoned);memory.store(GuestAddress{call(2)+6},16,0);}
            else {memory.store(GuestAddress{call(2)},bits,recursion?process:0);memory.store(GuestAddress{call(2)+unit},bits,recursion?thread:0);}
            if(call(4))memory.store(GuestAddress{call(4)},32,required);
            return finish(0);
        }
        if(name=="RtlRunOnceExecuteOnce"){
            memory.check(GuestAddress{call(0)},unit,Access::Write);
            if(call(3))memory.check(GuestAddress{call(3)},unit,Access::Write);
            // 使用相同 INIT_ONCE 狀態與回呼／排程流程，避免 NT 與 Win32 分裂。
            last_error_=0;
            return start("kernel32.dll","InitOnceExecuteOnce",{call(0),call(1),call(2),call(3)});
        }
        if(name=="RtlAreLongPathsEnabled")return finish(0); // 預設登錄 LongPathsEnabled=0。
        if(name=="RtlGetUnloadEventTrace" || name=="RtlGetUnloadEventTraceEx"){
            // 卸載事件陣列放在客體 heap：第一次查詢時配置並填入已記錄的事件，之後由 unmap_module 逐筆更新（見 RuntimeState::unload_events）。
            // Ex 版回傳三個「指向變數的指標」：元素大小、元素個數與陣列指標；舊版直接回傳陣列。
            if(!runtime_.unload_trace_block){
                const auto bytes=std::uint64_t{x86_?12U:16U}+std::uint64_t{RuntimeState::unload_event_capacity}*RuntimeState::unload_event_bytes(x86_);
                const auto block=allocate_heap(memory,bytes);if(!block)throw NativeError{0xc0000017U};
                memory.write(GuestAddress{block},std::vector<std::uint8_t>(static_cast<std::size_t>(bytes)));
                runtime_.unload_trace_block=block;
                memory.store(GuestAddress{block},32,RuntimeState::unload_event_bytes(x86_));memory.store(GuestAddress{block+4},32,RuntimeState::unload_event_capacity);
                memory.store(GuestAddress{block+8},bits,runtime_.unload_trace_array(x86_));
                for(const auto& event:runtime_.unload_events)runtime_.write_unload_event(memory,x86_,event);
            }
            if(name=="RtlGetUnloadEventTrace")return finish(runtime_.unload_trace_array(x86_));
            if(call(0))memory.store(GuestAddress{call(0)},bits,runtime_.unload_trace_block);
            if(call(1))memory.store(GuestAddress{call(1)},bits,runtime_.unload_trace_block+4);
            if(call(2))memory.store(GuestAddress{call(2)},bits,runtime_.unload_trace_block+8);
            return finish(0);
        }
        if(name=="LdrRegisterDllNotification" || name=="LdrUnregisterDllNotification"){
            // 與映射、通知、卸載共用 loader ownership；等待後重新進入，不提前返回成功。
            if(await_loader(state,memory))return std::nullopt;
            if(name=="LdrUnregisterDllNotification")
                return finish(runtime_.dll_notifications.erase(call(0))?0:0xc0000135U);
            if(static_cast<std::uint32_t>(call(0)) || !call(1) || !call(3))return finish(0xc000000dU);
            memory.check(GuestAddress{call(3)},unit,Access::Write);
            // Cookie 只供識別，不能被當作客體指標；不重用已取消的身分。
            const auto cookie=runtime_.next_dll_notification_cookie;
            if(!cookie || cookie>(x86_?UINT32_MAX:UINT64_MAX))return finish(0xc0000017U);
            runtime_.dll_notifications.emplace(cookie,DllNotificationRegistration{call(1),call(2)});
            ++runtime_.next_dll_notification_cookie;
            memory.store(GuestAddress{call(3)},bits,cookie);return finish(0);
        }
        if(name=="LdrGetDllHandle"){
            // (PathToFile, Flags, ModuleFileName, *ModuleHandle)：只查詢已載入模組，不載入。
            if(!call(2) || !call(3))return finish(0xc000000dU);
            const auto handle=find_module(native_string(memory,call(2),x86_));
            if(!handle)return finish(0xc0000135U);
            memory.check(GuestAddress{call(3)},unit,Access::Write);memory.store(GuestAddress{call(3)},bits,handle);return finish(0);
        }
        if(name=="LdrLoadDll"){
            // (PathToFile, Flags, ModuleFileName, *ModuleHandle)：以 LoadLibraryExW 流程載入（含 DllMain 排程），完成後寫回 handle。
            if(!call(2) || !call(3))return finish(0xc000000dU);
            const auto module_name=native_string(memory,call(2),x86_);if(module_name.empty())return finish(0xc000000dU);
            memory.check(GuestAddress{call(3)},unit,Access::Write);
            const auto encoded=utf16_bytes(module_name);const auto buffer=scratch(encoded.size()+2);memory.write(GuestAddress{buffer},encoded);
            return start("kernel32.dll","LoadLibraryExW",{buffer,0,0});
        }
        if(name=="LdrGetProcedureAddress"){
            // (ModuleHandle, PANSI_STRING Name, Ordinal, *Address)：名稱優先，否則依序數。
            if(!call(0) || !call(3))return finish(0xc000000dU);
            memory.check(GuestAddress{call(3)},unit,Access::Write);
            std::uint64_t lookup=call(2)&0xffffU;
            if(call(1)){
                const auto length=memory.load(GuestAddress{call(1)},16),source=memory.load(GuestAddress{call(1)+unit},bits);
                if(length && !source)return finish(0xc000000dU);
                std::vector<std::uint8_t> bytes;for(std::uint64_t n=0;n<length;++n)bytes.push_back(static_cast<std::uint8_t>(memory.load(GuestAddress{source+n},8)));
                lookup=scratch(bytes.size()+1);memory.write(GuestAddress{lookup},bytes);
            }
            return start("kernel32.dll","GetProcAddress",{call(0),lookup});
        }
        if(name=="LdrUnloadDll"){if(!call(0))return finish(0xc000000dU);return start("kernel32.dll","FreeLibrary",{call(0)});}
        if(name=="LdrLockLoaderLock" || name=="LdrUnlockLoaderLock"){
            const auto flags=static_cast<std::uint32_t>(call(0));
            const auto loader_error=[&](std::uint32_t status){
                if(flags&1U){auto caller=state;call.return_to(caller,status);begin_exception(caller,memory,status,0,{});state=caller;return std::optional<std::uint32_t>{};}
                return finish(status);
            };
            if(name=="LdrUnlockLoaderLock"){
                if(flags&~1U)return loader_error(0xc00000efU);
                if(!call(1))return finish(0);
                const auto cookie=runtime_.loader_lock_cookies.find(call(1));
                if(cookie==runtime_.loader_lock_cookies.end() || cookie->second!=threads_.current || runtime_.loader_owner!=threads_.current || !runtime_.loader_depth)return loader_error(0xc00000f0U);
                runtime_.loader_lock_cookies.erase(cookie);release_loader_notification();
                publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);return finish(0);
            }
            if(call(1))memory.store(GuestAddress{call(1)},32,0);if(call(2))memory.store(GuestAddress{call(2)},bits,0);
            if(flags&~3U)return loader_error(0xc00000efU);
            if((flags&2U) && !call(1))return loader_error(0xc00000f0U);
            if(!call(2))return loader_error(0xc00000f1U);
            if(runtime_.loader_owner && runtime_.loader_owner!=threads_.current){
                if(flags&2U){memory.store(GuestAddress{call(1)},32,2);return finish(0);}
                (void)await_loader(state,memory);return std::nullopt;
            }
            if(runtime_.loader_depth==UINT32_MAX || runtime_.loader_lock_cookies.size()>=8192)return loader_error(0xc0000017U);
            auto cookie=runtime_.next_loader_lock_cookie++;while(!cookie || runtime_.loader_lock_cookies.contains(cookie))cookie=runtime_.next_loader_lock_cookie++;
            runtime_.loader_lock_cookies.emplace(cookie,threads_.current);runtime_.loader_owner=threads_.current;++runtime_.loader_depth;
            publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);
            if(call(1))memory.store(GuestAddress{call(1)},32,1);memory.store(GuestAddress{call(2)},bits,cookie);return finish(0);
        }
        if(name=="RtlGetLastNtStatus")return finish(threads_.items.at(threads_.current).last_native_status);
        if(name=="RtlCompareUnicodeString" || name=="RtlEqualUnicodeString"){
            auto a=native_string(memory,call(0),x86_),b=native_string(memory,call(1),x86_);if(call(2))for(auto* value:{&a,&b})for(auto& c:*value)c=unicode_case_unit(c,true);
            const auto count=std::min(a.size(),b.size());std::int32_t result=0;for(std::size_t i=0;i<count;++i)if(a[i]!=b[i]){result=static_cast<std::int32_t>(a[i])-b[i];break;}if(!result)result=static_cast<std::int32_t>(a.size()*2)-static_cast<std::int32_t>(b.size()*2);
            return finish(name=="RtlEqualUnicodeString"?result==0:static_cast<std::uint32_t>(result));
        }
        if(name=="RtlAnsiStringToUnicodeString"){
            const auto length=memory.load(GuestAddress{call(1)},16),maximum=memory.load(GuestAddress{call(1)+2},16),source=memory.load(GuestAddress{call(1)+unit},bits);if(length>maximum || (length && !source))return finish(0xc000000dU);
            std::string bytes;for(std::uint64_t n=0;n<length;++n)bytes.push_back(static_cast<char>(memory.load(GuestAddress{source+n},8)));const auto converted=utf8_to_utf16(bytes);if(converted.size()>32766)return finish(0xc000000dU);
            const auto encoded=utf16_bytes(converted);memory.check(GuestAddress{call(0)},unit*2,Access::Write);auto destination=memory.load(GuestAddress{call(0)+unit},bits);
            if(call(2)){destination=allocate_heap(memory,encoded.size());if(!destination)return finish(0xc0000017U);memory.store(GuestAddress{call(0)+2},16,encoded.size());memory.store(GuestAddress{call(0)+unit},bits,destination);}
            else if(memory.load(GuestAddress{call(0)+2},16)<encoded.size())return finish(0x80000005U);
            memory.write(GuestAddress{destination},encoded);memory.store(GuestAddress{call(0)},16,converted.size()*2);return finish(0);
        }
        if(name=="RtlFreeUnicodeString"){const auto buffer=memory.load(GuestAddress{call(0)+unit},bits);if(buffer)(void)free_heap(memory,buffer,state);memory.write(GuestAddress{call(0)},std::vector<std::uint8_t>(unit*2));return finish(0);}
        if(name=="RtlCreateHeap"){
            const auto flags=static_cast<std::uint32_t>(call(0));
            if(call(1) || call(4) || call(5) || (flags&~3U))return finish(0);
            // NT 的 HEAP_GROWABLE 不屬於 HeapCreate 旗標；可成長 heap 的 ReserveSize
            // 是初始保留量，不能誤當最大容量。建立及後續配置仍共用私有 heap 的擁有權流程。
            const auto reserve=call(2),commit=reserve?std::min(call(3),reserve):call(3);
            return start("kernel32.dll","HeapCreate",{flags&1U,commit,(flags&2U)?0:reserve});
        }
        if(name=="RtlDestroyHeap")return start("kernel32.dll","HeapDestroy",{call(0)});
        if(name=="NtMapViewOfSection" || name=="NtMapViewOfSectionEx" || name=="NtExtendSection" || name=="NtFlushVirtualMemory")return invoke_nt_section(state,memory,service);
        if(name=="NtDeleteFile"){
            const auto file=path(object_attributes(memory,call(0),x86_));
            const auto at=scratch(file.size()*2+2);memory.write(GuestAddress{at},utf16_bytes(file));
            return start("kernel32.dll","DeleteFileW",{at});
        }
        if(name=="NtFlushBuffersFile"){
            memory.check(GuestAddress{call(1)},unit*2,Access::Write);
            // 沿用共用 FileIo 等待及完成通知；NT 呼叫同步返回後才發布 IOSB。
            return start("kernel32.dll","FlushFileBuffers",{call(0)});
        }
        if(name=="NtQuerySecurityObject"){
            memory.check(GuestAddress{call(4)},4,Access::Write);
            return start("advapi32.dll","GetKernelObjectSecurity",{call(0),call(1),call(2),call(3),call(4)});
        }
        if(name=="NtAccessCheck"){
            // Native 與 Win32 共用權杖、DACL、restricted SID 及特權裁決；
            // 僅將 BOOL 授權結果轉為獨立的 AccessStatus，不把呼叫成功當成授權。
            memory.check(GuestAddress{call(3)},16,Access::Read);
            memory.check(GuestAddress{call(5)},4,Access::Read);
            for(const auto index:{5U,6U,7U})memory.check(GuestAddress{call(index)},4,Access::Write);
            return start("advapi32.dll","AccessCheck",{call(0),call(1),call(2),call(3),call(4),call(5),call(6),call(7)});
        }
        if(name=="NtSetSecurityObject"){
            if(!call(2))return finish(0xc0000005U);
            return start("advapi32.dll","SetKernelObjectSecurity",{call(0),call(1),call(2)});
        }
        if(name=="NtQueryInformationByName"){
            memory.check(GuestAddress{call(1)},unit*2,Access::Write);
            const auto done=[&](std::uint32_t status,std::size_t count=0){memory.store(GuestAddress{call(1)},bits,status);memory.store(GuestAddress{call(1)+unit},bits,count);return finish(status);};
            try{
                const auto kind=static_cast<std::uint32_t>(call(4));const auto extent=file_stat::size(kind);
                if(!extent)return done(0xc000000dU);
                if(static_cast<std::uint32_t>(call(3))<extent)return done(0xc0000004U);
                memory.check(GuestAddress{call(2)},extent,Access::Write);
                const auto file=path(object_attributes(memory,call(0),x86_));std::uint32_t effective{};
                const auto info=filesystem_.information(utf16_to_utf8(file),effective);if(info.error)return done(nt_status(info.error));
                memory.write(GuestAddress{call(2)},file_stat::encode(kind,info.metadata,effective));process_io::record(process_io::Kind::Other,extent);
                return done(0,extent);
            }catch(const NativeError& error){return done(error.status);}catch(const MemoryFault&){return done(0xc0000005U);}catch(const std::bad_alloc&){return done(0xc0000017U);}
        }
        if(name=="NtQueryAttributesFile" || name=="NtQueryFullAttributesFile"){
            const auto file=path(object_attributes(memory,call(0),x86_));const auto info=filesystem_.information(utf16_to_utf8(file));if(info.error)return finish(nt_status(info.error));
            const auto error=filesystem_.security_access(utf16_to_utf8(file),0x80U);if(error)return finish(nt_status(error));
            const auto& m=info.metadata;const bool full=name=="NtQueryFullAttributesFile";memory.write(GuestAddress{call(1)},std::vector<std::uint8_t>(full?56U:40U));
            for(const auto& [offset,value]:std::array<std::pair<unsigned,std::uint64_t>,4>{{{0,m.created},{8,m.accessed},{16,m.modified},{24,m.changed}}})memory.store(GuestAddress{call(1)+offset},64,value);
            if(full){memory.store(GuestAddress{call(1)+32},64,m.allocation_size);memory.store(GuestAddress{call(1)+40},64,m.size);}memory.store(GuestAddress{call(1)+(full?48:32)},32,m.attributes);return finish(0);
        }
        if(name=="NtCreateNamedPipeFile" || name=="ZwCreateNamedPipeFile"){
            // 管道伺服端：NT 名稱（\Device\NamedPipe\、\??\pipe\、\DosDevices\pipe\、\GLOBAL??\pipe\）轉成 \\.\pipe\ 後沿用
            // CreateNamedPipeW 的跨程序命名空間、安全描述元、實例數與首實例契約。FILE_CREATE 對應 FILE_FLAG_FIRST_PIPE_INSTANCE，
            // FILE_OPEN_IF 為一般建立；FILE_OPEN（只加開既有管道的實例）與相對管道目錄 handle 的匿名管道未開放，明確回報。
            memory.check(GuestAddress{call(0)},unit,Access::Write);memory.check(GuestAddress{call(3)},unit*2,Access::Write);
            const auto attr=object_attributes(memory,call(2),x86_,false,true);
            const auto access=static_cast<std::uint32_t>(call(1)),share=static_cast<std::uint32_t>(call(4)),disposition=static_cast<std::uint32_t>(call(5)),options=static_cast<std::uint32_t>(call(6));
            const auto type=static_cast<std::uint32_t>(call(7)),read_mode=static_cast<std::uint32_t>(call(8)),completion=static_cast<std::uint32_t>(call(9)),instances=static_cast<std::uint32_t>(call(10));
            const auto upper=[](std::u16string_view text){std::u16string out(text);for(auto& c:out)c=windows_path_upper(c);return out;};
            std::u16string pipe;
            if(attr.root){
                // 相對於 \Device\NamedPipe\ 目錄 handle（kernelbase CreatePipe 與自行實作的匿名管道）：空名稱產生程序內唯一的匿名名稱。
                const auto root=files_.find(attr.root);
                if(root==files_.end())return finish(object_type(attr.root).empty()?0xc0000008U:0xc0000024U);
                if(root->second.key!=nt_pipe_root_key)return finish(0xc0000024U);
                if(attr.name.find(u'\\')!=attr.name.npos || attr.name.find(u'\0')!=attr.name.npos)return finish(0xc0000033U);
                if(attr.name.empty()){
                    static std::atomic<std::uint32_t> serial{0};char text[40];
                    std::snprintf(text,sizeof text,"Win32Pipes.%08x.%08x",static_cast<unsigned>(process_id()),static_cast<unsigned>(++serial));
                    pipe=utf8_to_utf16(text);
                }else pipe=attr.name;
            }else
            for(const std::u16string_view prefix:{std::u16string_view{u"\\DEVICE\\NAMEDPIPE\\"},std::u16string_view{u"\\??\\PIPE\\"},std::u16string_view{u"\\DOSDEVICES\\PIPE\\"},std::u16string_view{u"\\GLOBAL??\\PIPE\\"}})
                if(attr.name.size()>prefix.size() && upper(std::u16string_view(attr.name).substr(0,prefix.size()))==prefix){pipe=attr.name.substr(prefix.size());break;}
            if(pipe.empty() || pipe.find(u'\0')!=pipe.npos)return finish(0xc0000033U);
            const bool reading=access&(1U|0x80000000U|0x10000000U),writing=access&(2U|0x40000000U|0x10000000U);
            if((!reading && !writing) || (share&~7U) || type>1 || read_mode>1 || completion>1 || (read_mode && !type) || !instances ||
               (instances>254 && instances!=UINT32_MAX) || (options&~0x32U) || (options&0x30U)==0x30U)return finish(0xc000000dU);
            if(disposition!=2 && disposition!=3)return finish(disposition==1?0xc00000bbU:0xc000000dU);
            std::uint32_t timeout=0;
            if(call(13)){
                const auto interval=static_cast<std::int64_t>(memory.load(GuestAddress{call(13)},64));
                if(interval>0)return finish(0xc000000dU);
                timeout=static_cast<std::uint32_t>(std::min<std::uint64_t>((static_cast<std::uint64_t>(-interval)+9999)/10000,UINT32_MAX-1U));
            }
            const auto open_mode=(reading?1U:0U)|(writing?2U:0U)|(access&0x10c0000U)|((options&0x30U)?0U:0x40000000U)|((options&2U)?0x80000000U:0U)|(disposition==2?0x80000U:0U);
            const auto pipe_mode=(type?4U:0U)|(read_mode?2U:0U)|(completion?1U:0U);
            const auto full=u"\\\\.\\pipe\\"+pipe;
            const auto at=scratch(128+full.size()*2+2);memory.write(GuestAddress{at+128},utf16_bytes(full));security(attr,at);
            op.information=2;
            return start("kernel32.dll","CreateNamedPipeW",{at+128,open_mode,pipe_mode,instances==UINT32_MAX?255U:instances,call(12),call(11),timeout,at});
        }
        if(name=="NtCreateFile" || name=="NtOpenFile"){
            const bool create=name=="NtCreateFile";const auto attr=object_attributes(memory,call(2),x86_,false,true);
            // 管道檔案系統根目錄 \Device\NamedPipe\：回傳目錄 handle，供 NtCreateNamedPipeFile 以相對名稱建立（含匿名管道）。
            const auto upper_name=[&]{std::u16string out(attr.name);for(auto& c:out)c=windows_path_upper(c);return out;}();
            if(!attr.root && (upper_name==u"\\DEVICE\\NAMEDPIPE\\" || upper_name==u"\\DEVICE\\NAMEDPIPE")){
                memory.check(GuestAddress{call(0)},unit,Access::Write);memory.check(GuestAddress{call(3)},unit*2,Access::Write);
                FileHandle root;root.key=std::string(nt_pipe_root_key);root.path=root.key;root.metadata.attributes=0x10;root.access=static_cast<std::uint32_t>(call(1));
                root.bytes=std::make_shared<const std::vector<std::uint8_t>>();
                const auto handle=runtime_.allocate_wait_handle();root.object=handle;files_.emplace(handle,std::move(root));runtime_.handle_flags[handle]=(attr.flags&2U)?1U:0U;
                memory.store(GuestAddress{call(0)},bits,handle);memory.store(GuestAddress{call(3)},bits,0);memory.store(GuestAddress{call(3)+unit},bits,1);
                return finish(0);
            }
            // 以管道 handle 為根、名稱空白：開啟同一管道的另一端（kernelbase CreatePipe 取得寫入端的方式）。
            const auto pipe_root=attr.root && attr.name.empty()?files_.find(attr.root):files_.end();
            const auto file=pipe_root!=files_.end() && pipe_root->second.pipe?u"\\\\.\\pipe"+pipe_root->second.pipe->file_name():path(attr);const auto options=static_cast<std::uint32_t>(call(create?8:5)),disposition=create?static_cast<std::uint32_t>(call(7)):1U;
            if(trace_native_call(name))std::fprintf(stderr,"[native_file_open] pid=%u api=%.*s path=%s access=%x share=%x disposition=%u options=%x attributes=%x\n",process_id(),int(name.size()),name.data(),utf16_to_utf8(file).c_str(),static_cast<unsigned>(call(1)),static_cast<unsigned>(call(create?6:4)),disposition,options,create?static_cast<unsigned>(call(5)):0U);
            memory.check(GuestAddress{call(0)},unit,Access::Write);memory.check(GuestAddress{call(3)},unit*2,Access::Write);
            if(disposition>5 || (options&~0x0060587fU) || (options&0x30U)==0x30U || ((options&0x30U) && !(call(1)&0x100000U)) || ((options&1U) && (options&0x40U)))return finish(0xc000000dU);
            if(create && (call(9) || call(10)))return finish(0xc00000bbU);
            const bool device=file.starts_with(u"\\\\.\\") || detail::volume_device_path(file).volume;
            // DOS 裝置不以宿主路徑預查；分享、存在與建立契約由裝置 provider 驗證。
            const auto info=device?FileInformation{}:filesystem_.information(utf16_to_utf8(file));const bool existed=device || !info.error;if(info.error && info.error!=2 && info.error!=3)return finish(nt_status(info.error));
            if(existed && (options&0x40U) && (info.metadata.attributes&0x10U))return finish(0xc00000baU);
            if(existed && (options&1U) && !(info.metadata.attributes&0x10U))return finish(0xc0000103U);
            auto flags=create?static_cast<std::uint32_t>(call(5)):0x80U;if(!flags)flags=0x80U;flags|=attr.file_qos;
            if(options&2U)flags|=0x80000000U;if(options&4U)flags|=0x08000000U;if(options&8U)flags|=0x20000000U;
            if(options&0x800U)flags|=0x10000000U;if(options&0x400000U)flags|=0x00100000U;
            if(!(options&0x30U))flags|=0x40000000U;if(options&0x1000U)flags|=0x04000000U;if(options&0x4000U)flags|=0x02000000U;if(options&0x200000U)flags|=0x00200000U;
            // NT 沒有 Win32 的「未設 BACKUP_SEMANTICS 就禁止目錄」限制；
            // 未指定 FILE_NON_DIRECTORY_FILE 時，既有共用 provider 可接受兩種型別。
            // 這個內部旗標只開放目錄 handle，存取權與分享仍由 provider 驗證。
            if(!device && !(options&0x40U))flags|=0x02000000U;
            const auto at=scratch(128+file.size()*2+2);memory.write(GuestAddress{at+128},utf16_bytes(file));security(attr,at);
            unsigned win_disposition=disposition==0 || disposition==5?2U:disposition==1?3U:disposition==2?1U:disposition==3?4U:5U;
            if(existed && (info.metadata.attributes&0x10U) && disposition==3)win_disposition=3;
            if(options&1U){if(disposition!=1 && disposition!=2 && disposition!=3)throw NativeError{0xc000000dU};if(existed && disposition==2)throw NativeError{0xc0000035U};if(!existed && disposition==1)throw NativeError{0xc0000034U};if(!existed){const auto error=filesystem_.create_directory(utf16_to_utf8(file),[&]{return file_creation_descriptor(memory,at);});if(error)throw NativeError{nt_status(error)};}flags|=0x02000000U;win_disposition=3;}
            op.information=existed?(disposition==0?0U:disposition==4 || disposition==5?3U:1U):2U;op.attributes=options;
            if(create && call(4) && op.information!=1){op.allocation=memory.load(GuestAddress{call(4)},64);if(op.allocation>INT64_MAX)throw NativeError{0xc000000dU};}
            return start("internal-nt-file","CreateFileW",{at+128,call(1),call(create?6:4),at,win_disposition,flags,0});
        }
        if(name=="NtOpenProcess" || name=="NtOpenThread"){
            memory.check(GuestAddress{call(0)},unit,Access::Write);const auto attr=object_attributes(memory,call(2),x86_);if(!attr.name.empty() || attr.root || attr.security || !call(3))return finish(0xc000000dU);
            const auto pid=memory.load(GuestAddress{call(3)},bits),tid=memory.load(GuestAddress{call(3)+unit},bits);if(pid>UINT32_MAX || tid>UINT32_MAX)return finish(0xc000000bU);
            if(name=="NtOpenThread" && pid && pid!=process_id())return finish(0xc00000bbU);
            return start("kernel32.dll",name=="NtOpenProcess"?"OpenProcess":"OpenThread",{call(1),(attr.flags&2U)!=0,name=="NtOpenProcess"?pid:tid});
        }
        if(name.starts_with("NtOpen") && name.find("Token")!=name.npos){
            const bool process=name.starts_with("NtOpenProcess"),extended=name.ends_with("Ex");const auto attrs=extended?static_cast<std::uint32_t>(call(process?2:3)):0U;
            if(attrs&~2U)return finish(0xc000000dU);const auto output=call(process?(extended?3:2):(extended?4:3));memory.check(GuestAddress{output},unit,Access::Write);op.attributes=attrs;
            return start("advapi32.dll",process?"OpenProcessToken":"OpenThreadToken",process?std::vector<std::uint64_t>{call(0),call(1),output}:std::vector<std::uint64_t>{call(0),call(1),call(2),output});
        }
        if(name=="NtDuplicateObject"){
            if(call(5)&~0x22U)return finish(0xc000000dU);
            if(call(3))memory.check(GuestAddress{call(3)},unit,Access::Write);
            if(call(5)&0x20U){if((call(5)&2U) || (current_process_handle(call(0)) && object_type(call(1))!=u"Event"))return finish(0xc000000dU);return start("internal-nt-event","DuplicateHandle",{call(0),call(1),call(2),call(3),call(4),0,call(6),call(5)});}
            return start("kernel32.dll","DuplicateHandle",{call(0),call(1),call(2),call(3),call(4),(call(5)&2U)!=0,call(6)});
        }
        if(name=="NtWaitForSingleObject" || name=="NtSignalAndWaitForSingleObject" || name.starts_with("NtWaitForMultipleObjects")){
            const bool signal=name.starts_with("NtSignal"),multiple=name.starts_with("NtWaitForMultiple");
            std::uint64_t captured=0;
            if(multiple){
                const auto count=static_cast<std::uint32_t>(call(0)),type=static_cast<std::uint32_t>(call(2));
                if(!count || count>64)return finish(0xc00000efU); // STATUS_INVALID_PARAMETER_1
                if(type>1)return finish(0xc00000f1U); // STATUS_INVALID_PARAMETER_3
                const auto source_unit=name.ends_with("32")?4U:unit;
                memory.check(GuestAddress{call(1)},count*source_unit,Access::Read);
                // 至多 64 個 handle；先完整擷取、驗證再進入共用等待，失敗不消耗任何信號。
                std::array<std::uint64_t,64> handles{};
                std::array<std::pair<std::u16string_view,std::pair<std::uint64_t,std::uint64_t>>,64> identities{};
                for(unsigned i=0;i<count;++i){
                    auto handle=memory.load(GuestAddress{call(1)+i*source_unit},static_cast<std::uint8_t>(source_unit*8));
                    if(source_unit==4 && !x86_)handle=static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(handle)));
                    handles[i]=handle;const auto object=object_metadata(handle);const auto kind=object_type(handle);
                    if(!object)return finish(0xc0000008U);
                    const auto registry_key=registry_key_signaled(handle);
                    if((!registry_key.has_value() && !(object->access&0x100000U)) || (kind==u"Thread" && !(thread_access(handle)&0x100000U)))return finish(0xc0000022U);
                    if(!runtime_.wait_objects.contains(handle) && !thread_id(handle) && !process_signaled(handle).has_value() && !file_io_signaled(handle).has_value() && !registry_key.has_value())
                        return finish(kind==u"IoCompletion"?0xc0000008U:0xc0000024U);
                    identities[i]={kind,detail::object_identity(runtime_,handle,registry_key.has_value()?handle:object->identity)};
                }
                if(!type)for(unsigned i=0;i<count;++i)
                    if(std::find(identities.begin(),identities.begin()+i,identities[i])!=identities.begin()+i)return finish(0xc0000030U);
                captured=scratch(count*unit);
                for(unsigned i=0;i<count;++i)memory.store(GuestAddress{captured+i*unit},bits,handles[i]);
            }
            const auto timeout=call(multiple?4:signal?3:2);std::uint64_t ticks=UINT64_MAX;
            if(timeout){const auto value=memory.load(GuestAddress{timeout},64),now=runtime_.system_filetime();ticks=value>>63?(~value+1):value>now?value-now:0;}
            // 共用等待 API 的排程及 APC 行為；保留 NT 的 100 ns 截止時間供排程器使用。
            op.information=ticks==UINT64_MAX?UINT64_MAX:ticks>UINT64_MAX-runtime_.clock_ticks?UINT64_MAX:runtime_.clock_ticks+ticks;
            const auto millis=ticks==UINT64_MAX?UINT32_MAX:std::min<std::uint64_t>(0xfffffffeU,ticks/10000+(ticks%10000!=0));
            if(multiple)return start("kernel32.dll","WaitForMultipleObjectsEx",{call(0),captured,call(2)==0,millis,static_cast<std::uint8_t>(call(3))});
            return start("kernel32.dll",signal?"SignalObjectAndWait":"WaitForSingleObjectEx",signal?std::vector<std::uint64_t>{call(0),call(1),millis,static_cast<std::uint8_t>(call(2))}:std::vector<std::uint64_t>{call(0),millis,static_cast<std::uint8_t>(call(1))});
        }
        if(name=="NtCreateSection" || name=="NtOpenSection"){
            const bool create=name=="NtCreateSection";memory.check(GuestAddress{call(0)},unit,Access::Write);const auto attr=object_attributes(memory,call(2),x86_,create);
            auto section=rooted_object_name(runtime_,attr,create);if(!section.empty()){if(const auto status=namespace_object_name(section,create))return finish(status);}
            if(call(1)&~0xf001fU)return finish(0xc0000022U);
            if(create && call(5)!=0x8000000U && call(5)!=0 && call(5)!=0x1000000U && call(5)!=0x11000000U)return finish(0xc00000bbU);
            const auto at=scratch(128+section.size()*2+2);security(attr,at);if(!section.empty())memory.write(GuestAddress{at+128},utf16_bytes(section));op.attributes=static_cast<std::uint32_t>(call(1));op.information=attr.flags;
            if(create){const auto size=call(3)?memory.load(GuestAddress{call(3)},64):0;if(size>INT64_MAX)throw NativeError{0xc000000dU};const auto flags=call(5)?call(5):0x8000000U;return start("internal-nt-mapping","CreateFileMappingW",{call(6)?call(6):(x86_?UINT32_MAX:UINT64_MAX),at,call(4)|flags,size>>32,static_cast<std::uint32_t>(size),section.empty()?0:at+128,call(1)});}
            if(section.empty())throw NativeError{0xc0000033U};return start("internal-nt-mapping","OpenFileMappingW",{call(1),(attr.flags&2U)!=0,at+128});
        }
        if(name=="NtCancelIoFile" || name=="NtCancelIoFileEx"){
            const bool extended=name.ends_with("Ex");memory.check(GuestAddress{call(extended?2:1)},unit*2,Access::Write);
            return start("internal-nt-file",extended?"CancelIoEx":"CancelIo",extended?
                std::vector<std::uint64_t>{call(0),call(1)}:std::vector<std::uint64_t>{call(0)});
        }
        if(name=="NtReadFile" || name=="NtWriteFile"){
            memory.check(GuestAddress{call(4)},unit*2,Access::Write);const auto file=files_.find(call(0));if(file==files_.end())return finish(0xc0000008U);
            const bool sync=(file->second.mode&0x30U)!=0,writing=name=="NtWriteFile";
            const auto key=call(8)?static_cast<std::uint32_t>(memory.load(GuestAddress{call(8)},32)):0U;
            const auto event=call(1)&~UINT64_C(1);
            if(event && (!runtime_.wait_objects.contains(event) || runtime_.wait_objects.at(event).kind!=WaitObject::Kind::Event))return finish(0xc0000008U);
            if(call(2) && file->second.completion_port)return finish(0xc000000dU);
            auto offset=call(7)?memory.load(GuestAddress{call(7)},64):UINT64_MAX-1;if(offset==UINT64_MAX-1){if(!sync && !file->second.pipe && !file->second.serial && !file->second.hid)return finish(0xc000000dU);offset=file->second.pipe || file->second.serial || file->second.hid?0:static_cast<std::uint64_t>(runtime_.win_file_positions.at(file->second.object));}
            if(offset>INT64_MAX && !(writing && offset==UINT64_MAX))return finish(0xc000000dU);
            const auto at=scratch(128);
            // 共用 provider 完成後會同時發布 OVERLAPPED 與原始 IOSB；待命標記用來
            // 區分尚未送出的驗證失敗與已完成要求，不依稍後的 callback 順序推論。
            memory.store(GuestAddress{at+64},bits,0x103);memory.store(GuestAddress{at+64+unit},bits,0);
            memory.store(GuestAddress{at+64+unit*2},64,offset);memory.store(GuestAddress{at+64+(x86_?16U:24U)},bits,call(1));
            op.attributes=1; // 所有檔案種類沿共用完成流程，不能經 Win32 再次進入 Native hook。
            return start("internal-nt-file",writing?"WriteFile":"ReadFile",{call(0),call(5),call(6),at+32,at+64,key,call(4),call(3),call(2)});
        }
        throw HostCallFault("未知 NT 原生服務");
    }catch(const NativeError& error){if(op.scratch)(void)free_heap(memory,op.scratch,state);return failure(error.status);}
    catch(const MemoryFault&){if(op.scratch)(void)free_heap(memory,op.scratch,state);return failure(0xc0000005U);}
    catch(const std::bad_alloc&){if(op.scratch)(void)free_heap(memory,op.scratch,state);return failure(0xc0000017U);}
}
std::optional<std::uint32_t> WindowsServices::resume_nt_native(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    auto op=native_operations_.at(id);native_operations_.erase(id);const auto& a=op.arguments;const auto name=op.symbol;const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);auto status=nt_status(last_error_);bool keep_scratch=false;
    if (name.find("@critical_section@Concurrency@@") != name.npos || name.find("@_Condition_variable@details@Concurrency@@") != name.npos) {
        const bool scoped = name.starts_with("??0scoped_lock@"), waiting = name.starts_with("?wait");
        const bool trying = name.starts_with("?try_lock");
        if (scoped || waiting || name.starts_with("?lock@") || (trying && (value & 0xffU)))
            memory.store(GuestAddress{a[scoped || waiting ? 1 : 0]}, bits, threads_.current);
        last_error_ = op.saved_error;
        complete_call(state, memory, scoped ? a[0] : trying || name.starts_with("?wait_for@") ? (value & 0xffU) : 0);
        return std::nullopt;
    }
    // Concrt 鎖與輔助（concrt_lock_handlers.inc）的續接：取得成功才記錄擁有者／讀取者，建構子回傳 this。
    if (name.find("reader_writer_lock@Concurrency@@") != name.npos) {
        const auto lock = a[1]; const bool scoped_ctor = name.starts_with("??0scoped_lock"), trying = name.starts_with("?try_lock");
        const bool read = name.find("scoped_lock_read@") != name.npos || name.starts_with("?lock_read@") || name.starts_with("?try_lock_read@");
        if ((scoped_ctor || name.starts_with("?lock") || trying) && (!trying || (value & 0xffU))) {
            if (read) memory.store(GuestAddress{lock + 2 * unit}, 32, memory.load(GuestAddress{lock + 2 * unit}, 32) + 1);
            else memory.store(GuestAddress{lock}, bits, threads_.current);
        }
        last_error_ = op.saved_error; complete_call(state, memory, scoped_ctor ? a[0] : trying ? (value & 0xffU) : 0); return std::nullopt;
    }
    if (name.find("@event@Concurrency@@") != name.npos || name.starts_with("??0event@Concurrency@@") || name.starts_with("??1event@Concurrency@@")) {
        const auto wait_timeout = x86_ ? UINT64_C(0xffffffff) : UINT64_MAX; std::uint64_t result = 0; // COOPERATIVE_WAIT_TIMEOUT
        if (name.starts_with("??0")) { if (!value) throw HostCallFault("Concrt event 無法建立同步物件"); memory.store(GuestAddress{a[0]}, bits, value); result = a[0]; }
        else if (name.starts_with("?wait_for_multiple@")) { if (op.scratch) (void)free_heap(memory, op.scratch, state); const auto signaled = static_cast<std::uint32_t>(value); result = signaled < a[1] ? signaled : wait_timeout; }
        else if (name.starts_with("?wait@")) result = static_cast<std::uint32_t>(value) == 0 ? 0 : wait_timeout;
        last_error_ = op.saved_error; complete_call(state, memory, result); return std::nullopt;
    }
    if (name.find("PPLLock@details@Concurrency@@") != name.npos) {
        const auto lock = a[1]; const unsigned section = x86_ ? 32U : 56U;
        const bool acquiring = name.starts_with("?_Acquire@") || name.starts_with("??0_Scoped_lock@");
        if (acquiring) {
            memory.store(GuestAddress{lock}, bits, threads_.current);
            if (name.find("_ReentrantPPLLock@") != name.npos) { memory.store(GuestAddress{lock + section}, 32, 1); memory.store(GuestAddress{lock + section + 4}, 32, static_cast<std::uint32_t>(threads_.current)); }
        }
        last_error_ = op.saved_error; complete_call(state, memory, name.starts_with("??0_Scoped_lock@") ? a[0] : 0); return std::nullopt;
    }
    if (name.find("BlockingLock@details@Concurrency@@") != name.npos || name.find("_SpinWait@") != name.npos ||
        name.starts_with("?Yield@Context@") || name.starts_with("?_SpinYield@Context@") || name.starts_with("?_Yield@_Context@") || name == "?wait@Concurrency@@YAXI@Z") {
        std::uint64_t result = 0;
        if (name.starts_with("??0")) result = a[0];
        else if (name.starts_with("?_TryAcquire@")) result = value & 0xffU;
        else if (name.starts_with("?_SpinOnce@")) result = 1; // 讓出後仍在等待中
        last_error_ = op.saved_error; complete_call(state, memory, result); return std::nullopt;
    }
    if(name.find("_Lockit@std@@")!=name.npos){
        if(!op.information){
            if(!value)throw HostCallFault("STL 共用鎖初始化失敗");
            op.information=1;const auto token=next_native_operation_++;native_operations_.emplace(token,op);
            const auto lock=runtime_.msvcp_global_locks+unit+op.attributes*(x86_?24U:40U);
            const bool release=name.starts_with("??1");
            start_callbacks(state,memory,{{resolve("kernel32.dll",release?"LeaveCriticalSection":"EnterCriticalSection").value,{lock}}},Continuation::NativeOperation,token);return std::nullopt;
        }
        last_error_=op.saved_error;complete_call(state,memory,name.starts_with("??0")?a[0]:0);return std::nullopt;
    }
    if(name.starts_with("?_Schedule_chore@")){
        // CreateThreadpoolWork 成功後寫回 _M_work 再 SubmitThreadpoolWork；失敗回 GetLastError()。
        if(!op.information){
            if(!value){const auto error=last_error_?last_error_:8U;last_error_=op.saved_error;complete_call(state,memory,error);return std::nullopt;}
            memory.store(GuestAddress{a[0]},bits,value);op.information=1;const auto token=next_native_operation_++;native_operations_.emplace(token,op);
            start_callbacks(state,memory,{{resolve("kernel32.dll","SubmitThreadpoolWork").value,{value}}},Continuation::NativeOperation,token);return std::nullopt;
        }
        last_error_=op.saved_error;complete_call(state,memory,0);return std::nullopt;
    }
    if(name.starts_with("?_Release_chore@")){last_error_=op.saved_error;complete_call(state,memory,0);return std::nullopt;}
    if(name=="CoWaitForMultipleHandles" || name=="CoWaitForMultipleObjects"){
        const auto count=static_cast<std::uint32_t>(a[2]);const bool sta=op.attributes&1U,pump=op.attributes&2U,input=op.attributes&16U;
        const auto restart=[&](std::string_view family,std::string_view symbol,std::vector<std::uint64_t> arguments){
            const auto token=next_native_operation_++;native_operations_.emplace(token,op);start_callbacks(state,memory,{{resolve(family,symbol).value,std::move(arguments)}},Continuation::NativeOperation,token);
        };
        if(op.information==1){
            // 一則訊息分派完畢：以剩餘時間重新等待（期限已過時仍做一次零等待，與 Windows 先處理訊息再檢查控制代碼相同）。
            op.information=0;const auto now=runtime_.clock_ticks;
            const auto remaining=op.allocation==UINT64_MAX?UINT32_MAX:static_cast<std::uint32_t>(op.allocation>now?std::min<std::uint64_t>((op.allocation-now+9999)/10000,UINT32_MAX-1):0);
            restart("user32.dll","MsgWaitForMultipleObjectsEx",{count,a[3],remaining,0x4ffU,(op.attributes&8U?2U:0U)|(input?4U:0U)});return std::nullopt;
        }
        if(sta && pump && value==count){
            // 有輸入：PeekMessageW(PM_REMOVE)→TranslateMessage→DispatchMessageW。先清空 MSG，沒有取得訊息時後兩步不做事。
            op.information=1;memory.write(GuestAddress{op.scratch},std::vector<std::uint8_t>(x86_?28U:48U));
            const auto token=next_native_operation_++;native_operations_.emplace(token,op);
            start_callbacks(state,memory,{{resolve("user32.dll","PeekMessageW").value,{op.scratch,0,0,0,1}},{resolve("user32.dll","TranslateMessage").value,{op.scratch}},
                {resolve("user32.dll","DispatchMessageW").value,{op.scratch}}},Continuation::NativeOperation,token);
            return std::nullopt;
        }
        // 控制代碼索引、WAIT_ABANDONED_0+n 與 WAIT_IO_COMPLETION 都回 S_OK 並寫入原值；逾時為 RPC_S_CALLPENDING，失敗轉成 HRESULT。
        const auto result=static_cast<std::uint32_t>(value);std::uint32_t hresult=0;
        if(result==0x102U)hresult=0x80010115U;
        else if(result==UINT32_MAX)hresult=last_error_?0x80070000U|(last_error_&0xffffU):0x80004005U;
        else memory.store(GuestAddress{a[4]},32,result);
        if(op.scratch)(void)free_heap(memory,op.scratch,state);
        last_error_=op.saved_error;complete_call(state,memory,hresult);return std::nullopt;
    }
    if(name.starts_with("_Mtx_") || name.starts_with("_Cnd_") || name.starts_with("_Thrd_") || name.starts_with("?_Execute_once@std@@")){
        // _Thrd_result：_Success 0、_Timedout 2、_Busy 3、_Error 4。
        std::uint64_t result=0;
        const bool acquired=name=="_Mtx_lock" || name=="_Cnd_wait" || name=="_Cnd_timedwait" || (name=="_Mtx_trylock" && (value&0xffU));
        if(name=="_Mtx_trylock" && !(value&0xffU))result=3;
        if(name=="_Cnd_timedwait" && !value)result=last_error_==1460?2:4;
        if(name=="_Thrd_detach" && !value)result=4;
        // _Thrd_start 與 STL 相同：handle 寫回 _Thrd_t，建立失敗時 id 清為 0 並回 _Error。
        if(name=="_Thrd_start" || name=="_Thrd_create"){memory.store(GuestAddress{a[0]},bits,value);if(!value){memory.store(GuestAddress{a[0]+unit},32,0);result=4;}}
        if(acquired){
            const auto address=a[name.starts_with("_Cnd_")?1:0],owner=address+(x86_?40U:72U);
            memory.store(GuestAddress{owner},32,thread_identifier(threads_.current));
            memory.store(GuestAddress{owner+4},32,memory.load(GuestAddress{owner+4},32)+1);
        }
        // once 的回呼失敗及 LastError 完整沿用 InitOnceExecuteOnce。
        if(!name.starts_with("?_Execute_once@std@@"))last_error_=op.saved_error;
        complete_call(state,memory,name.starts_with("?_Execute_once@std@@")?value:result);
        return std::nullopt;
    }
    if(name=="RtlCreateHeap"){if(op.scratch)(void)free_heap(memory,op.scratch,state);last_error_=op.saved_error;complete_call(state,memory,value);return std::nullopt;}
    if(name=="RtlDestroyHeap"){last_error_=op.saved_error;complete_call(state,memory,value?0:a[0]);return std::nullopt;}
    if(name=="RtlRunOnceExecuteOnce"){
        status=value?0:last_error_?nt_status(last_error_):0xc0000001U;
        last_error_=op.saved_error;complete_call(state,memory,status);return std::nullopt;
    }
    if(name=="NtCreateFile" || name=="NtOpenFile"){
        const bool good=value!=(x86_?UINT32_MAX:UINT64_MAX) && value!=UINT64_MAX;status=good?0:status;
        if(good){if(const auto file=files_.find(value);file!=files_.end()){file->second.mode=op.attributes&0x38U;if(op.allocation && file->second.native)status=nt_status(file->second.native->allocate(op.allocation));}
            if(!status)memory.store(GuestAddress{a[0]},bits,value);else {files_.erase(value);runtime_.win_file_positions.erase(value);runtime_.handle_flags.erase(value);}}
        memory.store(GuestAddress{a[3]},bits,status);memory.store(GuestAddress{a[3]+unit},bits,status?0:op.information);
    }else if(name=="NtCreateNamedPipeFile" || name=="ZwCreateNamedPipeFile"){
        const bool good=value!=(x86_?UINT32_MAX:UINT64_MAX) && value!=UINT64_MAX;status=good?0:status;
        if(good)memory.store(GuestAddress{a[0]},bits,value);
        memory.store(GuestAddress{a[3]},bits,status);memory.store(GuestAddress{a[3]+unit},bits,good?op.information:0);
    }else if(name=="NtAccessCheck"){
        if(value){
            const bool allowed=memory.load(GuestAddress{a[7]},32)!=0;
            memory.store(GuestAddress{a[7]},32,allowed?0U:status?status:0xc0000022U);
            status=0;
        }
    }else if(name=="NtFlushBuffersFile"){
        status=value?0:status;memory.store(GuestAddress{a[1]},bits,status);memory.store(GuestAddress{a[1]+unit},bits,0);
    }else if(name=="NtOpenProcess" || name=="NtOpenThread"){status=value?0:status;if(value)memory.store(GuestAddress{a[0]},bits,value);}
    else if(name.find("Token")!=name.npos){status=value?0:status;if(value && (op.attributes&2U)){const bool process=name.starts_with("NtOpenProcess"),extended=name.ends_with("Ex");const auto output=a[process?(extended?3:2):(extended?4:3)];runtime_.handle_flags[memory.load(GuestAddress{output},bits)]|=1U;}}
    else if(name=="NtCreateSection" || name=="NtOpenSection"){
        const bool existed=name=="NtCreateSection" && last_error_==183;
        status=value?native_section_access(value,op.attributes):status;
        if(!status && existed && !(op.information&0x80U))status=0xc0000035U;
        if(!status){memory.store(GuestAddress{a[0]},bits,value);if(existed)status=0x40000000U;}
        else if(value)(void)close_mapping(value);
    }
    else if(name=="NtWaitForSingleObject" || name=="NtSignalAndWaitForSingleObject" || name.starts_with("NtWaitForMultipleObjects"))status=value==UINT32_MAX?status:static_cast<std::uint32_t>(value);
    else if(name=="NtCancelIoFile" || name=="NtCancelIoFileEx"){
        status=value?0:status;const auto ios=a[name.ends_with("Ex")?2:1];memory.store(GuestAddress{ios},bits,status);memory.store(GuestAddress{ios+unit},bits,0);
    }
    else if(name=="NtReadFile" || name=="NtWriteFile"){
        const auto completed=static_cast<std::uint32_t>(memory.load(GuestAddress{op.scratch+64},bits));
        status=value?completed:status;
        // callback 返回前可能已由別的執行緒完成並回收要求。保留最初的
        // STATUS_PENDING 返回值，不能覆寫已發布的 IOSB，也不能重送完成通知。
        if(completed==0x103U){
            if(status==0x103U){
                const auto pending=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.overlapped==op.scratch+64 && !io.done;});
                if(pending==pending_file_io_.end())throw HostCallFault("NT 待命 I/O 缺少共用要求");
                pending->native_scratch=op.scratch;keep_scratch=true;
            }
            memory.store(GuestAddress{a[4]},bits,status);memory.store(GuestAddress{a[4]+unit},bits,0);
        }
        if(!op.attributes && status!=0x103U && a[2] && (!status || status==0xc0000011U || status==0x80000005U)){threads_.items.at(threads_.current).apcs.push_back({a[2],{a[3],a[4],0}});threads_.yield=true;}
    }else if(name=="LdrLoadDll"){status=value?0:(last_error_==126?0xc0000135U:last_error_==193?0xc000007bU:status);if(value)memory.store(GuestAddress{a[3]},bits,value);}
    else if(name=="LdrGetProcedureAddress"){status=value?0:0xc0000139U;if(value)memory.store(GuestAddress{a[3]},bits,value);}
    else if(name=="LdrUnloadDll")status=value?0:0xc0000135U;
    else status=value?0:status;
    if(op.scratch && !keep_scratch)(void)free_heap(memory,op.scratch,state);last_error_=op.saved_error;threads_.items.at(threads_.current).last_native_status=status;complete_call(state,memory,status);return std::nullopt;
}
}
// _timespec64／xtime（__time64_t 秒＋long 奈秒）是 UTC 絕對時間；換成 Win32 等待的毫秒數，已過期為 0。
// MSVCP 等待服務與 worker 上的 _Thrd_sleep 尾跳共用。
namespace x64arm {
std::uint32_t WindowsServices::msvcp_wait_milliseconds(GuestMemory& memory,std::uint64_t target) const {
    constexpr std::int64_t limit=INT64_C(900000000000);
    const auto seconds=std::clamp(std::bit_cast<std::int64_t>(memory.load(GuestAddress{target},64)),-limit,limit);
    const auto nanoseconds=static_cast<std::int32_t>(memory.load(GuestAddress{target+8},32));
    const auto due=seconds*10000000+nanoseconds/100;
    const auto now=static_cast<std::int64_t>(runtime_.system_filetime()-UINT64_C(116444736000000000));
    if(due<=now)return 0;
    return static_cast<std::uint32_t>(std::min<std::int64_t>((due-now+9999)/10000,0xfffffffe));
}
}
namespace x64arm {
#include "windows_nt_shared.inc"
#include "windows_nt_objects.inc"
#include "windows_nt_families.inc"
#include "windows_object_namespace.inc"
#include "windows_ntdll_rtl.inc"
#include "windows_ntdll_loader.inc"
#include "windows_file_segments.inc"
}
