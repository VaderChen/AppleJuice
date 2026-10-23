#pragma once
#include "x64arm/host_services.hpp"
#include <atomic>
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
struct BluetoothState;
class MachineShutdown;
struct AccessibleState;
struct D3D9State;
struct D3D11State;
struct DirectSoundState;
struct WasapiState;
struct MediaFoundationState;
struct MediaSurface;
struct DirectInputState;
struct DirectMusicState;
class NativePowerMonitor;
struct FileMappingState;
struct ThreadpoolState;
struct HandleTransferState;
struct ProcessState;
struct PerflibState;
struct PdhState;
struct TransactionState;
struct KtmTransaction;
struct DdeState;
struct MciState;
struct JobState;
struct PrintingState;
struct MetafileState;
struct OleStreamState;
struct ComRpcState;
struct StorageState;
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
struct PmrState;
struct WebView2State;
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
struct WinmmBeepTone;
struct AcmState;
struct WinmmWaveInput;
struct PeImport;
struct PeOptions;
struct PeProgram;
struct UrlCacheState;
struct DebugMemoryState;
namespace debugger {struct TransportEvent;struct Frame;}
class WindowsServices final : public HostServices {
    friend struct WindowsNetworkTestAccess;
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
    bool invoke_parallel_service_impl(GuestState&,GuestMemory&,bool&,std::span<GuestState* const> = {});
#endif
    ExecutionPolicy execution_policy(const GuestState&) const override;
    void after_parallel_instructions(GuestState&,GuestMemory&,std::span<const std::uint64_t>) override;
    std::uint64_t timestamp_counter(const GuestState&,GuestMemory&) override;
#ifdef X64ARM_HOST_SERVICES_RESIDENT_TIMESTAMP
    bool resident_timestamp_counter(const GuestState&,const GuestMemory&,std::uint64_t&) const noexcept override;
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
#ifdef X64ARM_HOST_SERVICES_RESIDENT_SERVICE_REQUEST
    void resident_service_request(std::uint64_t,GuestAddress) override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTROL_REQUEST
    void resident_control_request(std::uint64_t,GuestAddress,bool) override;
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTROL_DRAIN
    bool resident_select_control_request(GuestState&,GuestMemory&) override;
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
    static constexpr std::size_t output_limit = 16U * 1024U * 1024U;
    static constexpr GuestAddress command_line_address{0x3001000000ULL}; // x64 系統區塊位於 MMU 視窗內（見 GuestMemory::native_mmu_window）。
    static constexpr std::uint64_t process_heap = 0x200;
    void initialize(GuestMemory& memory, std::u16string_view command_line, const std::filesystem::path& filesystem_root = {}, const std::filesystem::path& c_drive = {}, bool writable_files = false, const std::filesystem::path& install_root = {}, const std::map<char,std::filesystem::path>& drives = {}, std::string_view certificate_roots_file = {}, bool resident_translation = false);
    bool uses_guard_fast_path() const noexcept {return guard_fast_code_.value!=0;}
    std::u16string map_executable(const std::filesystem::path& path);
    void configure_image(GuestState& state, GuestMemory& memory, RuntimeImage image);
    void configure_loader(const std::filesystem::path& source, bool trap_unresolved);
    GuestAddress bind_image_import(GuestMemory&, const PeImport&, std::uint64_t owner);
    void prepare_image_startup(GuestState&);
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
    void set_process_directory(std::u16string directory);
    std::string video_diagnostics() const;
    std::string opengl_diagnostics() const;
    std::string dsound_diagnostics() const;
    std::string d3d9_diagnostics() const;
    std::string module_diagnostics() const;
private:
    std::size_t resident_execution_depth_{};
    // 僅讓主 EXE loader 交接其剛完成驗證並映射的映像；公開 configure_image 仍先驗證。
    void configure_image_after_unwind_validation(GuestState&,GuestMemory&,RuntimeImage);
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
    std::optional<std::uint32_t> invoke_pdh(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    struct PrivilegeAcquisition {
        bool process{},in_place{};std::uint64_t token{},thread{},previous_thread_token{},keeper{};
        std::vector<std::pair<std::uint64_t,std::uint32_t>> previous;
        bool operator==(const PrivilegeAcquisition&)const=default;
    };
    std::map<std::uint64_t,PrivilegeAcquisition> privilege_acquisitions_;
    std::vector<std::pair<std::u16string,std::uint64_t>> wer_runtime_modules_;
    std::map<std::uint32_t,std::uint32_t> gpu_scheduling_classes_;
    std::uint64_t open_local_thread(std::uint64_t thread,std::uint32_t rights,bool inherit);
    std::optional<std::uint32_t> invoke_system_objects(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // DDEML、使用者工作階段（視窗更新鎖、鍵盤配置、觸控注入）與 IInitializeSpy。
    std::shared_ptr<DdeState> dde_;
    DdeState& dde_state();
    std::optional<std::uint32_t> invoke_dde(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_dde(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    void discard_dde(std::uint64_t);
    std::optional<std::uint32_t> invoke_user_session(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool window_update_locked(std::uint64_t) const;
    std::uint64_t selected_keyboard_layout(std::uint64_t thread) const;
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
    std::optional<std::uint32_t> process_thread_target(GuestMemory&,std::uint64_t,std::uint32_t,std::uint32_t&,std::uint64_t* = nullptr);
    std::uint32_t process_memory_target(GuestMemory&,std::uint64_t,std::uint32_t,std::uint32_t&,std::uint64_t&);
    std::uint32_t process_basic_information(GuestMemory&,std::uint64_t,std::array<std::uint64_t,5>&,std::uint32_t&,std::uint32_t&);
    void publish_process_information();
    void publish_thread_lifetime(std::uint64_t,std::uint32_t);
    void configure_thread_record(std::uint64_t);
    std::uint32_t memory_status_fields(GuestMemory&,std::array<std::uint64_t,6>&) const;
    std::uint32_t refresh_memory_resource_notifications(bool force=false);
    std::uint64_t memory_resource_poll_deadline_{UINT64_MAX};
    std::uint32_t process_parent_id() const;
    std::uint32_t capture_process_snapshot(GuestMemory&,std::vector<ProcessSnapshot::Process>&);
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
    std::uint32_t process_memory_copy(GuestMemory&,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,bool,std::uint64_t&);
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
    std::uint64_t next_native_operation_{1};
    std::uint32_t native_section_access(std::uint64_t,std::uint32_t);
    std::optional<std::uint32_t> invoke_nt_section(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> finish_system_call(GuestState&,GuestMemory&);
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
    void publish_remote_frame(std::uint64_t,NativeGraphics&,const GraphicsView&);
    void poll_remote_frames();
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
    using FileOpenArguments = std::array<std::uint64_t,7>;
    std::optional<std::uint32_t> invoke_file_open(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_overlapped_result(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
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
    void set_file_io_signaled(std::uint64_t,bool);
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
    struct PendingFileIo {
        enum class Kind { Connect, Read, Write, Transact, Flush, Lock, Directory, CommEvent } kind{};
        std::shared_ptr<SerialTransfer> serial;
        std::shared_ptr<AsyncFileFlush> flush;
        std::shared_ptr<NamedPipe::Transaction> transaction;
        NamedPipe::WriteCompletion pipe_write;
        std::uint64_t handle{},thread{},overlapped{},event{},input{},output{},count_pointer{},routine{};
        std::uint64_t native_iosb{},native_context{},native_scratch{};
        std::uint32_t input_size{},output_size{};
        std::uint64_t offset{},length{},input_transferred{},transferred{};
        bool attempted{},io_recorded{};
        std::uint64_t io_accounted{};
        PipeResult result;
        bool done{},written{},notify{},blocking{},exclusive{},immediate{};
        bool hid{};
        std::uint64_t pipe_wake_epoch{},pipe_poll_deadline{};
    };
    PipeResult poll_hid_io(PendingFileIo&,GuestMemory&);
    std::vector<PendingFileIo> pending_file_io_;
    PipeResult read_directory_notification(const PendingFileIo&,GuestMemory&);
    bool close_process(std::uint64_t);
    bool create_guest_process(GuestMemory&,const std::array<std::uint64_t,10>&,bool,bool = false,std::uint64_t = 0,unsigned = 0);
    std::uint32_t query_association(std::uint32_t,std::uint32_t,std::u16string,const std::u16string&,std::u16string&);
    std::optional<std::uint32_t> invoke_shell_execute(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_shell_item(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_shell_item(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_shell_item(GuestState&,GuestMemory&,std::uint64_t);
    std::uint64_t create_shell_item(GuestMemory&,std::u16string);
    std::uint64_t create_shell_enumerator(GuestMemory&,std::vector<std::u16string>,std::size_t = 0);
    std::uint32_t open_shell_file_stream(GuestMemory&,std::u16string,std::uint32_t,std::uint32_t,bool,std::uint64_t&);
    bool current_process_handle(std::uint64_t,std::uint32_t = 0) const;
    std::u16string_view process_object_type(std::uint64_t) const;
    std::u16string_view object_type(std::uint64_t) const;
    struct ObjectMetadata {std::uint32_t access{},handles{},pointers{};std::uint64_t identity{};};
    std::optional<ObjectMetadata> object_metadata(std::uint64_t) const;
    std::optional<ObjectMetadata> mapping_object_metadata(std::uint64_t) const;
    std::optional<ObjectMetadata> job_object_metadata(std::uint64_t) const;
    std::optional<ObjectMetadata> process_object_metadata(std::uint64_t) const;
    std::optional<bool> process_signaled(std::uint64_t, bool input_idle = false);
    bool processes_waiting();
    const RuntimeImage* module_image(std::uint64_t address, bool containing = false) const;
    const RuntimeImage* unwind_image(GuestMemory&,std::uint64_t pc,std::uint64_t function_entry = 0);
    std::optional<std::uint32_t> invoke_function_table(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_eh4(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_eh4(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_crt_error(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t find_module(std::u16string_view name) const;
    GuestAddress resolve_service(std::string_view,std::string_view,bool required) const;
    std::map<std::string,std::uint64_t> builtin_export_targets(std::string_view) const;
    std::map<std::uint64_t,std::uint32_t> builtin_system_calls() const;
    std::uint64_t load_module(GuestMemory&, std::u16string name, std::uint32_t flags, std::vector<std::uint64_t>& created, std::u16string_view search_directory = {});
    std::uint64_t module_export(GuestMemory&, std::uint64_t module, std::string_view symbol, std::optional<std::uint16_t> ordinal, std::vector<std::uint64_t>& created, unsigned depth = 0);
    void unmap_module(GuestMemory&,std::uint64_t module);
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
    std::shared_ptr<PmrState> msvcp_pmr_;
    std::shared_ptr<WebView2State> webview2_;
    std::shared_ptr<D3D12State> d3d12_;
    std::shared_ptr<DXCoreState> dxcore_;
    std::optional<std::uint32_t> invoke_dxcore(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_d3d12(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool d3d12_queue(std::uint64_t) const;
    // DirectML 等建立在 D3D12 上的 API：錄製於命令清單、於 ExecuteCommandLists 重播時以裝置的共用後端執行。
    bool d3d12_record(std::uint64_t list,D3D12Replay replay);
    std::uint64_t d3d12_device_of(std::uint64_t object) const;
    bool d3d12_resource_width(std::uint64_t resource,std::uint64_t& width) const;
    std::optional<std::uint32_t> invoke_webview2(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_opencl(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_vulkan(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_directml(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_msvcp_pmr(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
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
    void finish_metafile_enumeration(std::uint64_t);
    std::shared_ptr<PrintingState> printing_;
    std::optional<std::uint32_t> invoke_printing(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_spooler(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void printing_changed(unsigned);
    std::optional<std::uint32_t> invoke_print_dialog(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_print_dialog(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<std::uint32_t> invoke_property_page(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_property_page(GuestState&,GuestMemory&,std::uint64_t,std::uint64_t);
    std::optional<GuiPropertyPage> read_property_page(GuestMemory&,std::uint64_t,bool,std::uint32_t&);
    std::uint32_t commit_print_dialog(GuestState&,GuestMemory&,GuiPrintDialog&);
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
    std::uint64_t wave_callback_registration_{};
    void poll_wave_output(GuestMemory&);
    bool wave_output_waiting() const;
    std::optional<std::uint32_t> invoke_wave_output(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void poll_wave_input(GuestMemory&);
    bool wave_input_waiting() const;
    std::optional<std::uint32_t> invoke_wave_input(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::map<std::uint64_t,std::shared_ptr<WinmmMidiOutput>> midi_outputs_;
    std::map<std::uint64_t,std::shared_ptr<WinmmMidiInput>> midi_inputs_;
    void poll_midi_input(GuestMemory&);
    bool midi_input_waiting() const;
    std::optional<std::uint32_t> invoke_midi_input(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_winmm_midi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_directmusic(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    void poll_dsound();
    bool dsound_waiting() const;
    void poll_directmusic();
    bool directmusic_waiting() const;
    std::shared_ptr<DirectSoundState> dsound_;
    std::shared_ptr<WasapiState> wasapi_;
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
    void poll_wasapi();
    bool wasapi_waiting() const;
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
    std::map<std::uint64_t,GuestAddress> resident_service_requests_;
    std::uint64_t thread_id(std::uint64_t handle) const;
    std::uint32_t thread_identifier(std::uint64_t) const;
    std::uint64_t thread_from_identifier(std::uint32_t) const;
    std::uint32_t allocate_thread_identifier(std::uint64_t);
    std::uint32_t remote_thread_identifier(std::uint32_t,std::uint64_t,std::uint32_t);
    std::uint32_t thread_access(std::uint64_t handle) const;
    bool dispatch_user_apc(GuestState&,GuestMemory&,std::uint64_t completion);
    struct CompletionResult {std::uint32_t value{},error{};bool ready{},guest_return{};};
    CompletionResult poll_completion(GuestState&,GuestMemory&,std::uint64_t handle,bool extended,bool expired);
    void wake_threads(GuestMemory& memory);
    enum class ThreadWakeScope { All, Condition, Slim, Critical, Objects };
    void resolve_thread_waits(GuestMemory&,ThreadWakeScope scope=ThreadWakeScope::All,std::uint64_t address=0);
    void schedule(GuestState& state, GuestMemory& memory);
    std::optional<std::uint32_t> force_finish_thread(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> finish_thread(GuestState& state, GuestMemory& memory, std::uint32_t code);
    std::optional<std::uint32_t> exit_thread(GuestState& state, GuestMemory& memory, std::uint32_t code);
    std::optional<std::uint32_t> invoke_error(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_registry(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_registry_hive(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint32_t flush_registry_hives();
    std::uint32_t commit_registry_transaction(RegistryTransaction&);
    std::map<std::u16string,std::shared_ptr<HostFile>> registry_hive_files_;
    std::optional<std::uint32_t> invoke_registry_security(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    const std::vector<std::uint8_t>& registry_security_descriptor(const std::u16string&);
    std::vector<std::uint8_t> registry_creation_descriptor(const std::vector<std::uint8_t>&,security::SecurityDescriptor);
    std::uint32_t registry_security_access(const std::u16string&,std::uint32_t);
    std::pair<std::u16string,std::uint32_t> registry_security_handle(std::uint64_t,std::uint32_t);
    std::optional<std::uint32_t> invoke_nt_registry(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
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
        std::uint64_t key{},event{},thread{};
        std::u16string path;std::uint32_t filter{};bool subtree{};
        std::map<std::u16string,std::map<std::u16string,RegistryValue>> snapshot;
        std::map<std::u16string,std::vector<std::uint8_t>> security_snapshot;
        bool operator==(const RegistryNotification&) const = default;
    };
    std::map<std::uint64_t,RegistryNotification> registry_notifications_;
    std::uint64_t next_registry_notification_{1},registry_poll_time_{};
    void poll_registry_notifications(bool reload=true,std::u16string_view mutation={},std::uint32_t filter=0);
    struct RegisteredWait {
        std::uint64_t target{},callback{},parameter{},worker{},deadline{},completion_event{},timer_owner{},pool_instance{},power_owner{},etw_operation{},device_owner{},perflib_operation{},bluetooth_delivery{};
        std::vector<GuestCallback> callback_batch;
        std::vector<std::uint64_t> callback_storage;
        std::uint32_t milliseconds{},flags{};bool armed{true},ready{},active{},closing{},timed_out{},work_item{},pool_cleanup{};
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
    void poll_timer_queues(GuestMemory&);
    void collect_timer_queues();
    std::optional<std::uint32_t> invoke_timer_queue(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t create_guest_thread(const GuestState&,GuestMemory&,std::uint64_t,std::uint64_t,std::uint64_t,std::uint32_t);
    void initialize_stack_guard(GuestMemory&,std::uint64_t,std::uint64_t,ThreadStackGuard&);
    std::uint32_t configure_stack_guarantee(GuestState&,GuestMemory&,std::uint32_t,bool,std::uint32_t&);
    bool release_stack_guard(GuestState&,GuestMemory&,std::uint64_t);
    void poll_registered_waits(GuestMemory&);
    void release_registered_wait(std::uint64_t);
    std::optional<std::uint32_t> resume_registered_wait(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> invoke_registered_wait(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_thread(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    void begin_exception(GuestState& state, GuestMemory& memory, std::uint32_t code, std::uint32_t flags, std::span<const std::uint64_t> parameters);
    bool callback_protects_exception(const CallbackFrame&,std::uint32_t)const;
    std::optional<std::uint32_t> finish_protected_callback_exception(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> advance_exception(GuestState& state, GuestMemory& memory);
    std::optional<std::uint32_t> finish_user_callback_exception(GuestState&,GuestMemory&);
    std::optional<std::uint32_t> resume_exception(GuestState& state, GuestMemory& memory, std::uint64_t result);
    void prepare_handler(GuestMemory& memory, ExceptionDispatch& dispatch, const ExceptionWalkFrame& frame, bool target);
    bool append_exception_frame(GuestMemory& memory, ExceptionDispatch& dispatch);
    std::optional<std::uint32_t> advance_x86_exception(GuestState& state, GuestMemory& memory);
    void begin_x86_unwind(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    void leave_x86_exception_handler(GuestMemory& memory, ExceptionDispatch& dispatch);
    void initialize_user_objects();
    std::uint64_t thread_desktop_object(std::uint64_t thread) const;
    bool interactive_station() const;
    void present_native_window(std::uint64_t handle,const GuiWindow& window);
    void show_native_window(std::uint64_t handle,bool visible);
    std::optional<std::uint32_t> invoke_winstation(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_heap(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool await_heap_service(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void prune_x86_exception_callbacks(GuestState& state, GuestMemory& memory);
    void begin_unwind(GuestState& state, GuestMemory& memory, std::size_t target, std::uint64_t ip, std::uint64_t result);
    std::optional<std::uint32_t> seh_filter_result(GuestState& state, GuestMemory& memory, std::uint64_t result);
    std::optional<std::uint32_t> invoke_exception(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::shared_ptr<FileMappingState> mappings_;
    std::optional<std::uint32_t> invoke_mapping(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool close_mapping(std::uint64_t);
    std::uint64_t map_file_view(GuestMemory&,std::uint64_t handle,std::uint32_t access,std::uint64_t offset,std::uint64_t size,std::uint64_t address=0,bool dib=false,bool copy_override=false);
    std::uint64_t map_dib_section(GuestMemory&,std::uint64_t handle,std::uint32_t offset,std::uint64_t size,std::uint64_t& view);
    bool unmap_file_view(GuestMemory&,std::uint64_t address,bool dib=false);
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
    bool export_process_handle(std::uint64_t,InheritedHandle&);
    void import_process_handle(const InheritedHandle&);
    void import_transfer_handle(const InheritedHandle&);
    void close_transfer_handle(GuestMemory&,std::uint64_t);
    std::optional<std::uint32_t> duplicate_remote_handle(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool mapping_handle(std::uint64_t) const;
    bool shared_mapping_page(std::uint64_t) const;
    struct MappingViewInfo {std::uint64_t base{},size{};std::uint32_t protection{};};
    std::optional<MappingViewInfo> mapping_view_info(std::uint64_t) const;
    bool export_mapping_handle(std::uint64_t,InheritedHandle&) const;
    void import_mapping_handle(const InheritedHandle&);
    GuiState gui_;
    std::map<std::uint64_t,std::shared_ptr<RasterSurface>> rasters_;
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
    void pump_gui_network();
    void enqueue_gui_message(GuiMessage);
    void remember_gui_message(const GuiMessage&);
    std::optional<std::uint32_t> invoke_imm(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::uint64_t imm_window_context(std::uint64_t);
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
    void pump_mouse_tracking();
    void pump_gui(GuestMemory& memory, bool wait,std::uint64_t wait_ticks=500000);
    void poll_desktop_notifications();
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
    std::uint64_t load_gui_icon(GuestMemory& memory,std::uint64_t instance,std::uint64_t name,bool wide,std::uint32_t width,std::uint32_t height,bool intrinsic=false,bool first=false,bool cursor=false,std::uint32_t flags=0);
    std::uint64_t store_gui_icon(GuiIcon,std::uint32_t flags=0);
    std::uint64_t shell_system_icons(std::uint32_t size);
    std::optional<std::uint32_t> invoke_icon_api(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool draw_gui_icon(GuestMemory&,std::uint64_t dc,std::uint64_t icon,std::int32_t x,std::int32_t y,std::uint32_t width,std::uint32_t height,std::uint32_t flags,std::uint64_t brush,std::uint32_t step=0);
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
    std::vector<std::uint8_t> file_creation_descriptor(const GuestMemory&,std::uint64_t);
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
    std::uint64_t msvc_exception_cells(GuestState&,GuestMemory&,bool create,bool refresh=false);
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
    void refresh_gui_areas();
    void notify_appbars(std::uint32_t,std::uint64_t parameter=0);
    void poll_appbars();
    bool common_dialog_message(GuestMemory& memory, const GuiMessage& message,bool from_hook=false);
    void common_dialog_refresh(std::uint64_t handle);
    std::optional<std::uint32_t> invoke_shell(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
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
    void register_type_library(std::uint64_t);
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
    std::uint64_t create_context_enumerator(GuestState&,GuestMemory&,com_context::Enumeration);
    std::uint64_t begin_context_operation(ComContextOperation);

    std::optional<std::uint32_t> invoke_property_keys(GuestState&, GuestMemory&, const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_com_class(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    bool begin_registered_com(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> resume_com_operation(GuestState&,GuestMemory&,std::uint64_t,std::uint32_t);
    void discard_com_operation(GuestState&,GuestMemory&,std::uint64_t);
    std::optional<std::uint64_t> com_apartment(std::uint64_t) const;
    std::vector<GuestCallback> revoke_com_classes(GuestMemory&,std::uint64_t,bool callbacks = true);
    std::optional<std::uint32_t> resume_ole(GuestState&, GuestMemory&, const CallbackFrame&);
    bool shell_dialog_message(GuestState& state, GuestMemory& memory, const GuiMessage& message);
    std::optional<std::uint64_t> shell_browse_control(GuestMemory& memory, const GuiMessage& message, bool wide);
    void shell_browse_refresh(std::uint64_t handle);
    std::uint64_t shell_make_pidl(GuestMemory& memory, std::u16string path);
    void invalidate_window(std::uint64_t handle);
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
    std::uint64_t runtime_allocate(GuestMemory& memory, std::span<const std::uint8_t> bytes, std::size_t size = 0, std::size_t alignment = 16);
    std::uint64_t allocate_heap(GuestMemory& memory, std::uint64_t size, std::uint64_t heap = process_heap);
    bool validate_heap(const GuestMemory& memory,std::uint64_t heap,std::uint64_t pointer=0) const;
    std::uint64_t reallocate_heap(GuestMemory& memory, std::uint64_t address, std::uint64_t size, const GuestState& state);
    bool free_heap(GuestMemory& memory, std::uint64_t address, const GuestState& state);
    // 客體小物件配置區：客體 malloc／free 常式的快速路徑與此處共用狀態頁、鎖及 chunk 格式。
    bool guest_heap_setup(GuestMemory& memory);
    bool guest_locale_lock_setup(GuestMemory& memory);
    // x64 為單一連續保留區；x86 的 chunk 散在 32 位元空間各處，以每 256 KiB 一位元的位元圖判斷。
    bool guest_heap_contains(std::uint64_t address) const noexcept {
        if(guest_heap_chunked_)return address<(UINT64_C(1)<<32) && ((guest_heap_chunk_bits_[address>>24]>>((address>>18)&63U))&1U);
        return guest_heap_base_ && address-guest_heap_base_<guest_heap_size_;
    }
    std::uint64_t guest_heap_reserve_chunk(GuestMemory& memory);
    std::uint64_t guest_heap_allocate(GuestMemory& memory, std::uint64_t size, std::uint64_t heap = process_heap);
    void guest_heap_free(GuestMemory& memory, std::uint64_t address);
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
    std::vector<std::uint8_t> read_crt_file(GuestMemory& memory, CrtFile& file, std::size_t size, bool stream = false);
    std::uint64_t write_crt_file(GuestMemory& memory, CrtFile& file, std::span<const std::uint8_t> bytes, bool stream = false);
    std::uint64_t write_crt_stream(GuestMemory& memory, std::uint64_t stream, std::span<const std::uint8_t> bytes);
    std::uint64_t write_crt_wide_stream(GuestState&,GuestMemory&, std::uint64_t stream, std::u16string_view text);
    std::optional<std::uint32_t> invoke_runtime(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_crt_format(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,const WindowsCall&);
    // 純計算 CRT 邊界：0=非此類，1=記憶體操作，2=字串操作；依 stub 位址直接索引預先分類的表。
    unsigned runtime_compute_boundary(GuestAddress address) const noexcept;
    bool x86_heap_compute_boundary(GuestAddress address) const noexcept;
    std::shared_ptr<struct LdapState> ldap_;
    std::optional<std::uint32_t> invoke_ldap(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_compatibility(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_crt_character(GuestState&,GuestMemory&,const WindowsServiceDescriptor&,
                                                   const ResidentHostEnvironment* = nullptr);
    std::optional<std::uint32_t> invoke_sync_object(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::uint64_t current_invalid_parameter_handler() const;
    std::optional<std::uint32_t> invoke_path(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_crt_string(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_crt_scan(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_math(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    bool fill_random(GuestMemory& memory, std::uint64_t address, std::uint64_t size);
    bool random_bytes(std::span<std::uint8_t> bytes);
    std::optional<std::uint32_t> invoke_dpapi(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
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
    std::optional<std::uint32_t> invoke_network_config(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_tls(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
    std::optional<std::uint32_t> invoke_digest(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_network(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service);
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
    std::optional<std::uint32_t> start_sort(GuestState& state, GuestMemory& memory, bool search = false);
    std::optional<std::uint32_t> advance_sort(GuestState& state, GuestMemory& memory, std::uint64_t comparison, bool reuse_callback = false);
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
        std::vector<DirectoryEntry> entries;
        std::size_t directory_cursor{};
        bool directory_loaded{};
        std::optional<std::u16string> directory_pattern;
        std::uint64_t completion_port{}, completion_key{};
        std::uint8_t completion_modes{};
        bool io_signaled{true};
        bool manage_volume_at_open{};
    };
    std::string stdout_, stderr_;
    std::array<int,3> live_stdio_{-1,-1,-1};
    std::array<std::shared_ptr<int>,3> inherited_stdio_{};
    void import_standard_handle(const InheritedHandle&);
    std::uint32_t standard_file_type(unsigned) const;
    FileInformation file_handle_information(std::uint64_t);
    std::array<std::shared_ptr<NativeConsole>,3> consoles_{};
    std::array<int,3> parent_stdio_{-1,-1,-1};
    bool console_handle(std::uint64_t) const;
    std::optional<std::uint32_t> invoke_console(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    std::optional<std::uint32_t> invoke_console_control(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    void publish_console_identity(bool attached,bool fresh=false);
    std::uint32_t generate_console_control(GuestMemory&,std::uint32_t,std::uint32_t);
    std::uint32_t receive_console_control(GuestMemory&,std::uint32_t,std::uint64_t,std::uint64_t,std::uint32_t,std::uint32_t);
    std::uint32_t queue_console_control(GuestMemory&,std::uint32_t);
    bool console_input_ready(std::uint64_t,unsigned=0);
    bool console_text_ready(bool,int=0);
    std::uint32_t console_sync(unsigned,std::size_t=0,std::size_t=0);
    bool live_input_eof_{};
    bool standard_input_ready(std::uint64_t count, bool text, bool line, int timeout = 0);
    bool await_standard_input(GuestState& state, GuestMemory& memory, std::uint64_t count, bool text = false, bool line = false);
    FileIoResult write_standard_output(unsigned stream, std::span<const std::uint8_t> bytes,bool retained=false);
    std::uint32_t last_error_{};
    std::string trace_services_;
    // 診斷：X64ARM_SERVICE_COUNTS=1 時統計每個服務入口的呼叫次數與累計宿主時間（協調者／managed 與 worker 分列），結束時輸出。
    struct ServiceCount {std::uint64_t calls{},ns{};};
    bool service_counts_enabled_{};std::string service_counts_directory_;
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
    void sync_crt_thread_state(GuestMemory&, std::uint64_t id);
    void sync_crt_thread_states(GuestMemory&);
    GuestAddress guest_heap_state_{};std::uint64_t guest_heap_base_{},guest_heap_size_{},guest_heap_next_{};
    bool guest_heap_chunked_{};std::array<std::uint64_t,256> guest_heap_chunk_bits_{};
    // x86 可成長私有 heap 的級距串列頭；Heap* 服務在 x86 只由協調者序列執行，不需客體鎖。
    // heads 為空表示尚未啟用 chunk，requests 累計一般路徑配置次數。
    struct GuestPrivateHeap {std::vector<std::uint64_t> heads;std::uint64_t requests{};};
    std::map<std::uint64_t,GuestPrivateHeap> guest_private_heaps_;
    // HeapWalk 用：x86 配置區已配置區塊（位址、區塊大小）依位址排序的快取；配置／釋放即使快取失效。
    std::uint64_t guest_heap_mutations_{},guest_heap_walk_generation_{UINT64_MAX},guest_heap_walk_heap_{};
    std::vector<std::pair<std::uint64_t,std::uint64_t>> guest_heap_walk_blocks_,guest_heap_walk_free_;
    const std::vector<std::pair<std::uint64_t,std::uint64_t>>& guest_heap_allocated_blocks(GuestMemory& memory, std::uint64_t heap = process_heap);
    GuestAddress guest_locale_lock_state_{};
    // Win64 TEB／TLS 與 pseudo handle 共用常式；只快取程式入口，不快取客體資料。
    GuestAddress guest_thread_routines_{};
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
    bool detached_console_{};
    std::uint64_t console_identity_{};
    std::uint32_t console_group_{};
    bool initialized_{};
};
}
