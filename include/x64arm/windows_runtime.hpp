#pragma once
#include "x64arm/windows_com_marshal.hpp"
#include "x64arm/process_io.hpp"
#include "x64arm/certificates.hpp"
#include "x64arm/http.hpp"
#include "x64arm/host_services.hpp"
#include "x64arm/eventlog.hpp"
#include "x64arm/etw_state.hpp"
#include "x64arm/crypto.hpp"
#include "x64arm/ncrypt.hpp"
#include "x64arm/wintrust.hpp"
#include "x64arm/network.hpp"
#include "x64arm/tls.hpp"
#include "x64arm/registry.hpp"
#include "x64arm/state.hpp"
#include "x64arm/readonly_files.hpp"
#include "x64arm/file_position.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/machine_clock.hpp"
#include "x64arm/windows_tokens.hpp"
#include "x64arm/windows_lsa.hpp"
#include "x64arm/windows_com_context.hpp"
#include "x64arm/windows_com_bind.hpp"
#include "x64arm/windows_com_rot.hpp"
#include "x64arm/com_error.hpp"
#include "x64arm/windows_com_error.hpp"
#include "x64arm/windows_type_library.hpp"
#include "x64arm/guest_callback.hpp"
#include "x64arm/windows_com_proxy.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <deque>
#include <vector>
#include <string>

namespace x64arm {
struct RuntimeFunction {
    std::uint32_t begin{}, end{}, unwind{};
    bool operator==(const RuntimeFunction&) const = default;
};
struct RuntimeImage {
    std::uint64_t base{}, size{}, stack_base{}, stack_size{};
    std::uint64_t tls_index{};
    std::vector<std::uint8_t> tls_data;
    std::size_t tls_zero{}, tls_alignment{16};
    std::uint64_t function_table{};
    std::vector<std::uint64_t> tls_callbacks;
    std::vector<RuntimeFunction> functions;
    std::map<std::string, std::uint32_t> export_names;
    std::map<std::uint32_t, std::uint32_t> export_ordinals;
    std::u16string path;
    std::map<std::uint32_t, std::string> export_forwarders;
    std::uint64_t entry{};
    bool large_address_aware{true};
    bool guard_cf{}, guard_long_jump{};
    std::uint64_t guard_check_cell{}, guard_dispatch_cell{};
    std::uint64_t guard_xfg_check_cell{},guard_xfg_dispatch_cell{},guard_xfg_table_dispatch_cell{};
    std::vector<std::uint32_t> guard_targets, guard_long_jump_targets;
    bool operator==(const RuntimeImage&) const = default;
};
struct RuntimeFunctionTable {
    RuntimeImage image;
    std::uint64_t range_end{};
    std::uint32_t count{};
    std::uint32_t capacity{};
    std::uint64_t handle{};
    bool operator==(const RuntimeFunctionTable&) const = default;
};
enum class MsvcExceptionKind : unsigned { BadCast,BadTypeid,NonRtti,Exception,BadAlloc,BadFunction,LogicError,LengthError,OutOfRange,RuntimeError,RegexError,InvalidArgument,OverflowError,SystemError,IosFailure,BadException,Count };
struct RuntimeCxxExceptions {
    RuntimeImage image;
    std::array<std::uint64_t,static_cast<unsigned>(MsvcExceptionKind::Count)> throw_info{},vtable{},message{};
    bool operator==(const RuntimeCxxExceptions&) const = default;
};
struct Eh4Operation {
    enum class Phase { Cookies, Filter, GlobalUnwind, Finally } phase{Phase::Cookies};
    enum class AfterCookies { Walk, Return, Transfer } after_cookies{AfterCookies::Walk};
    GuestState entry;
    std::uint32_t cookie{}, checker{}, record{}, frame{}, context{}, dispatcher{}, scope{}, ebp{}, handler{};
    std::uint64_t pointers{};
    std::int32_t level{-2}, parent{-2}, target{-2};
    // EH3 沒有 cookie header，scope 鏈以 -1 結束；EH4 使用 16-byte header 與 -2。
    std::int32_t root_level{-2};unsigned scope_header{16};
    unsigned arguments{6};
    std::uint32_t disposition{1};
    std::size_t exception_depth{}, callback_depth{};
    unsigned steps{};
    bool unwinding{}, called{},legacy3{},local_only{};
    bool operator==(const Eh4Operation&) const = default;
};
struct CrtJumpOperation {
    GuestState restored;std::uint64_t buffer{};
    std::uint32_t registration{UINT32_MAX};
    bool operator==(const CrtJumpOperation&) const = default;
};
struct RuntimeModule {
    RuntimeImage image;
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_IMAGE
    std::optional<TranslationImage> translation{};
#endif
    std::string name;
    std::set<std::uint64_t> dependencies;
    std::vector<std::pair<std::uint64_t,std::size_t>> mappings;
    std::uint32_t references{}, tls_slot{UINT32_MAX};
    bool loading{}, attached{}, pinned{}, no_thread_calls{};
    bool operator==(const RuntimeModule&) const = default;
};
struct ModuleStep {
    GuestCallback callback;
    std::uint64_t module{};
    bool check_result{}, complete{}, unmap{};
    bool operator==(const ModuleStep&) const = default;
};
struct ModuleOperation {
    std::vector<ModuleStep> steps;
    std::vector<std::uint64_t> created;
    std::size_t index{};
    std::uint64_t result{};
    std::uint32_t error{};
    bool startup{}, unloading{}, rollback{}, exit_thread{};
    std::uint32_t thread_exit_code{};
    bool operator==(const ModuleOperation&) const = default;
};
enum class Continuation { ComBindOperation, D3DXProgressOperation, D3DCompileOperation, Msvcp100Operation, UnwindConsolidate, CrtAssertion, CrtFatalDialog, GuiMouseInput, GuiMoveSize, ThemeTextOperation, MdiMoveOperation, StorageOperation, StorageCopyOperation, GuiGrayString, ComOperation, AutomationOperation, DirectInputEnumeration, StackWalkOperation, DemangleOperation, Eh4Operation, GuiAsyncSend, GuiAsyncCallback, GuiWaitMessage, DWriteOperation, CrtNewResult, CrtNewRetry, CrtInvalidNewMode, GuiSendTimeout, AccessibleOperation, SafeArrayOperation, OleStreamCopy, PrintAbort, UnhandledFilter, FiberDelete, UserApc, GuiIntegerText, WindowEnumeration, LocaleEnumeration, ResourceEnumeration, MetafileEnumeration, GuiWinEvent, FileTransfer, ReturnUntilFalse, Return, Startup, InitTermE, Exit, ThreadExit, ReturnCallback, Exception, ContinueException, SehFilter, GuiNcCreate, GuiCreate, GuiDialogInit, GuiModalInit, GuiModalDispatch, GuiModalEnd, GuiDestroy, GuiDestroyDefault, GuiMenuInit, GuiMenuFinish, GuiSentMessage, GuiControlOperation, CrtExit, Sort, OleVariantClear, OleVariantCopy, ModuleOperation, OleReleaseStgMedium, MsvcCopy, MsvcUnwind, MsvcCatch, MsvcDestroy, GuiWindowPos, GuiWindowNcCalc, GuiHook, GuiMsgFilterSystem, GuiModalSystemFilter, GuiModalFilter, GuiCbtCreate, GuiCreationNcCreate, GuiCreationNcCalc, GuiCreationCreate, GuiCreationFinish, GuiCreationAbort, GuiDialogProc, GuiBusyHook, GuiBusyResult, OleClipboardOperation, GuiPropertySheet, GuiCbtAction, FontEnumeration, OnExitTable, InitOnce, RegisteredWait, ReturnCallbackFreeHeap, NtCloseResult, ReturnZeroFreeHeap, CertificateDecodeAllocation, CertificateFind, SymbolFunctionEntry, MiniDumpOperation, EtwCallback, EtwProcess, WinrtOperation, NativeV8Operation, NativeOperation, TypeLibraryInvoke, TypeLibraryAddress, PrintDialogOperation, PrintPropertyPageOperation, ShellItemOperation, ComMarshalOperation, OleDragLoop, OleDragTarget, ComProxyDispatch, ComErrorSet, ComErrorRetire, RestrictedError, WinrtOriginSetup, WinrtOriginFinish, GuiDragDetection, ComContextOperation, MediaFoundationQuery, InitTerm, ReturnUntilFailure, ServiceMain, ServiceControl, GuiTextThunk, BeepTone, PolicyRetry, OlePersistence, CrtLongjmp, ComRotOperation, PerflibOperation, MciWait, ConsoleControl, TaskDialogOperation, DdeOperation, ComSpyOperation, GdiplusStream, FontPackage, WinUsbTransfer, WinUsbOverlapped, KernelRequest };
struct GuiHookFrame {
    std::vector<std::uint64_t> handles,scratch;
    std::size_t index{};
    Continuation continuation{Continuation::ReturnCallback};
    std::uint64_t value{},cbt_source{},cbt_marshaled{};std::int32_t type{};bool wide{true};
    bool operator==(const GuiHookFrame&) const = default;
};
struct CallbackFrame {
    GuestState saved;
    std::vector<GuestCallback> callbacks;
    std::size_t index{};
    Continuation continuation{Continuation::Return};
    std::uint64_t value{};
    std::size_t exception_depth{};
    bool loader_notification_lock{};
    GuiHookFrame hook{};
    std::vector<std::uint64_t> shell_deliveries{};
    std::uint32_t etw_seh_boundary{UINT32_MAX};
    std::uint32_t user_seh_boundary{UINT32_MAX};
    std::uint64_t initializer_next{}, initializer_end{};
    std::optional<GuestState> unwind_target{};
    bool operator==(const CallbackFrame&) const = default;
};
struct CrtIoLock {
    std::uint64_t key{};bool lowio{};
    bool operator==(const CrtIoLock&)const=default;
};
struct CrtIoOperation {
    std::uint64_t id{},call{},stack{};
    std::vector<CrtIoLock> locks;
    bool operator==(const CrtIoOperation&)const=default;
};
struct CrtFile {
    std::vector<std::uint8_t> bytes{};
    std::uint64_t position{};
    bool eof{}, error{},lowio_eof{};
    std::shared_ptr<HostFile> native{};
    FileMetadata metadata{};
    std::string key{};
    bool readable{true}, writable{}, append{}, text{},commit_on_close{};
    std::uint32_t descriptor{}, sharing{7};
    std::uint64_t description{}, standard_handle{}, os_handle{};
    std::vector<std::uint8_t> pushback{};
    bool has_read{};
    struct Buffer {
        enum class Direction { None, Read, Write } direction{};
        std::uint64_t address{}, end{};
        std::uint32_t capacity{}, used{}, cursor{};
        bool owned{};
        bool operator==(const Buffer&) const = default;
    } buffer{};
    std::uint64_t stream_address{};
    bool stdio{};
    std::uint32_t stdio_slot{UINT32_MAX};
    bool noinherit{},lowio_published{},lowio_handle_override{};
    std::uint64_t lowio_handle{};std::uint8_t lowio_flags{},lowio_read_flags{};
    std::uint32_t lowio_extended{4};
    std::array<std::uint8_t,3> lowio_lookahead{10,10,10};
    std::optional<char16_t> lowio_low_surrogate{};
    std::uint32_t lowio_error{};
    std::size_t lowio_needed_input{};
    std::array<std::uint8_t,8> lowio_multibyte{};
    std::uint8_t lowio_multibyte_size{};
    std::uint32_t lowio_multibyte_page{};
    std::uint8_t lowio_multibyte_maximum{};
    bool operator==(const CrtFile&) const = default;
};
struct MultimediaFile {
    std::shared_ptr<HostFile> native;
    std::string key;
    std::vector<std::uint8_t> bytes;
    std::uint64_t position{},size{},buffer{};
    std::uint32_t capacity{},growth{},mode{};
    std::uint64_t io_buffer{},io_offset{},io_next{};
    std::uint32_t io_capacity{},io_valid{},open_flags{};
    bool io_owned{},io_ready{};
    bool memory{};
    bool operator==(const MultimediaFile&) const = default;
};
struct WaitObject {
    enum class Kind { Event, Semaphore, Timer, Mutex, ChangeNotification, MemoryResource } kind{};
    std::uint32_t count{}, maximum{};
    bool manual_reset{};
    std::uint64_t due{}, period{},absolute_due{};
    std::uint64_t timer_owner{},timer_routine{},timer_argument{},timer_nominal_due{};
    std::uint32_t timer_tolerance{};
    std::u16string timer_reason;
    bool active{};
    std::uint32_t memory_notification_type{};
    std::uint64_t mutex{}; // Mutex handle 指向獨立物件；同名建立的多 handle 共用 ownership。
    std::shared_ptr<SharedEvent> shared_event{};
    std::uint32_t access{0x1f0003};
    std::uint64_t identity{}; // Semaphore／Timer 的 handle 與狀態分開；複製 runtime 仍保留值語義。
    std::u16string name{};
    std::vector<std::uint8_t> security_descriptor{};
    // 資源條件由本輪同一份全系統 snapshot 更新；不把另一程序不同時間的半組
    // 低／高訊號混成可同時滿足的 WaitAll。shared event 僅提供傳輸生命週期與喚醒。
    bool signaled()const noexcept{return kind==Kind::MemoryResource?count!=0:shared_event?shared_event->signaled():count!=0;}
    void set_signal(bool value){if(kind==Kind::MemoryResource)count=value?1U:0U;if(shared_event)shared_event->set(value);else count=value?1U:0U;}
    void consume_signal()noexcept{if(kind==Kind::MemoryResource)return;if(shared_event)shared_event->consume_locked();else if(!manual_reset && count)--count;}
    bool operator==(const WaitObject&) const = default;
};
// 單一程序的 handle 上限：Windows 每個程序最多 2^24（16,777,216）個 handle。
inline constexpr std::size_t process_handle_limit=std::size_t{1}<<24;
// wait 物件 handle 表：沿用 std::map 介面，另外維護計時器 handle 索引。
// 排程器每個時間片只需處理計時器；索引讓成本與計時器數量相關，而不是與全部 handle 數量相關。
// kind 只在建立時設定（插入後不改變），因此由 emplace／erase 維護即可。
class WaitObjectTable {
public:
    using map_type=std::map<std::uint64_t,WaitObject>;
    using key_type=map_type::key_type;using mapped_type=map_type::mapped_type;using value_type=map_type::value_type;
    using size_type=map_type::size_type;using iterator=map_type::iterator;using const_iterator=map_type::const_iterator;
    iterator begin() noexcept {return items_.begin();}
    iterator end() noexcept {return items_.end();}
    const_iterator begin() const noexcept {return items_.begin();}
    const_iterator end() const noexcept {return items_.end();}
    iterator find(key_type key){return items_.find(key);}
    const_iterator find(key_type key) const {return items_.find(key);}
    bool contains(key_type key) const {return items_.contains(key);}
    WaitObject& at(key_type key){return items_.at(key);}
    const WaitObject& at(key_type key) const {return items_.at(key);}
    size_type size() const noexcept {return items_.size();}
    bool empty() const noexcept {return items_.empty();}
    template<class... Args> std::pair<iterator,bool> emplace(Args&&... args){
        auto result=items_.emplace(std::forward<Args>(args)...);
        if(result.second && result.first->second.kind==WaitObject::Kind::Timer){
            try{timers_.insert(result.first->first);}catch(...){items_.erase(result.first);throw;}
        }
        return result;
    }
    size_type erase(key_type key){timers_.erase(key);return items_.erase(key);}
    iterator erase(iterator at){timers_.erase(at->first);return items_.erase(at);}
    iterator erase(const_iterator at){timers_.erase(at->first);return items_.erase(at);}
    const std::set<key_type>& timers() const noexcept {return timers_;}
    bool operator==(const WaitObjectTable&) const = default;
private:
    map_type items_;
    std::set<key_type> timers_;
};
struct MutexObject {
    std::u16string name;
    std::uint64_t owner{};
    std::uint32_t recursion{};
    bool abandoned{};
    std::shared_ptr<SharedMutex> shared{};
    std::vector<std::uint8_t> security_descriptor{};
    bool operator==(const MutexObject&) const = default;
};
struct SortState {
    std::uint64_t base{}, size{}, compare{};
    std::vector<std::size_t> order, merged;
    std::size_t width{1}, left{}, middle{}, right{}, i{}, j{}, k{};
    bool active{}, waiting{};
    bool search{};
    std::uint64_t key{};
    bool operator==(const SortState&) const = default;
};
struct VirtualRegion {
    std::uint64_t size{};
    std::uint32_t protection{};
    bool placeholder{},replaced_placeholder{};
    bool operator==(const VirtualRegion&) const = default;
};
struct VirtualAddressRequirements {
    std::uint64_t lowest{},highest{},alignment{};
};
struct OfferedMemoryPage {
    Permissions permissions{};
    std::uint32_t priority{};
    bool discarded{};
    bool operator==(const OfferedMemoryPage&) const = default;
};
struct VirtualMemoryInformation {
    std::uint64_t base{},allocation{},size{};
    std::uint32_t initial{},state{},protection{},type{};
};
struct WmiValue {
    std::uint16_t type{};std::uint32_t cim{};std::uint64_t number{};std::u16string text;
    std::vector<std::uint8_t> bytes{};
    bool operator==(const WmiValue&) const = default;
};
using WmiRow=std::map<std::u16string,WmiValue>;
struct WmiObject {
    unsigned kind{},references{1};std::size_t cursor{};std::vector<WmiRow> rows;
    std::u16string name_space;
    std::vector<std::u16string> property_names;bool enumerating{};
    bool operator==(const WmiObject&) const = default;
};
struct CriticalSection {
    std::uint64_t owner{}, debug{};
    std::uint32_t recursion{}, spin{};
    // 只在並行服務中以 atomic_ref 存取；協調者必須等所有 worker 返回才讀寫物件。
    std::uint32_t worker_guard{};
    bool operator==(const CriticalSection&) const = default;
};
struct SlimLock {
    std::uint64_t exclusive{};
    std::map<std::uint64_t,std::uint32_t> readers;
    bool operator==(const SlimLock&) const = default;
};
struct OnceInitialization {
    std::uint64_t owner{}, context{};
    bool complete{};
    bool operator==(const OnceInitialization&) const = default;
};
struct ProcessSnapshot {
    struct Process {
        std::uint32_t id{},parent{},threads{},priority{};std::u16string executable;
        bool operator==(const Process&) const = default;
    };
    std::vector<Process> processes;
    struct Module {
        std::uint64_t base{};std::uint32_t size{},references{};std::u16string path;
        bool operator==(const Module&) const = default;
    };
    std::vector<Module> modules;
    std::vector<std::uint64_t> threads;
    std::u16string executable;
    std::uint32_t flags{};
    std::size_t process_cursor{}, thread_cursor{}, module_cursor{};
    bool operator==(const ProcessSnapshot&) const = default;
};
struct CompletionPacket {
    std::uint64_t key{}, overlapped{}, status{};
    std::uint32_t bytes{};
    bool operator==(const CompletionPacket&) const = default;
};
struct CompletionPort {
    std::deque<CompletionPacket> packets;
    bool operator==(const CompletionPort&) const = default;
};
struct EventProviderRegistration {
    std::array<std::uint8_t,16> id{};
    std::uint64_t callback{}, context{};
    std::vector<std::uint8_t> traits;
    bool use_descriptor_type{};
    bool classic{};
    std::vector<std::pair<std::uint64_t,std::array<std::uint8_t,16>>> trace_classes;
    bool operator==(const EventProviderRegistration&) const = default;
};
struct DirectorySearch {
    bool streams{};
    std::vector<DirectoryEntry> entries;
    std::size_t cursor{};
    bool operator==(const DirectorySearch&) const = default;
};
struct AlignedAllocation {
    std::uint64_t base{}, size{}, alignment{}, offset{}; // offset：_aligned_offset_* 要求 (pointer+offset) 對齊。
    bool operator==(const AlignedAllocation&) const = default;
};
struct SymbolEntry {
    std::string name;
    std::uint64_t address{};
    std::uint32_t size{}, index{}, flags{}, tag{};
    bool operator==(const SymbolEntry&) const = default;
};
struct SymbolModule {
    RuntimeImage image;
    std::u16string name, image_name, loaded_image_name;
    std::vector<SymbolEntry> symbols;
    std::shared_ptr<const std::vector<std::uint8_t>> pending;
    std::uint32_t timestamp{}, checksum{}, machine{}, symbol_type{};
    bool no_symbols{}, virtual_module{}, invaded{};
    bool operator==(const SymbolModule&) const = default;
};
struct SymbolSession {
    std::u16string search_path;
    std::set<std::uint64_t> modules;
    std::map<std::uint64_t,SymbolModule> images;
    std::uint64_t function_callback{}, function_context{};
    bool function_callback_active{};
    bool operator==(const SymbolSession&) const = default;
};
struct ComClassRegistration {
    std::array<std::uint8_t,16> clsid{};
    std::uint64_t object{}, apartment{}; // 0 是共用 MTA；STA 使用客體執行緒 ID。
    std::uint32_t context{}, flags{};
    bool pending{}, canceled{}, suspended{}, reserved{}, used{};
    bool operator==(const ComClassRegistration&) const = default;
};
struct WinrtModule {std::u16string path{};std::uint32_t active{},references{1};bool unloading{};std::set<std::u16string> aliases{};bool operator==(const WinrtModule&)const=default;};
struct WinrtString {std::uint64_t data{};std::uint32_t length{},references{1};bool operator==(const WinrtString&)const=default;};
struct CrtLocaleCategory {
    std::u16string name{u"C"};
    std::uint32_t codepage{}, lcid{};
    unsigned maximum_character_bytes{1};
    std::array<bool,256> lead{},trail{};
    bool operator==(const CrtLocaleCategory&) const = default;
};
struct CrtLocaleObject {
    std::array<CrtLocaleCategory,6> categories;
    std::uint64_t storage{};
    bool legacy{};
    bool operator==(const CrtLocaleObject&) const = default;
};
struct RuntimeState {
    process_io::Counters io_counters;
    std::uint32_t startup_show{UINT32_MAX};bool startup_show_used{};
    std::optional<std::array<std::uint64_t,3>> startup_handles;
    std::vector<std::uint8_t> startup_reserved2;
    std::uint64_t startup_reserved2_address{};unsigned startup_reserved2_pointer_bytes{};
    std::optional<std::uint32_t> job_exit_code;
    std::optional<std::uint32_t> shutdown_exit_code;
    std::optional<std::uint32_t> forced_exit_code()const noexcept{return job_exit_code?job_exit_code:shutdown_exit_code;}
    std::uint64_t observed_commit_peak{};
    std::uint32_t symbol_options{6};
    std::map<std::uint64_t,SymbolSession> symbol_sessions;
    unsigned processor_count{1};
    std::uint64_t process_affinity{1};
    std::uint32_t shutdown_level{0x280}, shutdown_flags{};
    std::uint32_t dynamic_code_policy{}, child_process_policy{},signature_policy{};
    // 現代客體預設保留例外；顯式開啟 legacy callback filter 才忽略逸出 GUI 邊界的例外。
    std::uint32_t user_mode_exception_policy{};
    std::map<std::uint64_t,EventProviderRegistration> event_providers;
    std::uint64_t next_event_provider{1};
    std::uint32_t trace_message_sequence{};
    std::uint64_t winrt_oom_callback{}; // _SetWinRTOutOfMemoryExceptionCallback。
    etw::TraceState traces;
    std::uint64_t processor_sample_clock{}, processor_sample_user{}, processor_sample_kernel{};
    std::uint64_t processor_user{}, processor_kernel{}, processor_idle{};
    bool processor_sample_initialized{};
    std::uint64_t process_creation_time{}, process_start_user{}, process_start_kernel{};
    std::map<std::uint64_t,DirectorySearch> directory_searches;
    NetworkState network;
    HttpState http;
    EventLogState eventlog;
    TlsState tls;
    CertificateState certificates;
    std::map<std::uint64_t,CompletionPort> completion_ports;
    std::map<std::uint64_t,ProcessSnapshot> process_snapshots;
    std::map<std::u16string,std::pair<std::u16string,std::u16string>> environment, crt_environment;
    std::map<std::u16string,std::pair<std::uint64_t,std::uint64_t>> environment_values;
    std::uint64_t environment_allocation{};
    std::vector<std::uint64_t> console_handlers;
    bool ignore_ctrl_c{};
    std::optional<std::uint8_t> conio_byte_pushback;
    std::optional<std::uint16_t> conio_wide_pushback;
    std::uint32_t crt_umask{};
    bool file_apis_ansi{true};
    bool registry_cache_disabled{},registry_user_cache_disabled{};
    std::map<std::uint32_t,std::u16string> registry_predefined_cache;
    std::map<std::uint64_t,std::uint32_t> token_handles{};
    std::map<std::uint64_t,std::uint64_t> token_handle_objects;
    std::map<std::uint64_t,VirtualToken> token_objects;
    std::vector<std::uint8_t> process_security_descriptor,initial_thread_security_descriptor;
    std::uint64_t next_token_object{2};
    LsaState lsa;
    std::set<std::uint64_t> local_allocations, wts_allocations;
    std::map<std::uint64_t, AlignedAllocation> aligned_allocations;
    std::map<std::uint64_t, VirtualRegion> virtual_regions;
    struct PageProtection {std::uint32_t mode{},initial{};bool operator==(const PageProtection&) const = default;};
    std::map<std::uint64_t,PageProtection> virtual_page_protections;
    std::map<std::uint64_t, OfferedMemoryPage> offered_memory_pages;
    // MEM_RESET 的內容完整性；內容丟棄／offer 後不能再聲稱 RESET_UNDO 成功。
    std::map<std::uint64_t, bool> reset_memory_pages;
    std::uint64_t next_virtual{0x1000000000};
    std::map<std::uint64_t, CryptoAlgorithm> crypto_algorithms;
    std::map<std::uint64_t, CryptoHashObject> crypto_hashes;
    std::map<std::uint64_t,LegacyCryptHash> legacy_crypto_hashes;
    std::map<std::uint64_t,LegacyCryptKey> legacy_crypto_keys;
    std::map<std::pair<std::uint64_t,std::uint32_t>,LegacyCryptKey> legacy_crypto_user_keys;
    std::map<std::uint64_t, CryptoKey> crypto_keys;
    std::uint32_t dhcp_startups{};
    std::map<std::uint64_t,WinTrustState> trust_states;
    std::map<std::uint64_t,CatalogAdmin> catalog_admins;
    std::map<std::uint64_t,CatalogContext> catalog_contexts;
    std::set<std::uint64_t> storage_crypto_providers;
    std::map<std::uint64_t, StorageCryptoKey> storage_crypto_keys;
    std::map<std::uint64_t, std::vector<std::uint8_t>> crypto_secrets;
    std::uint64_t next_crypto_handle{0x80000};
    CrtFile standard_input;
    std::array<std::uint64_t,3> standard_handles{0x100,0x101,0x102};
    std::u16string console_input;
    std::vector<SortState> sorts;
    WaitObjectTable wait_objects;
    std::map<std::uint64_t, WaitObject> wait_states;
    WaitObject& wait_state(WaitObject& object){return object.identity?wait_states.at(object.identity):object;}
    const WaitObject& wait_state(const WaitObject& object)const{return object.identity?wait_states.at(object.identity):object;}
    bool wait_signaled(const WaitObject& object,const std::vector<std::uint64_t>& handles,bool all)const {
        const auto& signal=wait_state(object);if(!signal.signaled())return false;
        if(all && signal.kind==WaitObject::Kind::Semaphore && object.identity){
            std::uint32_t required=0;for(auto handle:handles){const auto entry=wait_objects.find(handle);if(entry!=wait_objects.end() && entry->second.identity==object.identity)++required;}
            return signal.count>=required;
        }
        return true;
    }
    std::map<std::uint64_t,std::pair<std::vector<char>,std::size_t>> volume_searches;
    std::map<std::uint64_t,std::pair<std::vector<std::string>,std::size_t>> volume_mount_searches;
    std::map<std::uint64_t, MutexObject> mutex_objects;
    int wait_lock_descriptor(const WaitObject& object)const {
        if(object.shared_event)return object.shared_event->lock_descriptor();
        if(object.kind==WaitObject::Kind::Mutex){const auto& mutex=mutex_objects.at(object.mutex);if(mutex.shared)return mutex.shared->lock_descriptor();}
        return -1;
    }
    bool wait_ready(const WaitObject& object,const std::vector<std::uint64_t>& handles,bool all,std::uint64_t thread)const {
        if(object.kind!=WaitObject::Kind::Mutex)return wait_signaled(object,handles,all);
        const auto& mutex=mutex_objects.at(object.mutex);
        return mutex.shared?mutex.shared->available_locked(thread):!mutex.owner || mutex.owner==thread;
    }
    bool acquire_wait_object(WaitObject& object,std::uint64_t thread){
        if(object.kind!=WaitObject::Kind::Mutex){wait_state(object).consume_signal();return false;}
        auto& mutex=mutex_objects.at(object.mutex);
        if(mutex.shared)return mutex.shared->acquire_locked(thread);
        if(mutex.recursion==UINT32_MAX)throw HostCallFault("Mutex 遞迴持有次數超過上限");
        const bool abandoned=mutex.abandoned;mutex.owner=thread;++mutex.recursion;mutex.abandoned=false;return abandoned;
    }
    std::map<std::uint64_t, std::pair<std::uint64_t, bool>> process_thread_handles;
    std::map<std::uint64_t,std::uint32_t> thread_handle_access;
    std::map<std::uint64_t,std::uint32_t> handle_flags;
    std::int32_t thread_priority{};
    std::uint64_t next_wait_handle{0x50000};
    std::uint64_t allocate_wait_handle(){const auto handle=next_wait_handle;next_wait_handle+=4;return handle;}
    std::map<std::uint64_t, CrtFile> streams;
    std::map<std::uint32_t,std::uint64_t> descriptors;
    std::map<std::uint64_t,std::uint64_t> file_positions;
    std::map<std::uint64_t,FilePosition> win_file_positions;
    std::uint32_t next_descriptor{3};
    std::uint64_t lowio_table{},bad_ioinfo{};
    std::array<std::uint64_t,256> lowio_blocks{};
    bool lowio_standard_dirty{true};
    std::uint32_t max_stdio{512};
    std::map<std::uint32_t,std::uint64_t> stdio_slots;
    std::array<CrtFile,2> standard_output;
    std::array<std::uint64_t, 128> fls_callbacks{}, fls_values{};
    std::array<bool, 128> fls_used{};
    RuntimeImage image;
    std::uint64_t arena_next{0x1000};
    std::uint64_t arena_base{},arena_capacity{1024U*1024U};
    std::uint64_t argc_cell{}, argv_cell{}, environ_cell{}, fmode_cell{}, commode_cell{}, errno_cell{}, command_ansi{}, crt_nerr_cell{}, crt_errlist{};
    std::uint64_t wargv_cell{}, wenviron_cell{};
    std::map<std::string, std::uint64_t> modules;
    std::map<std::uint64_t,RuntimeModule> loaded_modules;
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_IMAGE
    // 主程式 EXE 的來源身分：與 DLL 相同，供 resident AOT 記憶體快取以內容雜湊辨識。
    std::optional<TranslationImage> main_translation{};
#endif
    std::map<std::uint64_t,RuntimeImage> builtin_images;
    RuntimeCxxExceptions cxx_exceptions;
    std::vector<RuntimeFunctionTable> dynamic_function_tables;
    std::uint64_t next_function_table_handle{0x10000};
    std::map<std::uint64_t,Eh4Operation> eh4_operations;
    std::uint64_t next_eh4_operation{1};
    std::map<std::uint64_t,CrtJumpOperation> crt_jump_operations;
    std::uint64_t next_crt_jump_operation{1};
    std::map<std::uint64_t,RuntimeModule> resource_modules;
    // 只有共用邊緣頁需要 reference count；大型配置內部的完整頁可整段映射／釋放。
    std::map<std::uint64_t,std::uint32_t> heap_pages;
    std::map<std::uint64_t,std::uint64_t> heap_generations;
    std::uint64_t next_heap_generation{1};
    std::set<std::uint64_t> main_dependencies;
    std::vector<std::uint64_t> module_order;
    std::map<std::uint64_t,ModuleOperation> module_operations;
    std::uint64_t next_module_operation{1}, next_module_base{0x180000000ULL};
    std::uint64_t loader_owner{};
    std::uint32_t loader_depth{};
    std::map<std::uint64_t,std::uint64_t> loader_lock_cookies;
    std::uint32_t next_loader_lock_cookie{1};
    std::vector<std::string> module_events;
    std::filesystem::path application_source;
    std::u16string application_directory, dll_directory;
    std::map<std::uint64_t,std::u16string> added_dll_directories;
    std::uint32_t default_dll_directories{};
    std::uint32_t search_path_mode{};
    std::u16string current_directory{u"C:\\"};
    std::array<bool,64> static_tls_slots{true};
    bool trap_unresolved_imports{};
    bool exclude_current_dll_directory{};
    std::array<std::uint32_t, 3> descriptor_modes{0x4000, 0x4000, 0x4000};
    std::uint64_t clock_ticks{};
    bool host_clock{};
    std::uint64_t host_clock_pending{};
    std::map<std::uint32_t,std::uint32_t> timer_periods;
    std::map<std::uint64_t,MultimediaFile> multimedia_files;
    std::uint64_t filetime_base{}, timezone_name{};
    std::uint64_t process_creation_clock_ticks{},system_time_anchor_ticks{},system_time_anchor_filetime{};
    std::uint32_t system_time_increment{10000},system_time_adjustment{10000};
    bool system_time_adjustment_disabled{true};
    std::shared_ptr<MachineClock> machine_clock;
    std::uint64_t local_system_filetime() const noexcept {
        if(system_time_adjustment_disabled)return filetime_base+clock_ticks;
        const auto elapsed=clock_ticks>=system_time_anchor_ticks?clock_ticks-system_time_anchor_ticks:0;
        const auto periods=elapsed/system_time_increment;
        if(system_time_adjustment && periods>(UINT64_MAX-system_time_anchor_filetime)/system_time_adjustment)return INT64_MAX;
        const auto result=system_time_anchor_filetime+periods*system_time_adjustment;
        const auto remainder=(elapsed%system_time_increment)*system_time_adjustment/system_time_increment;
        return result>INT64_MAX || remainder>static_cast<std::uint64_t>(INT64_MAX)-result?INT64_MAX:result+remainder;
    }
    MachineTime system_time_state() const noexcept {
        return machine_clock?machine_clock->snapshot(clock_ticks):MachineTime{local_system_filetime(),clock_ticks,0,system_time_increment,system_time_adjustment,system_time_adjustment_disabled};
    }
    std::uint64_t system_filetime() const noexcept {return system_time_state().filetime;}
    void detach_machine_clock() noexcept {
        if(!machine_clock)return;const auto time=system_time_state();clock_ticks=time.monotonic;filetime_base=time.filetime-clock_ticks;
        system_time_anchor_ticks=clock_ticks;system_time_anchor_filetime=time.filetime;system_time_increment=time.increment;system_time_adjustment=time.adjustment;system_time_adjustment_disabled=time.disabled;machine_clock.reset();
    }
    std::uint64_t system_deadline(std::uint64_t absolute) const noexcept {
        // 已到期時回傳本程序目前的單調時間；共用時鐘快照會重新讀取宿主時間而略超前 clock_ticks，
        // 呼叫端每次輪詢重算會讓期限永遠落在 clock_ticks 之後而不觸發。
        const auto time=system_time_state();const auto now=time.filetime,monotonic=time.monotonic;if(absolute<=now)return std::min(monotonic,clock_ticks);
        const auto delta=absolute-now;
        if(time.disabled)return delta>UINT64_MAX-monotonic?UINT64_MAX:monotonic+delta;
        if(!time.adjustment)return UINT64_MAX;
        const auto periods=delta/time.adjustment;
        if(periods>(UINT64_MAX-monotonic)/time.increment)return UINT64_MAX;
        const auto tail=(delta%time.adjustment)*time.increment;
        const auto remainder=tail/time.adjustment+(tail%time.adjustment!=0);
        const auto result=monotonic+periods*time.increment;
        return remainder>UINT64_MAX-result?UINT64_MAX:result+remainder;
    }
    std::map<std::uint64_t,std::uint64_t> tm_buffers;
    std::uint32_t error_mode{};
    std::uint32_t wer_flags{};
    bool priority_boost_disabled{};
    std::uint32_t process_priority_class{0x20};
    bool process_background{};
    std::uint32_t process_power_control{},process_power_state{};
    std::map<std::uint64_t,bool> environment_blocks;
    std::shared_ptr<std::vector<std::uint32_t>> entropy{std::make_shared<std::vector<std::uint32_t>>()};
    std::map<std::uint64_t,LegacyCryptProvider> crypt_providers;
    std::uint64_t crypto_client_window{};
    std::size_t entropy_index{};
    // 與 entropy 相同，在 Interpreter／JIT 分支間共用原生隨機運算的結果。
    std::shared_ptr<std::map<std::pair<std::size_t,std::string>,std::vector<std::uint8_t>>> native_crypto_entropy{
        std::make_shared<std::map<std::pair<std::size_t,std::string>,std::vector<std::uint8_t>>>()};
    std::uint32_t random_state{1};
    std::uint64_t pointer_cookie{};
    std::uint64_t next_activation_cookie{1};
    std::map<std::uint64_t,std::vector<std::pair<std::uint64_t,std::uint64_t>>> activation_stacks;
    std::uint64_t locale_name{}, locale_data{}, ctype_table{}, locale_names{}, initial_environ{}, initial_wenviron{};
    std::uint64_t crt_tempnam_sequence{}, crt_tmpnam_sequence{};
    std::array<std::uint64_t,2> crt_tmpnam_buffers{};
    std::map<std::uint64_t,CrtLocaleObject> crt_locales;
    std::uint64_t crt_current_locale{};
    std::map<std::uint64_t,std::uint64_t> crt_thread_locales;
    std::map<std::uint64_t,std::uint64_t> crt_locale_query_buffers;
    std::array<std::uint8_t,8> crt_implicit_mb_decode{},crt_implicit_mb_encode{};
    std::uint32_t crt_multibyte_codepage{65001};
    std::vector<CallbackFrame> callbacks;
    std::vector<std::uint64_t> exit_callbacks,quick_exit_callbacks;
    std::uint64_t purecall_handler{};
    std::uint64_t tls_exit_callback{};
    std::array<bool, 1088> tls_used{};
    std::array<std::uint64_t, 1088> tls_values{};
    std::map<std::uint64_t,WmiObject> wmi_objects;
    std::map<unsigned,std::uint64_t> wmi_vtables;
    std::optional<std::array<std::uint32_t,3>> com_security;
    std::map<std::uint64_t,WinrtString> winrt_strings;
    std::map<std::uint64_t,WinrtModule> winrt_modules;
    std::uint64_t winrt_empty_string{};
    ComMarshalBridge com_marshal;
    ComContextBridge com_contexts;
    ComBindBridge com_bind;
    ComRotBridge com_rot;
    ComProxyBridge com_proxies;
    com_error::State com_errors;
    ComErrorBridge com_error_bridge;
    TypeLibraryBridge type_libraries;
    std::map<std::uint32_t,ComClassRegistration> com_classes;
    std::uint32_t next_com_class{1}, com_server_references{};
    bool com_classes_suspended{};
    std::map<std::uint64_t,std::pair<std::uint64_t,std::uint64_t>> private_heaps; // 最大容量、使用量
    std::map<std::uint64_t,std::uint64_t> heap_owners;
    std::map<std::uint64_t,CriticalSection> heap_locks;
    std::set<std::uint64_t> unserialized_heaps;
    std::map<std::uint64_t, CriticalSection> critical_sections;
    std::uint64_t msvcp_global_locks{}; // INIT_ONCE 與 STL 全域遞迴鎖；locale 沿用既有 CRT 鎖
    std::map<std::uint64_t, SlimLock> slim_locks;
    std::map<std::uint64_t, OnceInitialization> once_initializations;
    std::map<std::uint64_t, CriticalSection> file_locks;
    std::map<std::uint64_t,std::vector<CrtIoOperation>> crt_io_operations;
    std::uint64_t crt_io_next{1};
    std::map<unsigned, std::uint64_t> signals;
    RegistryState registry;
    std::map<std::uint64_t,std::uint64_t> crt_error_buffers;
    std::uint64_t unhandled_filter{}, invalid_parameter_handler{}, math_handler{}, new_handler{};
    int new_mode{}, thread_locale{}, app_type{};
    int crt_error_mode{};
    std::uint32_t crt_abort_behavior{3};
    std::map<std::uint64_t,std::uint64_t> crt_redirects;
    bool exiting{};
    bool operator==(const RuntimeState&) const = default;
};
}
