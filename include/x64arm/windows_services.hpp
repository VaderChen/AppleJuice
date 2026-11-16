#pragma once
#include <variant>
#include <tuple>
#include <iosfwd>
#include "x64arm/host_services.hpp"
#include <atomic>
#include <pthread.h>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <unordered_map>
#include "x64arm/readonly_files.hpp"
#include "x64arm/autoproxy.hpp"
#include "x64arm/windows_runtime.hpp"
#include "x64arm/windows_abi.hpp"
#include "x64arm/windows_address_layout.hpp"
#include "x64arm/native_library.hpp"
#include "x64arm/angle.hpp"
#include "x64arm/native_v8.hpp"
#include "x64arm/windows_exceptions.hpp"
#include "x64arm/windows_gui.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/windows_threads.hpp"
#include "x64arm/windows_process.hpp"
#include "x64arm/named_pipe.hpp"
#include "x64arm/windows_opengl.hpp"
#include "x64arm/raster_surface.hpp"
#include "x64arm/input.hpp"
#include "x64arm/shared_window.hpp"
#include "x64arm/shared_token.hpp"
#include "x64arm/windows_power.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/native_authentication.hpp"
#include "x64arm/debug_events.hpp"
#include "x64arm/usb.hpp"

namespace x64arm {
namespace dns_detail {struct Record;}
namespace wlan {class Backend;}
// 協調者本地一般服務的 provider 要啟動客體回呼時丟出；呼叫端恢復狀態交回完整路徑。
struct LocalServiceRefused {};
struct ResidentHostEnvironment;
struct PrivateKeyStoreContext;
class SharedPolicy;
namespace audio {class InputBackend;class MixerBackend;}
namespace dhcp {class Backend;}
namespace bluetooth {class Backend;}
struct WinmmMixerState;
namespace security {struct SecurityDescriptor;}
struct SynchronizationDescriptor;
struct DWriteState;
struct StackWalkState;
struct MinidumpState;
struct DemangleState;
struct Msvcp100State;
struct DeviceConfigurationState;
struct SetupInfState;
struct BluetoothState;
class MachineShutdown;
struct AccessibleState;
struct D3D9State;
struct D3D11State;
struct DirectSoundState;
struct WasapiState;
struct WicState;
struct MediaFoundationState;
struct MediaSurface;
struct DirectInputState;
struct DirectMusicState;
class NativePowerMonitor;
struct FileMappingState;
struct ThreadpoolState;
namespace service_callback {template<class T> class Task;}
struct HandleTransferState;
struct ProcessState;
struct RemoteThreadState;
struct PerflibState;
struct PdhState;
struct TransactionState;
struct KtmTransaction;
struct DdeState;
struct MciState;
struct LegacyJoystick;
struct JobState;
struct PrintingState;
struct MetafileState;
struct OleStreamState;
struct ComRpcState;
struct StorageState;
struct MsiState;
struct EsentState;
namespace compound {struct Node;struct Document;}
struct SafeArrayState;
struct GdiplusState;
struct RibbonState;
struct TaskDialogState;
struct OpenclState;
// 建立在 D3D12 上的 API（DirectML）錄製的重播回呼：參數為裝置的共用後端，以及由 ID3D12Resource 取得 GPU 資源的函式。
using D3D12Replay=std::function<void(NativeGraphics&,const std::function<std::shared_ptr<GraphicsResource>(std::uint64_t)>&)>;
struct VulkanState;
struct DirectMLState;
struct XessState;
struct FidelityFxState;
struct PmrState;
struct WebView2State;
struct WebBrowserState;
struct D3D12State;
struct DXCoreState;
struct MsxmlState;
std::span<const WindowsServiceDescriptor> msxml_service_descriptors();
struct NativeConsole;
class NativeSerial;
struct SerialTransfer;
class DirectoryWatch;
struct PowerRequest;
struct DataProtectionState;
struct WinmmMidiOutput;
struct WinmmMidiInput;
struct WinmmWaveOutput;
struct XAudioState;
struct WinmmBeepTone;
struct AcmState;
struct WinmmWaveInput;
struct PeImport;
struct PeOptions;
struct PeProgram;
struct UrlCacheState;
struct DebugMemoryState;
namespace debugger {struct TransportEvent;struct Frame;}
// 所有 D3D11／DXGI 服務共用的可重入鎖。持有時間多半不到 1 µs、每秒數十萬次取放（Fire Strike 4 vCPU 時四條錄製執行緒互搶）：
// pthread 可重入鎖每次取放都走慢路徑，競爭時 vCPU 執行緒有 12～20% 時間耗在 _pthread_mutex_firstfit_lock_slow。
// 這裡以擁有者欄位一次 CAS 取鎖、同執行緒只加深度；取不到時由 lock_graphics 先自旋，再以 atomic wait 進核心等待。
class GraphicsMutex {
public:
    GraphicsMutex()=default;GraphicsMutex(const GraphicsMutex&)=delete;GraphicsMutex& operator=(const GraphicsMutex&)=delete;
    bool try_lock() noexcept {
        const auto self=self_id();
        if(owner_.load(std::memory_order_relaxed)==self){++depth_;return true;}
        std::uintptr_t expected=0;
        if(owner_.load(std::memory_order_relaxed) || !owner_.compare_exchange_strong(expected,self,std::memory_order_acquire,std::memory_order_relaxed))return false;
        depth_=1;return true;
    }
    void lock() noexcept {
        if(try_lock())return;
        const auto self=self_id();
        waiters_.fetch_add(1,std::memory_order_seq_cst);
        for(;;){
            auto current=owner_.load(std::memory_order_seq_cst);
            if(!current){if(owner_.compare_exchange_strong(current,self,std::memory_order_acquire,std::memory_order_relaxed))break;continue;}
            owner_.wait(current,std::memory_order_seq_cst);
        }
        waiters_.fetch_sub(1,std::memory_order_seq_cst);
        depth_=1;
    }
    void unlock() noexcept {
        if(--depth_)return;
        owner_.store(0,std::memory_order_seq_cst);
        if(waiters_.load(std::memory_order_seq_cst))owner_.notify_one();
    }
private:
    // 執行緒識別：arm64 macOS 的 TPIDRRO_EL0 是執行緒 TSD 基底（低 3 位元是 CPU 編號），不必經 dyld stub 呼叫 pthread_self。
    static std::uintptr_t self_id() noexcept {
#if defined(__aarch64__) && defined(__APPLE__)
        std::uintptr_t value;asm volatile("mrs %0, tpidrro_el0":"=r"(value));return value&~std::uintptr_t{7};
#else
        return reinterpret_cast<std::uintptr_t>(pthread_self());
#endif
    }
    alignas(64) std::atomic<std::uintptr_t> owner_{0};
    std::atomic<unsigned> waiters_{0};
    unsigned depth_{0}; // 只由擁有者讀寫。
};
class WindowsServices final : public HostServices {
    friend struct WindowsNetworkTestAccess;
    friend struct WindowsHeapTestAccess;
    friend PeProgram load_pe(std::span<const std::uint8_t>,PeOptions);
public:
    WindowsServices();
    std::string native_library_diagnostics() const{return native_libraries_.diagnostics()+(native_angle_?native_angle_->diagnostics():std::string{})+(native_v8_?native_v8_->diagnostics():std::string{});}
    ~WindowsServices() override;
    // 建立服務時擷取 X64ARM_TRACE_SERVICES；執行中調整診斷請走此入口。
    void set_service_trace(std::string filter) { trace_services_=std::move(filter); }
    void retired(GuestMemory& memory, std::uint64_t instructions) override;
    void after_instructions(GuestState& state, GuestMemory& memory, std::uint64_t count) override;
    void set_processor_count(GuestMemory&,unsigned);
    void set_authentication_backend(std::shared_ptr<authentication::Backend> backend){authentication_backend_=std::move(backend);}
    void set_dhcp_backend(std::shared_ptr<dhcp::Backend> backend){dhcp_backend_=std::move(backend);}
    void set_bluetooth_backend(std::shared_ptr<bluetooth::Backend>);
    void set_network_resource_backend(std::shared_ptr<network_resources::Backend>);
    void set_input_backend(std::shared_ptr<input::Backend>);
    void set_usb_backend(std::shared_ptr<usb::Backend> backend){usb_backend_=std::move(backend);}
    void set_wlan_backend(std::shared_ptr<wlan::Backend>);
    void set_audio_input_backend(std::shared_ptr<audio::InputBackend>);
    void set_audio_mixer_backend(std::shared_ptr<audio::MixerBackend>);
    void set_power_notification_backend(std::shared_ptr<NativePowerNotifications>);
    bool set_process_affinity(GuestState&,std::uint64_t);
    bool concurrent_guest_threads() const noexcept override;
    unsigned parallel_workers()const noexcept override{return runtime_.processor_count;}
    std::vector<GuestState*> parallel_contexts(GuestState&) override;
#ifdef X64ARM_HOST_SERVICES_PARALLEL_SERVICE
    bool parallel_service(GuestAddress) const noexcept override;
    bool invoke_parallel_service(GuestState&,GuestMemory&) override;
    bool invoke_parallel_service_impl(GuestState&,GuestMemory&,ResidentServiceProgress&,std::span<GuestState* const> = {});
#endif
    ExecutionPolicy execution_policy(const GuestState&) const override;
    void after_parallel_instructions(GuestState&,GuestMemory&,std::span<const std::uint64_t>) override;
    std::uint64_t timestamp_counter(const GuestState&,GuestMemory&) override;
#ifdef X64ARM_HOST_SERVICES_RESIDENT_TIMESTAMP
    bool resident_timestamp_counter(const GuestState&,const GuestMemory&,std::uint64_t&) const noexcept override;
    void resident_mapping_read(bool acquire) const noexcept override;
#endif
    std::uint64_t native_chain_limit() const noexcept override;
#ifdef X64ARM_HOST_SERVICES_NATIVE_TIME_SLICE
    std::uint64_t native_chain_time_slice_ns() const noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_EXECUTION
    bool resident_execution() const noexcept override { return runtime_.host_clock; }
    void enter_resident_execution() noexcept override { ++resident_execution_depth_; }
    void leave_resident_execution() noexcept override { --resident_execution_depth_; }
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_PARALLEL
    std::uint64_t resident_context_key(const GuestState&) const noexcept override;
#ifdef X64ARM_HOST_SERVICES_MEMORY_WATCH
    std::uint64_t memory_watch_thread_id(const GuestState& state) const noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_SERVICE_REQUEST
    void resident_service_request(std::uint64_t,GuestAddress) override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTROL_REQUEST
    void resident_control_request(std::uint64_t,GuestAddress,bool) override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTROL_DRAIN
    bool resident_select_control_request(GuestState&,GuestMemory&) override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_PREEMPT
    bool resident_preempt_plan(GuestMemory&,std::span<GuestState* const>,ResidentPreemptPlan&) override;
    void resident_preempt_commit(GuestState&,GuestState&) override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_BLOCK
    bool resident_block_service(GuestState&,GuestMemory&,std::span<GuestState* const>,GuestState*&) override;
    bool resident_block_parking() const noexcept override;
    bool resident_context_runnable(const GuestState&) const override;
    void resident_unpark(GuestState&,GuestMemory&) override;
    bool resident_resolve_waits(GuestMemory&) override;
    // 獨占映射鎖外的前置判斷：有沒有任何等待可能在這次解析被推進（見 resident_resolve_waits）。只在協調執行緒呼叫。
    bool resident_resolve_waits_pending(GuestMemory&);
    // 不取共用命名空間鎖，判斷這組 handle 是否「可能」就緒（resolve_thread_waits 與前置判斷共用）。
    bool wait_may_be_ready(const std::vector<std::uint64_t>& handles,bool all,std::uint64_t thread_id);
    bool resident_runnable_waiting(std::span<GuestState* const>) const override;
    void trace_guest_thread_waits(std::FILE* out,const GuestMemory* memory=nullptr) const;
    // 診斷（X64ARM_TRACE_THREAD_DUMP）：事件操作環形紀錄，傾印時列出等待中 handle 最近的操作。
    void trace_event_operation(char operation,std::uint64_t handle,std::uint64_t thread,std::uint64_t caller) const;
    GuestState* resident_idle_replacement(GuestState&,GuestMemory&,std::span<GuestState* const>) override;
    bool resident_block_service_impl(GuestState&,GuestMemory&,std::span<GuestState* const>,GuestState*&,int* outcome);
#endif
    void after_resident_parallel(GuestState&,GuestMemory&) override;
    void after_resident_interrupt(GuestState&,GuestMemory&) override;
    std::uint64_t resident_parallel_wakeup_ns(std::span<GuestState* const>) const override;
    std::shared_ptr<const ResidentExternalWake> resident_external_wake(std::uint64_t& observed) const override;
#ifdef X64ARM_HOST_SERVICES_RESIDENT_IDLE_WAKE
    std::shared_ptr<const ResidentExternalWake> resident_idle_wake(std::uint64_t& observed) const override;
    void request_resident_poll() override;
#endif
    ResidentServiceProgress resident_parallel_service(GuestState&,GuestMemory&) override;
#ifdef X64ARM_HOST_SERVICES_RESIDENT_ACTIVE_CONTEXTS
    ResidentServiceProgress resident_parallel_service(GuestState&,GuestMemory&,std::span<GuestState* const>) override;
#endif
#ifdef X64ARM_HOST_SERVICES_COORDINATED_SERVICE
    bool resident_coordinated_service(GuestState&,GuestMemory&) override;
#endif
#endif
    bool allows_multiblock() const noexcept override;
    std::uint64_t instruction_slice() const noexcept override;
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_REGION
    std::optional<TranslationRegion> translation_region(GuestAddress) const override;
#endif
#ifdef X64ARM_HOST_SERVICES_TRANSLATION_IMAGE
    std::optional<TranslationImage> translation_image(GuestAddress) const override;
    void set_main_translation(TranslationImage image){runtime_.main_translation=std::move(image);}
    // 載入映像的內容身分（SHA-256）：以宿主檔案（裝置、inode、大小、mtime、ctime）為鍵跨程序快取。
    // libcef.dll 約 210 MiB，每個 webhelper 程序原本各算一次約 70 ms。身分只用於記憶體快取區段命名；
    // 還原時仍逐條比對客體位元組，鍵錯也只會未命中。X64ARM_IDENTITY_CACHE=0 關閉。
    static std::array<std::uint8_t,32> image_identity(const std::filesystem::path& source,std::span<const std::uint8_t> bytes);
#endif
    static constexpr GuestAddress stub_base{windows_address_layout::x64.services};
    // 服務登錄稽核（x64arm audit-services）：逐筆列出群組、ABI、排重結果與路由到的處理函式，並彙整同名登錄的 ABI 衝突。
    static void audit_runtime_services(std::ostream& out);
    // 合併後的延伸描述表（runtime_service_descriptors() 的來源）。
    static std::span<const WindowsServiceDescriptor> runtime_service_table();
    static constexpr std::size_t output_limit = 16U * 1024U * 1024U;
    static constexpr GuestAddress command_line_address{0x3001000000ULL}; // x64 系統區塊位於 MMU 視窗內（見 GuestMemory::native_mmu_window）。
    // 程序堆積的 handle 與 Windows 相同是可讀的 heap 開頭位址（PEB.ProcessHeap、GetProcessHeap）；固定在 KUSER 頁下方、
    // 2 GiB 以內（x86／x64 共用，客體常式可用 imm32 比較），開頭維持 Windows 10 相容的簽章、Flags 與 ForceFlags 欄位。
    static constexpr std::uint64_t process_heap = 0x7ffd0000;
    void write_heap_header(GuestMemory& memory,std::uint64_t address,std::uint32_t flags);
    void initialize(GuestMemory& memory, std::u16string_view command_line, const std::filesystem::path& filesystem_root = {}, const std::filesystem::path& c_drive = {}, bool writable_files = false, const std::filesystem::path& install_root = {}, const std::map<char,std::filesystem::path>& drives = {}, std::string_view certificate_roots_file = {}, bool resident_translation = false);
    bool uses_guard_fast_path() const noexcept {return guard_fast_code_.value!=0;}
    std::u16string map_executable(const std::filesystem::path& path);
    void configure_image(GuestState& state, GuestMemory& memory, RuntimeImage image);
    void configure_loader(const std::filesystem::path& source, bool trap_unresolved,const std::filesystem::path& directory={});
    GuestAddress bind_image_import(GuestMemory&, const PeImport&, std::uint64_t owner);
    void prepare_image_startup(GuestState&);
    // 以目前模組集合（主映像、內建系統模組、已載入 DLL）重建 PEB->Ldr 的三條模組串列；集合未變時不改寫。
    void publish_loader_data(GuestMemory& memory);
    std::optional<std::uint32_t> wait_for_initial_thread(GuestState&,GuestMemory&);
    void set_standard_input(std::span<const std::uint8_t> bytes);
    void set_live_standard_io(int input = 0, int output = 1, int error = 2);
    void map_serial_port(unsigned,const std::filesystem::path&);
    // 僅在客體啟動前設定；host 使用跨程序一致的宿主單調時間。
    void set_host_clock(GuestMemory& memory, bool enabled = true);
    bool uses_host_clock() const noexcept { return runtime_.host_clock; }
    void restrict_comparison() { runtime_.detach_machine_clock();machine_shutdown_.reset();runtime_.network.external_io=false;shared_tokens_.clear();mutating_shared_token_=0; }
    // 唯讀檢查；名稱須為 loader 使用的小寫 DLL basename。
    bool builtin_module(std::string_view name) const;
    GuestAddress resolve(std::string_view dll, std::string_view symbol) const;
    GuestAddress bind_import(std::string_view dll, std::string_view symbol, bool trap_unresolved);
    const std::vector<std::string>& unresolved_imports() const noexcept { return unresolved_imports_; }
    bool dispatch_cpu_exception(GuestState& state, GuestMemory& memory, std::uint32_t code) override;
    bool dispatch_cpu_trap(GuestState& state,GuestMemory& memory,std::uint32_t code,unsigned length) override;
    bool dispatch_system_call(GuestState&,GuestMemory&) override;
    bool dispatch_system_call(GuestState&,GuestMemory&,SystemCallEntry) override;
    bool software_interrupt_enabled(std::uint8_t vector) const noexcept override;
    bool system_enter_enabled(ExecutionMode mode) const noexcept override {return initialized_ && x86_ && mode==ExecutionMode::X86;}
    std::optional<ExecutionResult> dispatch_software_interrupt(GuestState&,GuestMemory&,std::uint8_t,unsigned) override;
    bool dispatch_fault(GuestState& state, GuestMemory& memory, const MemoryFault& fault) override;
    bool handles(GuestAddress address) const noexcept override;
#ifdef X64ARM_HOST_SERVICES_RESIDENT_SERVICE
    // 只允許已在 invoke 走計算邊界的服務；與 invoke 使用同一分類。
    bool resident_service(GuestAddress address) const noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_HOST_FP
    bool resident_service_host_fp(GuestAddress address) const noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_MANAGED_FP
    bool resident_service_manages_fp(GuestAddress) const noexcept override;
    std::optional<std::uint32_t> invoke_managed_service(GuestState&,GuestMemory&,const ResidentHostEnvironment&) override;
#endif
#ifdef X64ARM_HOST_SERVICES_SCALAR_NATIVE
    bool resident_scalar_service(GuestAddress) const noexcept override;
    bool invoke_resident_scalar(ResidentScalarCall&,GuestMemory&) noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_SCALAR_RECIPE
    unsigned resident_scalar_recipe(GuestAddress) const noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_CLOCK_RECIPE
    ResidentClockRecipe resident_clock_recipe(GuestAddress) const noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_HEAP
    ResidentHeapOperation resident_heap_recipe(GuestAddress) const noexcept override;
    const ResidentHeap* borrow_resident_heap(GuestMemory&) noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_SCALAR_ERRORS
    ResidentScalarErrors resident_scalar_errors(GuestMemory&) noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTEXT_ERRORS
    ResidentScalarErrors resident_context_errors(const GuestState&,GuestMemory&) noexcept override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTEXT_SCALAR
    bool resident_context_scalar_service(GuestAddress) const noexcept override;
    bool invoke_resident_context_scalar(ResidentScalarCall&,GuestMemory&,const ResidentScalarErrors&) noexcept override;
#endif
    bool acquire_slim_word(GuestMemory&,std::uint64_t address,std::uint64_t id,bool shared);
    // 臨界區段的共用操作：guest 區段依客體字組協定（見 windows_threads.cpp），宿主內部鎖依 owner／recursion。
    static bool guest_critical_enabled();
    bool critical_held(GuestMemory&,std::uint64_t address,const CriticalSection&);
    bool critical_owned_by(GuestMemory&,std::uint64_t address,const CriticalSection&,std::uint64_t thread);
    std::uint32_t critical_recursion(GuestMemory&,std::uint64_t address,const CriticalSection&);
    bool critical_acquire(GuestMemory&,std::uint64_t address,CriticalSection&,std::uint64_t thread,bool queue);
    bool critical_wait_needed(GuestMemory&,std::uint64_t address,CriticalSection&,std::uint64_t thread);
    bool critical_release(GuestMemory&,std::uint64_t address,CriticalSection&);
    void critical_settle_waiters(GuestMemory&,std::uint64_t address,CriticalSection&);
    std::string_view name(GuestAddress address) const noexcept override;
    std::optional<std::uint32_t> invoke(GuestState& state, GuestMemory& memory) override;
    const std::string& standard_output() const noexcept { return stdout_; }
    const std::string& standard_error() const noexcept { return stderr_; }
    std::uint32_t last_error() const noexcept { return last_error_; }
    void enable_native_gui(const std::filesystem::path& snapshot = {});
    void set_native_gui_backend(std::shared_ptr<NativeGui>);
    // 可在訂閱前接入其他平台的原生電源來源；未設定時使用預設宿主後端。
    void set_power_notification_monitor(std::shared_ptr<NativePowerMonitor>);
    const GuiState& gui_state() const noexcept { return gui_; }
    bool same_state(const WindowsServices& other) const;
    std::string mapping_session();
    std::shared_ptr<class SharedWake> mapping_wake() const;
    // 常駐中斷的外部來源：排程器完整輪詢前取得的共用喚醒世代，與包裝它的等待來源。
    std::uint64_t resident_external_polled_{};
    mutable std::shared_ptr<const ResidentExternalWake> resident_external_source_;
    // 螢幕／工作區查詢結果的短期快取（X64ARM_GUI_AREA_CACHE_MS，預設 100）；NSScreen 每次約 0.45 ms，Chromium 每幀都查。
    std::chrono::steady_clock::time_point gui_area_cached_{};
    std::array<std::int32_t,4> gui_screen_cache_{},gui_work_cache_{};
    std::vector<GuiMonitor> gui_monitor_cache_;
    // 常駐熱路徑（每次服務返回、每次重新進入本體）的執行緒索引。執行緒表只增不刪：已結束者
    // 保留供 handle 與結束碼查詢，逐一走訪的成本會隨程序存活時間上升。表大小改變即重建；
    // 走訪時已結束者過半也重建，熱路徑只看未結束者。複本指向另一份執行緒表，一律重建。
    struct ResidentThreadIndex {
        std::vector<std::pair<std::uint64_t,const GuestThread*>> live;
        std::unordered_map<const GuestState*,std::uint64_t> contexts;
        std::size_t size{SIZE_MAX};
        bool stale{true};
        std::mutex mutex;
        ResidentThreadIndex()=default;
        ResidentThreadIndex(const ResidentThreadIndex&) {}
        ResidentThreadIndex& operator=(const ResidentThreadIndex&) {
            std::lock_guard lock(mutex);live.clear();contexts.clear();size=SIZE_MAX;stale=true;return *this;
        }
    };
    mutable ResidentThreadIndex resident_threads_;
    ResidentThreadIndex& resident_thread_index(std::unique_lock<std::mutex>&) const;
    std::vector<std::pair<std::uint64_t,GuestThread*>> live_threads();
    int mapping_session_descriptor();
    void set_mapping_session(const std::string&,int inherited_descriptor=-1);
    void configure_handle_transfer();
    void configure_process_identity();
    void import_process_handles(const std::string&);
    void import_crt_startup(GuestState&,GuestMemory&);
    void configure_processes(ProcessOptions,GuestMemory&);
    void report_process_accounting();
    void record_job_exit(std::uint32_t);
    std::uint32_t local_process_accounting(std::array<std::uint64_t,5>&) const;
    std::uint32_t remote_process_accounting(GuestMemory&,std::uint32_t,std::array<std::uint64_t,5>&);
    std::uint32_t query_process_accounting(GuestMemory&,std::uint64_t,std::array<std::uint64_t,5>&,bool);
    std::uint32_t import_process_job(int);
    void import_primary_token(int);
    // 頂層程式的 manifest 要求 requireAdministrator／highestAvailable 時，沿用 runas broker 的提升權杖（等同使用者同意 UAC）。
    void apply_manifest_elevation(const std::filesystem::path& executable);
    void import_guest_environment(GuestMemory&,const GuestState&,int);
    void set_registry_file(std::filesystem::path path);
    std::string process_diagnostics() const;
    std::uint32_t process_id() const noexcept;
    void set_process_directory(GuestMemory&,std::u16string directory);
    std::uint32_t change_current_directory(GuestMemory&,std::u16string directory);
    std::string video_diagnostics() const;
    std::string opengl_diagnostics() const;
    std::string dsound_diagnostics() const;
    std::string d3d9_diagnostics() const;
    std::string module_diagnostics() const;
private:
    std::size_t resident_execution_depth_{};
    // 僅讓主 EXE loader 交接其剛完成驗證並映射的映像；公開 configure_image 仍先驗證。
    void configure_image_after_unwind_validation(GuestState&,GuestMemory&,RuntimeImage,bool initial_thread=true);
    bool awaiting_initial_thread_{};
    std::optional<std::uint64_t> initial_thread_entry_;
    void discard_unstarted_thread(GuestMemory&,std::uint64_t,std::uint32_t);
    std::uint32_t process_image_source(std::uint64_t,ProcessImageSource&);
    NativeLibraries native_libraries_;
    std::shared_ptr<NativeAngle> native_angle_;
    std::optional<std::uint32_t> invoke_angle(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<NativeV8> native_v8_;
    bool native_v8_aborted_{};
    std::optional<std::uint32_t> invoke_native_v8(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> advance_native_v8(GuestState&,GuestMemory&,std::uint32_t);
    std::optional<std::uint32_t> resume_native_v8(GuestState&,GuestMemory&,std::uint32_t,std::uint32_t);
    void discard_native_v8(GuestMemory&,std::uint32_t);
    std::optional<std::uint32_t> invoke_native_library(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void install_guard(GuestMemory&, const RuntimeImage&);
    bool guard_target(const GuestMemory&, std::uint64_t) const;
    void complete_call(GuestState&, GuestMemory&, std::uint64_t value);
    GuiDialogLayout gui_dialog_layout(GuestMemory&,std::uint64_t,std::uint64_t);
    std::filesystem::path registry_file_;
    RegistryStoreCache registry_store_cache_;
    struct RegistryIoWorker;
    std::shared_ptr<RegistryIoWorker> registry_io_worker_;
    bool registry_io_busy_{};
    std::uint64_t registry_io_owner_{};
    bool registry_io_ready() const;
    bool defer_registry_io_call(GuestState&,GuestMemory&);
    void await_registry_io(GuestState&,GuestMemory&,const std::function<void()>&);
    OpenGlState opengl_;
    std::optional<std::uint32_t> invoke_opengl(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::map<std::uint64_t,std::shared_ptr<PowerRequest>> power_requests_;
    std::map<std::uint64_t,std::shared_ptr<PowerRequest>> thread_power_requests_;
    // KTM 交易 handle、PDH 查詢與其他系統物件 API 的程序內狀態。
    std::shared_ptr<TransactionState> transactions_;
    TransactionState& transaction_state();
    bool transaction_handle(std::uint64_t) const;
    bool transactions_open() const;
    std::optional<std::pair<std::uint64_t,std::uint32_t>> transaction_identity(std::uint64_t) const;
    bool close_transaction(std::uint64_t);
    std::uint64_t duplicate_transaction(std::uint64_t);
    std::uint32_t commit_transaction(KtmTransaction&);
    std::optional<std::uint32_t> invoke_transactions(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<PdhState> pdh_;
    std::uint32_t collect_pdh_query(std::uint64_t,std::uint64_t* = nullptr);
    std::optional<std::uint32_t> advance_pdh(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_pdh(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct PrivilegeAcquisition {
        bool process{},in_place{};std::uint64_t token{},thread{},previous_thread_token{},keeper{};
        std::vector<std::pair<std::uint64_t,std::uint32_t>> previous;
        bool operator==(const PrivilegeAcquisition&)const=default;
    };
    std::map<std::uint64_t,PrivilegeAcquisition> privilege_acquisitions_;
    std::vector<std::pair<std::u16string,std::uint64_t>> wer_runtime_modules_;
    // WER 註冊（kernel32）：附加檔案、自訂中繼資料、要納入／排除傾印的記憶體區塊（回報旗標在 RuntimeState::wer_flags）。
    struct WerFile {std::u16string key,path;std::uint32_t type{},flags{};};
    std::vector<WerFile> wer_files_;
    std::vector<std::pair<std::u16string,std::u16string>> wer_metadata_;
    std::vector<std::pair<std::uint64_t,std::uint32_t>> wer_memory_blocks_,wer_excluded_blocks_;
    // Xbox 遊戲 UI：等待 ProcessPendingGameUI 呼叫完成常式的 UI（完成常式、context、結果、是否為玩家選擇器）。
    struct PendingGameUi {std::uint64_t routine{},context{};std::uint32_t result{};bool picker{};};
    std::vector<PendingGameUi> pending_game_ui_;
    // RegisterAppStateChangeNotification 配置的註冊區塊（客體位址）。
    std::set<std::uint64_t> app_state_registrations_;
    std::map<std::uint32_t,std::uint32_t> gpu_scheduling_classes_;
    std::uint64_t open_local_thread(std::uint64_t thread,std::uint32_t rights,bool inherit);
    std::optional<std::uint32_t> invoke_system_objects(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // DDEML、使用者工作階段（視窗更新鎖、鍵盤配置、觸控注入）與 IInitializeSpy。
    std::shared_ptr<DdeState> dde_;
    DdeState& dde_state();
    std::optional<std::uint32_t> invoke_dde(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_dde_lists(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_dde(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_dde(std::uint64_t);
    std::optional<std::uint32_t> invoke_user_session(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool window_update_locked(std::uint64_t) const;
    std::uint64_t selected_keyboard_layout(std::uint64_t thread) const;
    std::vector<std::uint64_t> keyboard_layouts() const;
    std::uint32_t touch_injection_feedback_{};
    void deliver_pointer_sample(const input::PointerSample&);
    bool promote_pointer_message(std::uint64_t window,std::uint64_t wparam,std::uint32_t message);
    struct ComSpyOperation {
        enum class Phase {Register,Before,Call,After} phase{};
        bool uninitialize{},extended{};std::uint64_t reserved{},scratch{},output{},thread{};std::uint32_t mode{},status{};
        std::vector<std::uint64_t> spies;std::size_t index{};
        bool operator==(const ComSpyOperation&)const=default;
    };
    std::map<std::uint64_t,ComSpyOperation> com_spy_operations_;
    std::map<std::uint64_t,std::vector<std::pair<std::uint64_t,std::uint64_t>>> com_spies_;
    std::set<std::uint64_t> com_spy_bypass_;
    std::uint64_t next_com_spy_operation_{1},next_com_spy_cookie_{1};
    std::uint32_t com_apartment_references(std::uint64_t thread) const;
    std::optional<std::uint32_t> invoke_com_spy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // RoRegisterActivationFactories 的程序內工廠與 RoRegisterForApartmentShutdown 回呼。
    std::map<std::u16string,std::pair<std::uint64_t,std::uint64_t>> winrt_class_factories_;
    std::map<std::uint64_t,std::vector<std::u16string>> winrt_factory_cookies_;
    struct ApartmentShutdown {std::uint64_t object{},apartment{},identifier{};bool operator==(const ApartmentShutdown&)const=default;};
    std::map<std::uint64_t,ApartmentShutdown> apartment_shutdowns_;
    std::vector<GuestCallback> retire_apartment_shutdowns(GuestMemory&,std::uint64_t thread,std::optional<std::uint64_t> apartment);
    std::optional<std::uint32_t> resume_gdiplus_stream(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    // urlmon 安全區域管理員與 DXVA2 裝置管理員／視訊服務。
    struct UrlSecurityManager {std::uint32_t references{1};std::uint64_t site{};bool operator==(const UrlSecurityManager&)const=default;};
    std::map<std::uint64_t,UrlSecurityManager> url_security_managers_;std::uint64_t url_security_table_{};
    std::uint32_t url_zone(std::u16string_view url);
    std::optional<std::uint32_t> invoke_url_security(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct Dxva2Manager {std::uint64_t device{},locked{};std::uint32_t references{1},token{},generation{};std::map<std::uint64_t,std::uint32_t> handles;bool operator==(const Dxva2Manager&)const=default;};
    struct Dxva2Service {std::uint64_t device{};std::uint32_t references{1};bool processor{};bool operator==(const Dxva2Service&)const=default;};
    std::map<std::uint64_t,Dxva2Manager> dxva2_managers_;std::map<std::uint64_t,Dxva2Service> dxva2_services_;
    std::uint64_t dxva2_manager_table_{},dxva2_decoder_table_{},dxva2_processor_table_{};
    std::optional<std::uint32_t> invoke_dxva2(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // ndfapi Web incident：handle 對應目標 URL。
    std::map<std::uint64_t,std::u16string> network_incidents_;
    std::optional<std::uint32_t> invoke_network_diagnostics(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_credential_ui(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t finish_credential_prompt(GuestMemory&,std::uint64_t dialog,bool confirmed);
    std::optional<std::uint32_t> invoke_certificate_ui(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // WinUSB：USB 裝置介面路徑以 CreateFile 開啟，WinUsb_* 經 IOUSBHost 傳輸。
    std::optional<std::uint32_t> invoke_winusb(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_winusb(GuestState&,GuestMemory&,std::uint64_t,bool overlapped);
    void discard_winusb(std::uint64_t);
    void poll_winusb(GuestMemory&);
    bool winusb_waiting() const;
    std::optional<usb::Target> usb_target(const std::u16string& path);
    std::shared_ptr<struct WinUsbState> winusb_;
    std::shared_ptr<usb::Backend> usb_backend_;
    usb::Backend& usb_backend()const;
    // 協調者本地 SetupDiGetClassDevs 在取獨占映射鎖前先列舉的 USB 拓撲（IOKit 每次數毫秒）；下一次裝置盤點取用後清空。只由協調者讀寫。
    std::optional<std::vector<usb::Device>> usb_prefetch_;
    std::shared_ptr<struct WlanState> wlan_;
    std::shared_ptr<wlan::Backend> wlan_backend_;
    std::optional<std::uint32_t> invoke_wlan(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_wlan(GuestMemory&);
    bool wlan_waiting()const;
    void show_certificate_selection(std::uint64_t dialog);
    // fontsub CreateFontPackage：等待客體 allocproc 配置輸出緩衝區。
    struct FontPackageOutput {std::vector<std::uint8_t> bytes;std::uint64_t dest{},dest_size{},written{};bool operator==(const FontPackageOutput&)const=default;};
    std::map<std::uint64_t,FontPackageOutput> font_packages_;std::uint64_t next_font_package_{1};
    std::optional<std::uint32_t> invoke_font_package(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_font_package(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_font_package(std::uint64_t);
    void discard_gdiplus_stream(GuestState&,GuestMemory&,std::uint64_t);
    bool begin_initialize_spies(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_spy(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_com_spy(GuestState&,GuestMemory&,std::uint64_t);
    struct PowerNotification {
        std::uint64_t window{},callback{},context{},payload{};
        std::array<std::uint8_t,16> setting{};
        std::vector<NativePowerEvent> pending;
        bool settings{},closing{},user32{};
    };
    std::map<std::uint64_t,PowerNotification> power_notifications_;
    std::shared_ptr<NativePowerNotifications> power_notification_backend_;
    std::shared_ptr<DataProtectionState> data_protection_;
    std::optional<std::uint32_t> invoke_data_protection(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_power_notifications(GuestMemory&);
    bool power_notification_pending(std::uint64_t) const;
    void poll_power_notification_events(GuestMemory&);
    std::optional<std::uint32_t> invoke_power_notification_event(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_power_notification(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct ServiceControlHandle {std::u16string label;std::uint32_t access{},owner{};bool manager{},system{},guest{},status_handle{};std::uint64_t instance{};};
    std::map<std::uint64_t,ServiceControlHandle> service_control_handles_;
    // LockServiceDatabase：Vista 起僅供相容，鎖定不影響資料庫，QueryServiceLockStatus 回報持有者與時間。
    struct ServiceDatabaseLock {std::uint64_t handle{},since{};std::u16string owner;};
    std::optional<ServiceDatabaseLock> service_database_lock_;
    std::vector<struct ServiceCatalogEntry> service_catalog_entries();
    std::optional<std::uint32_t> invoke_service_catalog(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct GuestServiceDispatch {
        std::u16string label;
        std::uint64_t main{},handler{},context{},thread{},dispatcher{},storage{},control_sequence{};
        std::uint32_t argc{};
        bool wide{},extended{},stopped{};
    };
    struct GuestServiceWait {std::u16string label;std::uint64_t sequence{},deadline{};unsigned kind{};};
    std::map<std::uint64_t,GuestServiceDispatch> guest_service_dispatchers_;
    std::map<std::uint64_t,GuestServiceWait> guest_service_waits_;
    std::uint64_t guest_service_poll_time_{};
    void poll_guest_services(GuestMemory&);
    std::optional<std::uint32_t> invoke_guest_service(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_guest_service(GuestState&,GuestMemory&,std::uint64_t,bool);
    std::optional<std::uint32_t> invoke_service_control(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_power(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_power_setting_notification(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<NativePowerMonitor> power_monitor_;
    void poll_power_setting_notifications(GuestMemory&);
    bool finish_power_notification(const GuiMessage&);
    void cancel_power_notifications(std::uint64_t window=0,std::uint64_t thread=0,std::uint64_t registration=0);
    std::shared_ptr<JobState> jobs_;
    bool job_handle(std::uint64_t) const;
    bool close_job(std::uint64_t);
    bool export_job_handle(std::uint64_t,InheritedHandle&) const;
    void import_job_handle(const InheritedHandle&);
    std::size_t job_handle_count() const;
    void append_job_handles(std::vector<std::uint64_t>&) const;
    std::uint64_t current_job_identity() const;
    int inherited_job_descriptor(std::uint32_t,std::uint32_t&) const;
    int startup_job_descriptor(std::span<const std::uint64_t>,int,std::uint32_t&) const;
    std::uint32_t job_security_location(std::uint64_t,std::uint32_t,int&,int&) const;
    std::uint32_t mapping_security_location(std::uint64_t,std::uint32_t,std::vector<std::uint8_t>*&,int&,int&);
    std::optional<std::uint32_t> process_thread_control(GuestMemory&,std::uint64_t,std::uint32_t,std::uint32_t&);
    std::uint32_t local_process_thread_control(std::uint32_t,std::uint32_t&);
    std::uint32_t remote_process_thread_control(GuestMemory&,std::uint32_t,std::uint32_t,std::uint32_t&);
    std::uint32_t begin_remote_process_thread_control(std::uint32_t,std::uint64_t,std::uint32_t,std::uint64_t&);
    std::optional<std::uint32_t> process_thread_target(GuestMemory&,std::uint64_t,std::uint32_t,std::uint32_t&,std::uint64_t* = nullptr);
    std::uint32_t process_memory_target(GuestMemory&,std::uint64_t,std::uint32_t,std::uint32_t&,std::uint64_t&);
    std::uint32_t query_process_debug_state(GuestMemory&,std::uint64_t,bool&);
    std::uint32_t process_basic_information(GuestMemory&,std::uint64_t,std::array<std::uint64_t,5>&,std::uint32_t&,std::uint32_t&);
    void publish_process_information();
    void publish_thread_lifetime(std::uint64_t,std::uint32_t);
    void configure_thread_record(std::uint64_t);
    void discard_thread_record(std::uint64_t);
    std::uint32_t memory_status_fields(GuestMemory&,std::array<std::uint64_t,6>&) const;
    std::uint32_t refresh_memory_resource_notifications(bool force=false);
    std::uint64_t memory_resource_poll_deadline_{UINT64_MAX};
    // GlobalMemoryStatusEx 的虛擬位址使用量要走訪全部映射與保留區；UE4 等每幀查詢時以短期快取避免每次排序。
    struct MemoryStatusCache {std::chrono::steady_clock::time_point at{},physical_at{};std::size_t regions{},modules{},resources{},capacity{};std::uint64_t used{};std::array<std::uint64_t,4> physical{};std::uint32_t physical_error{};bool valid{},physical_valid{};};
    mutable MemoryStatusCache memory_status_cache_;
    // KUSER_SHARED_DATA（0x7ffe0000）的靜態欄位與 TickCount；TickCount 只在 15.625 ms 刻度改變時改寫。
    void map_shared_user_data(GuestMemory&);
    std::uint32_t query_system_information(GuestMemory&,std::uint32_t,std::uint64_t,std::uint32_t,std::uint64_t);
    std::uint64_t shared_user_tick_{UINT64_MAX};
    std::uint32_t process_parent_id() const;
    std::uint32_t capture_process_snapshot(GuestMemory&,std::vector<ProcessSnapshot::Process>&);
    std::uint32_t capture_thread_snapshot(GuestMemory&,std::vector<ProcessSnapshot::Thread>&);
    std::vector<ProcessSnapshot::Thread> local_thread_snapshot();
    struct KernelModuleRecord {std::uint64_t base{},size{};std::uint32_t references{};std::u16string path;};
    std::vector<KernelModuleRecord> local_kernel_modules() const;
    std::uint32_t capture_kernel_modules(GuestMemory&,std::vector<KernelModuleRecord>&);
    std::uint32_t local_process_information(std::uint32_t,std::array<std::uint64_t,5>&,std::uint32_t&,int);
    std::uint32_t collect_pss_process_data(GuestMemory&,std::vector<std::uint8_t>&);
    std::uint32_t local_process_api_information(GuestMemory&,std::uint32_t,bool,std::array<std::uint64_t,5>&);
    std::uint32_t remote_process_request(GuestMemory&,std::uint32_t,std::uint64_t,std::uint32_t,std::uint32_t,std::array<std::uint64_t,5>&,std::uint32_t&,int=-1);
    struct RemoteFamilyResult {
        bool ready{};std::uint32_t error{};std::array<std::uint64_t,5> values{};std::uint32_t mode{};
    };
    std::uint32_t begin_remote_family_request(std::uint32_t,std::uint64_t,std::uint32_t,std::uint32_t,
        const std::array<std::uint64_t,5>&,std::uint32_t,int,int,std::uint64_t&,std::uint32_t=5000);
    RemoteFamilyResult poll_remote_family_request(GuestMemory&,std::uint32_t,std::uint64_t);
    void cancel_remote_family_request(std::uint32_t,std::uint64_t);
    std::uint32_t queue_remote_family_cancel(std::uint32_t,std::uint64_t,std::uint32_t,std::uint64_t);
    std::uint32_t complete_remote_family_request(std::uint32_t,std::uint64_t,std::uint64_t,std::uint32_t,
        const std::array<std::uint64_t,5>&,std::uint32_t);
    bool remote_family_waiting()const;
    std::uint32_t local_memory_operation(GuestMemory&,std::uint32_t,std::array<std::uint64_t,5>&,int,std::uint64_t=0,std::uint64_t=0,const VirtualAddressRequirements* =nullptr);
    std::uint32_t process_memory_copy(GuestMemory&,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,bool,std::uint64_t&,bool native=false);
    std::uint64_t working_set_page_flags(GuestMemory&,std::uint64_t);
    std::uint32_t process_working_set_ex(GuestMemory&,std::uint64_t,std::uint64_t,std::uint64_t);
#ifdef X64ARM_HOST_SERVICES_MEMORY_WATCH
    static std::uint32_t working_set_watch_status(std::uint32_t);
    std::uint32_t local_working_set_watch(GuestMemory&,bool,std::size_t,std::vector<std::uint8_t>&,std::size_t&);
    std::uint32_t process_working_set_watch(GuestMemory&,std::uint64_t,bool,bool,std::uint64_t,std::uint32_t,std::uint32_t&,bool=false);
#endif
    std::optional<std::uint32_t> invoke_process_memory(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_nt_process(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_nt_native(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_nt_native(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    struct NativeOperation {
        std::string symbol;
        std::array<std::uint64_t,11> arguments{};
        std::uint64_t scratch{},information{},allocation{};
        std::uint32_t saved_error{},attributes{};
        bool operator==(const NativeOperation&) const = default;
    };
    std::map<std::uint64_t,NativeOperation> native_operations_;
    std::optional<std::array<std::uint32_t,10>> concrt_default_policy_;
    std::array<std::uint64_t,2> gdi_palette_hooks_{}; // gdi32 的 pfnRealizePalette／pfnSelectPalette 資料格（user32 可改寫）。
    // keyed event：handle → 物件身分（名稱或 handle）；身分＋鍵 → 等待中的等待者與釋放者（各自的私有事件與是否已配對）。
    struct KeyedEventParty {std::uint64_t event{};bool paired{};};
    struct KeyedEventQueues {std::vector<KeyedEventParty> waiters,releasers;};
    std::map<std::uint64_t,std::u16string> keyed_event_handles_;
    std::map<std::u16string,std::map<std::uint64_t,KeyedEventQueues>> keyed_events_; // Scheduler::SetDefaultSchedulerPolicy 設定的預設政策值。
    std::uint64_t next_native_operation_{1};
    std::uint32_t native_section_access(std::uint64_t,std::uint32_t);
    std::optional<std::uint32_t> invoke_nt_section(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> finish_system_call(GuestState&,GuestMemory&);
    void enter_process_instrumentation(GuestState&);
    void restore_user_context(GuestState&,GuestMemory&,const GuestState&);
    std::uint64_t native_dispatcher_entry(std::string_view) const;
    bool enter_user_apc(GuestState&,GuestMemory&,GuestCallback,std::uint64_t,bool);
    std::optional<std::uint32_t> finish_user_apc_entry(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> process_thread_context(GuestMemory&,std::uint64_t,std::uint64_t,bool);
    std::uint32_t thread_context_bytes(std::uint64_t,std::vector<std::uint8_t>&,bool,bool=false);
    std::uint32_t remote_thread_context(GuestMemory&,std::uint32_t,std::vector<std::uint8_t>&,bool);
    void poll_job_termination(GuestMemory&);
    std::uint32_t job_ui_restrictions() const;
    std::shared_ptr<SharedJob> job_atom_table() const;
    bool allowed_gui_window(const SharedWindowInfo&) const;
    std::uint32_t remote_process_job(GuestMemory&,std::uint32_t,int,std::uint64_t&);
    std::optional<std::uint32_t> invoke_job(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<ProcessState> processes_;
    std::shared_ptr<DebugMemoryState> debug_memory_;
    void configure_debug_memory();
    void poll_debug_memory(GuestMemory&,unsigned milliseconds=0,bool force=true);
    bool dispatch_debug_request(GuestMemory&,const debugger::TransportEvent&,debugger::Frame&);
    bool debugger_attached() const noexcept;
    bool debugger_stopped() const noexcept;
    void wait_debugger(GuestMemory&);
    std::uint32_t emit_debug_event(GuestState&,GuestMemory&,debugger::Payload);
    void queue_debug_thread(std::uint64_t);
    void emit_debug_module(GuestState&,GuestMemory&,std::uint64_t,bool);
    void prepare_debug_execution(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> invoke_debugger(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t debugger_request(GuestMemory&,std::uint32_t,std::uint32_t,std::uint64_t,std::uint64_t,std::vector<std::uint8_t>&);
    bool finish_debug_wait(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> debug_thread_control(GuestMemory&,std::uint32_t,std::uint64_t,std::uint32_t,std::uint32_t,ExecutionMode,std::uint32_t,std::uint32_t&,std::vector<std::uint8_t>&);
    std::uint64_t make_debug_handle(std::uint32_t,std::uint64_t,std::uint32_t);
    void finish_debug_handles(std::uint32_t,std::uint64_t,std::uint32_t,std::uint32_t);
    std::uint32_t transfer_debug_memory(GuestMemory&,std::uint32_t,std::uint64_t,std::uint32_t,std::uint64_t,std::uint64_t,std::vector<std::uint8_t>&);
    std::uint32_t process_remote_target(std::uint64_t,std::uint32_t,std::uint32_t&,std::uint64_t&);
    std::shared_ptr<SharedWindowState> shared_windows_;
    void initialize_shared_windows();
    std::uint64_t allocate_gui_window();
    SharedWindowInfo describe_gui_window(std::uint64_t);
    std::uint64_t query_shell_window();
    bool register_shell_window(std::uint64_t,std::uint64_t);
    void publish_gui_window(std::uint64_t);
    void unpublish_gui_window(std::uint64_t);
    std::optional<SharedWindowInfo> shared_gui_window(std::uint64_t,bool enforce_job=true);
    bool invoke_remote_gui(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool invoke_remote_gui_message(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t receive_remote_gui_message(std::uint32_t,std::uint64_t,std::uint64_t,std::uint32_t,std::uint32_t,const std::array<std::uint64_t,5>&,std::uint32_t);
    void cancel_remote_gui_message(std::uint32_t,std::uint64_t,std::uint64_t);
    void poll_remote_gui_messages(GuestMemory&);
    void reply_remote_gui_message(std::uint64_t);
    bool remote_gui_messages_waiting()const;
    void publish_remote_frame(std::uint64_t,NativeGraphics&,const GraphicsView&);
    void publish_remote_pixels(std::uint64_t,std::uint32_t,std::uint32_t,std::span<const std::uint8_t>);
    void publish_shared_frame(std::uint64_t,std::uint32_t,std::uint32_t,std::uint32_t,const std::function<void(void*,std::size_t,std::uint32_t,const std::shared_ptr<void>&)>&);
    void poll_remote_frames();
    void hold_remote_reveal(std::uint64_t,bool);
    bool blit_raster(GuestMemory&,std::uint64_t,RasterSurface*,const GuiRect&,const GuiRect&,const GuiDc&,std::uint32_t,const RasterBlit&,RasterSurface* =nullptr);
    std::shared_ptr<RasterSurface> remote_raster_snapshot(GuestMemory&,std::uint64_t);
    std::uint32_t receive_remote_raster(GuestMemory&,std::uint32_t,std::uint32_t,std::array<std::uint64_t,5>&,int);
    std::uint32_t local_process_handle_count() const;
    std::vector<std::uint64_t> local_process_handles() const;
    std::size_t mapping_handle_count() const;
    void append_mapping_handles(std::vector<std::uint64_t>&) const;
    std::u16string mapping_object_name(std::uint64_t) const;
    std::u16string job_object_name(std::uint64_t) const;
    std::uint32_t query_process_handle_count(GuestMemory&,std::uint32_t,std::uint32_t&);
    std::uint32_t query_process_counters(GuestMemory&,std::uint32_t,std::uint32_t&,std::uint32_t&);
    std::uint32_t process_priority_operation(GuestMemory&,std::uint32_t,std::uint32_t,std::uint32_t&);
    std::uint32_t local_process_priority_operation(std::uint32_t,std::uint32_t&);
    std::uint32_t session_process_counters(GuestMemory&,std::uint32_t&,std::uint32_t&,std::uint32_t&);
    std::uint32_t session_process_ids(std::vector<std::uint32_t>&) const;
    std::uint32_t active_thread_count() const;
    std::optional<std::uint32_t> invoke_process(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_process_policy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_application_policy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool dynamic_code_blocked() const;
    bool executable_memory_range(const GuestMemory&,std::uint64_t,std::uint64_t) const;
    bool protection_allocation_range(std::uint64_t,std::uint64_t) const;
    void register_thread_stack_memory(GuestMemory&,std::uint64_t,std::uint64_t);
    void release_thread_stack_memory(GuestMemory&,std::uint64_t,std::uint64_t);
    using FileOpenArguments = std::array<std::uint64_t,7>;
    std::optional<std::uint32_t> invoke_file_open(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_file_close(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint64_t> open_native_file(GuestState&,GuestMemory&,const std::u16string&,const FileOpenArguments&);
    std::optional<std::uint32_t> invoke_overlapped_result(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_file_readwrite(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_file_lock(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_file_io(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){return invoke_file_io(state,memory,service,nullptr);}
    std::optional<std::uint32_t> invoke_file_io(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,const FileOpenArguments*);
    std::optional<std::uint32_t> invoke_serial(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){return invoke_serial(state,memory,service,nullptr);}
    std::optional<std::uint32_t> invoke_serial(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,const FileOpenArguments*);
    static std::optional<unsigned> serial_port(std::u16string_view);
    void scan_serial_ports();
    std::map<unsigned,std::string> serial_ports_;
    std::set<std::string> native_serial_paths_;
    bool serial_ports_scanned_{};
    void poll_file_io(GuestMemory&,bool scheduler_poll=false);
    std::optional<bool> file_io_signaled(std::uint64_t);
    bool resident_file_wait_pending(std::uint64_t) const noexcept;
    void set_file_io_signaled(std::uint64_t,bool);
    std::uint32_t bind_file_completion_port(std::uint64_t,std::uint64_t,std::uint64_t,bool = false);
    std::uint32_t set_file_completion_modes(std::uint64_t,std::uint32_t);
    void poll_directory_notifications();
    std::optional<std::uint32_t> invoke_shell_notification(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_shell_notifications(GuestMemory&);
    void shell_notify(GuestMemory&,std::uint32_t,const std::array<std::u16string,2>&,bool,std::vector<GuestCallback>* = nullptr,std::uint32_t = 0,bool = false);
    void release_shell_delivery(GuestMemory&,std::uint64_t);
    void consume_shell_message(const GuiMessage&);
    void retire_shell_messages(GuestMemory&);
    void retain_shell_callback(CallbackFrame&,const GuestCallback&);
    void finish_shell_callbacks(GuestMemory&,const CallbackFrame&);
    struct ShellWatch {
        std::u16string root;std::shared_ptr<DirectoryWatch> native;
        std::map<std::u16string,FileMetadata> known;
    };
    std::map<std::uint32_t,std::vector<ShellWatch>> shell_watches_;
    std::optional<std::uint32_t> invoke_directory_notification(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::map<std::uint64_t,std::shared_ptr<DirectoryWatch>> directory_reads_,change_notifications_;
    std::uint64_t effective_dpi_context() const;
    bool socket_select_ready(const ThreadWait&) const;
    std::optional<std::uint32_t> invoke_socket_poll(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_socket_namespace(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // dnsapi 的記錄比較／複製、訊息編解碼、解析器快取、設定查詢與 DnsQueryEx（windows_dns_records.cpp）。
    std::optional<std::uint32_t> invoke_dns_records(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // 將 RR 依 DNS_RECORD 版面逐筆配置並串接，登錄於 network.addresses 供 DnsRecordListFree 釋放；失敗時全部回收後丟出。
    std::uint64_t dns_store_records(GuestState&,GuestMemory&,const std::vector<dns_detail::Record>&,unsigned charset,const std::vector<unsigned>* charsets=nullptr);
    std::optional<std::uint32_t> invoke_socket_database(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_socket_lookups(GuestMemory&);
    void cancel_socket_lookups();
    void poll_network_io(GuestMemory&);
    void poll_socket_io(GuestMemory&);
    std::optional<std::uint32_t> invoke_socket_io(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool cancel_network_io(GuestMemory&,std::uint64_t handle=0,std::uint64_t thread=0,std::uint64_t overlapped=0);
    std::optional<std::uint32_t> invoke_network_extension(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void cancel_file_io(GuestMemory&,std::uint64_t,std::uint64_t = 0,std::uint64_t = 0);
    // nullopt 表示仍需原有外部輪詢；有限值是通知式管道的存活複查期限，UINT64_MAX 表示沒有等待。
    std::optional<std::uint64_t> file_io_poll_deadline(const SharedWake*) const;
    // 原生檔案的 FlushFileBuffers 交給宿主工作執行緒執行 fsync；完成旗標由排程器輪詢。
    struct AsyncFileFlush { std::atomic<bool> done{}; std::uint32_t error{}; };
    struct AsyncFileCopy;
    struct PendingFileIo {
        enum class Kind { Connect, Read, Write, Transact, Flush, Lock, Directory, CommEvent, Copy } kind{};
        std::shared_ptr<AsyncFileCopy> copy;
        std::uint64_t source_handle{},source_offset{};
        std::shared_ptr<SerialTransfer> serial;
        std::shared_ptr<AsyncFileFlush> flush;
        std::shared_ptr<NamedPipe::Transaction> transaction;
        NamedPipe::WriteCompletion pipe_write;
        std::uint64_t handle{},thread{},overlapped{},event{},input{},output{},count_pointer{},routine{};
        std::uint64_t native_iosb{},native_context{},native_scratch{};
        std::uint32_t input_size{},output_size{},lock_key{};
        std::uint64_t offset{},length{},input_transferred{},transferred{};
        bool attempted{},io_recorded{};
        std::uint64_t io_accounted{};
        PipeResult result;
        bool done{},written{},notify{},blocking{},exclusive{},immediate{};
        bool hid{};
        std::uint64_t pipe_wake_epoch{},pipe_poll_deadline{};
    };
    // 共用即時／待命 I/O 的結果發布、事件、APC 及完成埠通知。
    static std::uint32_t file_io_status(std::uint32_t);
    void complete_file_io(GuestMemory&,PendingFileIo&,std::uint32_t);
    std::optional<std::uint32_t> invoke_file_copy_chunk(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_fs_control(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_set_volume(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    FileVolumeLocation file_volume_location(std::uint64_t) const;
    PipeResult poll_hid_io(PendingFileIo&,GuestMemory&);
    std::vector<PendingFileIo> pending_file_io_;
    PipeResult read_directory_notification(const PendingFileIo&,GuestMemory&);
    bool close_process(std::uint64_t);
    bool create_guest_process(GuestMemory&,const std::array<std::uint64_t,10>&,bool,bool = false,std::uint64_t = 0,unsigned = 0,unsigned = 0,const NativeProcessParameters* = nullptr,const ProcessImageSource* = nullptr);
    bool finish_guest_process_creation(GuestState&,GuestMemory&,std::uint64_t);
    void cancel_guest_process_creation(std::uint64_t);
    std::shared_ptr<RemoteThreadState> remote_threads_;
    std::uint32_t begin_remote_thread_creation(GuestMemory&,std::uint64_t,std::uint32_t,std::uint64_t,std::uint64_t,std::uint64_t,const GuestThreadBootstrap&,RemoteThreadOutputs);
    bool finish_remote_thread_creation(GuestState&,GuestMemory&,std::uint64_t);
    void cancel_remote_thread_creation(std::uint64_t);
    std::uint32_t receive_remote_thread_creation(GuestMemory&,std::uint32_t,std::uint64_t,std::uint64_t,std::uint32_t,std::uint32_t,const std::array<std::uint64_t,5>&,std::uint32_t,int&);
    void poll_remote_thread_creations(GuestMemory&);
    bool remote_thread_creations_waiting()const;
    std::uint32_t remote_thread_process_identity(std::uint64_t,std::uint32_t,std::uint32_t&,std::uint64_t&);
    std::uint64_t import_created_thread(std::uint32_t,std::uint64_t,std::uint32_t,std::uint32_t,std::uint32_t,bool);
    // location：找到值的類別機碼（相對 HKEY_CLASSES_ROOT）；AssocQueryKey 與 IQueryAssociations::GetKey 用它開啟機碼。
    std::uint32_t query_association(std::uint32_t,std::uint32_t,std::u16string,const std::u16string&,std::u16string&,std::u16string* location=nullptr);
    std::optional<std::uint32_t> invoke_association(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct QueryAssociationsObject {std::uint32_t references{1},flags{};std::u16string source;};
    std::map<std::uint64_t,QueryAssociationsObject> query_associations_;std::uint64_t query_associations_vtable_{};
    std::optional<std::uint32_t> invoke_shell_execute(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_shell_item(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_shell_item(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_shell_item(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t create_shell_item(GuestMemory&,std::u16string);
    std::uint64_t create_shell_enumerator(GuestMemory&,std::vector<std::u16string>,std::size_t = 0);
    std::uint32_t open_shell_file_stream(GuestMemory&,std::u16string,std::uint32_t,std::uint32_t,bool,std::uint64_t&);
    // 同一套檔案開啟流程建立 IStream 或檔案 ILockBytes（StgCreateDocfile／StgOpenStorage 的後端）。
    std::uint32_t open_file_object(GuestMemory&,std::u16string,std::uint32_t,std::uint32_t,bool,bool lock_bytes,std::uint64_t&);
    bool current_process_handle(std::uint64_t,std::uint32_t = 0) const;
    std::u16string_view process_object_type(std::uint64_t) const;
    std::u16string_view object_type(std::uint64_t) const;
    struct ObjectMetadata {std::uint32_t access{},handles{},pointers{};std::uint64_t identity{};std::string system_key;};
    std::optional<ObjectMetadata> object_metadata(std::uint64_t) const;
    std::optional<ObjectMetadata> mapping_object_metadata(std::uint64_t) const;
    std::optional<ObjectMetadata> job_object_metadata(std::uint64_t) const;
    std::optional<ObjectMetadata> process_object_metadata(std::uint64_t) const;
    std::optional<ObjectMetadata> kernel_device_metadata(std::uint64_t) const;
    void append_kernel_device_handles(std::vector<std::uint64_t>&) const;
    void append_transaction_handles(std::vector<std::uint64_t>&) const;
    std::string system_object_key(std::uint64_t) const;
    struct SystemHandleRecord {std::uint64_t handle{},object{};std::uint32_t pid{},access{};std::uint16_t type{},flags{};};
    std::uint32_t local_system_handles(std::vector<SystemHandleRecord>&);
    std::uint32_t capture_system_handles(GuestMemory&,std::vector<SystemHandleRecord>&);
    std::optional<bool> process_signaled(std::uint64_t, bool input_idle = false);
    bool processes_waiting();
    const RuntimeImage* module_image(std::uint64_t address, bool containing = false) const;
    const RuntimeImage* unwind_image(GuestMemory&,std::uint64_t pc,std::uint64_t function_entry = 0);
    const RuntimeFunctionTable* function_table_callback(std::uint64_t pc) const;
    std::optional<std::uint32_t> finish_function_lookup(GuestState&,GuestMemory&,Continuation,std::uint64_t);
    std::optional<std::uint32_t> invoke_function_table(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_eh4(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_eh4(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_crt_error(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t find_module(std::u16string_view name) const;
    GuestAddress resolve_service(std::string_view,std::string_view,bool required) const;
    // 描述元本身的宿主服務入口（不經匯出改導向的客體快速常式）；沒有對應描述元時回 0。
    GuestAddress host_service_stub(std::string_view family,std::string_view symbol) const;
public:
    // 宿主服務暫借的客體 heap 區塊：配置時清零，離開範圍時全部釋放（含例外與協程結束路徑）。
    // 各服務共用，不再各自撰寫配置／釋放樣板。allocate／text 失敗時回 0，由呼叫端回報記憶體不足。
    class GuestScratch {
    public:
        GuestScratch(WindowsServices& services,GuestMemory& memory,const GuestState& state):services_(services),memory_(memory),state_(state){}
        GuestScratch(const GuestScratch&)=delete;GuestScratch& operator=(const GuestScratch&)=delete;
        ~GuestScratch();
        std::uint64_t allocate(std::size_t size);
        std::uint64_t text(std::u16string_view value); // NUL 結尾的 UTF-16 複本
    private:
        WindowsServices& services_;GuestMemory& memory_;const GuestState& state_;std::vector<std::uint64_t> blocks_;
    };
private:
    std::map<std::string,std::uint64_t> builtin_export_targets(std::string_view) const;
    GuestAddress builtin_constant_address(std::string_view,std::string_view) const;
    // NT 系統呼叫表：入口位址→編號（依位址排序的名次）、編號→入口與描述元。ntdll 入口在程序內固定，第一次建立後快取。
    struct SystemCallTable {
        std::map<std::uint64_t,std::uint32_t> numbers;
        std::vector<std::uint64_t> targets;
        std::vector<const WindowsServiceDescriptor*> descriptors;
    };
    const SystemCallTable& system_call_table() const;
    const std::map<std::uint64_t,std::uint32_t>& builtin_system_calls() const {return system_call_table().numbers;}
    // 複製的服務物件（例如 PeProgram 的副本）不沿用來源的表，第一次使用時重新建立。
    struct SystemCallCache {
        std::atomic<const SystemCallTable*> table{};std::unique_ptr<const SystemCallTable> owner;
        SystemCallCache()=default;
        SystemCallCache(const SystemCallCache&) noexcept {}
        SystemCallCache& operator=(const SystemCallCache&) noexcept {table.store(nullptr);owner.reset();return *this;}
    };
    mutable SystemCallCache system_call_cache_;
    // NtAllocateUuids 的時間版 UUID 時鐘：同一程序內單調遞增，時鐘倒退時遞增序號；節點為本程序的隨機節點。
    struct UuidClock {std::uint64_t last{};std::uint32_t sequence{};std::array<std::uint8_t,6> node{};bool node_ready{};} uuid_clock_;
    // RtlRandom／RtlRandomEx 的洗牌表（程序內共用）。
    struct RtlRandomState {std::array<std::uint32_t,128> table{};std::uint32_t last{};bool ready{};} rtl_random_state_;
    // kernel32 本機 Atom 表（AddAtom／FindAtom 等）；全域 Atom 表在視窗站命名空間。
    struct LocalAtomTable {std::map<std::uint16_t,GuiAtom> atoms;std::map<std::u16string,std::uint16_t> names;} local_atoms_;
    // 32 位元程序的 WOW64 檔案系統重新導向停用狀態（逐執行緒）。
    std::map<std::uint64_t,bool> wow64_fs_redirection_;
    // 同步屏障：以客體 SYNCHRONIZATION_BARRIER 位址為鍵；每輪一個手動重設事件。
    struct SynchronizationBarrier {std::uint32_t total{},remaining{};std::uint64_t generation{},event{};std::vector<std::uint64_t> retired;};
    std::map<std::uint64_t,SynchronizationBarrier> synchronization_barriers_;
    // user32 舊式掛鉤（執行緒、掛鉤類型、程序位址 → HHOOK）與逐執行緒的 DPI 主控行為。
    std::map<std::tuple<std::uint64_t,std::uint32_t,std::uint64_t>,std::uint64_t> legacy_hooks_;
    // shlwapi 的 HUSKEY：同一相對路徑在 HKCU 與 HKLM 各自開啟的機碼（未開啟者為 0）。
    struct ShellUsKey {std::uint64_t user{},machine{};};std::map<std::uint64_t,ShellUsKey> shell_us_keys_;
    // RtlCreateAtomTable 建立的 atom 表格（與本機 atom 相同的表格型別）與釘選的 atom。
    struct RtlAtomTable {LocalAtomTable atoms;std::set<std::uint16_t> pinned;};std::map<std::uint64_t,RtlAtomTable> rtl_atom_tables_;
    // SHSetThreadRef 的每執行緒參照（不 AddRef）、SHCreateThreadRef 建立的物件與其 vtable，以及 SHSetInstanceExplorer 的程序參照。
    std::map<std::uint64_t,std::uint64_t> shell_thread_refs_;std::set<std::uint64_t> shell_thread_ref_objects_;std::uint64_t shell_thread_ref_vtable_{},shell_instance_explorer_{};
    std::uint64_t shell_inverse_cmap_{}; // SHGetInverseCMAP 的 32768 位元組反查表（常駐客體記憶體）
    // SHOpenRegStream：讓既有記憶體 IStream 在最後一次 Release 時把內容寫回登錄值（接手 key 的所有權）。
    bool bind_registry_stream(std::uint64_t stream,std::uint64_t key,std::u16string value);
    std::map<std::uint64_t,std::uint32_t> dpi_hosting_behavior_;
    std::uint64_t load_module(GuestMemory&, std::u16string name, std::uint32_t flags, std::vector<std::uint64_t>& created, std::u16string_view search_directory = {}, GuestState* = nullptr);
    std::uint64_t module_export(GuestMemory&, std::uint64_t module, std::string_view symbol, std::optional<std::uint16_t> ordinal, std::vector<std::uint64_t>& created, unsigned depth = 0, GuestState* = nullptr);
    FileSnapshot read_module_file(GuestState*,GuestMemory&,const std::u16string&);
    std::uint32_t call_guest_native_file(GuestState&,GuestMemory&,std::uint64_t,std::vector<std::uint64_t>);
    std::uint64_t call_guest_nested(GuestState&,GuestMemory&,std::uint64_t,std::vector<std::uint64_t>);
    void notify_dll(GuestState&,GuestMemory&,const RuntimeImage&,std::uint32_t reason);
    std::pair<std::uint64_t,bool> native_file_entry(GuestMemory&,std::string_view);
    std::optional<std::uint64_t> invoke_native_file_io(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint64_t> invoke_native_file_position(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint64_t> invoke_native_file_information(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint64_t> invoke_native_file_completion(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint64_t> invoke_native_mapping(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    FileInformation query_native_file_attributes(GuestState&,GuestMemory&,const std::u16string&,bool);
    void unmap_module(GuestMemory&,std::uint64_t module,GuestState* state=nullptr);
    void append_module_detach(ModuleOperation&,std::uint64_t module,bool unmap,std::uint64_t reserved = 0);
    std::optional<std::uint32_t> start_module_operation(GuestState&,GuestMemory&,ModuleOperation);
    std::optional<std::uint32_t> advance_module_operation(GuestState&,GuestMemory&,std::uint64_t id);
    std::optional<std::uint32_t> resume_module_operation(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t value);
    std::optional<std::uint32_t> fail_module_initialization(GuestState&,GuestMemory&,std::uint64_t operation);
    std::optional<std::uint32_t> resume_loader_notification_exception(GuestState&,GuestMemory&,std::size_t callback_depth);
    void start_loader_callbacks(GuestState&,GuestMemory&,std::vector<GuestCallback>,Continuation,std::uint64_t value = 0);
    void release_loader_notification();
    std::optional<std::uint32_t> invoke_module(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void initialize_module_tls(GuestMemory&, RuntimeModule&);
    void initialize_thread_module_tls(GuestMemory&,std::uint64_t vector);
    std::vector<GuestCallback> module_thread_callbacks(std::uint32_t reason) const;
    bool await_loader(GuestState&,GuestMemory&);
    std::shared_ptr<AccessibleState> accessible_;
    std::optional<std::uint32_t> invoke_eventlog(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::map<std::uint64_t,PendingAutoProxy> autoproxy_;
    struct ProxyResolver {
        std::shared_ptr<NativeAutoProxy> task;
        AutoProxyResult result;
        std::uint64_t callback_wait{},context{};
        bool completed{};
    };
    struct HttpCallbackOperation {
        std::uint64_t handle{};
        std::vector<std::uint64_t> dependencies;
        bool ready{};
    };
    std::map<std::uint64_t,ProxyResolver> proxy_resolvers_;
    std::map<std::uint64_t,HttpCallbackOperation> http_callbacks_;
    std::map<std::uint64_t,std::uint64_t> proxy_result_allocations_;
    std::uint64_t queue_http_callback(const GuestState&,GuestMemory&,std::uint64_t,std::uint32_t,std::span<const std::uint8_t>,std::uint64_t,bool=true);
    void poll_autoproxy(GuestMemory&);
    void close_http_callbacks(GuestState&,GuestMemory&,std::uint64_t);
    void cancel_http_connection_probe(std::uint64_t thread);
    std::optional<std::uint32_t> invoke_autoproxy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool http_ready(std::uint64_t) const;
    bool http_waiting() const;
    std::optional<std::uint32_t> invoke_http(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_url_cache(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    UrlCacheState& url_cache_state();
    void release_url_cache() noexcept;
    void cache_http_response(HttpHandle&);
    // WinINet Cookie（windows_http_cookies.cpp）：請求自動帶入、保存 Set-Cookie，以及 Cookie API 共用的查詢與儲存。
    void add_http_cookies(HttpInput&);
    void remember_http_cookies(HttpHandle&);
    std::vector<HttpCookie> http_cookies(std::string_view url,bool http_only,bool third_party);
    std::uint32_t store_http_cookie(std::string_view url,std::string_view text,bool http_only,bool third_party);
    std::shared_ptr<NativeHttp> read_http_cache(HttpHandle&);
    std::optional<std::uint32_t> invoke_fiber(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void destroy_fiber(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_dib_transfer(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_gdiplus(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<GdiplusState> gdiplus_;
    std::shared_ptr<RibbonState> ribbon_;
    std::shared_ptr<TaskDialogState> task_dialogs_;
    std::shared_ptr<OpenclState> opencl_;
    std::shared_ptr<VulkanState> vulkan_;
    std::shared_ptr<DirectMLState> directml_;
    std::shared_ptr<XessState> xess_;
    std::shared_ptr<FidelityFxState> fidelityfx_;
    std::shared_ptr<PmrState> msvcp_pmr_;
    std::shared_ptr<WebView2State> webview2_;
    std::shared_ptr<WebBrowserState> webbrowser_;
    std::shared_ptr<D3D12State> d3d12_;
    // D3D12 提交執行緒（見 windows_d3d12.cpp 的 D3D12Submitter）：第一個裝置建立時啟動，在主體成員之後宣告以最先結束。
    void d3d12_start_submitter(GuestMemory&);
    void d3d12_submit_loop(GuestMemory&,struct D3D12Submitter&);
    void d3d12_drain();
    std::shared_ptr<DXCoreState> dxcore_;
    std::optional<std::uint32_t> invoke_dxcore(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_d3d12(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // worker 上的 COM 引用計數：AddRef 與不會歸零的 Release 只改計數；歸零（銷毀物件）回傳 false 交回協調者。
    bool d3d12_worker_reference(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool d3d12_worker_descriptor_transfer(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // MSVCP 絕對時間（_timespec64／xtime）到 Win32 等待毫秒數；已過期為 0。
    std::uint32_t msvcp_wait_milliseconds(GuestMemory&,std::uint64_t) const;
    // worker 上的 Resource.Map／Unmap：不需等 GPU 也不需讀回、輸出指標不在寫入時複製頁時完成，否則回傳 false 交回協調者。
    bool d3d12_worker_map(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool d3d12_queue(std::uint64_t) const;
    // DirectML 等建立在 D3D12 上的 API：錄製於命令清單、於 ExecuteCommandLists 重播時以裝置的共用後端執行。
    bool d3d12_record(GuestMemory&,std::uint64_t list,D3D12Replay replay);
    std::uint64_t d3d12_device_of(std::uint64_t object) const;
    bool d3d12_resource_width(std::uint64_t resource,std::uint64_t& width) const;
    std::optional<std::uint32_t> invoke_webview2(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_webbrowser(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool begin_webbrowser_com(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void queue_webbrowser_event(const GuiMessage&);
    std::optional<std::uint32_t> invoke_opencl(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_vulkan(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // Vulkan 命令錄製等熱點呼叫可在 worker 上執行（見 windows_vulkan.cpp 的 vulkan_worker_symbol）：橋接已載入宿主 loader 時為真。
    bool vulkan_worker_ready() const noexcept;
    std::optional<std::uint32_t> invoke_directml(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_xess(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_fidelityfx(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_gaming_tcui(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_msvcp_pmr(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_msvcp_values(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t secure_memory_copy(GuestMemory&,std::uint64_t destination,std::uint64_t capacity,std::uint64_t source,std::uint64_t count,unsigned unit,bool move);
    std::optional<std::uint32_t> invoke_msvcp_special_math(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_task_dialog(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_task_dialog(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    bool begin_ribbon_com(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ribbon(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<SafeArrayState> safe_arrays_;
    std::shared_ptr<MsxmlState> msxml_;
    bool begin_msxml_com(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_msxml(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_safearray(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t create_safearray_bytes(GuestState&,GuestMemory&,std::span<const std::uint8_t>);
    std::shared_ptr<OleStreamState> ole_streams_;
    bool same_ole_streams(const WindowsServices&) const;
    void detach_ole_streams();
    std::optional<std::uint32_t> invoke_ole_stream(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_ole_stream_copy(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    std::optional<std::uint32_t> invoke_ole_persistence(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_ole_persistence(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    std::optional<std::uint32_t> invoke_x86_jump(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> finish_x86_longjmp(GuestState&,GuestMemory&,std::uint64_t,const GuestState&);
    std::optional<std::uint32_t> resume_x86_longjmp(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_com_bind(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_bind(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_com_bind(GuestState&,GuestMemory&,std::uint64_t);
    std::vector<GuestCallback> release_com_bind_references(GuestMemory&,std::span<const ComBindReference>);
    std::vector<GuestCallback> dispose_com_bind(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t create_com_bind_enum(GuestState&,GuestMemory&,ComBindEnum);
    rot::SharedRot& com_running_table();
    rot::Scope com_rot_scope();
    bool com_rot_allow_any();
    std::optional<std::uint32_t> invoke_com_rot(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_rot(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_com_rot(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t create_com_rot_enum(GuestState&,GuestMemory&,ComRotEnumeration);
    std::vector<GuestCallback> dispose_com_rot_enum(GuestState&,GuestMemory&,std::uint64_t);
    ComRpcState& com_rpc_bridge();
    std::function<void(int,bool,bool)> com_system_file_protector();
    void send_com_rpc(const com_rpc::Endpoint&,const com_rpc::Message&);
    std::optional<com_rpc::Message> receive_com_rpc();
    std::optional<std::uint32_t> invoke_com_rpc(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<StorageState> storage_;
    StorageState& storage_bridge();
    bool same_storage(const WindowsServices&) const;
    std::shared_ptr<MsiState> msi_;
    std::shared_ptr<EsentState> esent_;
    EsentState& esent_bridge();
    bool same_esent(const WindowsServices&) const;
    std::optional<std::uint32_t> invoke_esent(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_esent_cursor(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_esent(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_esent(GuestState&,GuestMemory&,std::uint64_t);
    MsiState& msi_bridge();
    bool same_msi(const WindowsServices&) const;
    void collect_msi(GuestMemory&);
    std::optional<std::uint32_t> invoke_msi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_msi(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_msi(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> begin_msi_registry(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_msi_registry(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_storage(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_storage_stream(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_storage_enumerator(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t create_storage_object(GuestMemory&,unsigned,std::uint64_t,std::uint32_t,std::uint32_t);
    std::vector<GuestCallback> release_storage_object(GuestState&,GuestMemory&,std::uint64_t);
    void storage_stat(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t,const compound::Node&,std::uint32_t,bool);
    std::optional<std::uint32_t> resume_storage(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    std::optional<std::uint32_t> persist_storage(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t,std::map<std::uint64_t,compound::Document>,std::uint64_t=0,std::uint64_t=0,std::uint32_t=0,std::vector<std::uint64_t> = {},std::vector<std::uint64_t> = {},std::pair<std::uint64_t,std::uint32_t> = {},std::optional<std::uint64_t> = {});
    void discard_storage(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> begin_storage_copy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_storage_copy(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_storage_copy(GuestState&,GuestMemory&,std::uint64_t);
    std::shared_ptr<MetafileState> metafile_playback_;
    void finish_metafile_enumeration(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint64_t> import_enhanced_metafile(std::vector<std::uint8_t>);
    // WMF 播放（windows_wmf.cpp）：記錄依 PlayMetaFileRecord 契約呼叫 GDI，物件存在客體 handle table；結束時還原選取物件並刪除建立的物件。
    bool play_windows_metafile_record(GuestState&,GuestMemory&,std::uint64_t dc,std::uint64_t table,unsigned handles,std::span<const std::uint8_t> record);
    bool play_windows_metafile(GuestState&,GuestMemory&,std::uint64_t dc,std::span<const std::uint8_t> bytes);
    std::array<std::uint64_t,4> windows_metafile_selection(GuestState&,GuestMemory&,std::uint64_t dc);
    void finish_windows_metafile_playback(GuestState&,GuestMemory&,std::uint64_t dc,std::uint64_t table,unsigned handles,const std::array<std::uint64_t,4>& selected);
    bool write_guest_file(GuestState&,GuestMemory&,std::u16string_view path,std::span<const std::uint8_t> bytes);
    std::optional<std::vector<std::uint8_t>> read_guest_file(GuestState&,GuestMemory&,std::u16string_view path,std::size_t minimum,std::size_t limit);
    std::shared_ptr<PrintingState> printing_;
    std::optional<std::uint32_t> invoke_printing(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_spooler(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_printer_data(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void printing_changed(unsigned);
    void poll_printer_form_notifications(bool reload);
    bool printer_form_notifications_waiting()const;
    unsigned read_printer_settings(GuestMemory&,std::uint64_t,bool,PrinterSettings&);
    std::vector<PrinterSettings> available_printer_settings(unsigned&);
    std::optional<std::uint32_t> invoke_print_dialog(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_print_dialog(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_property_page(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_property_page(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<GuiPropertyPage> read_property_page(GuestMemory&,std::uint64_t,bool,std::uint32_t&);
    std::uint32_t commit_print_dialog(GuestState&,GuestMemory&,GuiPrintDialog&);
    std::optional<std::uint32_t> invoke_page_setup(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_page_setup(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    std::optional<std::uint32_t> page_setup_message(GuestState&,GuestMemory&,std::uint64_t,const GuiMessage&);
    std::uint32_t commit_page_setup(GuestState&,GuestMemory&,const GuiPageSetup&);
    std::array<std::uint64_t,2> allocate_printer_globals(GuestState&,GuestMemory&,bool,const PrinterSettings&);
    std::uint64_t release_print_services(GuestState&,GuestMemory&,std::uint64_t);
    void select_print_page(std::uint64_t,std::uint32_t);
    void refresh_print_settings(std::uint64_t);

    std::optional<std::uint32_t> resume_printing(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::uint64_t create_printer_dc(GuestMemory&,std::uint64_t,bool,bool);
    bool is_printer_dc(std::uint64_t) const;
    bool printer_drawing_enabled(std::uint64_t) const;
    std::uint32_t begin_printer_page(GuestMemory&,std::uint64_t);
    bool prepare_legacy_printer_page(GuestMemory&,std::uint64_t);
    void delete_printer_dc(std::uint64_t);
    std::optional<std::uint32_t> printer_device_cap(std::uint64_t,unsigned) const;
    std::optional<std::uint32_t> invoke_gdi_path(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_gdi_state(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t create_accessible(GuestMemory&,std::uint64_t,std::uint32_t);
    std::optional<std::uint32_t> resume_accessible(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_accessible(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<DWriteState> dwrite_;
    std::shared_ptr<DeviceConfigurationState> device_configuration_;
    std::shared_ptr<SetupInfState> setup_inf_;
    void prepare_machine_clock();
    std::shared_ptr<MachineShutdown> machine_shutdown_;
    void prepare_machine_shutdown();
    void poll_machine_shutdown();
    std::uint64_t machine_shutdown_deadline()const;
    void redirect_process_termination(std::uint32_t);
    std::optional<std::uint32_t> invoke_shutdown(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<BluetoothState> bluetooth_;
    bool prepare_bluetooth();
    bool bluetooth_handle(std::uint64_t) const;
    bool close_bluetooth(std::uint64_t);
    bool bluetooth_path(std::u16string_view) const;
    bool bluetooth_class(const std::array<std::uint8_t,16>&) const;
    bool bluetooth_ready(std::uint64_t) const;
    bool bluetooth_waiting() const;
    void poll_bluetooth(GuestMemory&);
    void finish_bluetooth_delivery(std::uint64_t);
    void cancel_bluetooth_thread(GuestState&,GuestMemory&,std::uint64_t);
    bool wait_bluetooth_operation();
    std::optional<std::uint32_t> finish_bluetooth_operation(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> enumerate_bluetooth_devices(GuestMemory&,bool low_energy);
    std::optional<std::uint32_t> invoke_bluetooth(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> open_bluetooth(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,const std::u16string&,const FileOpenArguments&);
    std::optional<std::uint32_t> duplicate_bluetooth(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_device_configuration(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t system_metric(std::uint32_t index,std::uint32_t dpi=96);
    std::u16string keyboard_layout_name();
    std::vector<std::u16string>* current_thread_ui_languages();
    std::pair<std::uint32_t,DirectorySearch> search_directory(const std::string& path);
    bool for_each_free_heap_page(GuestMemory&,const std::function<void(std::uint64_t,std::uint64_t)>&);
    std::uint64_t crt_readlc_active_{},crt_sbh_threshold_{};std::uint32_t crt_sse2_enabled_{1};
    void refresh_device_configuration(bool hid);
    void poll_device_notifications(GuestMemory&);
    void finish_device_notification(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_device_notification(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<StackWalkState> stack_walk_;
    std::shared_ptr<MinidumpState> minidump_;
    std::optional<std::uint32_t> invoke_minidump(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_minidump(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_minidump(std::uint64_t,GuestMemory&,const GuestState&);
    std::optional<std::uint32_t> advance_minidump(GuestState&,GuestMemory&,std::uint64_t,std::optional<std::uint64_t> = {});
    VirtualMemoryInformation virtual_memory_information(const GuestMemory&,std::uint64_t) const;
    std::uint64_t find_free_virtual_range(const GuestMemory&,std::uint64_t size,std::uint64_t alignment,
        std::uint64_t lowest,std::uint64_t limit,std::uint64_t preferred,bool descending=false) const;
    std::shared_ptr<DemangleState> demangle_;
    std::shared_ptr<Msvcp100State> msvcp100_;
    void initialize_msvcp100(GuestMemory&);
    GuestAddress msvcp100_data(std::string_view,bool modern=false) const;
    void initialize_msvcp_numeric_facet(GuestState&,GuestMemory&,std::uint64_t,bool,bool,const CrtLocaleCategory&,std::uint32_t,bool=false);
    void queue_msvcp_facet_retirement(std::uint64_t);
    std::optional<std::uint32_t> invoke_msvcp100(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_msvcp100(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_msvcp100(std::uint64_t,GuestState&,GuestMemory&);
    void prepare_msvcp_exception_capture(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> finish_msvcp_callback_exception(GuestState&,GuestMemory&,std::size_t);
    std::optional<std::uint32_t> invoke_demangle(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_demangle(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_demangle(std::uint64_t);
    std::optional<std::uint32_t> invoke_stack_walk(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_stack_walk(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_stack_walk(std::uint64_t,GuestMemory&,const GuestState&);
    FileInformation publish_font_file(std::span<const std::uint8_t>);
    std::uint32_t publish_module_file(std::u16string_view,std::span<const std::uint8_t>);
    std::optional<std::uint32_t> invoke_dwrite(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_dwrite(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_dwrite(std::uint64_t,GuestMemory&,const GuestState&);
    std::shared_ptr<DirectMusicState> dmusic_;
    std::shared_ptr<AcmState> acm_;
    std::optional<std::uint32_t> invoke_acm(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::map<std::uint64_t,std::shared_ptr<WinmmWaveOutput>> wave_outputs_;
    std::shared_ptr<MciState> mci_;
    MciState& mci_state();
    std::optional<std::uint32_t> invoke_mci(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_mci_string(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_mci_wait(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_mci_wait(std::uint64_t);
    void notify_mci(std::uint32_t,std::uint64_t,std::uint32_t);
    void complete_mci_transport(std::uint32_t,std::uint32_t);
    void poll_mci();
    bool mci_waiting()const;
    std::map<std::uint64_t,std::shared_ptr<WinmmBeepTone>> beep_tones_;
    std::uint64_t play_sound_handle_{};
    struct PolicySection {std::shared_ptr<SharedPolicy> policy;std::uint64_t thread{},generation{},wake_epoch{};};
    std::map<std::uint64_t,PolicySection> policy_sections_;
    void discard_policy_thread(std::uint64_t);
    void discard_beep(std::uint64_t);
    std::optional<std::uint32_t> finish_beep(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void stop_play_sound();
    std::optional<std::uint32_t> invoke_play_sound(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::shared_ptr<audio::MixerBackend> audio_mixer_backend_;
    std::shared_ptr<WinmmMixerState> winmm_mixer_;
    std::optional<std::uint32_t> invoke_winmm_mixer(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_winmm_mixer();
    std::shared_ptr<audio::InputBackend> audio_input_backend_;
    std::map<std::uint64_t,std::shared_ptr<WinmmWaveInput>> wave_inputs_;
    std::deque<GuestCallback> wave_callbacks_;
    // shcore 縮放變更登錄（RegisterScaleChangeEvent／RegisterScaleChangeNotifications）。
    struct ScaleChangeWindow {std::uint32_t device{};std::uint64_t window{};std::uint32_t message{};};
    std::map<std::uint64_t,std::uint64_t> scale_change_events_;std::map<std::uint32_t,ScaleChangeWindow> scale_change_windows_;std::uint64_t next_scale_change_cookie_{};
    // winmm 可安裝驅動程式執行個體（HDRVR → 模組、DriverProc、DRV_OPEN 回傳的 dwDriverId）。
    struct InstallableDriver {std::uint64_t module{},procedure{},id{};};
    std::map<std::uint64_t,InstallableDriver> installable_drivers_;
    std::uint64_t next_installable_driver_{0x5ad00000};
    std::uint64_t wave_callback_registration_{};
    void poll_wave_output(GuestMemory&);
    bool wave_output_waiting() const;
    std::optional<std::uint32_t> invoke_wave_output(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_wave_input(GuestMemory&);
    bool wave_input_waiting() const;
    std::optional<std::uint32_t> invoke_wave_input(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::map<std::uint64_t,std::shared_ptr<WinmmMidiOutput>> midi_outputs_;
    std::uint32_t midi_device_volume_{UINT32_MAX};
    void poll_midi_output(GuestMemory&);
    bool midi_output_waiting() const;
    std::map<std::uint64_t,std::shared_ptr<WinmmMidiInput>> midi_inputs_;
    void poll_midi_input(GuestMemory&);
    bool midi_input_waiting() const;
    std::optional<std::uint32_t> invoke_midi_input(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_winmm_midi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_directmusic(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    void poll_dsound();
    std::string retain_dsound_device(GuestState&,GuestMemory&,std::uint64_t);
    std::uint32_t release_dsound_object(GuestState&,GuestMemory&,std::uint64_t);
    bool dsound_waiting() const;
    void poll_directmusic();
    bool directmusic_waiting() const;
    std::shared_ptr<DirectSoundState> dsound_;
    std::shared_ptr<XAudioState> xaudio_;
    std::optional<std::uint32_t> invoke_xaudio(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_xaudio();
    bool xaudio_waiting() const;
    std::shared_ptr<WasapiState> wasapi_;
    std::shared_ptr<WicState> wic_;
    std::shared_ptr<MediaFoundationState> media_foundation_;
    std::optional<std::uint32_t> invoke_media_foundation(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_media_foundation(GuestMemory&);
    bool media_foundation_waiting() const;
    bool media_foundation_ready(std::uint64_t,std::uint64_t=0) const;
    std::optional<std::uint32_t> resume_media_foundation_query(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_media_foundation_query(std::uint64_t,GuestMemory&,GuestState&);
    bool media_graphics_device(std::uint64_t) const;
    std::optional<MediaSurface> media_graphics_surface(std::uint64_t,std::uint32_t) const;
    std::optional<std::uint32_t> invoke_wasapi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_wic(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_wasapi();
    void report_crt_fast_fail(const GuestState&,GuestMemory&,std::string_view);
    bool wasapi_waiting() const;
    std::uint64_t wasapi_wakeup_ns() const;
    // 共用 GPU 完成通知（D3D11 ID3D11Fence、D3D12 佇列 fence）：提交不等 GPU，完成旗標成立後在客體端依序執行
    // 套用動作（讀回、設定 fence 值、觸發事件）；同一 order 鍵（裝置）依登記順序，前一個未完成就停止該鍵。
    struct GpuSignal {std::shared_ptr<const std::atomic<bool>> done;std::uint64_t order{};std::function<void(GuestMemory&)> apply;};
    std::deque<GpuSignal> gpu_signals_;
    // 等顯示緩衝的 Present 重試（ThreadWaitKind::PresentRetry）：執行緒 → 交換鏈的原生呈現表面；只在協調者讀寫。
    std::map<std::uint64_t,std::shared_ptr<NativeGraphics>> present_waits_;
    // 等背景著色器編譯的建立重試（ThreadWaitKind::ShaderRetry）：執行緒 → 圖形後端與票號；只在協調者讀寫。
    std::map<std::uint64_t,std::pair<std::shared_ptr<NativeGraphics>,std::uint64_t>> shader_waits_;
    // 只有 X64ARM_TRACE_CRITICAL_STALLS 開啟時記錄；等待結束才輸出，不在正常路徑收集事件。
    struct CriticalWaitDiagnostic {
        std::uint64_t started_us{},address{},caller{},initial_owner{},dependency_us{},dependency_owner{},dependency_ticket{};
        int initial_owner_wait{-1};
    };
    std::map<std::uint64_t,CriticalWaitDiagnostic> critical_wait_diagnostics_;
    void trace_critical_wait_begin(std::uint64_t,const GuestState&,GuestMemory&,std::uint64_t);
    void trace_critical_wait_sample(std::uint64_t,GuestMemory&);
    void trace_critical_wait_end(std::uint64_t);
    // 最近一次呼叫 D3D11／DXGI Present 的執行緒（渲染執行緒）：它要的著色器照舊同步轉譯，不讓它讓出 vCPU 等背景編譯
    //（DQ3 2 vCPU 時渲染執行緒自己建立 739 個、每次等待中位 6 ms，長幀 >100 ms 從約 28 變 76～116）。
    std::uint64_t present_thread_{};
    // poll=false：只登記不套用（D3D12 提交執行緒不觸發事件、不改執行緒等待狀態，由客體端輪詢套用）。
    void queue_gpu_signal(GuestMemory&,std::shared_ptr<const std::atomic<bool>>,std::uint64_t,std::function<void(GuestMemory&)>,bool poll=true);
    // 待完成通知數（圖形鎖內以原子操作更新）：協調者計算喚醒期限時不取鎖讀取。WindowsServices 須可移動，不用 std::atomic 成員。
    std::size_t gpu_signal_count_{};
    // try_only：圖形鎖正被占用（提交執行緒重播中）時略過本次輪詢，供週期性的等待解析與免鎖取值使用。
    void poll_gpu_signals(GuestMemory&,bool try_only=false);
    std::uint64_t gpu_signal_wakeup_ns() const;
    void d3d12_signal_fence(std::uint64_t,std::uint64_t);
    std::shared_ptr<DirectInputState> dinput_;
    std::optional<std::uint32_t> invoke_dinput(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    void poll_dinput(bool background=false);
    bool dinput_waiting() const;
    void dinput_mouse_wheel(std::int32_t,bool horizontal=false);
    void inject_dinput(bool,unsigned,std::int64_t,std::uint32_t);
    void prepare_input_backend();
    void dinput_cursor_warped();
    std::shared_ptr<input::Backend> input_backend()const;
    std::shared_ptr<struct RawInputState> raw_input_;
    std::shared_ptr<struct PointerInputState> pointer_input_;
    void poll_pointer_input();
    void remember_pointer_message(const GuiMessage&);
    void release_pointer_input(std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_pointer_input(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void refresh_raw_input(const std::vector<input::Device>&);
    bool raw_input_injecting_{}; // 正在處理注入（SendInput／自動化）事件：前景判定不看宿主視窗。
    void raw_input_event(const input::Event&);
    void raw_input_report(const input::Report&);
    void raw_input_keyboard(const GuiMessage&,std::uint32_t,std::uint64_t,std::uint64_t=0);
    void raw_input_mouse(std::uint32_t,std::int32_t,std::int32_t,std::uint16_t,std::uint16_t,std::uint32_t,std::uint64_t,std::uint64_t=0);
    void queue_raw_input(std::uint64_t,std::uint32_t,std::vector<std::uint8_t>,std::uint32_t,std::uint64_t);
    bool raw_legacy_enabled(bool)const;
    bool raw_hotkeys_enabled()const;
    void release_raw_input(std::uint64_t,std::uint64_t=0,std::uint64_t=0);
    std::optional<std::uint32_t> invoke_raw_input(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_hid(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> open_hid(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,const std::u16string&,const FileOpenArguments* = nullptr);
    std::optional<std::uint32_t> open_usb(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,const std::u16string&,const FileOpenArguments* = nullptr);
    void finish_dinput_enumeration(std::uint64_t,GuestMemory&,const GuestState&);
    std::optional<std::uint32_t> invoke_dsound(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::shared_ptr<D3D9State> d3d9_;
    std::optional<std::uint32_t> resume_d3dx_progress(GuestState&, GuestMemory&, std::uint64_t, std::uint32_t);
    void discard_d3dx_progress(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_d3d9(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::shared_ptr<D3D11State> d3d11_;
    std::optional<std::uint32_t> resume_d3d_compile(GuestState&, GuestMemory&, std::uint64_t, std::uint32_t);
    void discard_d3d_compile(GuestState&, GuestMemory&, std::uint64_t);
    std::optional<std::uint32_t> invoke_d3d11(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    // D3D11 命令串流（見 d3d11_command_stream.inc）。entry 在 worker 服務入口、取映射鎖之前呼叫。
    enum class D3D11StreamStep:std::uint8_t {Continue,Done,Refuse,Coordinator};
    D3D11StreamStep d3d11_stream_entry(GuestState&,GuestMemory&);
    unsigned d3d11_stream_release(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool d3d11_stream_object_eligible(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,bool update,std::uint32_t& width);
    bool d3d11_stream_get_targets(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void d3d11_stream_claim(GuestMemory&,std::uint64_t context,std::uint64_t teb);
    void d3d11_stream_drain() noexcept;
    void d3d11_stream_resume() noexcept;
    void d3d11_stream_loop(GuestMemory&,struct D3D11CommandStream&);
    void d3d11_stream_rethrow();
    bool d3d11_stream_failed() const noexcept;
    void d3d11_stream_setup(GuestMemory&,std::uint64_t guest_base);
    void d3d11_stream_quiesced() noexcept;
    void d3d11_stream_full_service_sync(const WindowsServiceDescriptor&) noexcept;
    bool d3d11_stream_owner_resource_sync(const GuestState&,GuestMemory&,const WindowsServiceDescriptor&) noexcept;
    bool d3d11_stream_full_enqueue(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool d3d11_stream_present_eligible(const GuestState&) const;
    void d3d11_stream_prepare_present(struct D3D11CommandStream&,std::uint64_t swap_pointer);
    template<class Work> void d3d11_stream_post_present(const std::shared_ptr<class NativeOpenGlContext>&,Work);
    bool d3d11_stream_worker_stub(GuestAddress) const noexcept;
    void d3d11_stream_coordinated(const GuestState&,GuestMemory&,bool device_creation) noexcept;
    void d3d11_stream_trace() const noexcept;
    // 完整 D3D11 分派（巨型函式，堆疊框架約 336 KB）；invoke_d3d11 先以前置快速路徑處理最常見的簡單方法。
    std::optional<std::uint32_t> invoke_d3d11_full(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    // coordinator=true：協調者持獨占映射鎖，可配置／釋放 heap；worker（共用鎖）維持持久映射重用的限制。
    bool d3d11_local_eligible(const GuestState&,GuestMemory&,const WindowsServiceDescriptor&,bool coordinator=false) const;
    // D3D9 裝置方法能否在其他 worker 執行期間由協調者本地完成（見 resident_coordinated_service）；只讀物件表，不改狀態。
    bool d3d9_local_eligible(const GuestState&,GuestMemory&,const WindowsServiceDescriptor&) const;
    // object 是 DXGI 介面卡（只在協調者建立，worker 可不取圖形鎖查詢）。
    bool d3d11_adapter(std::uint64_t object) const;
    // worker 上的 D3D11 方法為 true；延後需要改動客體 heap 的銷毀，協調者本地路徑則可處理。
    static bool& graphics_local() noexcept {static thread_local bool value=false;return value;}
    // 協調者在其他 worker 繼續執行時本地完成 D3D11 方法：不可排程或讓呼叫者等待（見 resident_coordinated_service）。
    static bool& graphics_coordinator_local() noexcept {static thread_local bool value=false;return value;}
    // 目前執行緒正在 worker 上執行 D3D12 服務：程序事件紀錄（module_events）屬協調者，worker 端先暫存於 D3D12 狀態。
    static bool& d3d12_worker_context() noexcept {static thread_local bool value=false;return value;}
    // 目前執行緒是 D3D12 提交執行緒且正在重播的工作（D3D12Submitter::Job*）：invoke_d3d12 以工作參數代替客體呼叫框架。
    static void*& d3d12_submit_context() noexcept {static thread_local void* value=nullptr;return value;}
    // worker 上不取圖形鎖執行的 D3D12 方法（錄製、描述元、取值、提交排入）：invoke_d3d12 入口據此不取圖形鎖。
    static bool& d3d12_unlocked_call() noexcept {static thread_local bool value=false;return value;}
    // 所有 D3D11／DXGI 服務共用的鎖（可重入）：worker 上直接執行裝置內容方法時與協調者串行化。
    static GraphicsMutex& graphics_mutex() noexcept;
    // 等待圖形鎖的執行緒數：D3D12 提交執行緒在命令清單之間看到有人等待就暫時讓出（見 D3D12Submitter）。
    static std::atomic<unsigned>& graphics_waiters() noexcept {static std::atomic<unsigned> value{};return value;}
    // 目前執行緒累計等圖形鎖的時間（奈秒，只在競爭時計時）：X64ARM_TRACE_D3D12_SUBMIT 統計提交執行緒用。
    static std::uint64_t& graphics_wait_ns() noexcept {static thread_local std::uint64_t value=0;return value;}
    static void lock_graphics(std::unique_lock<GraphicsMutex>& lock){
        if(lock.try_lock())return;
        const auto start=std::chrono::steady_clock::now();
        graphics_waiters().fetch_add(1,std::memory_order_relaxed);
        // 持有者多半只持幾微秒（D3D11 命令串流每筆約 1 µs）：先自旋約 30 µs，進核心等待的喚醒延遲遠大於這段。
        bool acquired=false;
#if defined(__aarch64__)
        {
            std::uint64_t now=0,frequency=0,since=0;asm volatile("mrs %0, cntvct_el0":"=r"(since));asm volatile("mrs %0, cntfrq_el0":"=r"(frequency));
            const auto limit=frequency*30U/1000000U;
            do{
                for(unsigned i=0;i<16 && !acquired;++i){__builtin_arm_yield();acquired=lock.try_lock();}
                asm volatile("mrs %0, cntvct_el0":"=r"(now));
            }while(!acquired && now-since<limit);
        }
#endif
        if(!acquired)lock.lock();
        graphics_waiters().fetch_sub(1,std::memory_order_relaxed);
        graphics_wait_ns()+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
    }
    // 可直接在 worker 上執行的 D3D11 方法；輸出頁另核對 COW，不符資格回傳空指標。
    const WindowsServiceDescriptor* graphics_worker_service(GuestAddress) const noexcept;
    // 可直接在 worker 上執行的 D3D12 方法（命令清單錄製、描述元建立／複製、不寫客體記憶體的取值）；其餘回傳空指標。
    const WindowsServiceDescriptor* d3d12_worker_service(GuestAddress) const noexcept;
    // x64 客體常式頁（guest_thread_x64.s）中具名常式的入口；未映射、追蹤服務或啟動剖析時為 0（同 kernel32 常式的使用條件）。
    GuestAddress guest_thread_routine(std::string_view symbol) const noexcept;
    // 只發布既有 once_initializations 的已完成狀態，不建立第二套初始化／等待契約。
    void publish_once_cache(GuestMemory&,std::uint64_t,const OnceInitialization&);
    // parallel_service 的靜態部分（依 stub 描述表與環境開關；x64 結果由 parallel_service 依 stub 索引快取）。
    bool parallel_service_static(GuestAddress) const noexcept;
    bool parallel_crt_format_service(GuestAddress) const noexcept;
    bool invoke_parallel_crt_format(GuestState&,GuestMemory&,std::uint64_t);
    static constexpr GuestAddress unresolved_base{windows_address_layout::x64.unresolved};
    std::vector<std::string> unresolved_imports_;
    static constexpr GuestAddress runtime_base{0x3002000000ULL};
    static constexpr GuestAddress teb_base{0x3004000000ULL};
    static constexpr GuestAddress peb_base{0x3004010000ULL};
    static constexpr GuestAddress callback_return{windows_address_layout::x64.callbacks};
    static constexpr GuestAddress startup_entry{callback_return.value+0x10};
    static constexpr GuestAddress exception_entry{callback_return.value+0x20};
    static constexpr GuestAddress exception_base{0x3005000000ULL};
    ExceptionState exceptions_;
    static constexpr GuestAddress thread_return{callback_return.value+0x30}, thread_start{callback_return.value+0x40};
    static constexpr GuestAddress process_exit_retry{callback_return.value+0x50};
    // 每個程序的 Windows 位址配置；PE32 使用 32-bit 可表示的區域。
    bool x86_{};
    GuestAddress stub_base_ {stub_base};
    GuestAddress command_line_address_ {command_line_address};
    GuestAddress unresolved_base_ {unresolved_base};
    GuestAddress runtime_base_ {runtime_base};
    GuestAddress teb_base_ {teb_base};
    GuestAddress peb_base_ {peb_base};
    GuestAddress callback_return_ {callback_return};
    GuestAddress startup_entry_ {startup_entry};
    GuestAddress exception_entry_ {exception_entry};
    GuestAddress exception_base_ {exception_base};
    GuestAddress thread_return_ {thread_return};
    GuestAddress thread_start_ {thread_start};
    GuestAddress process_exit_retry_ {process_exit_retry};
    ThreadRuntime threads_;
    // 原生批次執行後 RIP 可能已到 API；計帳須沿用批次開始時選定的執行緒。
    std::array<std::uint64_t,64> parallel_thread_ids_{};
    std::size_t parallel_thread_count_{};
    std::uint64_t parallel_cursor_{};
    std::uint64_t resident_object_wake_after_{}; // 服務邊界下一次解析物件等待的單調時鐘（100 ns）。
    std::uint64_t object_wait_order_{}; // 物件等待的進入序號來源。
    GuestState* mutex_wake_handoff_{}; // 本地路徑判定「持互斥鎖 SetEvent 喚醒等待者」，交由單槽換手完成。
    // NULL handle 自旋等待的停駐者數與 ReleaseSemaphore(NULL) 累積的喚醒額度（worker 持共用鎖讀寫，協調者解析時消耗）。
    struct NullWaitPark {
        std::atomic<std::uint32_t> parked{},tokens{};
        NullWaitPark()=default;
        // 停駐狀態屬於執行中的排程，不隨服務物件複製（副本沒有停駐中的執行緒）。
        NullWaitPark(const NullWaitPark&) noexcept {}
        NullWaitPark& operator=(const NullWaitPark&) noexcept {return *this;}
    } null_park_;
    bool owns_local_mutex(std::uint64_t thread) const;
    std::uint64_t object_wait_entry_order(GuestThread& thread,const GuestState& state);
    bool mutex_has_waiters(std::uint64_t mutex) const;
    void hand_off_released_mutex(GuestMemory& memory,std::uint64_t mutex);
    // 上一個服務是 D3D11 裝置內容輕量入口時，常駐服務收尾的等待解析與排程限流（見 after_resident_parallel）。
    bool graphics_service_epilogue_{};std::uint64_t last_graphics_epilogue_{};
    // 協調者本地一般服務執行中（resident_coordinated_service）：provider 要啟動客體回呼時改丟 LocalServiceRefused，交回完整路徑重做。
    bool local_plain_service_{};
    // 協調者本地 D3D9 服務執行中：物件銷毀（可能釋放客體 heap、啟動私有資料回呼）延到下一次完整 D3D9 呼叫。
    bool d3d9_local_call_{};
    // 協調者的 PeekMessage 抽完宿主事件仍沒有訊息時記下的期限（單調 tick，約 1 ms）：期限內訊息來源全空，worker 上的
    // PeekMessage 直接回 FALSE（見 parallel_peek_service）。協調者只在獨占映射鎖或全停下寫入，worker 持共用鎖讀取。
    std::uint64_t gui_peek_idle_until_{};
    // 核心已選定、尚未換入的單 slot 搶占替換者（resident_preempt_plan 回報 true 時記錄）；單槽換手選人時排除，避免同一
    // 客體執行緒同時進入兩個 slot。搶占提交或重新選取平行集合時清除。
    GuestState* resident_preempt_pending_{};
    // concurrent_guest_threads 的近似快取：世代號未變時每 8 次呼叫才重新走訪（多執行緒讀寫，鬆散原子）。
    struct ConcurrentGuestCache {
        std::atomic<std::uint64_t> epoch{UINT64_MAX};
        std::atomic<std::uint32_t> countdown{};
        std::atomic<bool> value{};
        ConcurrentGuestCache()=default;
        // 快取不屬於服務物件的複製狀態；副本重新計算，也保留 PeProgram 的傳回契約。
        ConcurrentGuestCache(const ConcurrentGuestCache&) noexcept {}
        ConcurrentGuestCache& operator=(const ConcurrentGuestCache&) noexcept {
            epoch.store(UINT64_MAX,std::memory_order_relaxed);
            countdown.store(0,std::memory_order_relaxed);
            value.store(false,std::memory_order_relaxed);
            return *this;
        }
    };
    mutable ConcurrentGuestCache concurrent_cache_;
    // 最近一次計算喚醒期限時最早的客體等待期限（100 ns 單調刻度）；過期即需完整排程解析逾時，schedule 後清除。
    mutable std::uint64_t resident_wait_deadline_{UINT64_MAX}; // 只在協調執行緒讀寫。
    struct ResolveWaitStats {std::uint64_t calls{},skipped{},changed{};} resolve_wait_stats_; // 逾時路徑等待解析的次數（X64ARM_SERVICE_COUNTS 輸出）。
    std::uint64_t resident_full_poll_ticks_{}; // 上次完整排程輪詢外部完成（wake_threads）的單調時間；物件等待停駐的保險用。
    std::map<std::uint64_t,GuestAddress> resident_service_requests_;
    std::uint64_t thread_id(std::uint64_t handle) const;
    std::uint32_t thread_identifier(std::uint64_t) const;
    std::uint64_t thread_from_identifier(std::uint32_t) const;
    std::uint32_t allocate_thread_identifier(std::uint64_t);
    std::uint32_t remote_thread_identifier(std::uint32_t,std::uint64_t,std::uint32_t);
    std::uint32_t thread_access(std::uint64_t handle) const;
    bool dispatch_user_apc(GuestState&,GuestMemory&,std::uint64_t completion,bool completion_error=false);
    struct CompletionResult {std::uint32_t value{},error{};bool ready{},guest_return{};};
    CompletionResult poll_completion(GuestState&,GuestMemory&,std::uint64_t handle,bool extended,bool expired);
    void advance_waitable_timers();
    void wake_threads(GuestMemory& memory);
    enum class ThreadWakeScope { All, Condition, Slim, Critical, Objects, Address };
    bool resolve_thread_waits(GuestMemory&,ThreadWakeScope scope=ThreadWakeScope::All,std::uint64_t address=0);
    void schedule(GuestState& state, GuestMemory& memory, bool control_requests_only=false);
    // 常駐平行時協調者在其他 worker 執行中切換 slot 0 的目前客體（只做 schedule 的保存／載入，不輪詢、不選人）。
    void switch_resident_current(GuestState& state, GuestMemory& memory, std::uint64_t target);
    // slot 0 停駐：目前客體的活體狀態已存回執行緒表（見 resident_unpark）。
    void park_resident_current(GuestState& state, GuestMemory& memory);
    void save_resident_current(GuestState& state, GuestMemory& memory);
    void load_resident_current(GuestState& state, GuestMemory& memory, std::uint64_t target);
    GuestThread* select_resident_replacement(std::span<GuestState* const> active, std::uint32_t processor, std::uint64_t exclude);
    bool resident_current_parked_{};
    std::optional<std::pair<std::int32_t,std::int32_t>> automation_mouse_{}; // 自動化注入滑鼠的上一個客戶區位置（Raw Input 相對位移基準）
    std::optional<std::uint32_t> force_finish_thread(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> finish_thread(GuestState& state, GuestMemory& memory, std::uint32_t code);
    std::optional<std::uint32_t> exit_thread(GuestState& state, GuestMemory& memory, std::uint32_t code);
    std::optional<std::uint32_t> invoke_error(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_registry(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_registry_hive(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_registry_notify(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<bool> registry_key_signaled(std::uint64_t) const;
    std::uint32_t flush_registry_hives(bool explicit_flush=true);
    std::uint32_t load_registry_hive(RegistryTransaction&,const std::u16string&,const std::u16string&,const std::u16string&,std::uint32_t);
    std::uint32_t unload_registry_hive(RegistryTransaction&,const std::u16string&);
    std::uint32_t save_registry_hive(const std::u16string&,HostFile&,std::uint32_t);
    std::uint32_t restore_registry_hive(RegistryTransaction&,const std::u16string&,HostFile*,std::uint32_t,std::uint64_t);
    std::uint32_t replace_registry_hive(RegistryTransaction&,const std::u16string&,const std::u16string&,const std::u16string&);
    void initialize_registry_session();
    struct RegistrySession;
    std::shared_ptr<RegistrySession> registry_session_;
    std::u16string registry_object_path(const GuestMemory&,std::uint64_t,bool);
    std::uint32_t commit_registry_transaction(RegistryTransaction&);
    std::map<std::u16string,std::shared_ptr<HostFile>> registry_hive_files_;
    std::map<std::u16string,std::shared_ptr<HostFile>> registry_replacement_files_;
    std::optional<std::uint32_t> invoke_registry_security(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    const std::vector<std::uint8_t>& registry_security_descriptor(const std::u16string&);
    std::vector<std::uint8_t> registry_creation_descriptor(const std::vector<std::uint8_t>&,security::SecurityDescriptor);
    std::uint32_t registry_security_access(const std::u16string&,std::uint32_t);
    std::u16string registry_current_user_path(bool cached=false);
    std::pair<std::u16string,std::uint32_t> registry_security_handle(std::uint64_t,std::uint32_t);
    std::optional<std::uint32_t> invoke_nt_registry(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t query_registry_multiple_values(GuestMemory&,const WindowsCall&,const RegistryHandle&,bool,bool);
    std::optional<std::uint32_t> invoke_file_control(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t set_file_information(GuestMemory&,std::uint64_t,std::uint32_t,std::uint64_t,std::uint32_t);
    std::uint32_t set_file_valid_data(std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_file(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_file_encryption(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct EtwSessionInfo {
        std::shared_ptr<HostFile> file;std::string key,owner;
        std::uint32_t minimum{2},maximum{64},flush_timer{},mode{},enable_flags{},file_errors{};
        std::uint64_t deadline{UINT64_MAX};bool private_logger{};
        bool operator==(const EtwSessionInfo&)const=default;
    };
    struct EtwConsumerInfo {
        std::uint64_t snapshot{},last_event{},event_callback{},buffer_callback{},context{};
        std::uint32_t mode{},buffers_read{};etw::Header header;
        bool operator==(const EtwConsumerInfo&)const=default;
    };
    struct EtwProcessOperation {
        GuestState saved;std::vector<std::uint64_t> handles;
        std::uint64_t scratch{};bool buffer_callback{};
        std::uint64_t class_consumer{},class_event{};etw::Guid class_guid{};bool class_pending{};
        bool operator==(const EtwProcessOperation&)const=default;
    };
    struct EtwCallbackOperation {
        etw::Notification notification;std::uint64_t allocation{},worker{},owner{};bool late{};
        bool operator==(const EtwCallbackOperation&)const=default;
    };
    std::map<std::uint64_t,EtwSessionInfo> etw_sessions_;
    std::map<std::uint64_t,EtwConsumerInfo> etw_consumers_;
    std::map<etw::Guid,std::uint64_t> etw_class_callbacks_;
    std::map<std::uint64_t,EtwProcessOperation> etw_processes_;
    std::map<std::uint64_t,EtwCallbackOperation> etw_callbacks_;
    std::map<std::uint64_t,std::vector<std::uint64_t>> etw_enable_waits_;
    std::shared_ptr<PerflibState> perflib_;
    PerflibState& perflib_state();
    std::optional<std::uint32_t> invoke_perflib(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> advance_perflib(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> resume_perflib(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_perflib(GuestState&,GuestMemory&,std::uint64_t);
    void poll_perflib(GuestMemory&);
    std::uint32_t queue_perflib_remote(std::uint32_t,std::uint64_t,std::uint64_t,std::uint32_t,std::uint32_t,
        const std::array<std::uint64_t,5>&,std::uint32_t,int& payload,int token);
    void cancel_perflib_remote(std::uint32_t,std::uint64_t,std::uint64_t);
    bool perflib_remote_waiting()const;
    std::uint64_t finish_perflib_remote(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    bool perflib_consumer_ready(std::uint64_t)const;
    void release_perflib_subscription(std::uint64_t);
    bool advance_perflib_control(GuestState&,GuestMemory&,std::uint64_t);
    bool merge_perflib_consumer_data(std::uint64_t);
    std::uint32_t submit_perflib_consumer_target(std::uint64_t,std::size_t,std::uint32_t);
    void cleanup_perflib_thread(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_reparse(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_etw(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_etw_provider(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_etw_controller(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_etw_consumer(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_etw_callback(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_etw_callback(GuestState&,GuestMemory&,std::uint64_t);
    bool dispatch_etw_notifications(GuestState&,GuestMemory&,bool late=false,std::uint64_t registration=0);
    std::optional<std::uint32_t> advance_etw_process(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> resume_etw_process(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_etw_process(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> finish_etw_exception(GuestState&,GuestMemory&);
    void cleanup_etw_thread(GuestState&,GuestMemory&,std::uint64_t);
    void poll_etw(GuestMemory&);
    std::uint64_t etw_next_deadline()const;
    bool etw_session_access(const EtwSessionInfo&);
    std::uint32_t flush_etw_file(std::uint64_t);
    std::uint64_t etw_timestamp(std::uint32_t,const GuestState&)const;
    std::optional<std::uint32_t> invoke_volume(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct RegistryNotification {
        struct KeySnapshot {
            std::map<std::u16string,RegistryValue> values;
            std::uint64_t modified{};
            bool operator==(const KeySnapshot&) const = default;
        };
        std::uint64_t key{},event{},thread{},native_iosb{},routine{},context{};
        std::uint32_t completion_status{0x103U};
        std::u16string path;std::uint32_t filter{};bool subtree{};
        std::map<std::u16string,KeySnapshot> snapshot;
        std::map<std::u16string,std::vector<std::uint8_t>> security_snapshot;
        bool operator==(const RegistryNotification&) const = default;
    };
    std::map<std::uint64_t,RegistryNotification> registry_notifications_;
    std::uint64_t next_registry_notification_{1},registry_poll_time_{};
    void poll_registry_notifications(bool reload=true,std::u16string_view mutation={},std::uint32_t filter=0,GuestMemory* memory=nullptr);
    struct RegisteredWait {
        std::uint64_t target{},callback{},parameter{},worker{},deadline{},completion_event{},timer_owner{},pool_instance{},power_owner{},etw_operation{},device_owner{},perflib_operation{},bluetooth_delivery{},pdh_query{};
        // 共用 100 ns 間隔，避免 PDH 的 DWORD 秒數在轉成 DWORD 毫秒時溢位。
        std::uint64_t interval_ticks{UINT64_MAX};
        std::vector<GuestCallback> callback_batch;
        std::vector<std::uint64_t> callback_storage;
        std::uint32_t flags{};bool armed{true},ready{},active{},closing{},timed_out{},work_item{},pool_cleanup{};
        bool operator==(const RegisteredWait&) const = default;
    };
    std::map<std::uint64_t,RegisteredWait> registered_waits_;
    std::map<std::uint64_t,std::uint64_t> wait_workers_;
    std::uint64_t next_registered_wait_{0xc0000};
    struct TimerQueue {
        std::uint64_t completion_event{};bool closing{};
        bool operator==(const TimerQueue&) const = default;
    };
    struct QueueTimer {
        std::uint64_t queue{},callback{},parameter{},deadline{},completion_event{};
        std::uint32_t period{},flags{},outstanding{};bool expired{},closing{},multimedia{};
        bool operator==(const QueueTimer&) const = default;
    };
    std::map<std::uint64_t,TimerQueue> timer_queues_;
    std::map<std::uint64_t,QueueTimer> queue_timers_;
    std::uint64_t next_timer_queue_handle_{0xd0000};
    bool timer_workers_exhausted_{};
    std::set<std::uint64_t> multimedia_pulses_;
    std::shared_ptr<ThreadpoolState> threadpool_;
    bool threadpool_worker_reserved(std::uint64_t) const;
    std::uint64_t acquire_threadpool_worker(const GuestState&,GuestMemory&,std::uint64_t);
    void collect_threadpools();
    void poll_threadpool(GuestMemory&);
    std::uint64_t threadpool_next_deadline() const;
    bool threadpool_callbacks_pending(std::uint64_t) const;
    void release_threadpool_instance(std::uint64_t);
    bool cleanup_threadpool_callback(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_threadpool(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t acquire_wait_worker(const GuestState&,GuestMemory&,std::uint64_t);
    // 在執行緒池 worker 上執行一次性完成常式（例如 GetAddrInfoExW 的 lpCompletionRoutine）；失敗回 Win32 錯誤碼。
    std::uint32_t queue_pool_callback(const GuestState&,GuestMemory&,GuestCallback);
    void poll_timer_queues(GuestMemory&);
    void collect_timer_queues();
    std::optional<std::uint32_t> invoke_timer_queue(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t thread_stack_reservation(std::uint64_t parameter, bool reservation) const noexcept;
    std::uint64_t create_guest_thread(const GuestState&,GuestMemory&,std::uint64_t,std::uint64_t,std::uint64_t,std::uint32_t,const GuestThreadBootstrap* = nullptr);
    void initialize_stack_guard(GuestMemory&,std::uint64_t,std::uint64_t,ThreadStackGuard&);
    std::uint32_t configure_stack_guarantee(GuestState&,GuestMemory&,std::uint32_t,bool,std::uint32_t&);
    bool release_stack_guard(GuestState&,GuestMemory&,std::uint64_t);
    struct WaitCompletionPacketState {
        std::uint64_t target{},port{};CompletionPacket packet{};
        enum class Phase {Idle,Armed,Queued} phase{Phase::Idle};
        std::vector<std::uint8_t> security_descriptor;
        bool operator==(const WaitCompletionPacketState&)const=default;
    };
    std::map<std::uint64_t,std::pair<std::uint64_t,std::uint32_t>> wait_completion_handles_;
    std::map<std::uint64_t,WaitCompletionPacketState> wait_completion_packets_;
    std::set<std::uint64_t> wait_completion_targets_;
    std::uint64_t retain_wait_completion_target(std::uint64_t);
    std::uint64_t retain_wait_completion_process(std::uint64_t);
    void release_wait_completion_target(GuestMemory&,std::uint64_t);
    void poll_wait_completion_packets(GuestMemory&);
    void complete_wait_packet(std::uint64_t);
    std::uint32_t cancel_wait_packet(GuestMemory&,std::uint64_t,bool);
    bool close_wait_packet(GuestMemory&,std::uint64_t);
    bool close_completion_port(GuestMemory&,std::uint64_t);
    CompletionResult poll_native_completion(GuestState&,GuestMemory&,std::uint64_t,bool,bool);
    void poll_registered_waits(GuestMemory&);
    void release_registered_wait(std::uint64_t);
    std::optional<std::uint32_t> resume_registered_wait(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> invoke_registered_wait(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_thread(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    void begin_exception(GuestState& state, GuestMemory& memory, std::uint32_t code, std::uint32_t flags, std::span<const std::uint64_t> parameters);
    bool enter_user_exception(GuestState& state, GuestMemory& memory);
    std::optional<std::uint32_t> finish_user_exception_entry(GuestState& state, GuestMemory& memory);
    bool callback_protects_exception(const CallbackFrame&,std::uint32_t)const;
    std::optional<std::uint32_t> finish_protected_callback_exception(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> advance_exception(GuestState& state, GuestMemory& memory);
    std::optional<std::uint32_t> finish_user_callback_exception(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> resume_exception(GuestState& state, GuestMemory& memory, std::uint64_t result);
    void prepare_handler(GuestMemory& memory, ExceptionDispatch& dispatch, const ExceptionWalkFrame& frame, bool target);
    std::optional<bool> append_exception_frame(GuestState&, GuestMemory& memory, ExceptionDispatch& dispatch);
    std::optional<std::uint32_t> advance_x86_exception(GuestState& state, GuestMemory& memory);
    void begin_x86_unwind(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    void leave_x86_exception_handler(GuestMemory& memory, ExceptionDispatch& dispatch);
    void initialize_user_objects();
    std::uint64_t thread_desktop_object(std::uint64_t thread) const;
    bool interactive_station() const;
    void present_native_window(std::uint64_t handle,const GuiWindow& window);
    void show_native_window(std::uint64_t handle,bool visible,bool activate=true);
    std::optional<std::uint32_t> invoke_winstation(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_heap(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool await_heap_service(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void prune_x86_exception_callbacks(GuestState& state, GuestMemory& memory);
    void begin_unwind(GuestState& state, GuestMemory& memory, std::size_t target, std::uint64_t ip, std::uint64_t result);
    std::optional<std::uint32_t> seh_filter_result(GuestState& state, GuestMemory& memory, std::uint64_t result);
    std::optional<std::uint32_t> invoke_exception(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> seh_finally_result(GuestState& state, GuestMemory& memory);
    std::shared_ptr<FileMappingState> mappings_;
    std::optional<std::uint32_t> invoke_mapping(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool flush_file_view(std::uint64_t,std::uint64_t);
    bool close_mapping(std::uint64_t);
    std::uint64_t map_shared_table(GuestMemory&,std::u16string_view family,std::u16string_view generation,std::span<const std::uint8_t> bytes,bool writable,const std::function<void()>& publish={},std::u16string_view previous_generation={});
    std::uint64_t map_file_view(GuestMemory&,std::uint64_t handle,std::uint32_t access,std::uint64_t offset,std::uint64_t size,std::uint64_t address=0,bool dib=false,bool copy_override=false,bool replace_placeholder=false,const VirtualAddressRequirements* requirements=nullptr);
    std::uint64_t map_dib_section(GuestMemory&,std::uint64_t handle,std::uint32_t offset,std::uint64_t size,std::uint64_t& view);
    bool unmap_file_view(GuestMemory&,std::uint64_t address,bool dib=false,std::uint32_t flags=0);
    struct CanceledHttpIo {std::shared_ptr<NativeHttp> task;process_io::Counters accounted;};
    std::vector<CanceledHttpIo> canceled_http_io_;
    void account_http_io(HttpHandle&);
    void poll_http_io();
    std::shared_ptr<process_io::Publisher> io_publisher_;
    void configure_process_io();
    std::uint32_t query_process_io(std::uint64_t,process_io::Counters&);
    std::shared_ptr<HandleTransferState> handle_transfers_;
    void poll_handle_transfers(GuestMemory&,bool force=false);
    // 監看執行緒已看到本程序的 handle 轉移通道可讀（其他程序的同步請求或回覆待處理）；只讀原子旗標。
    bool handle_transfer_requested() const noexcept;
    bool handle_transfer_polling() const noexcept; // 通道沒有監看執行緒時，排程器須輪詢它。
    void watch_descriptor(int descriptor) noexcept; // 交給監看執行緒等待可讀（子程序回報管線等）。
    std::vector<int> child_pipe_descriptors() const; // 現有子程序的回報管線，供監看執行緒建立時加入。
    void drain_child_pipes(); // 監看執行緒回報可讀時讀取子程序回報，管線才不會保持可讀。
    std::uint32_t reserve_socket_duplication(std::shared_ptr<NativeSocket>,std::uint32_t sender=0,std::uint64_t sequence=0);
    std::uint32_t duplicate_socket_to_process(GuestMemory&,std::uint64_t socket,std::uint32_t pid,std::uint32_t& token);
    bool remote_process_available(std::uint32_t) const;
    std::uint32_t process_handle_pid(std::uint64_t,std::uint32_t) const;
    std::uint64_t open_process_thread(GuestMemory&,std::uint32_t,std::uint32_t,bool);
    std::uint32_t process_thread_target(std::uint64_t,std::uint32_t&,std::uint32_t&,std::uint64_t* birth=nullptr,std::uint32_t* tid=nullptr);
    std::uint32_t transfer_thread_control(GuestMemory&,std::uint32_t,std::uint32_t,ExecutionMode,std::uint32_t,std::uint32_t&,std::vector<std::uint8_t>&,std::uint64_t expected_birth=0,std::uint32_t target_thread=1);
    std::uint32_t local_primary_thread_control(std::uint32_t,ExecutionMode,std::uint32_t,std::uint32_t&,std::vector<std::uint8_t>&,std::uint32_t target_thread=1);
    std::optional<std::uint32_t> invoke_thread_control(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t export_transfer_handle(std::uint64_t,InheritedHandle&);
    std::uint32_t export_transfer_handle_impl(std::uint64_t,InheritedHandle&);
    bool export_process_handle(std::uint64_t,InheritedHandle&);
    void import_process_handle(const InheritedHandle&);
    void import_transfer_handle(const InheritedHandle&);
    void close_transfer_handle(GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> duplicate_remote_handle(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool finish_duplicate_remote_handle(GuestState&,GuestMemory&,std::uint64_t);
    void cancel_duplicate_remote_handle(GuestMemory&,std::uint64_t);
    bool mapping_handle(std::uint64_t) const;
    bool shared_mapping_page(std::uint64_t) const;
    struct MappingViewInfo {std::uint64_t base{},size{};std::uint32_t protection{};};
    std::optional<MappingViewInfo> mapping_view_info(std::uint64_t) const;
    std::u16string mapped_file_name(std::uint64_t) const;
    bool export_mapping_handle(std::uint64_t,InheritedHandle&) const;
    void import_mapping_handle(const InheritedHandle&);
    GuiState gui_;
    GuestAddress gui_timing_cell_{};
    void initialize_gui_timing(GuestMemory&,bool shared=false);
    std::uint32_t gui_timing(const GuestMemory&,unsigned) const;
    void set_gui_timing(GuestMemory&,unsigned,std::uint32_t);
    bool gui_input_connected(std::uint64_t,std::uint64_t) const;
    GuiCaret* gui_caret(std::uint64_t);
    void draw_gui_caret(GuestMemory&,GuiCaret&,bool);
    void pump_gui_carets(GuestMemory&);
    void paint_gui_caret(GuestMemory&,std::uint64_t,bool);
    std::uint64_t gui_caret_deadline() const;
    std::optional<std::uint32_t> invoke_gui_caret(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void classify_gui_click(GuestMemory&,GuiMessage&,std::int32_t=1,bool=false);
    std::map<std::uint64_t,std::shared_ptr<RasterSurface>> rasters_;
    struct GdiBatch {std::set<std::uint64_t> surfaces;std::uint32_t calls{},limit{};};
    std::map<std::uint64_t,GdiBatch> gdi_batches_;
    std::uint64_t gdi_batch_cell() const;
    void mark_gdi_surface(GuestMemory&,std::uint64_t);
    bool flush_gdi_batch(GuestMemory&);
    std::uint32_t gdi_batch_limit(GuestMemory&,std::optional<std::uint32_t> = {});
    bool begin_gdi_batch_call(GuestMemory&,const WindowsServiceDescriptor&);
    bool end_gdi_batch_call(GuestMemory&,bool);
    std::map<std::uint64_t,std::pair<std::vector<std::uint8_t>,GuiBitmap>> bitmap_templates_;
    std::map<std::uint64_t,std::shared_ptr<RasterSurface>> raster_templates_;
    std::set<std::uint64_t> raster_shared_;
    std::uint64_t next_bitmap_template_{1};
    std::vector<std::uint8_t> dib_scratch_;
    std::uint64_t create_dib_bitmap(GuestMemory&,const GuestState&,std::uint32_t width,std::int32_t height,std::uint16_t bits_per_pixel,std::uint64_t section=0,std::uint32_t offset=0);
    bool import_dib_bitmap(GuestMemory&,std::uint64_t handle);
    void export_dib_bitmap(GuestMemory&,std::uint64_t handle);
    std::shared_ptr<RasterSurface> raster_surface(GuestMemory&,std::uint64_t dc,bool writing=true);
    std::optional<std::uint32_t> invoke_script(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool record_metafile_draw(std::uint64_t, const GuiDraw&);
    std::optional<std::uint32_t> invoke_metafile(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool raster_draw(GuestMemory&,std::uint64_t dc,GuiDraw&);
    void sync_raster_bitmap(GuestMemory&,std::uint64_t handle);
    std::shared_ptr<NativeGui> native_gui_;
    // only 非零時只抽取該 socket 的監看（WSAEnumNetworkEvents）；為零時全量抽取，距上次未滿 X64ARM_NETWORK_PUMP_US 則略過。
    void pump_gui_network(std::uint64_t only=0);
    void enqueue_gui_message(GuiMessage);
    void remember_gui_message(const GuiMessage&);
    // 游標：控制代碼解析（自訂、被 SetSystemCursor 取代或原生系統游標）、依目前游標重設原生游標、回收動畫幀控制代碼。
    const GuiIcon* resolve_gui_cursor(std::uint64_t handle,GuiIcon& system) const;
    void present_gui_cursor();
    void release_cursor_frames(std::uint64_t cursor);
    // SwapMouseButton：實體左右鍵轉成邏輯按鍵（訊息、MK_* 旗標）；GetMouseMovePointsEx 的移動歷程記錄。
    void swap_gui_mouse_buttons(GuiMessage&) const;
    void record_mouse_move_point(std::int64_t x,std::int64_t y,std::uint32_t time,std::uint64_t extra);
    // InSendMessage／GetCIMSSM 共用：最內層作用中的跨執行緒（或跨程序）傳送與其回呼框架位置。
    std::pair<std::map<std::uint64_t,GuiSentMessage>::iterator,std::size_t> active_gui_sent_message(std::uint64_t* text_token=nullptr);
    // {deviceType, originId}；in_send_message 為 GetCIMSSM 語義，目前來源可用時回傳 nullopt。
    std::optional<std::array<std::uint32_t,2>> input_message_source(bool in_send_message);
    std::optional<std::uint32_t> invoke_imm(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t imm_window_context(std::uint64_t);
    // 輸入內容目前的目標視窗：使用此內容的焦點視窗、該執行緒最近的 IME 目標，或執行緒內第一個使用此內容的視窗。
    std::uint64_t imm_context_window(std::uint64_t handle);
    bool imm_disabled(std::uint64_t thread) const;
    std::optional<std::uint64_t> imm_control_message(GuestMemory&,const GuiMessage&,bool);
    void imm_release_context_storage(GuestMemory&,const GuestState&,std::uint64_t);
    void imm_notify_composition(std::uint64_t,std::uint32_t,bool=false,bool=false);
    bool imm_native_message(GuiMessage&);
    void imm_default_result(const GuiMessage&);
    std::optional<std::uint32_t> invoke_input_api(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void observe_gui_key(GuiMessage&,bool);
    void consume_gui_input(const GuiMessage&);
    bool match_gui_hotkey(const GuiMessage&);
    bool translate_gui_key(const GuiMessage&);
    std::optional<std::uint64_t> gui_edit_input(GuestMemory&,const GuiMessage&,bool);
    void refresh_clipboard(bool local = false);
    void notify_clipboard();
    GuiClipboardNoticeResult clipboard_notice(ClipboardNoticeAction,std::uint64_t=0,std::uint64_t=0,std::optional<std::uint64_t> = {});
    void dispatch_clipboard_notifications(GuestState&,GuestMemory&);
    void pump_mouse_tracking();
    void pump_gui(GuestMemory& memory, bool wait,std::uint64_t wait_ticks=500000);
    void poll_desktop_notifications();
    // Shell hook 視窗：依工作列視窗快照差異張貼 SHELLHOOK 訊息；notify 只送給同桌面的登錄視窗。
    void poll_shell_hooks(bool announce=true);
    // WinMM 搖桿：槽位與 HID 物件對應（joyGetPos(Ex)／DevCaps／SetCapture 共用）與擷取取樣。
    void legacy_joystick(std::uint64_t id,LegacyJoystick& out);
    void poll_joystick_captures();
    void notify_shell_hook(std::uint32_t code,std::uint64_t lparam,std::uint64_t desktop);
    bool shell_hook_candidate(std::uint64_t handle) const;
    std::array<std::int32_t,4> gui_window_screen_rect(const GuiWindow& window);
    std::optional<std::uint32_t> invoke_desktop_notification(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool gui_native_control_message(GuestMemory&,const GuiMessage&);
    void finish_gui_control_notify(GuestMemory&,const GuiMessage&,std::uint64_t result=0);
    void discard_gui_control_notify(const GuiMessage&);
    bool gui_message_matches(const GuiMessage&,std::uint64_t thread,std::uint64_t window=0,std::uint32_t minimum=0,std::uint32_t maximum=0) const;
    std::uint64_t gui_send_deadline(const GuiSentMessage&) const;
    bool pending_gui_send(std::uint64_t thread,bool callbacks=true) const;
    std::optional<std::uint32_t> invoke_gui_rpc(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ole_drag(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> advance_ole_drag(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> resume_ole_drag(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> resume_ole_drag_target(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_ole_drag(GuestState&,GuestMemory&,std::uint64_t,bool target=false);
    std::vector<GuestCallback> retire_ole_drag_thread(GuestState&,GuestMemory&,std::uint64_t,bool callbacks);
    ole_drag::Input ole_drag_input(const OleDragOperation&,bool physical) const;
    std::uint64_t ole_drag_cursor(std::uint32_t);
    std::optional<std::uint32_t> invoke_com_proxy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> advance_com_proxy_dispatch(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> resume_com_proxy_dispatch(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_com_proxy_dispatch(GuestState&,GuestMemory&,std::uint64_t);
    std::vector<GuestCallback> retire_com_proxy_thread(GuestState&,GuestMemory&,std::uint64_t,bool);
    std::uint64_t create_com_proxy_owned(GuestState&,GuestMemory&,com_proxy::Key,std::vector<std::uint64_t>&);
    std::uint64_t release_com_proxy_owned(GuestState&,GuestMemory&,com_proxy::Origin);
    void queue_com_proxy_retirement(GuestMemory&,std::uint64_t);
    bool route_gui_send(GuestState&,GuestMemory&,const GuiMessage&,bool wide,bool timed = false,std::uint32_t flags = 0,std::uint32_t timeout = 0,std::uint64_t result_pointer = 0);
    bool dispatch_gui_send(GuestState&,GuestMemory&,std::uint64_t modal=0,bool callbacks=true,bool wait_message=false);
    void queue_gui_mouse(GuiMessage,bool native_activation=false);
    bool dispatch_gui_mouse(GuestState&,GuestMemory&,std::uint64_t,bool);
    std::optional<std::uint32_t> resume_gui_mouse(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::uint64_t gui_root_window(std::uint64_t)const;
    void raise_gui_window(std::uint64_t);
    std::vector<GuestCallback> activate_gui_window(std::uint64_t,bool mouse=false,bool native=true);
    std::optional<std::uint32_t> begin_gui_move_size(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t,std::uint64_t);
    std::optional<std::uint32_t> resume_gui_move_size(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_gui_move_size(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void discard_gui_move_size(GuestMemory&,const GuestState&,std::uint64_t);
    bool gui_move_size_interrupted(std::uint64_t)const;
    std::optional<std::uint32_t> invoke_gui_drag(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_gui_drag(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_gui_drag(GuestMemory&,const GuestState&,std::uint64_t);
    bool gui_drag_interrupted(std::uint64_t) const;
    void wait_gui_input(GuestState&,GuestMemory&,ThreadWait wait,std::uint64_t modal=0);
    std::optional<std::uint32_t> resume_gui_send(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    void cancel_gui_send(std::uint64_t token,std::uint32_t error);
    void finish_gui_send(GuestMemory&,std::uint64_t token,std::uint64_t result);
    void queue_gui_send_callback(const GuiSentMessage&,std::uint64_t result);
    std::optional<std::uint32_t> invoke_gui_async(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> advance_gui_async(GuestState&,GuestMemory&,std::uint64_t token);
    std::optional<std::uint32_t> resume_gui_async(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t result);
    void cancel_gui_thread(std::uint64_t thread,GuestMemory&,const GuestState&);
    std::optional<std::uint32_t> advance_gui_modal(GuestState& state, GuestMemory& memory, std::uint64_t handle);
    bool dispatch_gui_modal_message(GuestState&,GuestMemory&,std::uint64_t,const GuiMessage&);
    void clear_layered_content(std::uint64_t);
    std::optional<std::uint32_t> invoke_graphics_compat(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_legacy_gdi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_legacy_user(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // 核心模式驅動宿主（windows_kernel_driver.cpp）：每個程序各自載入 SCM 啟動的 x64 驅動映像。
    struct KernelDriverState;
    std::shared_ptr<KernelDriverState> kernel_;bool kernel_image_load_{};
    KernelDriverState& kernel_state();
    std::optional<std::uint32_t> invoke_kernel_api(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> start_kernel_driver(GuestState&,GuestMemory&,const std::u16string& service,const std::u16string& image_path);
    std::optional<std::uint32_t> stop_kernel_driver(GuestState&,GuestMemory&,const std::u16string& service,std::uint64_t status_output);
    bool kernel_device_path(std::u16string_view) const;
    bool kernel_device_handle(std::uint64_t) const;
    std::optional<std::uint32_t> open_kernel_device(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,std::u16string_view,const FileOpenArguments&);
    std::optional<std::uint32_t> invoke_kernel_device_io(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> close_kernel_device(GuestState&,GuestMemory&,std::uint64_t handle);
    std::optional<std::uint32_t> resume_kernel_request(GuestState&,GuestMemory&,std::uint64_t id,std::uint64_t value);
    bool emulate_kernel_instruction(GuestState&,GuestMemory&);
    std::uint64_t kernel_irp(GuestMemory&,std::uint8_t major,std::uint64_t device,std::uint64_t file_object);
    std::uint64_t kernel_dispatch(GuestMemory&,std::uint64_t device,std::uint8_t major);
    std::vector<std::uint8_t> physical_memory_snapshot(std::uint64_t physical,std::size_t size,GuestMemory&);
    std::pair<std::uint64_t,std::uint32_t> load_kernel_image(GuestMemory&,const std::u16string& path);
    void sync_kernel_shared_data(GuestMemory&,bool throttled=false);
    // SMBIOS 記憶體裝置與 GetPhysicallyInstalledSystemMemory／GlobalMemoryStatusEx 共用的已安裝實體記憶體。
    std::uint64_t installed_physical_memory(const GuestMemory&) const;
    // 以宿主端暫存框架呼叫已登錄、不需客體回呼的同步服務，沿用其完整驗證與副作用。
    std::uint64_t nested_service(GuestState&,GuestMemory&,std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> arguments);
    std::uint64_t nested_service(GuestState&,GuestMemory&,std::string_view family,std::string_view symbol,std::span<const std::uint64_t> arguments);
    // 經客體回呼續行的宿主服務：factory 回傳的協程以 co_await service_callback::call 呼叫客體入口，結果以 NestedResult 交回。
    // 各家族共用這個入口，不再各自撰寫回呼起始樣板；kind 必須是靜態字串。
    std::optional<std::uint32_t> nested_continuation(GuestState& state,GuestMemory& memory,std::string_view kind,
        std::function<service_callback::Task<std::optional<std::uint32_t>>(GuestState&,GuestMemory&)> factory);
    void accumulate_dc_bounds(GuiDc&,const GuiDraw&);
    std::optional<std::uint32_t> invoke_gdi(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_gdi_text(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    void present_gui(std::uint64_t handle);
    void layout_gui_toolbar(GuiWindow& window);
    struct GuiTextThunk {
        enum class Kind { Input, Output, Line, Item, Length, Creation } kind{};
        GuiMessage message{};std::uint64_t procedure{},thread{},buffer{},capacity{},target_capacity{},value{};
        bool source_wide{},target_wide{};std::uint32_t phase{};
        Continuation completion{Continuation::ReturnCallback};std::vector<std::uint64_t> scratch;
    };
    struct GuiProcedureReference {std::uint64_t window{},procedure{};bool wide{};};
    std::map<std::uint64_t,GuiTextThunk> gui_text_thunks_;
    std::map<std::uint64_t,GuiProcedureReference> gui_procedure_references_;
    std::uint64_t gui_window_procedure(std::uint64_t window,bool wide);
    bool begin_gui_text_thunk(GuestState&,GuestMemory&,const GuiMessage&,bool,Continuation=Continuation::ReturnCallback,std::uint64_t=0,std::uint64_t=0,std::optional<bool> = {});
    std::optional<std::uint32_t> resume_gui_text_thunk(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    void discard_gui_text_thunk(GuestMemory&,const GuestState&,std::uint64_t);
    std::uint64_t load_gui_icon(GuestMemory& memory,std::uint64_t instance,std::uint64_t name,bool wide,std::uint32_t width,std::uint32_t height,bool intrinsic=false,bool first=false,bool cursor=false,std::uint32_t flags=0,bool prefer_larger=false);
    std::uint64_t store_gui_icon(GuiIcon,std::uint32_t flags=0);
    std::uint64_t shell_system_icons(std::uint32_t size);
    std::optional<std::uint32_t> invoke_icon_api(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_shell_icons(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ole_misc(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ntdll_tables(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ntdll_trees(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ntdll_registry(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_xstate_context(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ntdll_sync(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_keyed_event(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_comctl_arrays(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_comctl_misc(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ntdll_extras(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_kernel32_legacy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ole_user_marshal(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_version_install(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_kernelbase_extras(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_geo(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_installable_driver(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_http_helpers(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_http_cookies(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_glu(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_glu_nurbs(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_glu_tess(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_wgl_fonts(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_wts(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // SetupAPI INF 解析家族（windows_setup_inf.cpp）。
    std::optional<std::uint32_t> invoke_setup_inf(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // netapi32 工作群組電腦查詢家族（windows_netapi.cpp）。
    std::optional<std::uint32_t> invoke_netapi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_iphelper_extras(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_user32_window_extras(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // HKEY_CLASSES_ROOT 合併檢視（使用者 Software\Classes、HKCR 本身、機器 Software\Classes，依序）：讀取字串值；回報機碼所在的根（0 使用者、1 HKCR、2 機器，-1 不存在）。
    std::optional<std::u16string> class_registry_value(const std::u16string& key,const std::u16string& name);
    int class_registry_root(const std::u16string& key);
    // CoGetState／CoSetState 的每執行緒狀態物件、CoGetCurrentLogicalThreadId、CoRegisterPSClsid 的程序內對應、CoLoadLibrary(bAutoFree) 載入的模組。
    std::map<std::uint64_t,std::uint64_t> com_thread_states_;std::map<std::uint64_t,std::array<std::uint8_t,16>> com_logical_threads_;
    std::map<std::array<std::uint8_t,16>,std::array<std::uint8_t,16>> com_ps_clsids_;std::set<std::uint64_t> com_loaded_libraries_;
    // 系統影像清單（16 或 32）：原生圖示可用時同 shell_system_icons，否則以檔案／資料夾／磁碟三個佔位項目建立；SHGetFileInfo 與 Shell_GetImageLists 共用。
    std::uint64_t shell_image_list(std::uint32_t size);
    bool draw_gui_icon(GuestMemory&,std::uint64_t dc,std::uint64_t icon,std::int32_t x,std::int32_t y,std::uint32_t width,std::uint32_t height,std::uint32_t flags,std::uint64_t brush,std::uint32_t step=0);
    std::uint64_t draw_image_list(GuestMemory&,const GuiImageList&,std::uint32_t index,std::uint64_t dc,std::int32_t x,std::int32_t y,std::uint32_t cx,std::uint32_t cy,
        std::uint32_t source_x,std::uint32_t source_y,std::uint32_t background,std::uint32_t foreground,std::uint32_t style,std::uint32_t effect=0,std::uint32_t alpha=255);
    std::vector<GuiWindowCreation> plan_gui_control_children(std::uint64_t handle,bool wide);
    void layout_gui_combo(std::uint64_t handle);
    std::uint64_t write_gui_combo_info(GuestMemory&,std::uint64_t handle,std::uint64_t output);
    std::optional<std::uint32_t> invoke_property_sheet(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_property_sheet(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    std::optional<std::uint32_t> advance_property_sheet(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_gui_dialog(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_gui_dialog(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    std::uint64_t prepare_gui_create_data(GuestMemory&,GuiWindow&,std::vector<std::uint64_t>& scratch,bool wide,std::uint64_t parameter=0,std::optional<std::uint64_t> class_pointer={},std::optional<std::uint64_t> name_pointer={});
    std::optional<std::uint32_t> start_gui_creation(GuestState&,GuestMemory&,std::vector<GuiWindowCreation>,std::uint64_t init_parameter=0);
    std::optional<std::uint32_t> advance_gui_creation(GuestState&,GuestMemory&,std::uint64_t operation);
    std::optional<std::uint32_t> resume_gui_creation(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t result);
    std::optional<std::uint32_t> abort_gui_creation(GuestState&,GuestMemory&,std::uint64_t operation);
    void cancel_gui_creation(std::uint64_t operation,GuestMemory&,const GuestState&);
    std::uint64_t allocate_gui_scratch(GuestMemory&,std::vector<std::uint64_t>&,std::span<const std::uint8_t>,std::size_t size=0);
    void release_gui_scratch(GuestMemory&,const GuestState&,const std::vector<std::uint64_t>&);

    std::optional<std::uint32_t> start_gui_window_pos(GuestState&,GuestMemory&,std::vector<GuiWindowPosition>);
    std::optional<std::uint32_t> finish_gui_window_pos(GuestState&,GuestMemory&,std::uint64_t operation);
    void discard_gui_window_pos(GuestMemory&,const GuestState&,std::uint64_t operation);
    void present_gui_menu(GuestMemory&,std::uint64_t handle);
    bool snapshot_gui_menu(GuestMemory&,std::uint64_t menu,std::vector<GuiPopupItem>&);
    std::optional<std::uint32_t> invoke_ui_helpers(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_gray_string(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_gray_string(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    void discard_gray_string(std::uint64_t,GuestMemory&,const GuestState&);
    std::optional<std::uint32_t> invoke_draw_state(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_draw_state(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    std::optional<std::uint32_t> begin_gui_main_menu(GuestState& state,GuestMemory& memory,const GuiMessage& message);
    std::uint64_t load_gui_menu(GuestMemory&,std::uint64_t begin,std::uint64_t size);
    void destroy_gui_menu(std::uint64_t handle);
    std::uint64_t gui_child(std::uint64_t parent, std::uint64_t id) const;
    // language 小於 0 取第一個語系；其他值須完全符合（FindResourceEx）。
    std::uint64_t gui_resource(const GuestMemory& memory, std::uint64_t module, std::uint64_t name, std::uint64_t type, bool wide, bool first=false, int language=-1) const;
    const RuntimeImage* resource_image(std::uint64_t handle, bool containing = false) const;
    std::vector<GuestCallback> revoke_com_message_filter(GuestMemory& memory, std::uint64_t thread);
    std::optional<std::uint32_t> invoke_profile(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_legacy_device(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_vtable_proxy(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_version(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_locale(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_fonts(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> advance_font_enumeration(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> resume_font_enumeration(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    void discard_font_enumeration(std::uint64_t,GuestMemory&,const GuestState&);
    std::map<std::uint64_t,FontEnumeration> font_enumerations_;
    std::optional<std::uint64_t> gui_control_message(GuestMemory& memory, const GuiMessage& message, bool wide);
    std::optional<std::uint64_t> default_gui_control(GuestMemory& memory,const GuiMessage& message,bool wide);
    std::uint64_t stock_gui_proc(std::u16string_view name,bool wide) const;
    static std::u16string_view stock_gui_class(std::uint64_t procedure);
    GuiClass* gui_class_record(std::u16string_view name,bool system = true);
    void parse_gui_syslink(GuiWindow&);
    void layout_gui_tab(GuiWindow&);
    void layout_gui_header(std::uint64_t);
    void sync_gui_list_header(std::uint64_t);
    std::optional<std::uint64_t> gui_header_message(GuestMemory&,const GuiMessage&,bool);
    bool begin_gui_header(GuestState&,GuestMemory&,const GuiMessage&,bool);
    std::optional<std::uint32_t> resume_gui_header(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint64_t> gui_tab_message(GuestMemory&,const GuiMessage&,bool wide);
    bool begin_gui_tree_selection(GuestState&,GuestMemory&,const GuiMessage&,bool wide);
    bool begin_gui_tree(GuestState&,GuestMemory&,const GuiMessage&,bool wide);
    std::optional<std::uint32_t> resume_gui_tree(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> resume_gui_tree_selection(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    bool begin_gui_tab(GuestState&,GuestMemory&,const GuiMessage&,bool wide);
    std::optional<std::uint32_t> resume_gui_tab(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint64_t> gui_tooltip_message(GuestMemory&,const GuiMessage&,bool);
    bool begin_gui_tooltip(GuestState&,GuestMemory&,const GuiMessage&,bool);
    bool begin_gui_tooltip_relay(GuestState&,GuestMemory&,const GuiMessage&,bool,bool,std::uint64_t);
    std::optional<std::uint32_t> resume_gui_tooltip(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::u16string gui_tooltip_text(GuestMemory&,std::uint64_t,std::uint64_t,bool);
    void layout_gui_tooltip(GuiWindow&);
    void hide_gui_tooltip(std::uint64_t);
    void relay_gui_tooltip(std::uint64_t,const GuiMessage&);
    GuiLinkLayout layout_gui_syslink(const GuiWindow&,std::uint32_t width);
    std::optional<std::uint64_t> gui_syslink_message(GuestMemory&,const GuiMessage&,bool wide);
    void notify_gui_syslink(GuestMemory&,std::uint64_t window,std::uint32_t index,std::int32_t code);
    std::optional<std::uint32_t> invoke_stock_gui(GuestState&,GuestMemory&);
    std::vector<GuestCallback> revoke_gui_drop_targets(GuestMemory&,std::uint64_t hwnd);
    std::optional<GuiMessage> complete_gui_button_click(std::uint64_t handle);
    bool begin_gui_control(GuestState&,GuestMemory&,const GuiMessage&,bool wide);
    std::optional<std::uint32_t> resume_gui_control(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t result);
    void discard_gui_control(std::uint64_t token);
    std::optional<std::uint32_t> invoke_winhelp(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_gui_support(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_resource(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_security(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_security_descriptor(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::vector<std::uint8_t> security_trustee_name(std::string_view);
    std::uint32_t native_file_token_access(const security::SecurityDescriptor&,std::uint32_t);
    SynchronizationDescriptor synchronization_descriptor(const GuestMemory&,std::uint64_t,bool check_length=true);
    // check_length=false：CreateDirectory 與 Windows 相同只取 lpSecurityDescriptor／bInheritHandle，不驗證 nLength。
    std::vector<std::uint8_t> file_creation_descriptor(const GuestMemory&,std::uint64_t,bool check_length=true);
    std::uint32_t process_security_location(std::uint64_t,std::uint32_t,std::vector<std::uint8_t>*&,int&,int&,bool&);
    std::uint32_t kernel_security_access(const std::vector<std::uint8_t>&,std::uint32_t,bool) const;
    std::uint32_t kernel_pseudo_access(bool) const;
    std::uint32_t created_thread_pseudo_access(const std::vector<std::uint8_t>&) const;
    std::vector<std::uint8_t> process_startup_blob(const std::vector<std::uint8_t>&,const std::vector<std::uint8_t>&,const std::vector<std::uint8_t>&);
    void refresh_file_security_context();
    std::vector<std::uint8_t> object_security_descriptor(security::SecurityDescriptor,bool check_owner,bool default_label=false);
    std::uint32_t object_security_access(const std::vector<std::uint8_t>&,std::uint32_t,const std::array<std::uint32_t,4>&);
    std::uint32_t synchronization_access(const std::vector<std::uint8_t>&,std::uint32_t,std::uint32_t);
    std::optional<std::uint32_t> invoke_object_security(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool close_wait_object(std::uint64_t);
    std::optional<std::uint32_t> invoke_token(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // 依 token handle 與所需存取權取得虛擬 token；不存在回 ERROR_INVALID_HANDLE，權限不足回 ERROR_ACCESS_DENIED。
    VirtualToken& token_handle_object(std::uint64_t handle,std::uint32_t rights);
    std::vector<std::uint8_t> machine_account_domain();
    std::optional<std::uint32_t> invoke_private_security(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_msvcp_runtime(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_d3dkmt_sync(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_d3dkmt_resource(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_gdi_misc(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_windows_metafile(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_mask_blit(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_icm(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_mscms(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_mscms_transform(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void kmt_wake_waiters(GuestState&,GuestMemory&);
    std::uint64_t msvcp_ctype_vector(GuestState&,GuestMemory&,std::uint64_t output);
    void msvcp_cvt_vector(GuestState&,GuestMemory&,std::uint64_t output);
    std::pair<std::vector<std::uint8_t>,std::uint32_t> process_creation_token(std::uint64_t,unsigned = 0);
    std::pair<std::vector<std::uint8_t>,std::uint32_t> pipe_client_token(std::uint32_t);
    void initialize_tokens();
    std::map<std::uint64_t,std::shared_ptr<SharedToken>> shared_tokens_;
    std::map<std::uint64_t,std::uint64_t> shared_token_revisions_;
    std::uint64_t mutating_shared_token_{};
    std::shared_ptr<SharedToken> share_token(std::uint64_t);
    std::uint64_t attach_shared_token(const std::shared_ptr<SharedToken>&);
    std::uint32_t local_token_operation(std::uint32_t,int,std::uint32_t,std::uint32_t&,int&);
    std::uint32_t remote_token_operation(GuestMemory&,std::uint32_t,std::uint64_t,std::uint32_t,int,std::uint32_t,std::uint32_t&,std::shared_ptr<SharedToken>&);
    std::optional<std::uint32_t> process_token_target(GuestMemory&,std::uint64_t,std::uint32_t,std::uint32_t&,std::uint64_t&);
    std::uint64_t process_identity_birth()const;

    std::string token_user_sid_string(std::uint64_t) const;
    void collect_tokens();
    std::shared_ptr<authentication::Backend> authentication_backend_;
    std::shared_ptr<dhcp::Backend> dhcp_backend_;
    std::shared_ptr<struct MprState> mpr_;
    std::optional<std::uint32_t> invoke_mpr(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_lsa(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::pair<std::uint64_t,std::uint32_t> allocate_luid();
    std::pair<std::uint64_t,std::uint32_t> logon_token(std::uint32_t,const authentication::Identity&,const std::array<std::uint8_t,8>&,std::uint64_t);
    std::uint64_t effective_token_id() const;
    std::optional<std::uint32_t> invoke_dbghelp(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_dbghelp_symbols(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void refresh_symbol_modules(SymbolSession&,const GuestMemory&);
    void discard_symbol_callback(std::uint64_t);
    std::optional<std::uint32_t> resume_symbol_callback(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_control_api(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_userstring(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_msvc(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_rtti(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void initialize_msvc_exception_types(GuestMemory&);
    void throw_msvc_exception(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,MsvcExceptionKind,std::uint64_t message=0,std::uint32_t error=0,std::uint64_t category=0,bool random_error=false);
    std::uint64_t crt_duplicate_string(GuestMemory&,std::uint64_t,bool wide=false);
    std::uint64_t create_crt_locale(GuestState&,GuestMemory&,CrtLocaleObject);
    CrtLocaleObject& current_crt_locale(GuestState&,GuestMemory&);
    void crt_exception_data(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t,bool);
    std::optional<std::uint32_t> resume_msvc(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    std::optional<std::uint32_t> start_msvc_catch(GuestState&,GuestMemory&);
    void begin_msvc_unwind(GuestState&,GuestMemory&);
    std::uint64_t msvc_exception_cells(GuestState&,GuestMemory&,bool create,bool refresh=false);
    std::uint32_t msvc_uncaught_count() const noexcept;
    void sync_msvc_uncaught(GuestMemory&) const;
    std::optional<std::uint32_t> invoke_gui_extra(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_common_dialog(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_gui_busy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_gui_busy(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    std::optional<std::uint32_t> invoke_menu(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> resume_menu(GuestState& state, GuestMemory& memory, const CallbackFrame& frame, std::uint64_t result);
    std::optional<std::uint32_t> advance_menu_idle(GuestState&,GuestMemory&,std::uint64_t token);
    void dispatch_menu_message(GuestState&,GuestMemory&,const GuiMessage&);
    std::optional<std::uint32_t> invoke_shell_desktop(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool shell_open_with_message(const GuiMessage&);
    std::array<std::int32_t,4> gui_screen_area();
    std::array<std::int32_t,4> gui_work_area(std::uint64_t excluded=0);
    // HMONITOR 為索引＋1，1 固定為主螢幕；rect／work 為 Windows 虛擬桌面座標。
    const std::vector<GuiMonitor>& gui_monitors();
    std::uint64_t gui_monitor_from_rect(const std::array<std::int32_t,4>& rect,std::uint32_t flags);
    void refresh_gui_areas();
    void notify_appbars(std::uint32_t,std::uint64_t parameter=0);
    void poll_appbars();
    bool common_dialog_message(GuestMemory& memory, const GuiMessage& message,bool from_hook=false);
    void common_dialog_refresh(std::uint64_t handle);
    std::optional<std::uint32_t> invoke_shell(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_recent_document(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_recent_document(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_recent_document(GuestState&,GuestMemory&,std::uint64_t);
    std::uint32_t store_recent_document(std::u16string,std::u16string,bool,std::u16string = {},std::u16string = {},int = 0);
    struct RecentDocumentCall {std::uint64_t object{},scratch{};unsigned stage{};bool link{};std::u16string app,path,arguments,icon;int icon_index{};};
    std::map<std::uint64_t,RecentDocumentCall> recent_document_calls_;
    std::uint32_t finish_shell_delete(GuestMemory&,const GuiShellDelete&,bool confirmed);
    std::optional<std::uint32_t> invoke_wmi(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_automation(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_type_library(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_type_library_member(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_type_library_invoke(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_type_library_invoke(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> start_type_library_address(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t,std::u16string,std::string,std::optional<std::uint16_t>);
    std::optional<std::uint32_t> resume_type_library_address(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_type_library_address(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> finish_type_library_call(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t load_type_library(GuestState&,GuestMemory&,std::u16string_view);
    std::uint64_t type_library_info(GuestMemory&,std::uint64_t,std::uint32_t,bool native=false);
    std::uint64_t reference_type_library(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    std::uint32_t release_type_library(GuestState&,GuestMemory&,std::uint64_t);
    void register_type_library(std::uint64_t,std::u16string_view path={},std::u16string_view help_directory={},unsigned hive=0);
    std::u16string registered_type_library(const std::array<std::uint8_t,16>&,std::uint16_t,std::uint16_t,std::uint32_t);
    std::optional<std::uint32_t> resume_automation(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_automation(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_ole(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_ole_clipboard(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_ole_clipboard(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::vector<GuestCallback> retire_ole_clipboard(GuestState&,GuestMemory&,std::uint64_t,bool=true);
    void discard_ole_clipboard(GuestState&,GuestMemory&,std::uint64_t);
    void release_gui_clipboard(GuestState&,GuestMemory&);
    std::uint64_t allocate_gui_global(GuestMemory&,std::uint64_t,std::uint32_t);
    std::uint64_t create_clipboard_object(GuestMemory&,GuiClipboardObject);
    void check_bstr(const GuestMemory&,std::uint64_t) const;
    void free_bstr(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t allocate_bstr(GuestMemory&,std::uint64_t,std::span<const std::uint8_t>);
    std::optional<std::uint32_t> invoke_com_error(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_error_set(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> resume_com_error_retire(GuestState&,GuestMemory&);
    void discard_com_error_set(std::uint64_t);
    std::optional<std::uint32_t> invoke_winrt_error(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_matching_restricted_error(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_restricted_error(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_restricted_error(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> resume_winrt_origin(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t,bool setup);
    void discard_winrt_origin(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t create_com_error(GuestState&,GuestMemory&,com_error::Object);
    std::uint32_t release_com_error(GuestState&,GuestMemory&,std::uint64_t);
    std::vector<GuestCallback> retire_com_error(GuestState&,GuestMemory&,std::uint64_t,bool callbacks=true);
    WinrtString winrt_string(const GuestMemory&,std::uint64_t) const;
    std::u16string winrt_content(const GuestMemory&,const WinrtString&,std::uint32_t maximum=UINT32_MAX) const;
    std::optional<std::uint32_t> invoke_winrt(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t initialize_com_apartment(std::uint32_t);
    std::u16string winrt_string_content(const GuestMemory&,std::uint64_t) const;
    std::optional<std::uint32_t> invoke_winrt_activation(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool begin_inproc_com(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> start_activation_module(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> resume_winrt_operation(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_winrt_operation(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> release_unused_winrt_modules(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> invoke_com_context(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_context(GuestState&,GuestMemory&,std::uint64_t);
    void discard_com_context(GuestState&,GuestMemory&,std::uint64_t);
    std::vector<GuestCallback> retire_com_context(GuestState&,GuestMemory&,std::uint64_t,bool callbacks = true);
    std::vector<GuestCallback> release_com_context(GuestState&,GuestMemory&,std::uint64_t,bool callbacks = true);
    std::vector<GuestCallback> release_context_properties(GuestMemory&,std::span<const com_context::Property>);
    std::vector<GuestCallback> release_context_enumerator(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> invoke_com_marshal(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_marshal(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t=0);
    std::optional<std::uint32_t> resume_com_marshal_remote(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::uint64_t com_object_generation(GuestMemory&,std::uint64_t,std::uint64_t owner=0);
    com_rpc::Endpoint com_rpc_endpoint();
    std::vector<GuestCallback> com_exporter_retirements(GuestState&,GuestMemory&);
    void queue_com_exporter_retirements(std::uint64_t);
    std::optional<std::uint32_t> resume_com_marshal_public(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_com_marshal(GuestState&,GuestMemory&,std::uint64_t);
    std::vector<GuestCallback> retire_com_marshal(GuestMemory&,std::uint64_t,bool=true);
    std::uint64_t create_com_marshaler(GuestState&,GuestMemory&,std::uint64_t,bool=false);
    std::optional<std::uint32_t> invoke_com_agile(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_agile(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t=0);
    void discard_com_agile(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t com_agile_owner(std::uint64_t,std::uint64_t)const;
    void initialize_guest_com_agile(GuestMemory&);
    std::uint64_t create_context_enumerator(GuestState&,GuestMemory&,com_context::Enumeration);
    std::uint64_t begin_context_operation(ComContextOperation);

    std::optional<std::uint32_t> invoke_property_keys(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_com_class(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool begin_registered_com(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_operation(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_com_operation(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint64_t> com_apartment(std::uint64_t) const;
    // 是否仍有 MTA：有 MTA 執行緒（可排除正在離開的一個）或有 CoIncrementMTAUsage 的使用計數。
    bool mta_alive(std::optional<std::uint64_t> except=std::nullopt) const;
    std::optional<std::uint32_t> invoke_com_call_security(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::vector<GuestCallback> revoke_com_classes(GuestMemory&,std::uint64_t,bool callbacks = true);
    std::optional<std::uint32_t> resume_ole(GuestState&, GuestMemory&, const CallbackFrame&);
    bool shell_dialog_message(GuestState& state, GuestMemory& memory, const GuiMessage& message);
    std::optional<std::uint64_t> shell_browse_control(GuestMemory& memory, const GuiMessage& message, bool wide);
    void shell_browse_refresh(std::uint64_t handle);
    std::uint64_t shell_make_pidl(GuestMemory& memory, std::u16string path);
    // base 為空：path 的絕對 PIDL（並登錄）；否則為 path 相對於資料夾 base 的子 PIDL（base 為空字串時的子項目即磁碟項目）。
    std::uint64_t shell_encode_pidl(GuestMemory& memory, std::u16string_view base, std::u16string path, bool absolute);
    std::uint64_t create_shell_folder(GuestMemory& memory,std::u16string path);
    // 本 Shell 的 PIDL（磁碟根＋各路徑段）轉回完整路徑；不是本格式或不是絕對 PIDL 時回 nullopt。
    // base 非空時把 pidl 當成相對於 base 的子項目清單解碼（SHCreateShellItem／SHCreateShellItemArray 的子 PIDL）。
    std::optional<std::u16string> shell_pidl_path(GuestMemory& memory, std::uint64_t pidl, std::u16string base = {}) const;
    // 殼層路徑的 SFGAO 屬性（IShellItem／IShellItemArray::GetAttributes 與 SHParseDisplayName 共用）；回傳 HRESULT。
    std::uint32_t shell_path_attributes(const std::u16string& path,std::uint32_t requested,std::uint32_t& masked);
    std::uint64_t create_shell_item_array(GuestMemory& memory,std::vector<std::u16string> paths);
    static std::u16string known_folder_path(const GuestMemory& memory,std::uint64_t address); // KNOWNFOLDERID → 路徑（空字串＝不支援）
    // DT_PATH_ELLIPSIS 的路徑省略（DrawText、PathCompactPath、PathCompactPathEx、PathSetDlgItemPath 共用）。
    static std::u16string compact_path_text(std::u16string_view path,std::int32_t available,const std::function<std::int32_t(std::u16string_view)>& measure);
    // 以 DC 目前字型量測的路徑省略（與 GetTextExtentPoint 相同，不在 DC 表中的 DC 使用預設字型）；回傳結果與是否放得下。
    std::pair<std::u16string,bool> compact_path_for_dc(std::uint64_t hdc,std::u16string_view path,std::int32_t width);
    std::optional<std::uint32_t> invoke_shell_idlist(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    void invalidate_window(std::uint64_t handle);
    void update_window_region(std::uint64_t handle,GuiRegion region);
    void finish_gui_erase_callback(const CallbackFrame&,std::uint64_t result);
    void erase_window(std::uint64_t handle,bool destroy_menus=true);
    void destroy_window(GuestState& state, GuestMemory& memory, std::uint64_t handle, bool default_proc);
    std::optional<std::uint32_t> invoke_gui(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_mdi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool dispatch_mdi_client(GuestState&,GuestMemory&,const GuiMessage&,bool);
    std::uint32_t prepare_mdi_window(GuestMemory&,GuiWindow&,std::uint64_t,std::array<std::uint32_t,4>);
    void register_mdi_child(std::uint64_t);
    void erase_mdi_window(std::uint64_t);
    void refresh_mdi(std::uint64_t,GuestMemory* = nullptr);
    std::vector<GuestCallback> activate_mdi(std::uint64_t,std::uint64_t,GuestMemory&);
    std::vector<GuestCallback> finish_mdi_creation(std::uint64_t,GuestMemory&);
    std::uint64_t next_mdi_child(std::uint64_t,std::uint64_t,bool) const;
    std::uint64_t gui_system_menu(std::uint64_t);
    void start_mdi_move(GuestState&,GuestMemory&,std::uint64_t,bool,std::uint32_t);
    std::optional<std::uint32_t> resume_mdi_move(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    void discard_mdi_move(GuestMemory&,const GuestState&,std::uint64_t);
    std::optional<std::uint32_t> resume_gui(GuestState& state, GuestMemory& memory, const CallbackFrame& frame, std::uint64_t result);
    std::optional<std::uint32_t> invoke_service(GuestState& state, GuestMemory& memory);
    std::optional<std::uint32_t> finish_process(GuestState& state, GuestMemory& memory, std::uint32_t code);
    RuntimeState runtime_;
    // Wow64 的 32 位元 LAA 程序可使用 4 GiB 位址；容量仍由 GuestMemory 個別控管。
    // x64 與原生 MMU 視窗（64 TiB）一致：超出視窗的位址每次存取都走受檢 helper，常駐 worker 須回協調者
    // 單步執行。Chromium PartitionAlloc 依 47 位元隨機提示保留記憶體池，上限較低時提示失敗會改選其他位址。
    std::uint64_t user_address_limit() const noexcept {
        return x86_?(runtime_.image.large_address_aware?UINT64_C(0xffff0000):UINT64_C(0x7fff0000)):
            std::min<std::uint64_t>(UINT64_C(0x7fffffff0000),GuestMemory::native_mmu_window-0x10000);
    }
    std::uint64_t crt_stream(std::uint64_t pointer) const noexcept {
        const auto base=runtime_base_.value+0x800;
        pointer=pointer==base+48?base+128:pointer==base+96?base+256:pointer;
        const auto redirected=runtime_.crt_redirects.find(pointer);
        return redirected==runtime_.crt_redirects.end()?pointer:redirected->second;
    }
    void initialize_runtime(GuestMemory& memory, std::u16string_view command_line);
    void configure_runtime_arguments(GuestMemory&,std::u16string_view,std::uint64_t);
    void configure_initial_environment(GuestMemory&,const GuestState&,std::span<const std::uint8_t>);
    void configure_native_startup(GuestMemory&,const NativeProcessParameters&,std::uint64_t);
    std::uint32_t adopt_initial_process_parameters(GuestState&,GuestMemory&);
    std::uint64_t runtime_allocate(GuestMemory& memory, std::span<const std::uint8_t> bytes, std::size_t size = 0, std::size_t alignment = 16);
    std::uint64_t allocate_heap(GuestMemory& memory, std::uint64_t size, std::uint64_t heap = process_heap);
    bool validate_heap(GuestMemory& memory,std::uint64_t heap,std::uint64_t pointer=0) const;
    bool validate_guest_heap(GuestMemory& memory,std::uint64_t heap,std::uint64_t pointer=0) const;
    std::uint64_t reallocate_heap(GuestMemory& memory, std::uint64_t address, std::uint64_t size, const GuestState& state);
    bool free_heap(GuestMemory& memory, std::uint64_t address, const GuestState& state);
    // 把宿主持有的記憶體（圖形 API 的映射緩衝等）映入客體位址空間：依配置粒度搜尋不衝突的位址，以 StableShared
    // 讓原生 MMU 直接別名。owner 隨映射保留，撤除映射（unmap）後才可能釋放；呼叫時所有 worker 須已停止。失敗回傳 0。
    std::uint64_t map_host_memory(GuestMemory& memory,std::shared_ptr<void> owner,std::span<std::uint8_t> bytes,bool checked=false);
    // 客體小物件配置區：客體 malloc／free 常式的快速路徑與此處共用狀態頁、鎖及 chunk 格式。
    bool guest_heap_setup(GuestMemory& memory);
    bool guest_locale_lock_setup(GuestMemory& memory);
    // x64 為單一連續保留區；x86 的 chunk 散在 32 位元空間各處，以每 256 KiB 一位元的位元圖判斷。
    bool guest_heap_contains(std::uint64_t address) const noexcept {
        if(guest_heap_chunked_)return address<(UINT64_C(1)<<32) && ((guest_heap_chunk_bits_[address>>24]>>((address>>18)&63U))&1U);
        return guest_heap_base_ && address-guest_heap_base_<guest_heap_next_-guest_heap_base_; // 只含已建立的連續 chunk
    }
    std::uint64_t guest_heap_reserve_chunk(GuestMemory& memory);
    std::uint64_t guest_heap_allocate(GuestMemory& memory, std::uint64_t size, std::uint64_t heap = process_heap);
    void guest_heap_free(GuestMemory& memory, std::uint64_t address);
    void guest_heap_defer_free(GuestMemory& memory,std::uint64_t address,std::uint64_t cls);
    void guest_heap_publish_deferred(GuestMemory& memory);
    std::uint64_t guest_heap_block_size(GuestMemory& memory, std::uint64_t address);
    // chunk 標頭 +24 的擁有者；0（x64 常式與 process heap）回傳 process_heap。
    std::uint64_t guest_heap_owner(GuestMemory& memory, std::uint64_t address);
    // 可成長私有 heap 累計足夠配置後才啟用 chunk，避免大量短命小 heap 各占級距 chunk。
    bool guest_heap_private_ready(std::uint64_t heap);
    void guest_heap_destroy(GuestMemory& memory, std::uint64_t heap);
    void start_callbacks(GuestState& state, GuestMemory& memory, std::vector<GuestCallback> callbacks, Continuation continuation, std::uint64_t value = 0);
    void classify_callback(GuestCallback&, Continuation) const;
    void enter_callback(GuestState& state, GuestMemory& memory);
    void discard_callbacks(std::size_t depth, GuestState& state, GuestMemory& memory);
    bool reusable_sort_callback() const noexcept;
    std::optional<std::uint32_t> resume_callback(GuestState& state, GuestMemory& memory);
    std::optional<std::uint32_t> invoke_hooks(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_theme(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> finish_theme_text(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    void discard_theme_text(std::uint64_t);
    std::optional<std::uint32_t> invoke_properties(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void release_gui_atom(std::uint16_t atom,const std::shared_ptr<SharedJob>& job={});
    std::optional<std::uint32_t> invoke_activation(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool intercept_gui_cbt(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_gui_cbt_action(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    void cancel_gui_cbt_action(std::uint64_t,GuestMemory&,const GuestState&);
    bool start_gui_hooks(GuestState&,GuestMemory&,std::int32_t type,std::int32_t code,std::uint64_t wparam,std::uint64_t lparam,Continuation completion,std::uint64_t value=0,bool wide=true);
    bool enter_gui_hook(GuestState&,GuestMemory&,GuiHookFrame,std::int32_t code,std::uint64_t wparam,std::uint64_t lparam);
    std::optional<std::uint32_t> resume_gui_hook(GuestState&,GuestMemory&,const CallbackFrame&,std::uint64_t);
    std::optional<std::uint32_t> invoke_crt_io(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    bool invoke_parallel_crt_file(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,std::uint64_t thread,std::uint64_t errno_cell);
    std::shared_ptr<std::mutex> crt_file_worker_mutex_{std::make_shared<std::mutex>()};
    FilePositionReference crt_file_position(CrtFile& file);
    CrtFile* crt_file(std::uint64_t stream);
    void initialize_crt_lowio(GuestMemory&);
    void refresh_crt_lowio(GuestMemory&);
    std::uint64_t crt_lowio_record(GuestMemory&,std::uint32_t,bool create);
    std::uint64_t crt_lowio_handle(const CrtFile&)const;
    void publish_crt_lowio(GuestMemory&,CrtFile&);
    bool import_crt_lowio(GuestMemory&,CrtFile&);
    void close_crt_lowio(GuestMemory&,std::uint32_t);
    std::optional<std::uint64_t> begin_crt_io_locks(GuestState&,GuestMemory&,std::span<const CrtIoLock>);
    void retire_crt_io_lock_scope(GuestMemory&,std::uint64_t owner,std::uint64_t operation,bool force=false)noexcept;
    void discard_crt_io_locks(GuestMemory&,std::uint64_t owner)noexcept;
    struct CrtIoScope {
        WindowsServices& services;
        GuestMemory& memory;
        std::uint64_t owner;
        std::optional<std::uint64_t> operation;
        CrtIoScope(WindowsServices&,GuestState&,GuestMemory&,std::span<const CrtIoLock>);
        CrtIoScope(WindowsServices&,GuestState&,GuestMemory&,std::uint64_t stream);
        ~CrtIoScope();
        CrtIoScope(const CrtIoScope&)=delete;
        CrtIoScope& operator=(const CrtIoScope&)=delete;
        explicit operator bool()const{return operation.has_value();}
    };
    std::uint64_t crt_descriptor_stream(std::uint32_t descriptor);
    std::uint64_t crt_install_file(GuestState&,GuestMemory&,CrtFile,std::optional<std::uint32_t> descriptor={},bool duplicate=false);
    bool crt_close_file(GuestState&,GuestMemory&,std::uint64_t stream,bool flush=true,bool keep_allocation=false);
    bool crt_attach_stream(GuestMemory&,CrtFile&);
    std::uint64_t crt_stream_position(GuestMemory&,CrtFile&,bool pushback=true);
    void crt_buffer_fields(GuestMemory&,CrtFile&);
    bool flush_crt_file(GuestMemory&,CrtFile&,bool discard_input=true);
    bool flush_crt_streams(GuestMemory&);
    std::vector<std::uint8_t> read_crt_file(GuestMemory& memory,CrtFile& file,std::size_t size,bool stream=false,std::uint64_t worker_errno=0);
    std::uint64_t write_crt_file(GuestMemory& memory, CrtFile& file, std::span<const std::uint8_t> bytes, bool stream = false);
    std::uint64_t write_crt_stream(GuestMemory& memory, std::uint64_t stream, std::span<const std::uint8_t> bytes);
    std::uint64_t write_crt_wide_stream(GuestState&,GuestMemory&, std::uint64_t stream, std::u16string_view text);
    using RuntimeHandler=std::optional<std::uint32_t>(WindowsServices::*)(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct RuntimeCatalog;
    static const RuntimeCatalog& runtime_catalog();
    static std::string_view runtime_handler_name(RuntimeHandler handler) noexcept;
    std::optional<std::uint32_t> invoke_runtime(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_crt_format(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,const WindowsCall&);
    // 純計算 CRT 邊界：0=非此類，1=記憶體操作，2=字串操作；依 stub 位址直接索引預先分類的表。
    unsigned runtime_compute_boundary(GuestAddress address) const noexcept;
    bool x86_heap_compute_boundary(GuestAddress address) const noexcept;
    std::shared_ptr<struct LdapState> ldap_;
    std::optional<std::uint32_t> invoke_ldap(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_legacy_file(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_call_named_pipe(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_resolve_delay_loaded(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_ntdll_misc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_ntdll_pure(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_kernel32_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_advapi32_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_user32_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_gdi32_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_oleaut32_conversion(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_oleaut32_dispatch(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_crt_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_crt_mbcs(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_crt_runtime_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    void initialize_crt_wctype(GuestMemory& memory);
    // CreateStdDispatch 物件的內部 IUnknown 與 IDispatch vtable（每個程序建立一次）。
    std::array<std::uint64_t,2> std_dispatch_vtables_{};
    std::optional<std::uint32_t> invoke_shlwapi_string(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_shlwapi_misc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_shell_paths(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_path_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_debug_print(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_url(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_rpc_client(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_nt_object(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_nt_continuation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_nt_family(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_rtl_family(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_ntdll_loader(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::map<std::uint64_t,std::uint64_t> thread_alert_events_; // NtAlertThreadByThreadId：執行緒索引 → 自動重設的警示事件 handle。
    std::optional<std::uint32_t> invoke_ndr_client_call(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_compatibility(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_crt_character(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,
                                                   const ResidentHostEnvironment* = nullptr);
    std::optional<std::uint32_t> invoke_sync_object(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::uint64_t current_invalid_parameter_handler() const;
    std::optional<std::uint32_t> invoke_path(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_crt_string(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_crt_scan(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_math(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_x87_crt_math(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,std::uint64_t errno_cell);
    bool fill_random(GuestMemory& memory, std::uint64_t address, std::uint64_t size);
    bool random_bytes(std::span<std::uint8_t> bytes);
    std::optional<std::uint32_t> invoke_dpapi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_credentials(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t data_protection_audit(std::u16string_view,std::span<const std::uint8_t>,std::uint32_t,bool,std::uint32_t status=0);
    std::uint32_t legacy_crypto_call(GuestMemory&,const WindowsCall&,std::string_view);
    std::uint64_t acquire_crypto_provider(std::string,std::string,std::uint32_t,std::uint32_t);
    bool retain_crypto_provider(std::uint64_t);
    bool release_crypto_provider(std::uint64_t);
    bool store_crypto_user_key(std::uint64_t,std::uint32_t,const LegacyCryptKey&);
    std::uint32_t legacy_crypto_provider(GuestMemory&,const WindowsCall&,std::string_view);
    std::uint32_t legacy_crypto_cipher(GuestMemory&,const WindowsCall&,std::string_view);
    PrivateKeyStoreContext private_key_context(std::string user_sid = {});
    std::uint32_t crypto_key_call(GuestMemory& memory, const WindowsCall& arg, std::string_view symbol);
    std::uint32_t storage_crypto_call(GuestMemory&,const WindowsCall&,std::string_view);
    std::uint64_t trust_call(GuestState&,GuestMemory&,const WindowsCall&,std::string_view);
    std::uint32_t image_certificate_call(GuestMemory&,const WindowsCall&,std::string_view);
    void rebuild_crt_environment(GuestMemory& memory, const GuestState& state);
    std::optional<std::uint32_t> invoke_environment(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    bool executable_needs_current_directory(std::u16string_view)const;
    std::uint32_t read_power_reason(GuestMemory&,std::uint64_t,std::u16string&)const;
    std::uint64_t coalesce_waitable_timer(const WaitObject&)const;
    void cancel_waitable_timer_apcs(std::uint64_t);
    struct AddressNotification {std::uint64_t overlapped{},event{};std::vector<std::uint8_t> snapshot;bool pending{true};};
    std::map<std::uint64_t,AddressNotification> address_notifications_;
    std::uint64_t next_address_notification_{0xb0000},address_poll_time_{};
    bool address_notification_waiting() const;
    void poll_address_notifications(GuestMemory&);
    std::uint32_t tcp_table_call(GuestMemory&,const WindowsCall&,std::string_view);
    std::uint32_t udp_table_call(GuestMemory&,const WindowsCall&,std::string_view);
    std::optional<std::uint32_t> invoke_network_config(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_tls(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_digest(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_network(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_socket_accept(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_socket_accept(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_socket_accept(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t publish_accepted_socket(GuestMemory&,const ConditionalAccept&);
    std::optional<std::uint32_t> invoke_memory(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_certificate(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_certificate_decode(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_certificate_decode(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::uint64_t create_certificate(GuestMemory&,const std::vector<std::uint8_t>&,std::uint64_t store=0,std::uint64_t identity=0,std::uint64_t source_store=0,std::uint64_t source_identity=0,bool crl=false);
    bool free_certificate(GuestState&,GuestMemory&,std::uint64_t,bool crl=false);
    bool free_certificate_chain(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t build_certificate_chain(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,const std::vector<std::string>&,bool,std::uint32_t,bool);
    std::optional<std::uint32_t> invoke_certificate_find(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_certificate_find(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_certificate_find(GuestState&,GuestMemory&,std::uint64_t);
    void prune_certificate_properties();
    std::uint32_t certificate_key_property(GuestMemory&,const WindowsCall&,std::string_view);
    bool acquire_certificate_key(std::uint64_t,std::uint32_t,std::uint64_t&,std::uint32_t&,bool&);
    std::optional<std::uint32_t> invoke_certificate_key(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_crypto(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_time(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_mmio(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> start_sort(GuestState& state, GuestMemory& memory, std::string_view symbol);
    std::optional<std::uint32_t> advance_sort(GuestState& state, GuestMemory& memory, std::uint64_t comparison, bool reuse_callback = false);
    struct DirectoryQueryState {
        std::vector<DirectoryEntry> entries;
        std::size_t cursor{};
        bool loaded{};
        std::optional<std::u16string> pattern;
        bool operator==(const DirectoryQueryState&) const = default;
    };
    struct FileHandle {
        std::string key;
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        std::uint64_t object{};
        std::uint32_t sharing{}, access{0x80000000}, mode{0x20};
        FileMetadata metadata;
        std::shared_ptr<HostFile> native;
        std::shared_ptr<HostFile> identity; // 唯讀快照保留 inode 身分，資料仍由不可變 bytes 提供。
        std::shared_ptr<NamedPipe> pipe;
        std::shared_ptr<NativeSerial> serial;
        std::shared_ptr<struct HidFile> hid;
        std::shared_ptr<struct UsbFile> usb;
        std::uint32_t pipe_mode{};
        std::string path;
        // 同一檔案物件的複製控制代碼共用列舉狀態，避免游標分岔與目錄快照複製。
        std::shared_ptr<DirectoryQueryState> directory{std::make_shared<DirectoryQueryState>()};
        std::uint64_t completion_port{}, completion_key{};
        std::uint8_t completion_modes{};
        bool io_signaled{true};
        bool manage_volume_at_open{};
    };
    struct BackupStream {std::uint32_t type{},attributes{};std::uint64_t size{};std::vector<std::uint8_t> header,data;};
    struct FileBackup {std::uint64_t object{},position{};std::size_t stream{};bool writing{},security{},header_complete{};std::vector<BackupStream> streams;std::vector<std::uint8_t> header,data;std::uint32_t type{},attributes{},name_size{};std::uint64_t size{};};
    std::map<std::uint64_t,FileBackup> file_backups_;
    std::optional<std::uint32_t> invoke_file_backup(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    using ResourceUpdateName=std::variant<std::u16string,std::uint16_t>;
    struct ResourceUpdateValue {std::uint32_t codepage{};std::vector<std::uint8_t> bytes;};
    struct ResourceUpdate {std::string path;std::shared_ptr<HostFile> file;std::vector<std::uint8_t> original;std::map<std::tuple<ResourceUpdateName,ResourceUpdateName,std::uint16_t>,ResourceUpdateValue> resources;};
    std::map<std::uint64_t,ResourceUpdate> resource_updates_;
    std::optional<std::uint32_t> invoke_resource_update(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::string stdout_, stderr_;
    std::array<int,3> live_stdio_{-1,-1,-1};
    std::array<std::shared_ptr<int>,3> inherited_stdio_{};
    void import_standard_handle(const InheritedHandle&);
    std::uint32_t standard_file_type(unsigned) const;
    FileInformation file_handle_information(std::uint64_t);
    std::array<std::shared_ptr<NativeConsole>,3> consoles_{};
    // CreateConsoleScreenBuffer 的螢幕緩衝區（windows_console_screen.cpp）：額外 handle（0x104 起，間隔 4）對應共享的螢幕內容。
    // 作用中的螢幕內容一律放在主控台共用緩衝區，其餘保存在狀態物件；操作非作用中的螢幕時，服務入口暫時換入其內容
    // 並以不顯示的視窗取代原生視窗，處理完再換回。console_screen_count_ 為 0 時入口完全不檢查。
    struct ConsoleScreen;struct ConsoleScreenState;
    // 色彩管理共用儲存與 mscms 已開啟設定檔（windows_icm_store.hpp）。
    struct IcmStore;struct IcmProfiles;std::shared_ptr<IcmProfiles> icm_profiles_;IcmProfiles& icm_profiles();
    std::shared_ptr<ConsoleScreenState> console_screen_state_;
    std::size_t console_screen_count_{};
    ConsoleScreenState& console_screens();
    bool console_screen_handle(std::uint64_t) const;
    bool console_screen_swapped() const; // 正在操作換入的非作用中螢幕：不可觸及原生終端設定
    ConsoleBuffer* console_output_buffer();
    unsigned console_output_stream() const;
    bool console_screen_enter(GuestState&,GuestMemory&);
    void console_screen_leave();
    std::uint32_t activate_console_screen(std::shared_ptr<ConsoleScreen> target);
    void close_console_screen(std::uint64_t handle);
    std::array<int,3> parent_stdio_{-1,-1,-1};
    bool console_handle(std::uint64_t) const;
    std::optional<std::uint32_t> invoke_console(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_console_control(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void publish_console_identity(bool attached,bool fresh=false);
    std::uint32_t generate_console_control(GuestMemory&,std::uint32_t,std::uint32_t);
    struct ConsoleMember {std::uint32_t pid,group;std::uint64_t birth;};
    std::uint32_t console_members(std::vector<ConsoleMember>&);
    // 主控台別名與命令歷程使用的執行檔名稱：SetConsoleInputExeName 設定值，否則為主程式檔名。
    std::u16string console_input_exe() const;
    std::u16string console_input_exe_name_;
    std::optional<std::uint32_t> invoke_console_screen(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t console_terminal_write(unsigned stream,std::string_view text);
    std::vector<std::uint64_t> gui_broadcast_targets() const;
    std::optional<std::uint32_t> invoke_user32_legacy(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t receive_console_control(GuestMemory&,std::uint32_t,std::uint64_t,std::uint64_t,std::uint32_t,std::uint32_t);
    std::uint32_t queue_console_control(GuestMemory&,std::uint32_t);
    bool console_input_ready(std::uint64_t,unsigned=0);
    bool console_text_ready(bool,int=0);
    std::uint32_t console_sync(unsigned,std::size_t=0,std::size_t=0);
    std::uint32_t write_crt_console(std::u16string_view,bool);
    bool live_input_eof_{};
    bool standard_input_ready(std::uint64_t count, bool text, bool line, int timeout = 0);
    bool await_standard_input(GuestState& state, GuestMemory& memory, std::uint64_t count, bool text = false, bool line = false);
    FileIoResult write_standard_output(unsigned stream, std::span<const std::uint8_t> bytes,bool retained=false,bool unicode=false);
    std::uint32_t last_error_{};
    std::string trace_services_;
    // 診斷：X64ARM_SERVICE_COUNTS=1 時統計每個服務入口的呼叫次數與累計宿主時間（協調者／managed 與 worker 分列），結束時輸出。
    // refused：worker 端拒絕、交回協調者（診斷 X64ARM_SERVICE_COUNTS 開啟時才累計）。
    struct ServiceCount {std::uint64_t calls{},ns{},cpu_ns{},mapping_wait_ns{},refused{};};
    bool service_counts_enabled_{};unsigned worker_service_sample_bits_{};std::string service_counts_directory_;
    std::unordered_map<std::uint64_t,ServiceCount> service_counts_,worker_service_counts_;
    std::shared_ptr<std::mutex> worker_service_counts_mutex_{std::make_shared<std::mutex>()};
    std::uint64_t service_count_events_{};std::chrono::steady_clock::time_point service_count_dumped_{};
    // 值為目錄時每 5 秒改寫 <目錄>/service-counts-<pid>.txt（程序被終止也留得下）；否則結束時輸出到 stderr。
    void dump_service_counts();
    // 客體端小型 CRT 記憶體常式頁；未映射（x86、停用或追蹤）時匯入仍指向宿主服務入口。
    GuestAddress guest_memory_routines_{};
    // Win64 純量常式頁；追蹤時 resolver 沿用原服務，特殊數值從常式尾跳原服務。
    GuestAddress guest_scalar_math_routines_{};
    // 具有 Windows 函式表的共用 CRT 客體模組；供正常呼叫、SEH 與 JIT 函式範圍查詢。
    RuntimeImage guest_crt_algorithms_;
    void initialize_guest_crt_algorithms(GuestMemory&, GuestAddress runtime_stubs);
    void initialize_guest_msvcp90(GuestMemory&);
    void sync_crt_thread_state(GuestMemory&, std::uint64_t id);
    void sync_crt_thread_states(GuestMemory&);
    GuestAddress guest_heap_state_{};std::uint64_t guest_heap_base_{},guest_heap_size_{},guest_heap_next_{};
    // 協調者專用的待發布空閒串列；客體持鎖且已停駐時不能在宿主等待它解鎖。
    // 每級距保留首尾，下一次成功取得客體鎖便接回全域串列；節點仍存於已釋放區塊內。
    std::vector<std::pair<std::uint64_t,std::uint64_t>> guest_heap_deferred_;
    bool guest_heap_deferred_pending_{};
    bool guest_heap_chunked_{};std::array<std::uint64_t,256> guest_heap_chunk_bits_{};
    // x86 可成長私有 heap 的級距串列頭；協調者在一致邊界擴充，worker 以客體狀態鎖同步。
    // heads 為空表示尚未啟用 chunk，requests 累計一般路徑配置次數。
    struct GuestPrivateHeap {std::vector<std::uint64_t> heads;std::uint64_t requests{};};
    std::map<std::uint64_t,GuestPrivateHeap> guest_private_heaps_;
    // 頭表的生命期／HeapLock 與 chunk 登錄各有世代；配置／釋放只改 mutations，不重建描述元。
    std::uint64_t guest_heap_topology_generation_{1},guest_heap_chunk_generation_{1};
#ifdef X64ARM_HOST_SERVICES_RESIDENT_HEAP
    struct ResidentHeapCache {
        ResidentHeap descriptor;
        std::unique_ptr<std::array<std::uint8_t*,resident_heap_chunk_count>> chunks;
        std::vector<ResidentHeapEntry> entries;
        std::uint64_t memory_identity{},memory_generation{},topology_generation{},chunk_generation{},borrow_generation{};
        ResidentHeapCache()=default;
        // WindowsServices 的快照可複製；借用不可跟著複製到另一份 private heads／GuestMemory。
        ResidentHeapCache(const ResidentHeapCache&) noexcept {}
        ResidentHeapCache& operator=(const ResidentHeapCache&) noexcept {
            descriptor={};chunks.reset();entries.clear();memory_identity=memory_generation=topology_generation=chunk_generation=borrow_generation=0;
            return *this;
        }
    } resident_heap_cache_;
#endif
    // HeapWalk 用：x86 配置區已配置區塊（位址、區塊大小）依位址排序的快取；配置／釋放即使快取失效。
    std::uint64_t guest_heap_mutations_{},guest_heap_walk_generation_{UINT64_MAX},guest_heap_walk_heap_{};
    std::vector<std::pair<std::uint64_t,std::uint64_t>> guest_heap_walk_blocks_,guest_heap_walk_free_;
    const std::vector<std::pair<std::uint64_t,std::uint64_t>>& guest_heap_allocated_blocks(GuestMemory& memory, std::uint64_t heap = process_heap);
    void guest_heap_flush_thread_cache(GuestMemory& memory, std::uint64_t teb);
    GuestAddress guest_locale_lock_state_{};
    // Win32／Win64 TEB、TLS 與 pseudo handle 共用常式；只快取入口，不快取客體資料。
    GuestAddress guest_thread_routines_{};
    // 程序共用配置位元圖；新執行緒沿用同一頁，Alloc／Free 只在協調生命週期邊界修改。
    GuestAddress guest_tls_bitmap_{};
    void set_tls_allocation(GuestMemory&,std::size_t index,bool used);
    // 客體 FlsGetValue 讀取的 TEB 鏡像（TEB+0x1840：128 個值與使用中位元圖；FlsData 指向它）。
    bool fls_mirror_{};
    void sync_fls_mirror(GuestMemory&,std::uint64_t teb,const std::array<std::uint64_t,128>& values) noexcept;
    void sync_fls_mirrors(GuestMemory&) noexcept;
    void sync_current_fls_mirror(GuestMemory&) noexcept;
    // Win32／Win64 IOCP 的資料寫回與 ABI 返回交給客體 JIT；佇列與喚醒仍由協調執行緒管理。
    GuestAddress guest_io_completion_{};
    struct {std::uint64_t packets{},timeouts{},immediate{},waits{};} io_wait_stats_;
    // x64 CFG 客體快速路徑：檢查／dispatch 常式頁與已驗證目標表。未命中仍走宿主完整檢查；
    // 可執行映射撤銷或權限變更（code_generation 變動）後於服務邊界整表清空。x86 或追蹤時不映射。
    GuestAddress guard_fast_code_{},guard_fast_table_{};
    std::uint64_t guard_table_generation_{};
    void remember_guard_target(GuestMemory&,std::uint64_t) noexcept;
    void refresh_guard_table(GuestMemory&) noexcept;
    std::optional<std::uint32_t> invoke_boundary(GuestState&,GuestMemory&,const ResidentHostEnvironment* = nullptr);
    ReadOnlyFiles filesystem_;
    std::shared_ptr<rot::SharedRot> com_running_table_;
    std::shared_ptr<ComRpcState> com_rpc_;
    std::shared_ptr<UrlCacheState> url_cache_;
    std::map<std::uint64_t, FileHandle> files_;
    struct FileTransfer {
        std::string source,destination;std::shared_ptr<HostFile> input,output;FileMetadata metadata;
        std::uint64_t position{},callback{},parameter{},cancel{},source_handle{},target_handle{},owner{},message{},chunk{},chunk_size{},resume_key{};
        std::uint32_t flags{},stage{},message_type{},finish_error{};
        bool pending{},started{},move{},flush{},copy2{},finishing{};
    };
    std::map<std::uint64_t,FileTransfer> file_transfers_;
    std::optional<std::uint32_t> start_file_transfer(GuestState&,GuestMemory&,std::string,std::string,std::uint32_t,std::uint64_t,std::uint64_t,std::uint64_t,bool=false,bool=false,bool=false);
    std::optional<std::uint32_t> advance_file_transfer(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_file_transfers(GuestState&,GuestMemory&,std::uint64_t);
    std::map<std::uint64_t, std::size_t> allocations_;
    std::map<std::uint64_t, std::size_t> free_heap_blocks_;
    // 位址索引負責合併，大小／位址索引讓配置略過所有放不下的空洞。
    std::set<std::pair<std::size_t,std::uint64_t>> free_heap_sizes_;
    // 小物件的快速重用清單（類似 Windows 低碎片堆積的 lookaside）：process heap 與可成長私有 heap 中已釋放、
    // 依 16 bytes 級距分類的區塊保留原位址與頁面參照，下次同一 heap、同級距的配置直接取用並清零。
    // 區塊不在 allocations_ 或空閒結構中，由 heap_quick_blocks_ 讓位址搜尋視為佔用；私有 heap 的擁有者與用量
    // 在保留時比照釋放扣除、重用時加回。走訪、整理、銷毀或一般配置失敗前先歸還一般空閒結構。
    // 只有物件觸及的每一頁都仍有其他存活物件時才保留；頁面最後一個存活物件釋放時，該頁的快速區塊
    // 一併歸還，因此沒有存活物件的頁面與一般釋放一樣立即解除映射，已提交量不變。X64ARM_HEAP_QUICK=0 停用。
    // 比照 LFH 的啟用方式：同一 heap、同一級距累計釋放 heap_quick_activation 次後才開始保留，少量配置的版面不變。
    static constexpr std::size_t heap_quick_classes=64,heap_quick_depth=256,heap_quick_activation=16;
    struct HeapQuickBins {
        std::array<std::vector<std::uint64_t>,heap_quick_classes> lists{};
        std::array<std::uint32_t,heap_quick_classes> activity{};
        bool operator==(const HeapQuickBins&)const=default;
    };
    struct HeapQuickBlock {std::size_t size{};std::uint64_t heap{};bool operator==(const HeapQuickBlock&)const=default;};
    std::map<std::uint64_t,HeapQuickBins> heap_quick_bins_;      // heap → 各級距清單與啟用計數
    std::map<std::uint64_t,HeapQuickBlock> heap_quick_blocks_;   // 位址 → 尺寸與所屬 heap
    std::map<std::uint64_t,unsigned> heap_quick_pages_; // 頁 → 該頁上的快速區塊數（包含在 heap_pages 參照內）
    bool retain_heap_quick(std::map<std::uint64_t,std::size_t>::iterator allocation);
    bool release_heap_block(GuestMemory& memory,std::map<std::uint64_t,std::size_t>::iterator allocation);
    void evict_heap_quick_block(GuestMemory& memory,std::uint64_t address);
    void evict_heap_quick_page(GuestMemory& memory,std::uint64_t page);
    void flush_heap_quick(GuestMemory& memory);
    void flush_heap_quick(GuestMemory& memory,std::uint64_t heap);
    // 一般 heap 由低於 8 TiB 的區域開始配置，保留舊 64 位元 runtime
    // 對 43-bit 指標的相容性；明確指定的 VirtualAlloc 位址仍可使用完整客體空間。
    std::uint64_t next_handle_{0x1000}, next_allocation_{0x2000000000ULL}; // x64 堆積高於 4 GiB 但位於 MMU 視窗內。
    std::array<bool, 3> standard_open_{true, true, true};
    // 同程序 DuplicateHandle 對標準串流取得的額外參照；CloseHandle 先抵銷複本，最後一個才關閉串流。
    std::array<std::uint32_t, 3> standard_duplicates_{};
    // std::notify_all_at_thread_exit／promise::*_at_thread_exit 的登記：{執行緒, _Cnd_t, _Mtx_t, int* 結果}。
    std::vector<std::array<std::uint64_t,4>> msvcp_thread_exit_notifications_;
    // CPU Sets（ID 與 GetSystemCpuSetInformation 相同為 0x100＋CPU）：程序預設與各執行緒選取的集合。
    // Windows 視為排程偏好；本引擎保存並回報，不改寫執行緒的硬親和性。
    std::vector<std::uint32_t> process_cpu_sets_;std::map<std::uint64_t,std::vector<std::uint32_t>> thread_cpu_sets_;
    bool detached_console_{};
    std::uint64_t console_identity_{};
    std::uint32_t console_group_{};
    bool initialized_{};
    // 最後宣告：解構時最先等提交執行緒結束，它仍會使用上方的 D3D12 狀態與 GPU 通知佇列。
    std::shared_ptr<struct D3D12Submitter> d3d12_submitter_;
    // 同樣最後宣告：解構時先停止 D3D11 命令串流執行緒，它仍會使用 d3d11_ 與客體記憶體。
    std::shared_ptr<struct D3D11CommandStream> d3d11_stream_;
};
}
