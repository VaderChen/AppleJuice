#pragma once
#include "x64arm/host_services.hpp"
#include <bit>

namespace x64arm {
struct WindowsServiceDescriptor { std::string_view family, symbol; unsigned arguments; std::uint32_t wide_arguments{},aggregate_arguments{},narrow_arguments{}; bool caller_cleanup{}; };
std::span<const WindowsServiceDescriptor> gui_rpc_service_descriptors();
std::span<const WindowsServiceDescriptor> com_proxy_service_descriptors();
std::span<const WindowsServiceDescriptor> lsa_service_descriptors();
std::span<const WindowsServiceDescriptor> ole_drag_service_descriptors();
class WindowsCall {
public:
    WindowsCall(const GuestState& state, GuestMemory& memory, unsigned arguments, bool callee_cleanup = true,std::uint32_t wide_arguments=0,std::uint32_t aggregate_arguments=0,std::uint32_t narrow_arguments=0)
        : state_(state), memory_(memory), arguments_(arguments), wide_arguments_(wide_arguments), aggregate_arguments_(aggregate_arguments), narrow_arguments_(narrow_arguments), callee_cleanup_(callee_cleanup), x86_(memory.execution_mode() == ExecutionMode::X86) {
        if (x86_) {
            const auto sp = state.reg(Gpr::Rsp);
            const auto extent = (std::uint64_t{arguments} + static_cast<unsigned>(std::popcount(wide_arguments_))+3U*static_cast<unsigned>(std::popcount(aggregate_arguments_)) + 1) * 4;
            if (sp > UINT32_MAX || extent > UINT64_C(0x100000000) - sp) throw HostCallFault("Win32 呼叫框架超出 32-bit 位址空間");
            memory.check(GuestAddress{sp}, static_cast<std::size_t>(extent), Access::Read);
            return;
        }
        const auto sp = state.reg(Gpr::Rsp);
        const auto extent = UINT64_C(40) + (arguments > 4 ? (static_cast<std::uint64_t>(arguments) - 4) * 8 : 0);
        if (sp % 16 != 8) throw HostCallFault("Windows x64 呼叫入口 RSP 必須為 16n+8");
        if (sp > UINT64_MAX - extent) throw MemoryFault(GuestAddress{sp}, Access::Read, extent, "Windows 呼叫框架溢位");
        memory.check(GuestAddress{sp}, 8, Access::Read);
        memory.check(GuestAddress{sp + 8}, 32, Access::Write);
        if (extent > 40) memory.check(GuestAddress{sp + 40}, extent - 40, Access::Read);
    }
    WindowsCall(const GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service)
        : WindowsCall(state, memory, service.arguments, !service.caller_cleanup && service.family != "crt" && service.symbol != "wsprintfA" && service.symbol != "wsprintfW",service.wide_arguments,service.aggregate_arguments,service.narrow_arguments) {}
    std::uint64_t operator()(unsigned index) const {
        if (x86_) {
            const auto preceding=index<32?wide_arguments_&((UINT32_C(1)<<index)-1):wide_arguments_;
            const auto aggregate_preceding=index<32?aggregate_arguments_&((UINT32_C(1)<<index)-1):aggregate_arguments_;
            const auto slot=std::uint64_t{index}+static_cast<unsigned>(std::popcount(preceding))+3U*static_cast<unsigned>(std::popcount(aggregate_preceding));
            // 16-byte 結構：Win32 堆疊內值，Win64 呼叫者配置的間接指標。
            if(index<32 && (aggregate_arguments_&(UINT32_C(1)<<index)))return state_.reg(Gpr::Rsp)+(slot+1)*4;
            return memory_.load(GuestAddress{state_.reg(Gpr::Rsp)+(slot+1)*4},index<32 && (wide_arguments_&(UINT32_C(1)<<index))?64:32);
        }
        // Win64 的 DWORD／LONG／BOOL 參數只保證低 32 位元；堆疊 slot 的高位元可保留舊資料。
        const auto scalar=[&](std::uint64_t value){return index<32 && (narrow_arguments_&(UINT32_C(1)<<index))?static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)):value;};
        constexpr std::array registers{Gpr::Rcx, Gpr::Rdx, Gpr::R8, Gpr::R9};
        if (index < 4) return scalar(state_.reg(registers[index]));
        const auto offset = UINT64_C(40) + (static_cast<std::uint64_t>(index) - 4) * 8;
        const auto sp = state_.reg(Gpr::Rsp);
        if (offset > UINT64_MAX - sp) throw MemoryFault(GuestAddress{sp}, Access::Read, 8, "Windows 參數地址溢位");
        return scalar(memory_.load(GuestAddress{sp + offset}, 64));
    }
    void return_to(GuestState& state, std::uint64_t value, bool wide_result = false) const {
        if (x86_) {
            const auto target = memory_.load(GuestAddress{state_.reg(Gpr::Rsp)}, 32);
            state.rip = GuestAddress{target}; state.reg(Gpr::Rax) = static_cast<std::uint32_t>(value);
            if(wide_result)state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(value>>32);
            const auto slots=arguments_+static_cast<unsigned>(std::popcount(wide_arguments_))+3U*static_cast<unsigned>(std::popcount(aggregate_arguments_));
            state.reg(Gpr::Rsp) = static_cast<std::uint32_t>(state.reg(Gpr::Rsp) + 4 + (callee_cleanup_ ? slots * 4U : 0));
            return;
        }
        const auto target = memory_.load(GuestAddress{state_.reg(Gpr::Rsp)}, 64);
        const auto next_sp=state.reg(Gpr::Rsp)+8;
        state.rip=GuestAddress{target};state.reg(Gpr::Rax)=value;state.reg(Gpr::Rsp)=next_sp;
    }
private:
    const GuestState& state_; GuestMemory& memory_;
    unsigned arguments_{};std::uint32_t wide_arguments_{},aggregate_arguments_{},narrow_arguments_{}; bool callee_cleanup_{true}, x86_{};
};

std::span<const WindowsServiceDescriptor> locale_service_descriptors();
std::span<const WindowsServiceDescriptor> activation_service_descriptors();
std::span<const WindowsServiceDescriptor> theme_service_descriptors();
std::span<const WindowsServiceDescriptor> service_control_descriptors();
std::span<const WindowsServiceDescriptor> shell_memory_stream_descriptors();
std::span<const WindowsServiceDescriptor> shell_item_service_descriptors();
std::span<const WindowsServiceDescriptor> property_key_service_descriptors();
std::span<const WindowsServiceDescriptor> printing_escape_service_descriptors();
std::span<const WindowsServiceDescriptor> imm_service_descriptors();
std::span<const WindowsServiceDescriptor> property_service_descriptors();
std::span<const WindowsServiceDescriptor> profile_service_descriptors();
std::span<const WindowsServiceDescriptor> winstation_service_descriptors();
std::span<const WindowsServiceDescriptor> version_service_descriptors();
std::span<const WindowsServiceDescriptor> hook_service_descriptors();
std::span<const WindowsServiceDescriptor> mapping_service_descriptors();
std::span<const WindowsServiceDescriptor> process_service_descriptors();
std::span<const WindowsServiceDescriptor> nt_process_service_descriptors();
std::span<const WindowsServiceDescriptor> nt_native_service_descriptors();
std::span<const WindowsServiceDescriptor> graphics_compat_service_descriptors();
std::span<const WindowsServiceDescriptor> legacy_gdi_service_descriptors();
std::span<const WindowsServiceDescriptor> legacy_user_service_descriptors();
std::span<const WindowsServiceDescriptor> kernel_driver_service_descriptors();
std::span<const WindowsServiceDescriptor> gdi_service_descriptors();
std::span<const WindowsServiceDescriptor> dib_transfer_service_descriptors();
std::span<const WindowsServiceDescriptor> gdiplus_service_descriptors();
std::span<const WindowsServiceDescriptor> ribbon_service_descriptors();
std::span<const WindowsServiceDescriptor> task_dialog_service_descriptors();
std::span<const WindowsServiceDescriptor> opencl_service_descriptors();
std::span<const WindowsServiceDescriptor> vulkan_service_descriptors();
std::span<const WindowsServiceDescriptor> directml_export_service_descriptors();
std::span<const WindowsServiceDescriptor> msvcp_pmr_service_descriptors();
std::span<const WindowsServiceDescriptor> directml_object_service_descriptors();
std::span<const WindowsServiceDescriptor> webview2_loader_service_descriptors();
std::span<const WindowsServiceDescriptor> webview2_object_service_descriptors();
std::span<const WindowsServiceDescriptor> d3d12_export_service_descriptors();
std::span<const WindowsServiceDescriptor> d3d12_object_service_descriptors();
std::span<const WindowsServiceDescriptor> dxcore_export_service_descriptors();
std::span<const WindowsServiceDescriptor> dxcore_object_service_descriptors();
std::span<const WindowsServiceDescriptor> gdi_state_service_descriptors();
std::span<const WindowsServiceDescriptor> gdi_path_service_descriptors();
std::span<const WindowsServiceDescriptor> printing_service_descriptors();
std::span<const WindowsServiceDescriptor> metafile_service_descriptors();
std::span<const WindowsServiceDescriptor> script_service_descriptors();
std::span<const WindowsServiceDescriptor> gui_service_descriptors();
std::span<const WindowsServiceDescriptor> resource_service_descriptors();
std::span<const WindowsServiceDescriptor> security_service_descriptors();
std::span<const WindowsServiceDescriptor> anonymous_token_service_descriptors();
std::span<const WindowsServiceDescriptor> power_notification_service_descriptors();
std::span<const WindowsServiceDescriptor> data_protection_service_descriptors();
std::span<const WindowsServiceDescriptor> application_policy_service_descriptors();
std::span<const WindowsServiceDescriptor> etw_service_descriptors();
std::span<const WindowsServiceDescriptor> perflib_service_descriptors();
std::span<const WindowsServiceDescriptor> pdh_service_descriptors();
std::span<const WindowsServiceDescriptor> transaction_service_descriptors();
std::span<const WindowsServiceDescriptor> system_object_service_descriptors();
std::span<const WindowsServiceDescriptor> dde_service_descriptors();
std::span<const WindowsServiceDescriptor> user_session_service_descriptors();
std::span<const WindowsServiceDescriptor> com_spy_service_descriptors();
std::span<const WindowsServiceDescriptor> url_security_service_descriptors();
std::span<const WindowsServiceDescriptor> dxva2_service_descriptors();
std::span<const WindowsServiceDescriptor> network_diagnostic_service_descriptors();
std::span<const WindowsServiceDescriptor> credential_ui_service_descriptors();
std::span<const WindowsServiceDescriptor> certificate_ui_service_descriptors();
std::span<const WindowsServiceDescriptor> winusb_service_descriptors();
std::span<const WindowsServiceDescriptor> font_package_service_descriptors();
std::span<const WindowsServiceDescriptor> mci_service_descriptors();
std::span<const WindowsServiceDescriptor> reparse_service_descriptors();
std::span<const WindowsServiceDescriptor> dbghelp_service_descriptors();
std::span<const WindowsServiceDescriptor> dbghelp_symbol_service_descriptors();
std::span<const WindowsServiceDescriptor> heap_service_descriptors();
std::span<const WindowsServiceDescriptor> working_set_service_descriptors();
std::span<const WindowsServiceDescriptor> minidump_service_descriptors();
std::span<const WindowsServiceDescriptor> dwrite_service_descriptors();
std::span<const WindowsServiceDescriptor> accessible_service_descriptors();
std::span<const WindowsServiceDescriptor> control_service_descriptors();
std::span<const WindowsServiceDescriptor> userstring_service_descriptors();
std::span<const WindowsServiceDescriptor> msvc_service_descriptors();
std::span<const WindowsServiceDescriptor> common_dialog_service_descriptors();
std::span<const WindowsServiceDescriptor> shell_service_descriptors();
std::span<const WindowsServiceDescriptor> shell_notification_service_descriptors();
std::span<const WindowsServiceDescriptor> ole_service_descriptors();
std::span<const WindowsServiceDescriptor> type_library_service_descriptors();
std::span<const WindowsServiceDescriptor> winrt_service_descriptors();
std::span<const WindowsServiceDescriptor> com_context_service_descriptors();
std::span<const WindowsServiceDescriptor> com_error_service_descriptors();
std::span<const WindowsServiceDescriptor> winrt_error_service_descriptors();
std::span<const WindowsServiceDescriptor> desktop_notification_service_descriptors();
std::span<const WindowsServiceDescriptor> gui_drag_service_descriptors();
std::span<const WindowsServiceDescriptor> gui_move_size_service_descriptors();
std::span<const WindowsServiceDescriptor> ole_stream_service_descriptors();
std::span<const WindowsServiceDescriptor> storage_service_descriptors();
std::span<const WindowsServiceDescriptor> msi_service_descriptors();
std::span<const WindowsServiceDescriptor> esent_service_descriptors();
std::span<const WindowsServiceDescriptor> safearray_service_descriptors();
std::span<const WindowsServiceDescriptor> wmi_service_descriptors();
std::string_view ole_ordinal_name(std::uint16_t ordinal);
std::span<const WindowsServiceDescriptor> menu_service_descriptors();
std::span<const WindowsServiceDescriptor> runtime_service_descriptors();
std::span<const WindowsServiceDescriptor> compatibility_service_descriptors();
std::span<const WindowsServiceDescriptor> crt_character_service_descriptors();
std::span<const WindowsServiceDescriptor> math_service_descriptors();
std::span<const WindowsServiceDescriptor> crt_string_service_descriptors();
std::span<const WindowsServiceDescriptor> path_service_descriptors();
std::span<const WindowsServiceDescriptor> time_service_descriptors();
std::span<const WindowsServiceDescriptor> crypto_service_descriptors();
std::span<const WindowsServiceDescriptor> certificate_service_descriptors();
std::span<const WindowsServiceDescriptor> certificate_decode_service_descriptors();
std::span<const WindowsServiceDescriptor> certificate_key_service_descriptors();
std::span<const WindowsServiceDescriptor> certificate_find_service_descriptors();
std::span<const WindowsServiceDescriptor> dpapi_service_descriptors();
std::span<const WindowsServiceDescriptor> memory_service_descriptors();
std::span<const WindowsServiceDescriptor> network_service_descriptors();
std::span<const WindowsServiceDescriptor> mpr_service_descriptors();
std::span<const WindowsServiceDescriptor> environment_service_descriptors();
std::span<const WindowsServiceDescriptor> thread_service_descriptors();
std::span<const WindowsServiceDescriptor> fiber_service_descriptors();
std::span<const WindowsServiceDescriptor> http_service_descriptors();
std::span<const WindowsServiceDescriptor> eventlog_service_descriptors();
std::span<const WindowsServiceDescriptor> registered_wait_service_descriptors();
std::span<const WindowsServiceDescriptor> file_service_descriptors();
std::span<const WindowsServiceDescriptor> file_encryption_service_descriptors();
std::span<const WindowsServiceDescriptor> serial_service_descriptors();
std::span<const WindowsServiceDescriptor> console_service_descriptors();
std::span<const WindowsServiceDescriptor> volume_service_descriptors();
std::span<const WindowsServiceDescriptor> error_service_descriptors();
std::span<const WindowsServiceDescriptor> tls_service_descriptors();
std::span<const WindowsServiceDescriptor> registry_service_descriptors();
std::span<const WindowsServiceDescriptor> d3d9_service_descriptors();
std::span<const WindowsServiceDescriptor> d3d11_service_descriptors();
std::span<const WindowsServiceDescriptor> dsound_service_descriptors();
std::span<const WindowsServiceDescriptor> wasapi_service_descriptors();
std::span<const WindowsServiceDescriptor> media_foundation_service_descriptors();
std::span<const WindowsServiceDescriptor> dinput_service_descriptors();
std::span<const WindowsServiceDescriptor> directmusic_service_descriptors();
std::vector<std::u16string> parse_windows_arguments(std::u16string_view input);
std::string read_ansi(const GuestMemory& memory, std::uint64_t pointer, std::size_t limit = 1024U * 1024U);
std::size_t ansi_length(const GuestMemory& memory,std::uint64_t pointer,std::size_t limit=1024U*1024U);
int compare_ansi(const GuestMemory& memory,std::uint64_t left,std::uint64_t right,std::size_t count);
}
