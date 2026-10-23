#include "x64arm/windows_services.hpp"
#include "guest_crt_state.h"
#include "guest_heap_layout.hpp"
#include "windows_loader_lock.hpp"
#include "x64arm/startup_profile.hpp"
#include "service_descriptor_index.hpp"
#include "service_descriptor_catalog.hpp"
#include "windows_crt_character.hpp"
#include "crt_os_error.hpp"
#include "x64arm/process_debugger.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/windows_pointer.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/windows_default_environment.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/network_registry.hpp"
#include "x64arm/windows_unwind.hpp"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <algorithm>
#include <unordered_set>
#include <atomic>
#include <cstring>
#include <mutex>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <random>

namespace x64arm {
std::span<const WindowsServiceDescriptor> com_marshal_service_descriptors();
std::span<const WindowsServiceDescriptor> print_dialog_service_descriptors();
std::uint64_t WindowsServices::current_invalid_parameter_handler() const {
    const auto found=threads_.items.find(threads_.current);
    return found!=threads_.items.end() && found->second.invalid_parameter_handler?found->second.invalid_parameter_handler:runtime_.invalid_parameter_handler;
}

std::span<const WindowsServiceDescriptor> runtime_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll", "TlsAlloc", 0}, {"kernel32.dll", "TlsFree", 1}, {"kernel32.dll", "TlsGetValue", 1}, {"kernel32.dll", "TlsGetValue2", 1}, {"kernel32.dll", "TlsSetValue", 2},
        {"kernel32.dll", "InitializeCriticalSection", 1}, {"kernel32.dll", "DeleteCriticalSection", 1},
        {"kernel32.dll", "EnterCriticalSection", 1}, {"kernel32.dll", "LeaveCriticalSection", 1},
        {"kernel32.dll", "SetUnhandledExceptionFilter", 1}, {"kernel32.dll", "Sleep", 1},
        {"kernel32.dll", "GetProcessUserModeExceptionPolicy", 1}, {"kernel32.dll", "SetProcessUserModeExceptionPolicy", 1},
        {"kernel32.dll", "VirtualQuery", 3},
        {"kernel32.dll", "GetModuleHandleA", 1}, {"kernel32.dll", "GetModuleHandleW", 1},
        {"kernel32.dll", "GetCommandLineA", 0}, {"kernel32.dll", "GetCurrentThreadId", 0}, {"kernel32.dll", "GetCurrentProcessId", 0},
        {"kernel32.dll", "GetACP", 0}, {"kernel32.dll", "GetStartupInfoA", 1}, {"kernel32.dll", "GetStartupInfoW", 1},
        {"crt", "__p___argc", 0}, {"crt", "__p___argv", 0}, {"crt", "__p__environ", 0},
        {"crt", "_set_fmode", 1}, {"crt", "_get_fmode", 1}, {"crt", "__p__fmode", 0}, {"crt", "__p__commode", 0}, {"crt", "__p__adjust_fdiv", 0}, {"crt", "_errno", 0},
        {"crt","__crtGetShowWindowMode",0},{"crt","_get_show_window_mode",0},
        {"crt","_get_narrow_winmain_command_line",0},{"crt","_get_wide_winmain_command_line",0},
        {"crt","__crtSetUnhandledExceptionFilter",1},{"crt","__crtUnhandledException",1},
        {"crt","__crtCapturePreviousContext",1},{"crt","__crtTerminateProcess",1},
        {"crt","_crt_debugger_hook",1},{"crt","__crt_debugger_hook",1},
        {"crt", "_configure_narrow_argv", 1}, {"crt", "_initialize_narrow_environment", 0},
        {"crt", "_get_initial_narrow_environment", 0}, {"crt", "_get_initial_wide_environment", 0}, {"crt", "_set_app_type", 1}, {"crt", "_set_new_handler", 1}, {"crt", "_query_new_handler", 0}, {"crt", "_query_new_mode", 0}, {"crt", "_callnewh", 1}, {"crt", "_set_new_mode", 1}, {"crt", "_configthreadlocale", 1},
        {"crt", "__setusermatherr", 1}, {"crt", "_set_invalid_parameter_handler", 1},{"crt","_set_thread_local_invalid_parameter_handler",1},{"crt","_getpid",0},{"crt","_msize",1},{"crt","_abs64",1},
        {"crt", "_initterm", 2}, {"crt", "_initterm_e", 2}, {"crt", "_crt_at_quick_exit", 1}, {"crt", "at_quick_exit", 1}, {"crt", "quick_exit", 1}, {"crt", "_crt_atexit", 1}, {"crt", "atexit", 1}, {"crt", "_onexit", 1},
        {"crt", "_cexit", 0}, {"crt", "exit", 1}, {"crt", "_exit", 1}, {"crt", "abort", 0}, {"crt", "_set_abort_behavior", 2},
        {"crt","raise",1},{"crt","_invalid_parameter",5},{"crt","_invalid_parameter_noinfo_noreturn",0},{"crt","_invoke_watson",5},
        {"crt", "_c_exit", 0}, {"crt", "__dllonexit", 3},
        {"crt", "_initialize_onexit_table", 1}, {"crt", "_register_onexit_function", 2}, {"crt", "_execute_onexit_table", 1},
        {"crt", "_register_thread_local_exe_atexit_callback", 1},
        {"crt", "malloc", 1}, {"crt", "calloc", 2}, {"crt", "realloc", 2}, {"crt", "_recalloc", 3}, {"crt", "free", 1},
        {"crt", "_aligned_malloc", 2}, {"crt", "_aligned_realloc", 3}, {"crt", "_aligned_free", 1}, {"crt","_aligned_offset_malloc",3}, {"crt","_aligned_offset_realloc",4}, {"crt","_aligned_recalloc",4}, {"crt","_aligned_offset_recalloc",5}, {"crt","_aligned_msize",3},
        {"kernel32.dll", "RtlMoveMemory", 3}, {"ntdll.dll", "RtlMoveMemory", 3}, {"crt", "memcpy", 3}, {"crt", "memmove", 3}, {"crt", "memset", 3}, {"crt", "memcmp", 3},
        {"crt", "strlen", 1}, {"crt", "strcmp", 2}, {"crt", "strncmp", 3}, {"crt", "strcpy", 2}, {"crt", "strncpy", 3},
        {"crt", "strcat", 2}, {"crt", "strncat", 3}, {"crt", "wcscat", 2}, {"crt", "wcsncat", 3},
        {"crt", "_get_stream_buffer_pointers", 4}, {"crt", "__acrt_iob_func", 1}, {"crt", "__iob_func", 0}, {"crt", "fflush", 1}, {"crt", "setvbuf", 4}, {"crt", "setbuf", 2}, {"crt", "_getmaxstdio", 0}, {"crt", "_setmaxstdio", 1}, {"crt", "_dup2", 2}, {"crt", "_locking", 3},
        {"crt", "puts", 1}, {"crt", "putchar", 1}, {"crt", "printf", 1}, {"crt", "fprintf", 2},
        {"crt", "vprintf", 2}, {"crt", "wprintf", 1}, {"crt", "vwprintf", 2}, {"crt", "fwprintf", 2}, {"crt", "vfwprintf", 3}, {"crt", "vfprintf", 3}, {"crt", "__stdio_common_vfprintf", 5,1}, {"crt", "__stdio_common_vfwprintf", 5,1}, {"crt", "snprintf", 3},
        {"kernel32.dll", "DebugBreak", 0}, {"kernel32.dll", "UnhandledExceptionFilter", 1}, {"kernel32.dll", "RtlUnwind", 4}, {"ntdll.dll", "RtlUnwind", 4},
        {"kernel32.dll", "RaiseFailFastException", 3},
        {"kernel32.dll", "RtlAddFunctionTable", 3,1U<<2}, {"ntdll.dll", "RtlAddFunctionTable", 3,1U<<2},
        {"kernel32.dll", "RtlDeleteFunctionTable", 1}, {"ntdll.dll", "RtlDeleteFunctionTable", 1},
        {"ntdll.dll", "RtlAddGrowableFunctionTable", 6},
        {"ntdll.dll", "RtlGrowFunctionTable", 2}, {"ntdll.dll", "RtlDeleteGrowableFunctionTable", 1},
        {"kernel32.dll", "RaiseException", 4}, {"kernel32.dll", "AddVectoredExceptionHandler", 2}, {"kernel32.dll", "RemoveVectoredExceptionHandler", 1},
        {"kernel32.dll", "AddVectoredContinueHandler", 2}, {"kernel32.dll", "RemoveVectoredContinueHandler", 1},
        {"kernel32.dll", "RtlPcToFileHeader", 2}, {"ntdll.dll", "RtlPcToFileHeader", 2}, {"kernel32.dll", "RtlCaptureContext", 1}, {"kernel32.dll", "RtlLookupFunctionEntry", 3}, {"kernel32.dll", "RtlVirtualUnwind", 8}, {"kernel32.dll", "RtlUnwindEx", 6},
        {"user32.dll", "RegisterClassW", 1}, {"user32.dll", "RegisterClassExW", 1}, {"user32.dll", "RegisterClassA", 1}, {"user32.dll", "RegisterClassExA", 1},
        {"user32.dll", "CreateWindowExW", 12}, {"user32.dll", "CreateWindowExA", 12}, {"user32.dll", "DefWindowProcW", 4}, {"user32.dll", "DefWindowProcA", 4},
        {"user32.dll", "ShowWindow", 2}, {"user32.dll", "UpdateWindow", 1}, {"user32.dll", "DestroyWindow", 1},
        {"user32.dll", "PostQuitMessage", 1}, {"user32.dll", "GetMessageW", 4}, {"user32.dll", "GetMessageA", 4},
        {"user32.dll", "PeekMessageW", 5}, {"user32.dll", "PeekMessageA", 5}, {"user32.dll", "TranslateMessage", 1},
        {"user32.dll", "DispatchMessageW", 1}, {"user32.dll", "DispatchMessageA", 1}, {"user32.dll", "SendMessageTimeoutA",7},{"user32.dll", "SendMessageTimeoutW",7},{"user32.dll", "SendMessageW", 4}, {"user32.dll", "SendMessageA", 4},
        {"user32.dll", "SendMessageCallbackA", 6}, {"user32.dll", "SendMessageCallbackW", 6}, {"user32.dll", "SendNotifyMessageA", 4}, {"user32.dll", "SendNotifyMessageW", 4}, {"user32.dll", "WaitMessage", 0},
        {"user32.dll", "PostThreadMessageW", 4},{"user32.dll", "PostThreadMessageA", 4},{"user32.dll", "PostMessageW", 4}, {"user32.dll", "PostMessageA", 4}, {"user32.dll", "BeginPaint", 2}, {"user32.dll", "EndPaint", 2},
        {"user32.dll", "GetClientRect", 2}, {"user32.dll", "InvalidateRect", 3}, {"user32.dll", "InvalidateRgn", 3},
        {"user32.dll", "GetWindowLongPtrW", 2}, {"user32.dll", "SetWindowLongPtrW", 3}, {"user32.dll", "GetWindowLongPtrA", 2}, {"user32.dll", "SetWindowLongPtrA", 3},
        {"user32.dll", "LoadCursorW", 2}, {"user32.dll", "LoadCursorA", 2}, {"user32.dll", "GetSystemMetrics", 1},
        {"gdi32.dll", "GetStockObject", 1}, {"gdi32.dll", "CreateSolidBrush", 1},
        {"kernel32.dll", "IsDBCSLeadByte", 1},
        {"kernel32.dll", "FlsAlloc", 1}, {"kernel32.dll", "FlsFree", 1}, {"kernel32.dll", "FlsGetValue", 1}, {"kernel32.dll", "FlsGetValue2", 1}, {"kernel32.dll", "FlsSetValue", 2},
        {"crt", "__p__acmdln", 0}, {"crt", "__p__wcmdln", 0}, {"crt", "strstr", 2}, {"crt", "strchr", 2}, {"crt", "strrchr", 2}, {"crt", "getenv", 1},
        {"crt", "fopen", 2}, {"crt", "fopen_s", 3}, {"crt", "_wfopen_s", 3}, {"crt", "_commit", 1}, {"crt", "_fsopen", 3}, {"crt", "_wfsopen", 3}, {"crt", "fclose", 1}, {"crt", "fread", 4}, {"crt", "fwrite", 4},
        {"crt","_chsize",2},{"crt","_chsize_s",2,1U<<1},{"crt","_open_osfhandle",2},{"crt","_filelength",1},{"crt","_filelengthi64",1},
        {"crt", "_creat", 2}, {"crt", "_wcreat", 2}, {"crt", "freopen", 3}, {"crt", "_wfreopen", 3}, {"crt", "_dup", 1}, {"crt", "_fdopen", 2}, {"crt", "_wfdopen", 2}, {"crt", "_close", 1}, {"crt", "_read", 3},
        {"crt", "_open", 3}, {"crt", "_wopen", 3}, {"crt", "_sopen", 4}, {"crt", "_wsopen", 4}, {"crt", "_sopen_s", 5}, {"crt", "_wsopen_s", 5},
        {"crt","_umask",1},{"crt","_umask_s",2},
        {"crt", "_lseek", 3}, {"crt", "_lseeki64", 3,1U<<1}, {"crt", "_tell", 1}, {"crt", "_telli64", 1},
        {"crt", "fgetpos", 2}, {"crt", "fsetpos", 2}, {"crt", "fseek", 3}, {"crt", "_fseeki64", 3,1U<<1}, {"crt", "ftell", 1}, {"crt", "_ftelli64", 1},
        {"crt", "feof", 1}, {"crt", "ferror", 1}, {"crt", "clearerr", 1}, {"crt", "rewind", 1},
        {"crt", "fgetwc", 1}, {"crt", "fputwc", 2}, {"crt", "ungetc", 2}, {"crt", "ungetwc", 2}, {"crt", "getchar", 0}, {"crt", "fgetc", 1}, {"crt", "getc", 1}, {"crt", "fgets", 3}, {"crt", "fputs", 2},
        {"crt", "__stdio_common_vsnprintf_s", 7,1}, {"crt", "__stdio_common_vsnwprintf_s", 7,1}, {"crt", "_invalid_parameter_noinfo", 0}, {"crt", "__stdio_common_vsprintf_s", 6,1}, {"crt", "__stdio_common_vsprintf", 6,1}, {"crt", "vsnprintf", 4},
        {"crt","sprintf",2},
        {"crt","vsprintf",3},
        {"crt","sprintf_s",3},
        {"crt","vsprintf_s",4},
        {"crt","swprintf_s",3},
        {"crt","vswprintf_s",4},
        {"crt","_snprintf",3},
        {"crt","_vsnprintf",4},
        {"crt","_snwprintf",3},
        {"crt","_vsnwprintf",4},
        {"crt","_snprintf_s",4},
        {"crt","_vsnprintf_s",5},
        {"crt","_snwprintf_s",4},
        {"crt","_vsnwprintf_s",5},
        {"crt","_scprintf",1},
        {"crt","_vscprintf",2},
        {"crt","_scwprintf",1},
        {"crt","_vscwprintf",2},
        {"crt","__stdio_common_vswprintf",6,1},{"crt","__stdio_common_vswprintf_s",6,1},
        {"crt", "signal", 2}, {"crt", "__getmainargs", 5}, {"crt", "__wgetmainargs", 5}, {"crt", "__C_specific_handler", 4},
        {"kernel32.dll", "__C_specific_handler", 4}, {"ntdll.dll", "__C_specific_handler", 4},
        {"crt", "setjmp", 1}, {"crt", "_setjmp", 2}, {"crt", "__intrinsic_setjmp", 1},
        {"crt", "_setjmpex", 2}, {"crt", "__intrinsic_setjmpex", 2}, {"crt", "longjmp", 2},
        {"crt", "_setjmp3", 2}, {"crt", "_longjmpex", 2},
        {"crt", "__p___wargv", 0}, {"crt", "__p__wenviron", 0},
        {"crt", "__p___initenv", 0}, {"crt", "__p___winitenv", 0},
        {"crt", "_initialize_wide_environment", 0}, {"crt", "_configure_wide_argv", 1}, {"crt", "_wfopen", 2}
    };
    static const auto combined = [&] {
        startup_profile::Scope phase("services","descriptor_table",true);
        const detail::ServiceDescriptorGroup groups[]{
            {table, false},
            {compatibility_service_descriptors(), true},
            {crt_character_service_descriptors(), true},
            {gui_service_descriptors(), true},
            {gdi_service_descriptors(), true},
            {input_service_descriptors(), true},
            {raw_input_service_descriptors(), true},
            {pointer_service_descriptors(), true},
            {hid_service_descriptors(), true},
            {icon_service_descriptors(), true},
            {dib_transfer_service_descriptors(), true},
            {gdiplus_service_descriptors(), true},
            {task_dialog_service_descriptors(), true},
            {opencl_service_descriptors(), true},
            {vulkan_service_descriptors(), true},
            {directml_export_service_descriptors(), true},
            {directml_object_service_descriptors(), false},
            {msvcp_pmr_service_descriptors(), true},
            {webview2_loader_service_descriptors(), true},
            {webview2_object_service_descriptors(), false},
            {d3d12_export_service_descriptors(), true},
            {d3d12_object_service_descriptors(), false},
            {dxcore_export_service_descriptors(), true},
            {dxcore_object_service_descriptors(), false},
            {common_dialog_service_descriptors(), true},
            {menu_service_descriptors(), true},
            {shell_service_descriptors(), true},
            {ole_service_descriptors(), true},
            {ole_stream_service_descriptors(), true},
            {storage_service_descriptors(), true},
            {safearray_service_descriptors(), true},
            {resource_service_descriptors(), true},
            {security_service_descriptors(), true},
            {msvc_service_descriptors(), true},
            {control_service_descriptors(), true},
            {userstring_service_descriptors(), true},
            {msxml_service_descriptors(), false},
            {ribbon_service_descriptors(), false},
            {wmi_service_descriptors(), false},
            {native_library_service_descriptors(), false},
            {dbghelp_service_descriptors(), false},
            {script_service_descriptors(), false},
            {metafile_service_descriptors(), false},
            {eventlog_service_descriptors(), false},
            {http_service_descriptors(), false},
            {fiber_service_descriptors(), false},
            {printing_service_descriptors(), false},
            {print_dialog_service_descriptors(), false},
            {gdi_path_service_descriptors(), false},
            {gdi_state_service_descriptors(), false},
            {accessible_service_descriptors(), false},
            {dwrite_service_descriptors(), false},
            {registered_wait_service_descriptors(), false},
            {math_service_descriptors(), false},
            {crt_string_service_descriptors(), false},
            {path_service_descriptors(), false},
            {time_service_descriptors(), false},
            {mapping_service_descriptors(), false},
            {process_service_descriptors(), false},
            {nt_process_service_descriptors(), false},
            {nt_native_service_descriptors(), false},
            {winrt_service_descriptors(), false},
            {com_marshal_service_descriptors(), false},
            {com_context_service_descriptors(), false},
            {com_error_service_descriptors(), false},
            {type_library_service_descriptors(), false},
            {winrt_error_service_descriptors(), false},
            {desktop_notification_service_descriptors(), false},
            {gui_drag_service_descriptors(), false},
            {gui_move_size_service_descriptors(), false},
            {ole_drag_service_descriptors(), false},
            {gui_rpc_service_descriptors(), false},
            {com_proxy_service_descriptors(), false},
            {power_notification_service_descriptors(), false},
            {data_protection_service_descriptors(), false},
            {application_policy_service_descriptors(), false},
            {opengl_service_descriptors(), false},
            {angle_service_descriptors(), false},
            {graphics_compat_service_descriptors(), false},
            {legacy_gdi_service_descriptors(), false},
            {legacy_user_service_descriptors(), false},
            {kernel_driver_service_descriptors(), false},
            {hook_service_descriptors(), false},
            {activation_service_descriptors(), false},
            {theme_service_descriptors(), false},
            {property_service_descriptors(), false},
            {profile_service_descriptors(), false},
            {winstation_service_descriptors(), false},
            {version_service_descriptors(), false},
            {locale_service_descriptors(), false},
            {font_service_descriptors(), false},
            {crypto_service_descriptors(), false},
            {certificate_service_descriptors(), false},
            {network_service_descriptors(), false},
            {mpr_service_descriptors(), false},
            {environment_service_descriptors(), true},
            {thread_service_descriptors(), true},
            {file_service_descriptors(), false},
            {file_encryption_service_descriptors(), false},
            {serial_service_descriptors(), false},
            {console_service_descriptors(), false},
            {volume_service_descriptors(), false},
            {error_service_descriptors(), false},
            {tls_service_descriptors(), false},
            {registry_service_descriptors(), false},
            {d3d9_service_descriptors(), false},
            {d3d11_service_descriptors(), false},
            {dsound_service_descriptors(), false},
            {wasapi_service_descriptors(), false},
            {media_foundation_service_descriptors(), false},
            {dinput_service_descriptors(), false},
            {directmusic_service_descriptors(), false},
            {memory_service_descriptors(), true},
            {service_control_descriptors(), false},
            {shell_memory_stream_descriptors(), false},
            {shell_item_service_descriptors(), false},
            {lsa_service_descriptors(), false},
            {property_key_service_descriptors(), false},
            {printing_escape_service_descriptors(), false},
            {imm_service_descriptors(), false},
            {shell_notification_service_descriptors(), false},
            {certificate_decode_service_descriptors(), false},
            {certificate_key_service_descriptors(), false},
            {certificate_find_service_descriptors(), false},
            {dpapi_service_descriptors(), false},
            {dbghelp_symbol_service_descriptors(), false},
            {minidump_service_descriptors(), false},
            {heap_service_descriptors(), false},
            {working_set_service_descriptors(), false},
            {anonymous_token_service_descriptors(), false},
            {etw_service_descriptors(), false},
            {perflib_service_descriptors(), false},
            {pdh_service_descriptors(), true},
            {transaction_service_descriptors(), true},
            {system_object_service_descriptors(), true},
            {dde_service_descriptors(), true},
            {user_session_service_descriptors(), true},
            {com_spy_service_descriptors(), true},
            {url_security_service_descriptors(), true},
            {dxva2_service_descriptors(), true},
            {network_diagnostic_service_descriptors(), true},
            {credential_ui_service_descriptors(), true},
            {certificate_ui_service_descriptors(), true},
            {winusb_service_descriptors(), true},
            {font_package_service_descriptors(), true},
            {mci_service_descriptors(), false},
            {reparse_service_descriptors(), true},
        };
        return detail::combine_service_descriptors(groups);
    }();
    return combined;
}
std::size_t ansi_length(const GuestMemory& memory,std::uint64_t pointer,std::size_t limit) {
    for(std::size_t i=0;i<=limit;){
        if(i>UINT64_MAX-pointer)throw MemoryFault(GuestAddress{pointer},Access::Read,i,"字串地址溢位");
        const auto bytes=memory.readable_prefix(GuestAddress{pointer+i},std::min<std::size_t>(limit-i,4095)+1);
        const auto* end=static_cast<const std::uint8_t*>(std::memchr(bytes.data(),0,bytes.size()));
        if(end)return i+static_cast<std::size_t>(end-bytes.data());
        if(bytes.size()>limit-i)throw HostCallFault("字串超過服務上限");
        i+=bytes.size();
    }
    throw HostCallFault("無效字串");
}
int compare_ansi(const GuestMemory& memory,std::uint64_t left,std::uint64_t right,std::size_t count) {
    for(std::size_t i=0;i<count;){
        std::span<const std::uint8_t> a,b;
        try{a=memory.readable_prefix(GuestAddress{left+i},count-i);b=memory.readable_prefix(GuestAddress{right+i},count-i);}
        catch(const MemoryFault&){
            // 保留原本先左後右的逐 byte 故障地址、寬度與訊息。
            const auto x=memory.load(GuestAddress{left+i},8),y=memory.load(GuestAddress{right+i},8);
            if(x!=y)return static_cast<int>(x)-static_cast<int>(y);if(!x)return 0;++i;continue;
        }
        const auto size=std::min(a.size(),b.size());std::size_t at=0;
#if defined(__aarch64__)
        if(size>=16){
            if(a[0]!=b[0])return static_cast<int>(a[0])-b[0];if(!a[0])return 0;at=1;
        }
        for(;size-at>=16;at+=16){
            const auto x=vld1q_u8(a.data()+at),y=vld1q_u8(b.data()+at);
            if(vminvq_u8(vceqq_u8(x,y))!=255 || vminvq_u8(x)==0)break;
        }
#endif
        for(;at<size;++at){const auto x=a[at],y=b[at];if(x!=y)return static_cast<int>(x)-y;if(!x)return 0;}
        i+=size;
    }
    return 0;
}
std::vector<std::u16string> parse_windows_arguments(std::u16string_view input) {
    std::vector<std::u16string> result;
    std::size_t at = 0;
    while (at < input.size()) {
        while (at < input.size() && (input[at] == u' ' || input[at] == u'\t')) ++at;
        if (at == input.size()) break;
        std::u16string item; bool quoted = false;
        while (at < input.size() && (quoted || (input[at] != u' ' && input[at] != u'\t'))) {
            std::size_t slashes = 0;
            while (at < input.size() && input[at] == u'\\') { ++slashes; ++at; }
            if (at < input.size() && input[at] == u'"') {
                item.append(slashes / 2, u'\\');
                if (slashes % 2) item.push_back(u'"');
                else if (quoted && at + 1 < input.size() && input[at + 1] == u'"') { item.push_back(u'"'); ++at; }
                else quoted = !quoted;
                ++at;
            } else {
                item.append(slashes, u'\\');
                if (at < input.size() && (quoted || (input[at] != u' ' && input[at] != u'\t'))) item.push_back(input[at++]);
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}
namespace {
std::vector<std::uint8_t> bytes_of(std::string_view value) {
    std::vector<std::uint8_t> bytes(value.begin(), value.end()); bytes.push_back(0); return bytes;
}
bool crt_format_symbol(std::string_view symbol) noexcept {
    return symbol=="printf" || symbol=="vprintf" || symbol=="wprintf" || symbol=="vwprintf" || symbol=="fwprintf" || symbol=="vfwprintf" || symbol=="fprintf" || symbol=="vfprintf" || symbol=="__stdio_common_vfprintf" || symbol=="__stdio_common_vfwprintf" || symbol=="snprintf" || symbol=="vsnprintf" || symbol.starts_with("__stdio_common_vs") || symbol=="sprintf" || symbol=="vsprintf" || symbol=="sprintf_s" || symbol=="vsprintf_s" || symbol=="swprintf_s" || symbol=="vswprintf_s" || symbol=="_snprintf" || symbol=="_vsnprintf" || symbol=="_snwprintf" || symbol=="_vsnwprintf" || symbol=="_snprintf_s" || symbol=="_vsnprintf_s" || symbol=="_snwprintf_s" || symbol=="_vsnwprintf_s" || symbol=="_scprintf" || symbol=="_vscprintf" || symbol=="_scwprintf" || symbol=="_vscwprintf";
}
struct CrtFormatLayout {
    bool common{},common_stream{},common_buffer{},stream{},standard_output{},counting{};
    bool wide{},secure{},limited{},unbounded{},va{},underscore{};
    unsigned format_index{};
};
CrtFormatLayout parse_crt_format_layout(std::string_view symbol){
    CrtFormatLayout result;
    result.common=symbol.starts_with("__stdio_common_");
    result.common_stream=symbol=="__stdio_common_vfprintf" || symbol=="__stdio_common_vfwprintf";
    result.common_buffer=result.common && !result.common_stream;
    result.standard_output=symbol=="printf" || symbol=="vprintf" || symbol=="wprintf" || symbol=="vwprintf";
    result.stream=result.standard_output || symbol=="fwprintf" || symbol=="vfwprintf" || symbol=="fprintf" || symbol=="vfprintf" || result.common_stream;
    result.counting=symbol=="_scprintf" || symbol=="_vscprintf" || symbol=="_scwprintf" || symbol=="_vscwprintf";
    result.wide=symbol.find("wprintf")!=symbol.npos;result.secure=symbol.ends_with("_s");
    result.limited=result.secure && (symbol.starts_with("_sn") || symbol.starts_with("_vsn") || symbol.starts_with("__stdio_common_vsn"));
    result.unbounded=symbol=="sprintf" || symbol=="vsprintf";
    result.va=result.common || symbol.starts_with('v') || symbol.starts_with("_v");
    result.underscore=symbol.starts_with('_');
    result.format_index=result.common_buffer?(result.limited?4U:3U):result.common_stream?2:
        result.standard_output || result.counting?0:result.stream || result.unbounded?1:result.limited?3:2;
    return result;
}
CrtFormatLayout crt_format_layout(const WindowsServiceDescriptor& service){
    static const bool cached=[] {const auto* setting=std::getenv("X64ARM_CRT_FORMAT_PROVIDER");return !setting || std::string_view(setting)!="0";}();
    if(!cached)return parse_crt_format_layout(service.symbol);
    const auto table=runtime_service_descriptors();
    const auto address=reinterpret_cast<std::uintptr_t>(&service),begin=reinterpret_cast<std::uintptr_t>(table.data());
    if(address<begin || address-begin>=table.size_bytes() || (address-begin)%sizeof(WindowsServiceDescriptor))
        return parse_crt_format_layout(service.symbol);
    // API 的 ABI 形狀不隨呼叫改變；只快取 descriptor 的常數，客體格式字串及
    // options、locale、容量、va_list 仍逐次檢查，不快取客體資料或可變記憶體。
    static const auto layouts=[table]{
        std::vector<CrtFormatLayout> result(table.size());
        for(std::size_t index=0;index<table.size();++index)
            if(crt_format_symbol(table[index].symbol))result[index]=parse_crt_format_layout(table[index].symbol);
        return result;
    }();
    return layouts[(address-begin)/sizeof(WindowsServiceDescriptor)];
}
struct FormatError {unsigned code;};
struct Formatted {std::u16string text;unsigned encoding_error{};bool failed{};};
Formatted format(const GuestMemory& memory,std::u16string_view specification,bool wide,bool wide_strings,unsigned pointer_size,bool three_digit_exponents,const std::function<std::uint64_t(unsigned)>& next){
    Formatted result;auto& output=result.text;
    for(std::size_t at=0;at<specification.size();){
        if(specification[at]!=u'%'){output.push_back(specification[at++]);continue;}
        ++at;if(at<specification.size() && specification[at]==u'%'){output.push_back(u'%');++at;continue;}
        std::string flags;
        while(at<specification.size() && std::u16string_view(u"-+ #0").find(specification[at])!=std::u16string_view::npos)flags+=static_cast<char>(specification[at++]);
        const auto decimal=[&]{unsigned value=0;while(at<specification.size() && specification[at]>=u'0' && specification[at]<=u'9'){if(value>6553)throw FormatError{22};value=value*10+static_cast<unsigned>(specification[at++]-u'0');}return value;};
        int width=0,precision=-1;
        if(at<specification.size() && specification[at]==u'*'){++at;width=static_cast<std::int32_t>(next(4));}else width=static_cast<int>(decimal());
        if(at<specification.size() && specification[at]==u'.'){++at;if(at<specification.size() && specification[at]==u'*'){++at;precision=static_cast<std::int32_t>(next(4));}else precision=static_cast<int>(decimal());}
        if(width<-65536 || width>65536 || precision>65536)throw FormatError{22};if(width<0){flags+='-';width=-width;}
        std::u16string_view length;
        const auto length_at=at;
        if(specification.substr(at,3)==u"I64" || specification.substr(at,3)==u"I32")at+=3;
        else if(specification.substr(at,2)==u"ll" || specification.substr(at,2)==u"hh")at+=2;
        else if(at<specification.size() && std::u16string_view(u"hlztjLIw").find(specification[at])!=std::u16string_view::npos)++at;
        length=specification.substr(length_at,at-length_at);if(at==specification.size())throw FormatError{22};const auto conversion=specification[at++];
        const bool integer=std::u16string_view(u"diouxX").find(conversion)!=std::u16string_view::npos;
        const bool floating=std::u16string_view(u"fFeEgGaA").find(conversion)!=std::u16string_view::npos;
        const bool string=conversion==u's' || conversion==u'S',character=conversion==u'c' || conversion==u'C';
        if(!integer && !floating && !string && !character && conversion!=u'p')throw FormatError{22};
        const bool pointer_integer=length==u"z" || length==u"t" || length==u"I";
        const unsigned integer_bits=length==u"ll" || length==u"I64" || length==u"j"?64:pointer_integer?pointer_size*8:length==u"h"?16:length==u"hh"?8:32;
        if(integer && (length==u"L" || length==u"w"))throw FormatError{22};
        if(floating && !length.empty() && length!=u"l" && length!=u"L")throw FormatError{22};
        if((string || character) && !length.empty() && length!=u"h" && length!=u"l" && length!=u"w")throw FormatError{22};
        const unsigned size=floating?8:integer?std::max(4U,integer_bits/8):character?4:pointer_size;const auto value=next(size);
        std::u16string rendered;
        if(string || character){
            const bool opposite=conversion==u'S' || conversion==u'C';const bool source_wide=length==u"l" || length==u"w" || (length.empty() && (opposite?!wide_strings:wide_strings));
            if(character){const auto c=static_cast<char16_t>(value&(source_wide?65535:255));if(!wide && c>255){result.encoding_error=42;continue;}rendered.push_back(c);}
            else if(!value)rendered=u"(null)";
            else {
                const auto limit=precision<0?UINT64_C(1048576):static_cast<std::uint64_t>(precision);const unsigned unit=source_wide?2:1;
                static const bool batched=[] {const auto* setting=std::getenv("X64ARM_CRT_FORMAT_PROVIDER");return !setting || std::string_view(setting)!="0";}();
                if(batched){
                    // 沿用 GuestMemory 的可讀前綴，不逐字重查映射。精度上限內才
                    // 借用；跨映射的單一 UTF-16 字元仍由原 16-bit load 處理故障。
                    bool terminated=false;
                    for(std::uint64_t i=0;i<limit && !terminated;){
                        if(i>(UINT64_MAX-value)/unit)throw HostCallFault("CRT 字串位址溢位");
                        const GuestAddress address{value+i*unit};
                        std::span<const std::uint8_t> bytes;
                        try{bytes=memory.readable_prefix(address,static_cast<std::size_t>(std::min<std::uint64_t>(limit-i,256))*unit);}
                        catch(const MemoryFault&){(void)memory.load(address,static_cast<std::uint8_t>(unit*8));throw;}
                        const auto count=std::max<std::size_t>(1,bytes.size()/unit);
                        for(std::size_t unit_index=0;unit_index<count;++unit_index){
                            const auto c=bytes.size()<unit?static_cast<char16_t>(memory.load(address,static_cast<std::uint8_t>(unit*8))):
                                static_cast<char16_t>(bytes[unit_index*unit]|(source_wide?static_cast<unsigned>(bytes[unit_index*unit+1])<<8:0));
                            if(!c){terminated=true;break;}
                            if(!wide && c>255){result.encoding_error=42;result.failed=true;return result;}
                            rendered.push_back(c);++i;
                            if(precision<0 && i==limit)throw HostCallFault("CRT 格式字串超過上限");
                        }
                    }
                }else {
                    for(std::uint64_t i=0;i<limit;++i){if(i>(UINT64_MAX-value)/unit)throw HostCallFault("CRT 字串位址溢位");const auto c=static_cast<char16_t>(memory.load(GuestAddress{value+i*unit},static_cast<std::uint8_t>(unit*8)));if(!c)break;
                        if(!wide && c>255){result.encoding_error=42;result.failed=true;return result;}rendered.push_back(c);
                        if(precision<0 && i+1==limit)throw HostCallFault("CRT 格式字串超過上限");
                    }
                }
            }
            if(string && precision>=0 && rendered.size()>static_cast<std::size_t>(precision))rendered.resize(static_cast<std::size_t>(precision));
            if(rendered.size()<static_cast<std::size_t>(width)){const std::u16string pad(static_cast<std::size_t>(width)-rendered.size(),u' ');if(flags.find('-')!=flags.npos)rendered+=pad;else rendered=pad+rendered;}
        }else {
            std::string pattern="%"+flags+(width?std::to_string(width):"")+(precision>=0?"."+std::to_string(precision):"");
            // 先以不清零的小型堆疊緩衝格式化；超出時再配置剛好大小重做。上限與錯誤碼同原本 131200 bytes 的緩衝。
            // 原寫法每個數值轉換都清零 128 KiB，占 sprintf 服務時間六成以上。
            const auto render=[&](auto argument)->std::string{
                constexpr std::size_t limit=131200;std::array<char,512> small;
                const int count=std::snprintf(small.data(),small.size(),pattern.c_str(),argument);
                if(count<0 || static_cast<std::size_t>(count)>=limit)throw FormatError{22};
                if(static_cast<std::size_t>(count)<small.size())return std::string(small.data(),static_cast<std::size_t>(count));
                std::string large(static_cast<std::size_t>(count)+1,'\0');
                if(std::snprintf(large.data(),large.size(),pattern.c_str(),argument)!=count)throw FormatError{22};
                large.resize(static_cast<std::size_t>(count));return large;
            };
            std::string number;
            if(integer){auto value_bits=value;if(integer_bits<64)value_bits&=(UINT64_C(1)<<integer_bits)-1;pattern+="ll";pattern+=static_cast<char>(conversion);
                if(conversion==u'd' || conversion==u'i'){if(integer_bits<64 && (value_bits&(UINT64_C(1)<<(integer_bits-1))))value_bits|=~((UINT64_C(1)<<integer_bits)-1);number=render(static_cast<long long>(std::bit_cast<std::int64_t>(value_bits)));}
                else number=render(static_cast<unsigned long long>(value_bits));
            }else if(floating){pattern+=static_cast<char>(conversion);number=render(std::bit_cast<double>(value));}
            else {pattern="%"+flags+std::to_string(std::max(width,static_cast<int>(pointer_size*2)))+"."+std::to_string(pointer_size*2)+"llX";number=render(static_cast<unsigned long long>(value));}
            if(floating && three_digit_exponents){const auto exponent=number.find_first_of("eE");if(exponent!=number.npos && exponent+4==number.size() && (number[exponent+1]=='+' || number[exponent+1]=='-'))number.insert(exponent+2,1,'0');}
            rendered.assign(number.begin(),number.end());
        }
        if(output.size()+rendered.size()>WindowsServices::output_limit/(wide?2U:1U))throw HostCallFault("CRT 格式輸出超過上限");output+=rendered;
    }
    return result;
}
}
std::uint64_t WindowsServices::runtime_allocate(GuestMemory& memory, std::span<const std::uint8_t> bytes, std::size_t size, std::size_t alignment) {
    size = std::max({std::size_t{1}, size, bytes.size()});
    if(!std::has_single_bit(alignment) || alignment>65536)throw std::invalid_argument("CRT／TLS 對齊無效");
    alignment=std::max<std::size_t>(16,alignment);
    if(size>memory.limit() || size>SIZE_MAX-15)throw std::bad_alloc();
    const auto rounded = (size + 15) & ~std::size_t{15};
    auto offset=(runtime_.arena_next+alignment-1)&~(std::uint64_t{alignment}-1);
    if(offset>runtime_.arena_capacity || rounded>runtime_.arena_capacity-offset){
        constexpr std::uint64_t granularity=65536;
        const auto capacity=std::max(granularity,(std::uint64_t{rounded}+4095)&~UINT64_C(4095));
        if(capacity>memory.remaining_capacity())throw std::bad_alloc();
        const auto limit=user_address_limit();auto base=(runtime_.next_virtual+granularity-1)&~(granularity-1);
        for(;;){
            if(base>=limit || capacity>limit-base)throw std::bad_alloc();
            auto end=base;
            for(const auto& [at,region]:runtime_.virtual_regions)if(at<base+capacity && base<at+region.size)end=std::max(end,at+region.size);
            if(const auto mapped=memory.query(GuestAddress{base}))end=std::max(end,mapped->base.value+mapped->size);
            if(const auto mapped=memory.next_mapping(GuestAddress{base});mapped && mapped->base.value<base+capacity)end=std::max(end,mapped->base.value+mapped->size);
            if(end==base)break;base=(end+granularity-1)&~(granularity-1);
        }
        memory.map(GuestAddress{base},static_cast<std::size_t>(capacity),read_write);
        runtime_.arena_base=base;runtime_.arena_capacity=capacity;runtime_.arena_next=0;offset=0;
        runtime_.next_virtual=(base+capacity+granularity-1)&~(granularity-1);
    }
    const auto address = (runtime_.arena_base?runtime_.arena_base:runtime_base_.value)+offset;
    memory.write(GuestAddress{address}, bytes);runtime_.arena_next=offset+rounded;return address;
}
void WindowsServices::initialize_runtime(GuestMemory& memory, std::u16string_view command_line) {
    startup_profile::Scope phase("services","initialize_runtime",true);
    runtime_.filetime_base = UINT64_C(116444736000000000) + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::duration<std::int64_t,std::ratio<1,10000000>>>(std::chrono::system_clock::now().time_since_epoch()).count());
    // 先擷取一次熵來源，再隨服務狀態複製，確保 interpreter／JIT 使用同一輸入。
    {startup_profile::Scope entropy_phase("services","entropy",true);
    std::random_device entropy;
    if (entropy.entropy() > 0) {
        runtime_.entropy->resize(256);
        for (auto& value : *runtime_.entropy) value = entropy();
    }}
    memory.map(runtime_base_, 1024U * 1024U, read_write);
    initialize_msvcp100(memory);
    for (unsigned i=0;i<3;++i) {
        const auto stream=runtime_base_.value+0x800+i*48U;
        memory.store(GuestAddress{stream+24},32,i?2:1);memory.store(GuestAddress{stream+28},32,i);
    }
    runtime_.argc_cell = runtime_base_.value;
    runtime_.argv_cell = runtime_base_.value + 8;
    runtime_.environ_cell = runtime_base_.value + 16;
    runtime_.fmode_cell = runtime_base_.value + 24;
    runtime_.commode_cell = runtime_base_.value + 32;
    runtime_.errno_cell = runtime_base_.value + 40;
    runtime_.wargv_cell = runtime_base_.value + 56;
    runtime_.wenviron_cell = runtime_base_.value + 64;
    initialize_crt_lowio(memory);
    runtime_.command_ansi = runtime_allocate(memory, bytes_of(utf16_to_utf8(command_line)));
    std::vector<std::uint64_t> pointers;
    std::vector<std::uint64_t> wide_pointers;
    for (const auto& item : parse_windows_arguments(command_line)) {
        pointers.push_back(runtime_allocate(memory, bytes_of(utf16_to_utf8(item))));
        wide_pointers.push_back(runtime_allocate(memory, utf16_bytes(item)));
    }
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto argv = runtime_allocate(memory, {}, (pointers.size() + 1) * unit);
    for (std::size_t i = 0; i < pointers.size(); ++i) memory.store(GuestAddress{argv + i * unit}, bits, pointers[i]);
    memory.store(GuestAddress{runtime_.argc_cell}, 32, pointers.size()); memory.store(GuestAddress{runtime_.argv_cell}, bits, argv);
    memory.store(GuestAddress{runtime_.environ_cell}, bits, runtime_allocate(memory, {}, unit));
    const auto wargv = runtime_allocate(memory, {}, (wide_pointers.size() + 1) * unit);
    for (std::size_t i = 0; i < wide_pointers.size(); ++i) memory.store(GuestAddress{wargv + i * unit}, bits, wide_pointers[i]);
    memory.store(GuestAddress{runtime_.wargv_cell}, bits, wargv);
    memory.store(GuestAddress{runtime_.wenviron_cell}, bits, runtime_allocate(memory, {}, unit));
    memory.store(GuestAddress{runtime_base_.value + 48}, bits, runtime_.command_ansi);
    // CRT 資料匯出保存指標，而非字串本身；與 GetCommandLineW 共用程序文字。
    memory.store(GuestAddress{runtime_base_.value + 104}, bits, command_line_address_.value);
    // 資料匯出可在第一次時間 API 呼叫前由 IAT 直接讀取。
    runtime_.timezone_name=runtime_allocate(memory,std::array<std::uint8_t,4>{'U','T','C',0});
    memory.store(GuestAddress{runtime_base_.value+88},bits,runtime_.timezone_name);
    memory.store(GuestAddress{runtime_base_.value+88+unit},bits,runtime_.timezone_name);
    // 程序看到的是一致的 Windows 安裝位置；宿主路徑只由 filesystem 映射決定。
    runtime_.environment=guest_default_environment(true);runtime_.crt_environment=runtime_.environment;
    rebuild_crt_environment(memory,GuestState{});
    std::u16string key(1,2);runtime_.registry.keys.try_emplace(key);
    for(const auto* part:{u"software",u"microsoft",u"windows",u"currentversion"}) {key+=u'\\';key+=part;runtime_.registry.keys.try_emplace(key);}
    for(const auto* name:{u"programfilesdir",u"programw6432dir"}) {
        auto data=utf16_bytes(u"C:\\Program Files");runtime_.registry.bytes+=data.size();++runtime_.registry.values;
        runtime_.registry.keys[key][name]={1,std::move(data)};
    }
    {startup_profile::Scope network("services","network_configuration",true);
        if(runtime_.network.external_io && !runtime_.network.configuration->captured)
            (void)snapshot_network_configuration(*runtime_.network.configuration);
        (void)initialize_network_registry(runtime_.registry,*runtime_.network.configuration);}
}
void WindowsServices::configure_image(GuestState& state, GuestMemory& memory, RuntimeImage image) {
    validate_unwind(memory, image);
    configure_image_after_unwind_validation(state,memory,std::move(image));
}
void WindowsServices::configure_image_after_unwind_validation(GuestState& state, GuestMemory& memory, RuntimeImage image) {
    runtime_.image = std::move(image);
    std::vector<std::uint64_t> builtins;const auto native=load_module(memory,u"ntdll.dll",0,builtins);runtime_.main_dependencies.insert(native);
    install_guard(memory,runtime_.image);
    memory.map(teb_base_, x86_?0x4000:GUEST_CRT_TEB_SIZE, read_write); memory.map(peb_base_, 0x1000, read_write);
    publish_loader_lock(memory,peb_base_.value,x86_,0,0);
    (void)process_debugger::self(memory,peb_base_,runtime_.network.external_io);
    memory.map(exception_base_, 16 * 8192, read_write);
    if (x86_) {
        state.fs_base = teb_base_.value;
        state.segments={0x23,0x1b,0x23,0x23,0x3b,0};
        memory.store(GuestAddress{teb_base_.value},32,UINT32_MAX);
        memory.store(GuestAddress{teb_base_.value+4},32,runtime_.image.stack_base+runtime_.image.stack_size);
        memory.store(GuestAddress{teb_base_.value+8},32,runtime_.image.stack_base);
        memory.store(GuestAddress{teb_base_.value+0x18},32,teb_base_.value);
        memory.store(GuestAddress{teb_base_.value+0x20},32,process_id()); memory.store(GuestAddress{teb_base_.value+0x24},32,1);
        memory.store(GuestAddress{teb_base_.value+0x30},32,peb_base_.value);
        memory.store(GuestAddress{peb_base_.value+8},32,runtime_.image.base);
        memory.store(GuestAddress{peb_base_.value+0x18},32,process_heap);
        const auto parameters=runtime_allocate(memory,{},0x2c4);
        memory.store(GuestAddress{peb_base_.value+0x10},32,parameters);
        memory.store(GuestAddress{parameters},32,0x2c4);memory.store(GuestAddress{parameters+4},32,0x2c4);memory.store(GuestAddress{parameters+8},32,1);
        for(unsigned i=0;i<3;++i)memory.store(GuestAddress{parameters+0x18+i*4},32,0x100+i);
        const auto unicode=[&](std::uint64_t offset,std::u16string_view text){const auto data=utf16_bytes(text);const auto buffer=runtime_allocate(memory,data);memory.store(GuestAddress{parameters+offset},16,data.size()-2);memory.store(GuestAddress{parameters+offset+2},16,data.size());memory.store(GuestAddress{parameters+offset+4},32,buffer);};
        unicode(0x24,u"C:\\");unicode(0x38,runtime_.image.path);unicode(0x40,read_utf16(memory,command_line_address_));
        const auto vector=runtime_allocate(memory,{},64*4);memory.store(GuestAddress{teb_base_.value+0x2c},32,vector);
        memory.store(GuestAddress{teb_base_.value+0xf94},32,teb_base_.value+0x2000);
        if(runtime_.image.tls_index){
            const auto tls=runtime_allocate(memory,runtime_.image.tls_data,std::max<std::size_t>(1,runtime_.image.tls_data.size()+runtime_.image.tls_zero),runtime_.image.tls_alignment);
            memory.store(GuestAddress{runtime_.image.tls_index},32,0);memory.store(GuestAddress{vector},32,tls);
        }
    } else {
    state.gs_base = teb_base_.value;
    state.segments={0x2b,0x33,0x2b,0x2b,0x53,0x2b};
    memory.store(GuestAddress{teb_base_.value + 8}, 64, runtime_.image.stack_base + runtime_.image.stack_size);
    memory.store(GuestAddress{teb_base_.value + 16}, 64, runtime_.image.stack_base);
    memory.store(GuestAddress{teb_base_.value + 0x30}, 64, teb_base_.value);
    memory.store(GuestAddress{teb_base_.value + 0x40}, 64, process_id()); memory.store(GuestAddress{teb_base_.value + 0x48}, 64, 1);
    memory.store(GuestAddress{teb_base_.value + 0x60}, 64, peb_base_.value);
    memory.store(GuestAddress{peb_base_.value + 0x10}, 64, runtime_.image.base);
    // RTL_USER_PROCESS_PARAMETERS64：供靜態 CRT 與原生 Win32 程式讀取程序資料。
    const auto parameters=runtime_allocate(memory,{},0x448);memory.store(GuestAddress{peb_base_.value+0x20},64,parameters);
    memory.store(GuestAddress{parameters},32,0x448);memory.store(GuestAddress{parameters+4},32,0x448);memory.store(GuestAddress{parameters+8},32,1);
    for(unsigned i=0;i<3;++i)memory.store(GuestAddress{parameters+0x20+i*8},64,0x100+i);
    const auto unicode=[&](std::uint64_t offset,std::u16string_view text){const auto data=utf16_bytes(text);const auto buffer=runtime_allocate(memory,data);memory.store(GuestAddress{parameters+offset},16,data.size()-2);memory.store(GuestAddress{parameters+offset+2},16,data.size());memory.store(GuestAddress{parameters+offset+8},64,buffer);};
    unicode(0x38,u"C:\\");const auto argv=memory.load(GuestAddress{runtime_.wargv_cell},64);const auto image_name=memory.load(GuestAddress{argv},64);unicode(0x60,runtime_.image.path.empty()?(image_name?read_utf16(memory,GuestAddress{image_name}):std::u16string{}):runtime_.image.path);unicode(0x70,read_utf16(memory,command_line_address_));
    memory.store(GuestAddress{teb_base_.value + 0x1780}, 64, teb_base_.value + 0x2000);
    const auto tls_vector = runtime_allocate(memory, {}, 64 * 8);
    memory.store(GuestAddress{teb_base_.value + 0x58}, 64, tls_vector);
    if (runtime_.image.tls_index) {
        const auto tls = runtime_allocate(memory, runtime_.image.tls_data, std::max<std::size_t>(1, runtime_.image.tls_data.size() + runtime_.image.tls_zero),runtime_.image.tls_alignment);
        memory.store(GuestAddress{runtime_.image.tls_index}, 32, 0); memory.store(GuestAddress{tls_vector}, 64, tls);
    }
    }
    if (!runtime_.image.tls_callbacks.empty()) state.rip = startup_entry_;
    GuestThread main_thread; main_thread.affinity=runtime_.process_affinity; main_thread.context = state; main_thread.teb = teb_base_.value;
    main_thread.exception_area = exception_base_.value; main_thread.stack_base = runtime_.image.stack_base;
    main_thread.stack_size = runtime_.image.stack_size; main_thread.errno_cell = runtime_.errno_cell; main_thread.entry=runtime_.image.entry;
    initialize_stack_guard(memory,main_thread.stack_base,main_thread.stack_size,main_thread.stack_guard);
    memory.store(GuestAddress{main_thread.teb+(x86_?8U:16U)},x86_?32:64,main_thread.stack_guard.limit(main_thread.stack_base));
    threads_.items.emplace(1, std::move(main_thread));
    sync_crt_thread_state(memory,1);
    sync_fls_mirror(memory, teb_base_.value, threads_.items.at(1).fls_values);
}
namespace {
// 小物件觸及的 4 KiB 頁（最多兩頁）；與一般配置／釋放的 edge 計算一致。
std::array<std::uint64_t,2> heap_block_pages(std::uint64_t address,std::size_t size,unsigned& count){
    constexpr std::uint64_t page=GuestMemory::guest_page_size,mask=page-1;
    std::array<std::uint64_t,2> pages{};count=0;const auto end=address+size;
    if((address&mask) || size<page)pages[count++]=address&~mask;
    if((end&mask) && (!count || pages[0]!=(end&~mask)))pages[count++]=end&~mask;
    return pages;
}
}
std::uint64_t WindowsServices::allocate_heap(GuestMemory& memory, std::uint64_t requested, std::uint64_t heap) {
    const auto fail=[&](const char* reason)->std::uint64_t {
        if(std::getenv("X64ARM_TRACE_HEAP_FAILURES"))std::fprintf(stderr,"heap_failure pid=%u reason=%s requested=%llu objects=%zu remaining=%zu next=0x%llx\n",process_id(),reason,static_cast<unsigned long long>(requested),allocations_.size(),memory.remaining_capacity(),static_cast<unsigned long long>(next_allocation_));
        return 0;
    };
    if (requested > memory.limit())return fail("request_limit");
    if(!runtime_.next_heap_generation || runtime_.next_heap_generation>=UINT64_C(0x8000000000000000))return fail("generation_limit");
    // growable heap 依可用記憶體與位址空間配置；存活物件數不是 Windows heap 的固定上限。
    const auto size = static_cast<std::size_t>((std::max(UINT64_C(1), requested) + 15) & ~UINT64_C(15));
    const auto limit=user_address_limit();
    if (size >= limit) return fail("address_limit");
    // 一般配置失敗前先歸還快速清單，讓保留的頁面與空洞也能參與搜尋。
    const auto reclaim=[&]{if(heap_quick_blocks_.empty())return false;flush_heap_quick(memory);return true;};
    if(heap!=process_heap){
        const auto owner=runtime_.private_heaps.find(heap);if(owner==runtime_.private_heaps.end())return fail("heap_handle");
        if(owner->second.first && size>owner->second.first-owner->second.second)return fail("heap_capacity");
    }
    // 同一 heap、同級距的已釋放小物件直接重用：頁面參照仍保留，重新登錄為存活物件（私有 heap 另加回擁有者與用量）
    // 並依既有語義清零。
    if(size<=heap_quick_classes*16)if(const auto bins=heap_quick_bins_.find(heap);bins!=heap_quick_bins_.end()){
        auto& list=bins->second.lists[size/16-1];
        while(!list.empty()){
            const auto address=list.back();list.pop_back();
            const auto quick=heap_quick_blocks_.find(address);
            if(quick==heap_quick_blocks_.end() || quick->second.size!=size || quick->second.heap!=heap)throw HostCallFault("heap 快速重用清單不一致");
            heap_quick_blocks_.erase(quick);
            unsigned page_count=0;const auto pages=heap_block_pages(address,size,page_count);
            for(unsigned i=0;i<page_count;++i){const auto entry=heap_quick_pages_.find(pages[i]);if(entry==heap_quick_pages_.end() || !entry->second)throw HostCallFault("heap 快速重用頁計數不一致");if(!--entry->second)heap_quick_pages_.erase(entry);}
            const auto [entry,inserted]=allocations_.emplace(address,size);
            if(!inserted)throw HostCallFault("heap 快速重用區塊與存活物件重疊");
            if(heap!=process_heap){runtime_.heap_owners.emplace(address,heap);runtime_.private_heaps.at(heap).second+=size;}
            static constexpr std::array<std::uint8_t,heap_quick_classes*16> zero{};
            try{
                memory.write(GuestAddress{address},std::span(zero).first(size));
                runtime_.heap_generations.emplace(address,runtime_.next_heap_generation++);
                return address;
            }catch(const MemoryFault&){
                // 客體改過該頁保護或解除映射：交回一般釋放流程處理頁面參照與用量，改用其他區塊或一般配置。
                (void)release_heap_block(memory,entry);
            }
        }
    }
    constexpr std::uint64_t page=GuestMemory::guest_page_size,mask=page-1;
    const auto find_space=[&](std::uint64_t start,std::uint64_t stop)->std::uint64_t {
      auto address=start;
      while(address<stop && size<=stop-address){
        const auto begin=address&~mask,end=(address+size+mask)&~mask;
        std::uint64_t conflict_end=0;
        auto object=allocations_.upper_bound(address);
        if(object!=allocations_.begin()){
            const auto previous=std::prev(object);
            if(previous->first+previous->second>address)conflict_end=previous->first+previous->second;
        }
        if(!conflict_end && object!=allocations_.end() && object->first<address+size)conflict_end=object->first+object->second;
        if(!conflict_end && !heap_quick_blocks_.empty()){
            const auto quick=heap_quick_blocks_.upper_bound(address);
            if(quick!=heap_quick_blocks_.begin()){const auto previous=std::prev(quick);if(previous->first+previous->second.size>address)conflict_end=previous->first+previous->second.size;}
            if(!conflict_end && quick!=heap_quick_blocks_.end() && quick->first<address+size)conflict_end=quick->first+quick->second.size;
        }
        // VirtualAlloc 的 MEM_RESERVE 尚無 backing，也不可被 heap 偷用。
        auto reserved=runtime_.virtual_regions.upper_bound(begin);
        if(!conflict_end && reserved!=runtime_.virtual_regions.begin()){
            const auto previous=std::prev(reserved);
            if(previous->first+previous->second.size>begin)conflict_end=previous->first+previous->second.size;
        }
        if(!conflict_end && reserved!=runtime_.virtual_regions.end() && reserved->first<end)conflict_end=reserved->first+reserved->second.size;
        if(!conflict_end){
            auto mapping=memory.query(GuestAddress{begin});if(!mapping)mapping=memory.next_mapping(GuestAddress{begin});
            while(mapping && mapping->base.value<end){
                // 回收區間的左右邊界都可能與存活物件共頁；完整頁、外部映射及改過保護的頁仍不可重用。
                const auto base=mapping->base.value;
                const bool edge=(base==begin && ((address&mask) || size<page)) || ((address+size)&mask && base==((address+size)&~mask));
                const bool heap_edge=edge && mapping->size==page && mapping->permissions==read_write && runtime_.heap_pages.contains(base);
                if(!heap_edge){conflict_end=base+mapping->size;break;}
                mapping=memory.next_mapping(GuestAddress{mapping->base.value+mapping->size});
            }
        }
        if(!conflict_end)return address;
        if(conflict_end>=stop)return 0;
        address=(conflict_end+mask)&~mask;
      }
      return 0;
    };
    std::uint64_t address=0;
    for(auto entry=free_heap_sizes_.lower_bound({size,0});entry!=free_heap_sizes_.end();++entry){
        address=find_space(entry->second,entry->second+entry->first);
        if(address)break;
    }
    if(!address)address=find_space(next_allocation_,limit);
    // heap、VirtualAlloc、section view 與載入器共用位址空間；固定區間或單向 cursor
    // 不得在仍有可用空洞時回報 OOM。回繞後仍逐一避開映射、保留區及存活物件。
    if(!address)address=find_space(UINT64_C(0x10000),limit);
    if(!address){if(reclaim())return allocate_heap(memory,requested,heap);return fail("address_limit");}
    const auto end=address+size,full_begin=(address+mask)&~mask,full_end=end&~mask;
    const auto full_size=full_end>full_begin?static_cast<std::size_t>(full_end-full_begin):0;
    std::array<std::uint64_t,2> edges{};unsigned count=0;
    if((address&mask) || size<page)edges[count++]=address&~mask;
    if((end&mask) && (!count || edges[0]!=(end&~mask)))edges[count++]=end&~mask;
    std::size_t required=full_size;for(unsigned i=0;i<count;++i)if(!runtime_.heap_pages.contains(edges[i]))required+=page;
    if(required>memory.remaining_capacity()){if(reclaim())return allocate_heap(memory,requested,heap);return fail("committed_limit");}
    if(full_size && !memory.range_available(GuestAddress{full_begin},full_size))return fail("range_collision");
    for(unsigned i=0;i<count;++i)if(!runtime_.heap_pages.contains(edges[i]) && !memory.range_available(GuestAddress{edges[i]},page))return fail("page_collision");
    std::array<bool,2> added{},mapped{};bool full_mapped=false,registered=false;
    std::vector<decltype(free_heap_blocks_)::node_type> reused_nodes;
    std::vector<decltype(free_heap_sizes_)::node_type> reused_sizes;
    std::array<std::uint64_t,2> fragments{};unsigned fragment_count=0;
    const auto add_fragment=[&](std::uint64_t begin,std::size_t bytes){
        const auto [entry,inserted]=free_heap_blocks_.emplace(begin,bytes);
        if(!inserted)throw HostCallFault("heap 分割區間重複");
        try{free_heap_sizes_.emplace(bytes,begin);}catch(...){free_heap_blocks_.erase(entry);throw;}
        fragments[fragment_count++]=begin;
    };
    try {
        // 全域搜尋可能跨過多個已釋放物件與已解除的外部保留區；一併扣除所有
        // 相交回收區間，避免舊 free-list 節點日後與新物件重疊。
        auto range_begin=free_heap_blocks_.upper_bound(address);
        if(range_begin!=free_heap_blocks_.begin()){const auto before=std::prev(range_begin);if(before->first+before->second>address)range_begin=before;}
        auto range_end=range_begin;while(range_end!=free_heap_blocks_.end() && range_end->first<end)++range_end;
        reused_nodes.reserve(static_cast<std::size_t>(std::distance(range_begin,range_end)));
        reused_sizes.reserve(reused_nodes.capacity());
        while(range_begin!=range_end){
            auto node=free_heap_blocks_.extract(range_begin++);
            const auto original_begin=node.key(),original_end=original_begin+node.mapped();
            reused_sizes.push_back(free_heap_sizes_.extract({node.mapped(),original_begin}));
            reused_nodes.push_back(std::move(node));
            if(address>original_begin)add_fragment(original_begin,static_cast<std::size_t>(address-original_begin));
            if(end+16<original_end)add_fragment(end+16,static_cast<std::size_t>(original_end-end-16));
        }
        const auto [allocation_entry,allocation_inserted]=allocations_.emplace(address,size);(void)allocation_entry;
        if(!allocation_inserted)throw HostCallFault("heap 回收區間與存活物件重疊");registered=true;
        runtime_.heap_generations.emplace(address,runtime_.next_heap_generation++);
        if(heap!=process_heap)runtime_.heap_owners.emplace(address,heap);
        for(unsigned i=0;i<count;++i){
            auto [entry,inserted]=runtime_.heap_pages.try_emplace(edges[i],0);(void)entry;added[i]=inserted;
            if(inserted){memory.map(GuestAddress{edges[i]},page,read_write);mapped[i]=true;}
        }
        if(full_size){memory.map(GuestAddress{full_begin},full_size,read_write);full_mapped=true;}
        // 保留既有配置的 zero-fill 行為；只需清除已存在共頁的新物件區間。
        static constexpr std::array<std::uint8_t,page> zero{};
        for(unsigned i=0;i<count;++i)if(!added[i]){
            const auto first=std::max(address,edges[i]),last=std::min(end,edges[i]+page);
            memory.write(GuestAddress{first},std::span(zero).first(static_cast<std::size_t>(last-first)));
        }
    } catch (...) {
        if(full_mapped)memory.unmap(GuestAddress{full_begin},full_size);
        for(unsigned i=0;i<count;++i)if(added[i]){if(mapped[i])memory.unmap(GuestAddress{edges[i]},page);runtime_.heap_pages.erase(edges[i]);}
        if(registered)allocations_.erase(address);runtime_.heap_owners.erase(address);runtime_.heap_generations.erase(address);
        for(unsigned i=0;i<fragment_count;++i){const auto fragment=free_heap_blocks_.find(fragments[i]);free_heap_sizes_.erase({fragment->second,fragment->first});free_heap_blocks_.erase(fragment);}
        for(auto& node:reused_nodes)free_heap_blocks_.insert(std::move(node));
        for(auto& node:reused_sizes)free_heap_sizes_.insert(std::move(node));
        try{throw;}catch(const std::bad_alloc&){return fail("host_allocation");}catch(...){throw;}
    }
    for(unsigned i=0;i<count;++i)++runtime_.heap_pages.at(edges[i]);
    if(heap!=process_heap)runtime_.private_heaps.at(heap).second+=size;
    next_allocation_=std::max(next_allocation_,end+16);
    return address;
}
std::uint64_t WindowsServices::reallocate_heap(GuestMemory& memory,std::uint64_t address,std::uint64_t requested,const GuestState& state) {
    const auto source=allocations_.find(address);
    if(source==allocations_.end())throw HostCallFault("重配收到非配置起始地址");
    const auto owner=runtime_.heap_owners.find(address);const auto heap=owner==runtime_.heap_owners.end()?process_heap:owner->second;
    if(requested>memory.limit())return 0;
    if(heap!=process_heap){const auto& limits=runtime_.private_heaps.at(heap);const auto rounded=(std::max(UINT64_C(1),requested)+15)&~UINT64_C(15);if(limits.first && rounded>limits.first-(limits.second-source->second))return 0;}
    const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(source->second,requested));
    const auto replacement=allocate_heap(memory,requested);if(!replacement)return 0;
    try {
        // 不為舊物件另配同等大小的宿主暫存，且只在所有資料複製成功後釋放舊塊。
        // read 成功後才使用該 span；毋須為每次 realloc 預先清零整個暫存。
        std::array<std::uint8_t,65536> buffer;
        for(std::size_t offset=0;offset<count;) {
            const auto bytes=std::span(buffer).first(std::min(buffer.size(),count-offset));
            memory.read(GuestAddress{address+offset},bytes);memory.write(GuestAddress{replacement+offset},bytes);
            offset+=bytes.size();
        }
        (void)free_heap(memory,address,state);
    } catch(const std::bad_alloc&) {(void)free_heap(memory,replacement,state);return 0;}
      catch(...) {(void)free_heap(memory,replacement,state);throw;}
    if(heap!=process_heap){runtime_.heap_owners[replacement]=heap;runtime_.private_heaps.at(heap).second+=allocations_.at(replacement);}
    return replacement;
}
namespace {
// 與 guest_scalar_math_x64.s 共用：4 KiB 內保留 16-byte 級距，
// 其後以 256-byte 級距到 64 KiB；含對齊標頭的 497 個串列頭仍容納於原狀態頁。
using namespace detail::guest_heap_layout;
static_assert(guest_heap_class(guest_heap_limit+guest_heap_aligned_header)*8+0x40+8<=GuestMemory::guest_page_size,"配置區串列頭須容納於狀態頁");
std::uint64_t guest_heap_request_limit() {
    static const bool medium=[] {const auto* setting=std::getenv("X64ARM_GUEST_HEAP_MEDIUM");
        return !setting || std::string_view(setting)!="0";}();
    return medium?guest_heap_limit:guest_heap_small_limit;
}
// callback 區的頁面偏移：0x80000 記憶體常式、0x81000 純量 CRT 常式、0x82000 執行緒常式、0x8b000 I/O、
// 0x8c000 配置區狀態、0x8d000 CFG 常式、0x8e000–0x95fff CFG 表、0x96000 locale 鎖狀態。
constexpr std::uint64_t guest_heap_state_page_offset=0x8c000,guest_locale_lock_page_offset=0x96000;
bool guest_locale_lock_enabled(){
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_GUEST_LOCALE_LOCK");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
bool guest_heap_enabled(){
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_GUEST_HEAP");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
std::uint64_t guest_heap_block_size_impl(GuestMemory& memory,std::uint64_t address) {
    const auto chunk=address&~(guest_heap_chunk-1);
    if(memory.load(GuestAddress{chunk},64)!=guest_heap_magic)throw HostCallFault("配置區指標不屬於任何 chunk");
    const auto cls=memory.load(GuestAddress{chunk+8},64),block=memory.load(GuestAddress{chunk+16},64);
    if(!valid_block(address,cls,block))throw HostCallFault("配置區指標不是區塊起點");
    return block;
}
AlignedAllocation guest_heap_aligned_allocation(GuestMemory& memory,std::uint64_t address) {
    const auto base=address-guest_heap_aligned_header;
    const auto block=guest_heap_block_size_impl(memory,base);
    const auto size=memory.load(GuestAddress{base},64),alignment=memory.load(GuestAddress{base+8},64);
    if(block<=guest_heap_aligned_header || size>block-guest_heap_aligned_header ||
       !alignment || alignment>16 || (alignment&(alignment-1)) || (address&(alignment-1)))
        throw HostCallFault("對齊配置標頭不符");
    return {base,size,alignment,0};
}
}
namespace {
bool guest_heap_x86_requested(){
    // PE32 的 process heap 小型配置改用同一套 chunk 級距配置區（微基準每次 HeapAlloc／HeapFree 約 630→110 ns）；
    // X64ARM_GUEST_HEAP_X86=0 停用，恢復一般 heap。
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_GUEST_HEAP_X86");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
}
std::uint64_t WindowsServices::guest_heap_reserve_chunk(GuestMemory& memory) {
    // x86 不預留大段位址：每次只找一段 256 KiB 對齊、未映射、未保留的位址，登錄後才映射；
    // heap、VirtualAlloc 與載入器都會避開 virtual_regions 與既有映射。
    const auto limit=std::min<std::uint64_t>(user_address_limit(),UINT64_C(1)<<32);
    const auto search=[&](std::uint64_t from)->std::uint64_t {
        for(auto base=(from+guest_heap_chunk-1)&~(guest_heap_chunk-1);base+guest_heap_chunk<=limit;base+=guest_heap_chunk){
            if(!memory.range_available(GuestAddress{base},guest_heap_chunk))continue;
            auto region=runtime_.virtual_regions.upper_bound(base+guest_heap_chunk-1);
            if(region!=runtime_.virtual_regions.begin() && std::prev(region)->first+std::prev(region)->second.size>base)continue;
            return base;
        }
        return 0;
    };
    auto base=search(guest_heap_next_);
    if(!base && guest_heap_next_>UINT64_C(0x10000))base=search(UINT64_C(0x10000));
    if(!base)return 0;
    runtime_.virtual_regions.emplace(base,VirtualRegion{guest_heap_chunk,4U});
    guest_heap_chunk_bits_[base>>24]|=UINT64_C(1)<<((base>>18)&63U);
    guest_heap_next_=base+guest_heap_chunk;
    return base;
}
bool WindowsServices::guest_heap_setup(GuestMemory& memory) {
    if(!guest_heap_enabled() || guest_heap_base_ || guest_heap_chunked_)return false;
    if(x86_){
        if(!guest_heap_x86_requested())return false;
        const auto state=GuestAddress{callback_return_.value+guest_heap_state_page_offset};
        if(!memory.range_available(state,GuestMemory::guest_page_size))return false;
        // x86 沒有客體常式：狀態頁只存鎖與各級距串列頭；+24 為請求上限，與 x64 相同位置。
        std::vector<std::uint8_t> page(GuestMemory::guest_page_size,0);
        for(unsigned n=0;n<8;++n)page[24+n]=static_cast<std::uint8_t>(guest_heap_small_limit>>(8*n));
        memory.map(state,GuestMemory::guest_page_size,read_write,page);
        guest_heap_state_=state;guest_heap_chunked_=true;guest_heap_next_=runtime_.next_virtual;
        return true;
    }
    // 保留一段客體視窗內未映射、未保留的位址；以 virtual_regions 登錄，heap 與 VirtualAlloc 不會再使用。
    const auto limit=std::min<std::uint64_t>(user_address_limit(),GuestMemory::native_mmu_window);
    for(auto base=UINT64_C(0x1000000000);base+guest_heap_reservation<=limit;base+=guest_heap_reservation){
        if(!memory.range_available(GuestAddress{base},guest_heap_reservation))continue;
        auto region=runtime_.virtual_regions.upper_bound(base+guest_heap_reservation-1);
        if(region!=runtime_.virtual_regions.begin() && std::prev(region)->first+std::prev(region)->second.size>base)continue;
        const auto state=GuestAddress{callback_return_.value+guest_heap_state_page_offset};
        std::vector<std::uint8_t> page(GuestMemory::guest_page_size,0);
        const auto put=[&](unsigned at,std::uint64_t value){for(unsigned n=0;n<8;++n)page[at+n]=static_cast<std::uint8_t>(value>>(8*n));};
        put(8,base);put(16,guest_heap_reservation);put(24,guest_heap_request_limit());
        memory.map(state,GuestMemory::guest_page_size,read_write,page);
        runtime_.virtual_regions.emplace(base,VirtualRegion{guest_heap_reservation,4U});
        guest_heap_state_=state;guest_heap_base_=base;guest_heap_size_=guest_heap_reservation;guest_heap_next_=base;
        return true;
    }
    return false;
}
bool WindowsServices::guest_locale_lock_setup(GuestMemory& memory) {
    // locale 鎖狀態格：+0 擁有者公開執行緒編號、+4 遞迴次數、+8 宿主等待者數；客體常式與服務共用此唯一狀態。
    if(!guest_locale_lock_enabled() || x86_)return false;
    if(guest_locale_lock_state_.value)return true;
    const auto state=GuestAddress{callback_return_.value+guest_locale_lock_page_offset};
    memory.map(state,GuestMemory::guest_page_size,read_write,std::vector<std::uint8_t>(GuestMemory::guest_page_size,0));
    guest_locale_lock_state_=state;return true;
}
std::uint64_t WindowsServices::guest_heap_allocate(GuestMemory& memory, std::uint64_t size, std::uint64_t heap) {
    // 宿主補充器另容納對齊標頭；客體常式仍依狀態頁上的 payload 上限判斷。
    // x86 只收 4 KiB 內的級距，限制 32 位元位址空間中各級距 chunk 的總占用。
    const auto request_limit=guest_heap_chunked_?guest_heap_small_limit:guest_heap_request_limit();
    if((!guest_heap_base_ && !guest_heap_chunked_) || size>request_limit+guest_heap_aligned_header)return 0;
    const auto cls=guest_heap_class(size),block=guest_heap_class_size(cls);
    // 新 chunk：標頭（+24 擁有者，process heap 為 0）＋整段空閒鏈一次映射；第一個區塊直接回傳，其餘成為級距串列。
    const auto fill=[&](std::uint64_t owner)->std::uint64_t {
        const bool room=guest_heap_chunked_ || guest_heap_next_+guest_heap_chunk<=guest_heap_base_+guest_heap_size_;
        const auto chunk=room && memory.remaining_capacity()>=guest_heap_chunk?
            (guest_heap_chunked_?guest_heap_reserve_chunk(memory):guest_heap_next_):0;
        if(!chunk)return 0;
        std::vector<std::uint8_t> image(guest_heap_chunk,0);
        const auto put=[&](std::uint64_t at,std::uint64_t value){for(unsigned n=0;n<8;++n)image[at+n]=static_cast<std::uint8_t>(value>>(8*n));};
        put(0,guest_heap_magic);put(8,cls);put(16,block);put(24,owner);
        const auto count=(guest_heap_chunk-guest_heap_header)/block;
        for(std::uint64_t i=0;i+1<count;++i)put(guest_heap_header+i*block,chunk+guest_heap_header+(i+1)*block);
        memory.map(GuestAddress{chunk},guest_heap_chunk,read_write,image);
        if(!guest_heap_chunked_)guest_heap_next_+=guest_heap_chunk;
        return chunk+guest_heap_header;
    };
    if(heap!=process_heap){
        // 私有 heap 只在 x86 chunk 模式啟用；宿主串列頭由協調者於一致邊界補充。
        const auto found=guest_private_heaps_.find(heap);
        if(!guest_heap_chunked_ || found==guest_private_heaps_.end() || found->second.heads.size()<=cls)return 0;
        auto& head=found->second.heads[cls];
        auto result=head?head:fill(heap);
        if(result){head=memory.load(GuestAddress{result},64);++guest_heap_mutations_;}
        return result;
    }
    const GuestAddress lock{guest_heap_state_.value},head{guest_heap_state_.value+guest_heap_heads+cls*8};
    // 有界自旋：鎖由其他客體執行緒短暫持有；取不到就改走一般 heap，不等待。
    bool locked=false;
    for(unsigned n=0;n<4096 && !locked;++n)locked=memory.atomic_rmw(lock,32,GuestMemory::AtomicRmw::CompareExchange,1,0)==0;
    if(!locked)return 0;
    std::uint64_t result=0;
    try{
        result=memory.load(head,64);
        if(!result)result=fill(0);
        if(result){memory.store(head,64,memory.load(GuestAddress{result},64));++guest_heap_mutations_;}
    }catch(...){memory.store(lock,32,0);throw;}
    memory.store(lock,32,0);
    return result;
}
std::uint64_t WindowsServices::guest_heap_block_size(GuestMemory& memory, std::uint64_t address) {
    return guest_heap_block_size_impl(memory,address);
}
std::uint64_t WindowsServices::guest_heap_owner(GuestMemory& memory, std::uint64_t address) {
    const auto owner=memory.load(GuestAddress{(address&~(guest_heap_chunk-1))+24},64);
    return owner?owner:process_heap;
}
void WindowsServices::guest_heap_free(GuestMemory& memory, std::uint64_t address) {
    const auto block=guest_heap_block_size(memory,address);
    if(const auto owner=guest_heap_owner(memory,address);owner!=process_heap){
        const auto found=guest_private_heaps_.find(owner);
        if(found==guest_private_heaps_.end() || found->second.heads.size()<=guest_heap_class(block))throw HostCallFault("配置區區塊的私有 heap 不存在");
        auto& head=found->second.heads[guest_heap_class(block)];
        memory.store(GuestAddress{address},64,head);head=address;++guest_heap_mutations_;
        return;
    }
    const GuestAddress lock{guest_heap_state_.value},head{guest_heap_state_.value+guest_heap_heads+guest_heap_class(block)*8};
    bool locked=false;
    for(std::uint64_t n=0;n<(UINT64_C(1)<<24) && !locked;++n)locked=memory.atomic_rmw(lock,32,GuestMemory::AtomicRmw::CompareExchange,1,0)==0;
    if(!locked)throw HostCallFault("配置區鎖長時間被持有");
    try{memory.store(GuestAddress{address},64,memory.load(head,64));memory.store(head,64,address);++guest_heap_mutations_;}
    catch(...){memory.store(lock,32,0);throw;}
    memory.store(lock,32,0);
}
bool WindowsServices::guest_heap_private_ready(std::uint64_t heap) {
    const auto found=guest_private_heaps_.find(heap);
    if(found==guest_private_heaps_.end())return false;
    auto& lists=found->second;
    if(!lists.heads.empty())return true;
    if(++lists.requests<1024)return false;
    lists.heads.assign(static_cast<std::size_t>(guest_heap_class(guest_heap_small_limit+guest_heap_aligned_header))+1,0);
    return true;
}
void WindowsServices::guest_heap_destroy(GuestMemory& memory, std::uint64_t heap) {
    // HeapDestroy 歸還此 heap 的所有 chunk：解除映射、清除位元圖並移除保留區登錄。
    const auto found=guest_private_heaps_.find(heap);
    if(found==guest_private_heaps_.end())return;
    if(!found->second.heads.empty()){
        for(std::size_t word=0;word<guest_heap_chunk_bits_.size();++word)
            for(auto bits=guest_heap_chunk_bits_[word];bits;bits&=bits-1U){
                const auto bit=static_cast<unsigned>(std::countr_zero(bits));
                const auto chunk=(static_cast<std::uint64_t>(word)<<24)|(static_cast<std::uint64_t>(bit)<<18);
                if(memory.load(GuestAddress{chunk+24},64)!=heap)continue;
                memory.unmap(GuestAddress{chunk},guest_heap_chunk);
                guest_heap_chunk_bits_[word]&=~(UINT64_C(1)<<bit);
                runtime_.virtual_regions.erase(chunk);
            }
        ++guest_heap_mutations_;
    }
    guest_private_heaps_.erase(found);
}
const std::vector<std::pair<std::uint64_t,std::uint64_t>>& WindowsServices::guest_heap_allocated_blocks(GuestMemory& memory, std::uint64_t heap) {
    // x86 沒有客體常式，配置與釋放都經宿主，變動計數準確；只在變動或改查其他 heap 後重建。
    // x64 客體常式直接改串列、不更新計數，每次重建並在客體鎖內讀取串列。
    // 已配置區塊逐一列出；同一 chunk 內相鄰的空閒區塊合併為一段，存入 guest_heap_walk_free_。
    if(guest_heap_chunked_ && guest_heap_walk_generation_==guest_heap_mutations_ && guest_heap_walk_heap_==heap)return guest_heap_walk_blocks_;
    guest_heap_walk_blocks_.clear();guest_heap_walk_free_.clear();
    if(!guest_heap_chunked_ && (!guest_heap_base_ || heap!=process_heap))return guest_heap_walk_blocks_;
    const auto owner=heap==process_heap?0:heap;
    std::vector<std::uint64_t> chunks;std::uint64_t total=0;
    const auto add_chunk=[&](std::uint64_t chunk){
        if(memory.load(GuestAddress{chunk},64)!=guest_heap_magic)return;
        const auto block=memory.load(GuestAddress{chunk+16},64);if(!block || memory.load(GuestAddress{chunk+24},64)!=owner)return;
        chunks.push_back(chunk);total+=(guest_heap_chunk-guest_heap_header)/block;
    };
    if(guest_heap_chunked_){
        for(std::size_t word=0;word<guest_heap_chunk_bits_.size();++word)
            for(auto bits=guest_heap_chunk_bits_[word];bits;bits&=bits-1U)
                add_chunk((static_cast<std::uint64_t>(word)<<24)|(static_cast<std::uint64_t>(std::countr_zero(bits))<<18));
    }else for(auto chunk=guest_heap_base_;chunk<guest_heap_next_;chunk+=guest_heap_chunk)add_chunk(chunk);
    // 空閒區塊沿各級距串列收集；長度以區塊總數為上限，損毀的串列不會造成無窮迴圈。
    std::unordered_set<std::uint64_t> free;
    const auto collect=[&](std::uint64_t at){for(std::uint64_t n=0;at && n<=total;++n){free.insert(at);at=memory.load(GuestAddress{at},64);}};
    if(heap==process_heap){
        const GuestAddress lock{guest_heap_state_.value};bool locked=false;
        for(std::uint64_t n=0;n<(UINT64_C(1)<<24) && !locked;++n)locked=memory.atomic_rmw(lock,32,GuestMemory::AtomicRmw::CompareExchange,1,0)==0;
        if(!locked)throw HostCallFault("配置區鎖長時間被持有");
        try{for(std::uint64_t cls=1;cls<=guest_heap_class(guest_heap_limit+guest_heap_aligned_header);++cls)
            collect(memory.load(GuestAddress{guest_heap_state_.value+guest_heap_heads+cls*8},64));}
        catch(...){memory.store(lock,32,0);throw;}
        memory.store(lock,32,0);
    }else if(const auto found=guest_private_heaps_.find(heap);found!=guest_private_heaps_.end())
        for(const auto at:found->second.heads)collect(at);
    for(const auto chunk:chunks){
        const auto block=memory.load(GuestAddress{chunk+16},64);
        for(auto at=chunk+guest_heap_header;at+block<=chunk+guest_heap_chunk;at+=block){
            if(!free.contains(at)){guest_heap_walk_blocks_.emplace_back(at,block);continue;}
            if(!guest_heap_walk_free_.empty() && guest_heap_walk_free_.back().first+guest_heap_walk_free_.back().second==at)
                guest_heap_walk_free_.back().second+=block;
            else guest_heap_walk_free_.emplace_back(at,block);
        }
    }
    std::sort(guest_heap_walk_blocks_.begin(),guest_heap_walk_blocks_.end());
    std::sort(guest_heap_walk_free_.begin(),guest_heap_walk_free_.end());
    guest_heap_walk_generation_=guest_heap_chunked_?guest_heap_mutations_:UINT64_MAX;guest_heap_walk_heap_=heap;
    return guest_heap_walk_blocks_;
}
bool WindowsServices::free_heap(GuestMemory& memory, std::uint64_t address, const GuestState& state) {
    if (!address) return true;
    if (guest_heap_contains(address)) {guest_heap_free(memory,address);return true;}
    const auto found = allocations_.find(address);
    if (found == allocations_.end()) return false;
    const auto active = [&](const GuestState& context) { const auto sp = context.reg(Gpr::Rsp); return sp <= UINT64_MAX - 40 && address < sp + 40 && sp < address + found->second; };
    if (active(state) || std::any_of(runtime_.callbacks.begin(), runtime_.callbacks.end(), [&](const CallbackFrame& frame) { return active(frame.saved); }))
        throw HostCallFault("free 不可釋放正在使用的呼叫框架");
    if(retain_heap_quick(found))return true;
    return release_heap_block(memory,found);
}
bool WindowsServices::retain_heap_quick(std::map<std::uint64_t,std::size_t>::iterator allocation){
    // 只保留 process heap 與可成長私有 heap 的小物件；有上限的私有 heap 仍立即歸還，維持其容量契約。
    // 驗證完成前預設停用；X64ARM_HEAP_QUICK=1 才啟用。停用時行為與一般釋放完全相同。
    static const bool enabled=[]{const auto* value=std::getenv("X64ARM_HEAP_QUICK");return value && std::string_view(value)=="1";}();
    const auto address=allocation->first;const auto size=allocation->second;
    if(!enabled || !size || size>heap_quick_classes*16 || (size&15U))return false;
    const auto owner=runtime_.heap_owners.find(address);const auto heap=owner==runtime_.heap_owners.end()?process_heap:owner->second;
    if(heap!=process_heap){const auto limits=runtime_.private_heaps.find(heap);if(limits==runtime_.private_heaps.end() || limits->second.first)return false;}
    auto& bins=heap_quick_bins_[heap];
    auto& activity=bins.activity[size/16-1];
    if(activity<heap_quick_activation){++activity;return false;}
    auto& list=bins.lists[size/16-1];
    if(list.size()>=heap_quick_depth)return false;
    // 觸及的每一頁都須仍有其他存活物件；否則走一般釋放，讓空頁立即解除映射。
    unsigned page_count=0;const auto pages=heap_block_pages(address,size,page_count);
    for(unsigned i=0;i<page_count;++i){
        const auto references=runtime_.heap_pages.find(pages[i]);if(references==runtime_.heap_pages.end())return false;
        const auto quick_pages=heap_quick_pages_.find(pages[i]);const auto retained=quick_pages==heap_quick_pages_.end()?0U:quick_pages->second;
        if(references->second<=retained+1U)return false;
    }
    if(list.capacity()<heap_quick_depth)list.reserve(heap_quick_depth);
    const auto [quick,inserted]=heap_quick_blocks_.emplace(address,HeapQuickBlock{size,heap});(void)quick;
    if(!inserted)throw HostCallFault("heap 快速重用區塊重複");
    list.push_back(address);
    for(unsigned i=0;i<page_count;++i)++heap_quick_pages_[pages[i]];
    // 比照一般釋放：私有 heap 扣除用量並移除擁有者，區塊在清單中視為已釋放。
    if(heap!=process_heap){runtime_.private_heaps.at(heap).second-=size;runtime_.heap_owners.erase(owner);}
    allocations_.erase(allocation);runtime_.heap_generations.erase(address);gui_.task_allocations.erase(address);
    return true;
}
void WindowsServices::evict_heap_quick_block(GuestMemory& memory,std::uint64_t address){
    // 自快速清單移出並走一般釋放；同頁其他快速區塊可能在一般釋放中連帶歸還。
    const auto quick=heap_quick_blocks_.find(address);if(quick==heap_quick_blocks_.end())return;
    const auto size=quick->second.size;const auto heap=quick->second.heap;
    const auto bins=heap_quick_bins_.find(heap);if(bins==heap_quick_bins_.end())throw HostCallFault("heap 快速重用清單不一致");
    auto& list=bins->second.lists[size/16-1];
    const auto item=std::find(list.begin(),list.end(),address);if(item==list.end())throw HostCallFault("heap 快速重用清單不一致");
    list.erase(item);heap_quick_blocks_.erase(quick);
    unsigned page_count=0;const auto pages=heap_block_pages(address,size,page_count);
    for(unsigned i=0;i<page_count;++i){const auto entry=heap_quick_pages_.find(pages[i]);if(entry==heap_quick_pages_.end() || !entry->second)throw HostCallFault("heap 快速重用頁計數不一致");if(!--entry->second)heap_quick_pages_.erase(entry);}
    const auto [entry,inserted]=allocations_.emplace(address,size);
    if(!inserted)throw HostCallFault("heap 快速重用區塊與存活物件重疊");
    // 一般釋放會扣除私有 heap 的用量並移除擁有者；先恢復保留時扣掉的帳目。
    if(heap!=process_heap){runtime_.heap_owners.emplace(address,heap);runtime_.private_heaps.at(heap).second+=size;}
    (void)release_heap_block(memory,entry);
}
void WindowsServices::evict_heap_quick_page(GuestMemory& memory,std::uint64_t page){
    // 頁面上已沒有存活物件：歸還其中所有快速區塊，頁面隨最後一個參照解除映射。
    std::vector<std::uint64_t> blocks;const auto reach=heap_quick_classes*16;
    for(auto at=heap_quick_blocks_.lower_bound(page>=reach?page-reach:0);at!=heap_quick_blocks_.end() && at->first<page+GuestMemory::guest_page_size;++at)
        if(at->first+at->second.size>page)blocks.push_back(at->first);
    for(const auto address:blocks)evict_heap_quick_block(memory,address);
}
void WindowsServices::flush_heap_quick(GuestMemory& memory){
    // 依位址順序走一般釋放流程，頁面參照與相鄰空洞合併的結果與逐一釋放相同。
    while(!heap_quick_blocks_.empty())evict_heap_quick_block(memory,heap_quick_blocks_.begin()->first);
}
void WindowsServices::flush_heap_quick(GuestMemory& memory,std::uint64_t heap){
    // 只歸還指定 heap 的區塊；其他 heap 的清單不受影響。
    std::vector<std::uint64_t> blocks;
    for(const auto& [address,block]:heap_quick_blocks_)if(block.heap==heap)blocks.push_back(address);
    for(const auto address:blocks)evict_heap_quick_block(memory,address);
}
bool WindowsServices::release_heap_block(GuestMemory& memory,std::map<std::uint64_t,std::size_t>::iterator found){
    const auto address=found->first;
    constexpr std::uint64_t page=GuestMemory::guest_page_size,mask=page-1;
    const auto end=address+found->second,full_begin=(address+mask)&~mask,full_end=end&~mask;
    std::array<std::uint64_t,2> edges{};unsigned count=0;
    if((address&mask) || found->second<page)edges[count++]=address&~mask;
    if((end&mask) && (!count || edges[0]!=(end&~mask)))edges[count++]=end&~mask;
    // 先取得回收節點，避免宿主配置失敗後遺失已釋放的位址區間。
    auto [released,inserted]=free_heap_blocks_.emplace(address,found->second);
    if(!inserted)throw HostCallFault("heap 釋放區間重複");
    try{free_heap_sizes_.emplace(found->second,address);}catch(...){free_heap_blocks_.erase(released);throw;}
    if(full_end>full_begin)memory.unmap(GuestAddress{full_begin},static_cast<std::size_t>(full_end-full_begin));
    std::array<std::uint64_t,2> quick_only{};unsigned quick_only_count=0;
    for(unsigned i=0;i<count;++i){auto page_entry=runtime_.heap_pages.find(edges[i]);if(page_entry==runtime_.heap_pages.end() || !page_entry->second)throw HostCallFault("heap backing 頁計數不一致");if(!--page_entry->second){memory.unmap(GuestAddress{edges[i]},page);runtime_.heap_pages.erase(page_entry);}
        else if(const auto quick=heap_quick_pages_.find(edges[i]);quick!=heap_quick_pages_.end() && quick->second==page_entry->second)quick_only[quick_only_count++]=edges[i];}
    if(const auto owner=runtime_.heap_owners.find(address);owner!=runtime_.heap_owners.end()){runtime_.private_heaps.at(owner->second).second-=found->second;runtime_.heap_owners.erase(owner);}
    allocations_.erase(address);
    runtime_.heap_generations.erase(address);
    gui_.task_allocations.erase(address);
    // 配置器在物件間保留 16 bytes；連同這些間隙合併，較大的重配才可使用相鄰空洞。
    if(released!=free_heap_blocks_.begin()){
        const auto previous=std::prev(released);
        if(previous->first+previous->second+16>=released->first){
            auto size_node=free_heap_sizes_.extract({previous->second,previous->first});
            free_heap_sizes_.erase({released->second,released->first});
            previous->second=static_cast<std::size_t>(std::max(previous->first+previous->second,end)-previous->first);
            free_heap_blocks_.erase(released);released=previous;
            size_node.value()={released->second,released->first};free_heap_sizes_.insert(std::move(size_node));
        }
    }
    for(auto next=std::next(released);next!=free_heap_blocks_.end() && released->first+released->second+16>=next->first;next=std::next(released)){
        auto size_node=free_heap_sizes_.extract({released->second,released->first});
        free_heap_sizes_.erase({next->second,next->first});
        released->second=static_cast<std::size_t>(std::max(released->first+released->second,next->first+next->second)-released->first);
        free_heap_blocks_.erase(next);
        size_node.value()={released->second,released->first};free_heap_sizes_.insert(std::move(size_node));
    }
    // 合併完成後，再歸還只剩快速區塊的頁面；遞迴釋放不會碰到上方仍在使用的迭代器。
    for(unsigned i=0;i<quick_only_count;++i)evict_heap_quick_page(memory,quick_only[i]);
    return true;
}
void WindowsServices::enter_callback(GuestState& state, GuestMemory& memory) {
    auto& frame = runtime_.callbacks.back(); const auto& call = frame.callbacks.at(frame.index);
    if (call.arguments.size() > 16 || (call.wide_arguments >> call.arguments.size()) || (call.floating_arguments >> call.arguments.size()) ||
        (call.floating_result && call.floating_result!=32 && call.floating_result!=64) || call.x86_register_arguments>2 ||
        call.x86_register_arguments>call.arguments.size() || (call.wide_arguments&((1U<<call.x86_register_arguments)-1U))) throw HostCallFault("callback 參數數量、寬度或暫存器無效");
    if(!handles(GuestAddress{call.target}))memory.check(GuestAddress{call.target}, 1, Access::Execute);
    retain_shell_callback(frame,call);
    const auto original_sp = frame.saved.reg(Gpr::Rsp);
    if (original_sp < 256) throw HostCallFault("callback 堆疊不足");
    if (x86_) {
        const auto sp=(original_sp-256)&~UINT64_C(3);
        memory.check(GuestAddress{sp},(call.arguments.size()-call.x86_register_arguments+static_cast<unsigned>(std::popcount(call.wide_arguments))+1)*4,Access::Write);
        memory.store(GuestAddress{sp},32,callback_return_.value);
        std::uint64_t at=sp+4;
        for(std::size_t i=call.x86_register_arguments;i<call.arguments.size();++i){const bool wide=(call.wide_arguments&(1U<<i))!=0;memory.store(GuestAddress{at},wide?64:32,call.arguments[i]);at+=wide?8:4;}
        state=frame.saved;
        if(call.x86_register_arguments)state.reg(Gpr::Rcx)=static_cast<std::uint32_t>(call.arguments[0]);
        if(call.x86_register_arguments>1)state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(call.arguments[1]);
        state.rip=GuestAddress{call.target};state.reg(Gpr::Rsp)=sp;return;
    }
    const auto sp = ((original_sp - 256) & ~UINT64_C(15)) - 8;
    memory.check(GuestAddress{sp}, 168, Access::Write);
    memory.store(GuestAddress{sp}, 64, callback_return_.value);
    // 先完成可能失敗的客體寫入，再提交暫存器；保留 fault 時原狀態，
    // 同時避免每次 x64 callback 將完整 GuestState 複製兩遍。
    for (std::size_t i = 4; i < call.arguments.size(); ++i)
        memory.store(GuestAddress{sp + 40 + (i - 4) * 8}, 64, call.arguments[i]);
    state = frame.saved; state.rip = GuestAddress{call.target}; state.reg(Gpr::Rsp) = sp;
    constexpr std::array registers{Gpr::Rcx, Gpr::Rdx, Gpr::R8, Gpr::R9};
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
        if (i < 4) {
            state.reg(registers[i])=call.arguments[i];
            if(call.floating_arguments&(1U<<i)){state.xmm[i].fill(0);for(unsigned byte=0;byte<8;++byte)state.xmm[i][byte]=static_cast<std::uint8_t>(call.arguments[i]>>(8*byte));}
        }
    }
}
void WindowsServices::classify_callback(GuestCallback& callback, Continuation continuation) const {
    // 每次進入都依目前 GUI 註冊分類；比較器本身亦可註冊新的 callback。
    const auto target=callback.target;
    callback.user_mode_callback|=continuation==Continuation::WindowEnumeration || continuation==Continuation::GuiHook ||
        std::any_of(gui_.windows.begin(),gui_.windows.end(),[&](const auto& item){return item.second.procedure==target;}) ||
        std::any_of(gui_.classes.begin(),gui_.classes.end(),[&](const auto& item){return item.second.procedure==target;}) ||
        std::any_of(gui_.hooks.begin(),gui_.hooks.end(),[&](const auto& item){return item.second.procedure==target;}) ||
        std::any_of(gui_.timers.begin(),gui_.timers.end(),[&](const auto& item){return item.second.procedure==target;});
}
void WindowsServices::start_callbacks(GuestState& state, GuestMemory& memory, std::vector<GuestCallback> callbacks, Continuation continuation, std::uint64_t value) {
    if (runtime_.callbacks.size() >= 128) throw HostCallFault("客體 callback 巢狀深度超過上限");
    for(auto& callback:callbacks)classify_callback(callback,continuation);
    runtime_.callbacks.emplace_back(state, std::move(callbacks), 0, continuation, value, exceptions_.stack.size());
    try {
        if(x86_ && continuation==Continuation::EtwProcess)runtime_.callbacks.back().etw_seh_boundary=static_cast<std::uint32_t>(memory.load(GuestAddress{state.fs_base},32));
        if(x86_)runtime_.callbacks.back().user_seh_boundary=static_cast<std::uint32_t>(memory.load(GuestAddress{state.fs_base},32));
        // 先保留整批訊息資料；第一個 callback 的巢狀 PeekMessage 不得釋放後續資料。
        for(const auto& call:runtime_.callbacks.back().callbacks)retain_shell_callback(runtime_.callbacks.back(),call);
        if (runtime_.callbacks.back().callbacks.empty()) {
            state.rip = callback_return_; state.reg(Gpr::Rax) = 0;
        } else enter_callback(state, memory);
    } catch (...) { finish_shell_callbacks(memory,runtime_.callbacks.back());runtime_.callbacks.pop_back(); throw; }
}
void WindowsServices::release_loader_notification() {
    if (runtime_.loader_owner != threads_.current || !runtime_.loader_depth)
        throw HostCallFault("DLL 執行緒通知的 loader lock ownership 不符");
    if (!--runtime_.loader_depth) runtime_.loader_owner = 0;
}
void WindowsServices::start_loader_callbacks(GuestState& state, GuestMemory& memory,
                                           std::vector<GuestCallback> callbacks,
                                           Continuation continuation, std::uint64_t value) {
    if (runtime_.loader_owner && runtime_.loader_owner != threads_.current)
        throw HostCallFault("DLL 執行緒通知必須先等待 loader lock");
    if (runtime_.loader_depth == UINT32_MAX) throw HostCallFault("loader lock 遞迴深度溢位");
    runtime_.loader_owner = threads_.current; ++runtime_.loader_depth;
    publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);
    try {
        start_callbacks(state, memory, std::move(callbacks), continuation, value);
        // ownership 隨 callback frame 保存，Sleep／排程切換不能釋放此鎖。
        runtime_.callbacks.back().loader_notification_lock = true;
    } catch (...) { release_loader_notification();publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);throw; }
}
void WindowsServices::discard_callbacks(std::size_t depth, GuestState& state, GuestMemory& memory) {
    if(depth>runtime_.callbacks.size())throw HostCallFault("callback 清理深度不合法");
    while(runtime_.callbacks.size()>depth) {
        if(runtime_.callbacks.back().continuation==Continuation::FiberDelete){const auto handle=runtime_.callbacks.back().value;runtime_.callbacks.pop_back();destroy_fiber(state,memory,handle);continue;}
        if(runtime_.callbacks.back().continuation==Continuation::Sort) {
            if(runtime_.sorts.empty())throw HostCallFault("qsort callback 清理時缺少排序狀態");
            runtime_.sorts.pop_back();
        }
        const auto& frame=runtime_.callbacks.back();
        if(frame.continuation==Continuation::NativeV8Operation)discard_native_v8(memory,static_cast<std::uint32_t>(frame.value));
        finish_shell_callbacks(memory,frame);
        if(frame.continuation==Continuation::CertificateFind)discard_certificate_find(state,memory,frame.value);
        if(frame.continuation==Continuation::DirectInputEnumeration)finish_dinput_enumeration(frame.value,memory,state);
        if(frame.continuation==Continuation::D3DXProgressOperation)discard_d3dx_progress(state,memory,frame.value);
        if(frame.continuation==Continuation::D3DCompileOperation)discard_d3d_compile(state,memory,frame.value);
        if(frame.continuation==Continuation::AutomationOperation)discard_automation(state,memory,frame.value);
        if(frame.continuation==Continuation::TypeLibraryInvoke)discard_type_library_invoke(state,memory,frame.value);
        if(frame.continuation==Continuation::TypeLibraryAddress)discard_type_library_address(state,memory,frame.value);
        if(frame.continuation==Continuation::MediaFoundationQuery)discard_media_foundation_query(frame.value,memory,state);
        if(frame.continuation==Continuation::ComOperation)discard_com_operation(state,memory,frame.value);
        if(frame.continuation==Continuation::OlePersistence)(void)free_heap(memory,frame.value,state);
        if(frame.continuation==Continuation::WinrtOperation)discard_winrt_operation(state,memory,frame.value);
        if(frame.continuation==Continuation::ComMarshalOperation)discard_com_marshal(state,memory,frame.value);
        if(frame.continuation==Continuation::ShellItemOperation)discard_shell_item(state,memory,frame.value);
        if(frame.continuation==Continuation::ComBindOperation)discard_com_bind(state,memory,frame.value);
        if(frame.continuation==Continuation::ComRotOperation)discard_com_rot(state,memory,frame.value);
        if(frame.continuation==Continuation::ComContextOperation)discard_com_context(state,memory,frame.value);
        if(frame.continuation==Continuation::OleDragLoop || frame.continuation==Continuation::OleDragTarget)discard_ole_drag(state,memory,frame.value,frame.continuation==Continuation::OleDragTarget);
        if(frame.continuation==Continuation::OleClipboardOperation)discard_ole_clipboard(state,memory,frame.value);
        if(frame.continuation==Continuation::ComProxyDispatch)discard_com_proxy_dispatch(state,memory,frame.value);
        if(frame.continuation==Continuation::ComErrorSet)discard_com_error_set(frame.value);
        if(frame.continuation==Continuation::RestrictedError)discard_restricted_error(state,memory,frame.value);
        if(frame.continuation==Continuation::WinrtOriginSetup || frame.continuation==Continuation::WinrtOriginFinish)discard_winrt_origin(state,memory,frame.value);
        if(frame.continuation==Continuation::GuiDragDetection)discard_gui_drag(memory,state,frame.value);
        if(frame.continuation==Continuation::GuiMoveSize)discard_gui_move_size(memory,state,frame.value);
        if(frame.continuation==Continuation::GuiMouseInput)gui_.mouse_inputs.erase(frame.value);
        if(frame.continuation==Continuation::GuiWindowPos)discard_gui_window_pos(memory,state,frame.value);
        if(frame.continuation==Continuation::MdiMoveOperation)discard_mdi_move(memory,state,frame.value);
        if(frame.continuation==Continuation::ThemeTextOperation)discard_theme_text(frame.value);
        if(frame.continuation==Continuation::StorageOperation)discard_storage(state,memory,frame.value);
        if(frame.continuation==Continuation::StorageCopyOperation)discard_storage_copy(state,memory,frame.value);
        if(frame.continuation==Continuation::DWriteOperation)discard_dwrite(frame.value,memory,state);
        if(frame.continuation==Continuation::StackWalkOperation)discard_stack_walk(frame.value,memory,state);
        if(frame.continuation==Continuation::SymbolFunctionEntry)discard_symbol_callback(frame.value);
        if(frame.continuation==Continuation::MiniDumpOperation)discard_minidump(frame.value,memory,state);
        if(frame.continuation==Continuation::EtwCallback)discard_etw_callback(state,memory,frame.value);
        if(frame.continuation==Continuation::EtwProcess)discard_etw_process(state,memory,frame.value);
        if(frame.continuation==Continuation::PerflibOperation)discard_perflib(state,memory,frame.value);
        if(frame.continuation==Continuation::MciWait)discard_mci_wait(frame.value);
        if(frame.continuation==Continuation::DdeOperation)discard_dde(frame.value);
        if(frame.continuation==Continuation::GdiplusStream)discard_gdiplus_stream(state,memory,frame.value);
        if(frame.continuation==Continuation::FontPackage)discard_font_package(frame.value);
        if(frame.continuation==Continuation::WinUsbTransfer)discard_winusb(frame.value);
        if(frame.continuation==Continuation::ComSpyOperation)discard_com_spy(state,memory,frame.value);
        if(frame.continuation==Continuation::DemangleOperation)discard_demangle(frame.value);
        if(frame.continuation==Continuation::Msvcp100Operation)discard_msvcp100(frame.value,state,memory);
        if(frame.continuation==Continuation::PolicyRetry)policy_sections_.erase(frame.value);
        if(frame.continuation==Continuation::BeepTone)discard_beep(frame.value);
        if(frame.continuation==Continuation::Eh4Operation)runtime_.eh4_operations.erase(frame.value);
        if(frame.continuation==Continuation::CrtLongjmp)runtime_.crt_jump_operations.erase(frame.value);
        if(frame.continuation==Continuation::MetafileEnumeration)finish_metafile_enumeration(frame.value);
        if(frame.continuation==Continuation::SafeArrayOperation){const auto locks=memory.load(GuestAddress{frame.value+8},32);if(locks)memory.store(GuestAddress{frame.value+8},32,locks-1);}
        if(frame.continuation==Continuation::CrtAssertion || frame.continuation==Continuation::CrtFatalDialog || frame.continuation==Continuation::CertificateDecodeAllocation || frame.continuation==Continuation::AccessibleOperation || frame.continuation==Continuation::OleStreamCopy || frame.continuation==Continuation::GuiIntegerText || frame.continuation==Continuation::ReturnCallbackFreeHeap || frame.continuation==Continuation::GuiBusyResult || frame.continuation==Continuation::ReturnZeroFreeHeap || frame.continuation==Continuation::MetafileEnumeration || frame.continuation==Continuation::LocaleEnumeration || frame.continuation==Continuation::ResourceEnumeration)(void)free_heap(memory,frame.value,state);
        if(frame.continuation==Continuation::GuiMenuInit || frame.continuation==Continuation::GuiMenuFinish){release_gui_scratch(memory,state,frame.hook.scratch);gui_.menu_tracks.erase(frame.value);}
        if(frame.continuation==Continuation::GuiSentMessage)cancel_gui_send(frame.value,995);
        if(frame.continuation==Continuation::GuiAsyncSend || frame.continuation==Continuation::GuiAsyncCallback)gui_.async_sends.erase(frame.value);
        if(frame.continuation==Continuation::GuiControlOperation)discard_gui_control(frame.value);
        if(frame.continuation==Continuation::GuiTextThunk)discard_gui_text_thunk(memory,state,frame.value);
        if(frame.continuation==Continuation::GuiGrayString)discard_gray_string(frame.value,memory,state);
        if(frame.continuation==Continuation::GuiCbtCreate || frame.continuation==Continuation::GuiCreationNcCreate ||
           frame.continuation==Continuation::GuiCreationNcCalc || frame.continuation==Continuation::GuiCreationCreate || frame.continuation==Continuation::GuiCreationFinish ||
           frame.continuation==Continuation::GuiCreationAbort)cancel_gui_creation(frame.value,memory,state);
        if(frame.continuation==Continuation::GuiHook && frame.hook.continuation==Continuation::GuiCbtCreate)cancel_gui_creation(frame.hook.value,memory,state);
        if(frame.continuation==Continuation::GuiCbtAction)cancel_gui_cbt_action(frame.value,memory,state);
        if(frame.continuation==Continuation::FontEnumeration)discard_font_enumeration(frame.value,memory,state);
        if(frame.continuation==Continuation::GuiHook && frame.hook.continuation==Continuation::GuiCbtAction)cancel_gui_cbt_action(frame.hook.value,memory,state);
        if(frame.continuation==Continuation::GuiHook)release_gui_scratch(memory,state,frame.hook.scratch);
        if(frame.continuation==Continuation::InitOnce){
            const WindowsCall call(frame.saved,memory,4);auto& once=runtime_.once_initializations.at(call(0));once={};
            memory.store(GuestAddress{call(0)},x86_?32:64,0);(void)free_heap(memory,frame.value,state);
        }
        if(frame.loader_notification_lock){release_loader_notification();publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);}
        runtime_.callbacks.pop_back();
    }
}
bool WindowsServices::reusable_sort_callback() const noexcept {
    if(x86_ || runtime_.callbacks.empty() || runtime_.sorts.empty())return false;
    const auto& frame=runtime_.callbacks.back();
    return frame.continuation==Continuation::Sort && frame.index==0 &&
        frame.callbacks.size()==1 && !frame.callbacks.front().user_mode_callback &&
        frame.shell_deliveries.empty() && !frame.loader_notification_lock && !frame.unwind_target;
}
std::optional<std::uint32_t> WindowsServices::resume_callback(GuestState& state, GuestMemory& memory) {
    if (runtime_.callbacks.empty()) throw HostCallFault("callback 返回沒有對應框架");
    auto& frame = runtime_.callbacks.back();
    auto callback_value=x86_ && frame.index<frame.callbacks.size() && frame.callbacks[frame.index].wide_result?(state.reg(Gpr::Rax)&UINT32_MAX)|((state.reg(Gpr::Rdx)&UINT32_MAX)<<32):state.reg(Gpr::Rax);
    if(frame.index<frame.callbacks.size() && frame.callbacks[frame.index].floating_result)
        callback_value=floating_callback_result(state,frame.callbacks[frame.index].floating_result,x86_);
    // 排序／搜尋在同一 API 呼叫中反覆使用相同比較器。保留其父狀態與
    // 參數容器，下一次進入才一次還原 GuestState；巢狀呼叫仍另建框架。
    // GUI／Shell、Win32 SEH 及 unwind 特殊框架沿用一般生命週期。
    if(reusable_sort_callback())return advance_sort(state,memory,callback_value,true);
    if (frame.continuation == Continuation::InitTerm || frame.continuation == Continuation::InitTermE) {
        if(frame.continuation == Continuation::InitTermE && static_cast<std::uint32_t>(callback_value))frame.value=static_cast<std::uint32_t>(callback_value);
        else {
            // 每個 initializer 結束後才讀取下一項，保留其修改後續表格的效果。
            const unsigned unit=x86_?4U:8U;
            while(frame.initializer_next<frame.initializer_end){
                const auto target=memory.load(GuestAddress{frame.initializer_next},static_cast<std::uint8_t>(unit*8));
                frame.initializer_next+=unit;
                if(target){frame.callbacks[0].target=target;frame.index=0;enter_callback(state,memory);return std::nullopt;}
            }
        }
        frame.index=frame.callbacks.size();
    }
    else if(frame.continuation==Continuation::ConsoleControl && static_cast<std::uint32_t>(callback_value)){frame.value=1;frame.index=frame.callbacks.size();}
    else if(frame.continuation==Continuation::ReturnUntilFailure && (static_cast<std::uint32_t>(callback_value)&0x80000000U))frame.index=frame.callbacks.size();
    else if((frame.continuation==Continuation::ReturnUntilFalse || frame.continuation==Continuation::DirectInputEnumeration || frame.continuation==Continuation::WindowEnumeration || frame.continuation==Continuation::MetafileEnumeration || frame.continuation==Continuation::LocaleEnumeration || frame.continuation==Continuation::ResourceEnumeration) && !static_cast<std::uint32_t>(callback_value))frame.index=frame.callbacks.size();
    else if (frame.index < frame.callbacks.size()) ++frame.index;
    if(frame.continuation==Continuation::WindowEnumeration)while(frame.index<frame.callbacks.size() && !gui_.windows.contains(frame.callbacks[frame.index].arguments[0]))++frame.index;
    if (frame.index < frame.callbacks.size()) { enter_callback(state, memory); return std::nullopt; }
    // 框架即將離開堆疊；移交其參數與保留資源，無需配置並複製容器。
    const auto finished = std::move(frame); runtime_.callbacks.pop_back(); state = finished.saved;
    finish_shell_callbacks(memory,finished);
    if(finished.continuation==Continuation::UnwindConsolidate){
        if(!finished.unwind_target)throw HostCallFault("unwind consolidation 缺少目標 CONTEXT");
        memory.check(GuestAddress{callback_value},1,Access::Execute);
        state=*finished.unwind_target;state.rip=GuestAddress{callback_value};
        if(finished.value>exceptions_.stack.size())throw HostCallFault("unwind consolidation 例外深度不符");
        exceptions_.stack.resize(static_cast<std::size_t>(finished.value));return std::nullopt;
    }
    if(finished.continuation==Continuation::CrtAssertion || finished.continuation==Continuation::CrtFatalDialog){
        (void)free_heap(memory,finished.value,state);
        if(finished.continuation==Continuation::CrtAssertion){
            if(callback_value==5){complete_call(state,memory,0);return std::nullopt;}
            if(callback_value==4){start_callbacks(state,memory,{{resolve("kernel32.dll","DebugBreak").value,{}}},Continuation::Return,0);return std::nullopt;}
        }
        const auto code=finished.continuation==Continuation::CrtFatalDialog?255U:3U;
        record_job_exit(code);(void)emit_debug_event(state,memory,debugger::ExitProcess{code});return code;
    }
    if(finished.continuation==Continuation::NtCloseResult){
        complete_call(state,memory,callback_value?0U:last_error_==6?0xc0000008U:last_error_==5?0xc0000022U:0xc0000001U);return std::nullopt;
    }
    if(finished.loader_notification_lock){release_loader_notification();publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);}
    if(finished.continuation==Continuation::D3DXProgressOperation)return resume_d3dx_progress(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::D3DCompileOperation)return resume_d3d_compile(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::AutomationOperation)return resume_automation(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::TypeLibraryInvoke)return resume_type_library_invoke(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::TypeLibraryAddress)return resume_type_library_address(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::MediaFoundationQuery)return resume_media_foundation_query(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::TaskDialogOperation)return resume_task_dialog(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::ComOperation)return resume_com_operation(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::NativeOperation)return resume_nt_native(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::NativeV8Operation)return resume_native_v8(state,memory,static_cast<std::uint32_t>(finished.value),static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::WinrtOperation)return resume_winrt_operation(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::ComMarshalOperation)return resume_com_marshal(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::ShellItemOperation)return resume_shell_item(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::ComBindOperation)return resume_com_bind(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::ComRotOperation)return resume_com_rot(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::ComContextOperation)return resume_com_context(state,memory,finished.value);
    if(finished.continuation==Continuation::OleDragLoop)return resume_ole_drag(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::OleDragTarget)return resume_ole_drag_target(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::ComProxyDispatch)return resume_com_proxy_dispatch(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::ComErrorSet)return resume_com_error_set(state,memory,finished.value);
    if(finished.continuation==Continuation::ComErrorRetire)return resume_com_error_retire(state,memory);
    if(finished.continuation==Continuation::RestrictedError)return resume_restricted_error(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::WinrtOriginSetup || finished.continuation==Continuation::WinrtOriginFinish)return resume_winrt_origin(state,memory,finished.value,static_cast<std::uint32_t>(callback_value),finished.continuation==Continuation::WinrtOriginSetup);
    if(finished.continuation==Continuation::GuiDragDetection)return resume_gui_drag(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::StorageOperation)return resume_storage(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::StorageCopyOperation)return resume_storage_copy(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::DWriteOperation)return resume_dwrite(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::StackWalkOperation)return resume_stack_walk(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::SymbolFunctionEntry)return resume_symbol_callback(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::MiniDumpOperation)return resume_minidump(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::EtwCallback)return resume_etw_callback(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::EtwProcess)return resume_etw_process(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::PerflibOperation)return resume_perflib(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::MciWait)return resume_mci_wait(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::DdeOperation)return resume_dde(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::GdiplusStream)return resume_gdiplus_stream(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::KernelRequest)return resume_kernel_request(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::FontPackage)return resume_font_package(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::WinUsbTransfer || finished.continuation==Continuation::WinUsbOverlapped)return resume_winusb(state,memory,finished.value,finished.continuation==Continuation::WinUsbOverlapped);
    if(finished.continuation==Continuation::ComSpyOperation)return resume_com_spy(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::CertificateFind)return resume_certificate_find(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::CertificateDecodeAllocation)return resume_certificate_decode(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::DemangleOperation)return resume_demangle(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::Msvcp100Operation)return resume_msvcp100(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::PolicyRetry)return std::nullopt; // Sleep 後從原 API 入口重試，不累積 callback 堆疊。
    if(finished.continuation==Continuation::BeepTone)return finish_beep(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::Eh4Operation)return resume_eh4(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::CrtLongjmp)return resume_x86_longjmp(state,memory,finished.value);
    if(finished.continuation==Continuation::CrtNewResult){complete_call(state,memory,static_cast<std::uint32_t>(callback_value)!=0);return std::nullopt;}
    if(finished.continuation==Continuation::CrtNewRetry){
        // 還原原 API 入口後交回引擎重試；不累積宿主／客體 callback 堆疊。
        // handler 可釋放配置、切換執行緒或修改下一次失敗時使用的 handler／mode。
        if(static_cast<std::uint32_t>(callback_value))return std::nullopt;
        memory.store(GuestAddress{runtime_.errno_cell},32,12);complete_call(state,memory,0);return std::nullopt;
    }
    if(finished.continuation==Continuation::CrtInvalidNewMode){
        memory.store(GuestAddress{runtime_.errno_cell},32,22);complete_call(state,memory,UINT32_MAX);return std::nullopt;
    }
    if(finished.continuation==Continuation::GuiSendTimeout){
        const WindowsCall call(state,memory,7);const bool destroyed=(static_cast<std::uint32_t>(call(4))&0x20U) && !gui_.windows.contains(call(0));
        if(destroyed)last_error_=1400;
        if(call(6))memory.store(GuestAddress{call(6)},x86_?32:64,destroyed?0:callback_value);
        complete_call(state,memory,destroyed?0:1);return std::nullopt;
    }
    if(finished.continuation==Continuation::UserApc){
        if(dispatch_user_apc(state,memory,finished.value))return std::nullopt;
        if(finished.value!=UINT64_MAX){if(!finished.value)last_error_=0xc0;complete_call(state,memory,finished.value);}return std::nullopt;
    }
    if(finished.continuation==Continuation::GuiIntegerText){
        const WindowsCall call(state,memory,4);const auto& message=finished.callbacks.front();const auto capacity=message.arguments[2];
        const auto copied=std::min<std::uint64_t>(static_cast<std::uint32_t>(callback_value),capacity-1);
        std::u16string text;for(std::uint64_t i=0;i<copied;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{finished.value+i*2},16));if(!c)break;text.push_back(c);}
        (void)free_heap(memory,finished.value,state);
        if(text.size()==capacity-1){
            // 先取得完整文字，避免將遭截斷的溢位值或大量前導空白誤判為有效整數。
            const auto scratch=capacity<=memory.limit()/4?allocate_heap(memory,capacity*4):0;
            if(!scratch){last_error_=8;complete_call(state,memory,0);return std::nullopt;}
            start_callbacks(state,memory,{{message.target,{message.arguments[0],13,capacity*2,scratch}}},Continuation::GuiIntegerText,scratch);return std::nullopt;
        }
        std::size_t i=0;while(i<text.size() && (text[i]==u' ' || (text[i]>=u'\t' && text[i]<=u'\r')))++i;
        bool negative=false;if(i<text.size() && (text[i]==u'-' || text[i]==u'+')){negative=text[i]==u'-';if(!negative || call(3))++i;}
        const auto begin=i;const std::uint64_t limit=call(3)?(negative?UINT64_C(2147483648):UINT64_C(2147483647)):UINT64_C(4294967295);
        std::uint64_t value=0;bool overflow=false;
        for(;i<text.size() && text[i]>=u'0' && text[i]<=u'9';++i){const auto digit=static_cast<unsigned>(text[i]-u'0');if(value>(limit-digit)/10)overflow=true;else if(!overflow)value=value*10+digit;}
        const bool valid=i>begin && !overflow;if(valid && call(2))memory.store(GuestAddress{call(2)},32,1);
        complete_call(state,memory,valid?static_cast<std::uint32_t>(negative?UINT64_C(0)-value:value):0);return std::nullopt;
    }
    if(finished.continuation==Continuation::DirectInputEnumeration){finish_dinput_enumeration(finished.value,memory,state);complete_call(state,memory,0);return std::nullopt;}
    if(finished.continuation==Continuation::WindowEnumeration){complete_call(state,memory,static_cast<std::uint32_t>(callback_value)!=0);return std::nullopt;}
    if(finished.continuation==Continuation::ResourceEnumeration){
        if(finished.value)(void)free_heap(memory,finished.value,state);
        const bool success=static_cast<std::uint32_t>(callback_value)!=0;
        if(!success)last_error_=15106;
        complete_call(state,memory,success);return std::nullopt;
    }
    if(finished.continuation==Continuation::LocaleEnumeration){(void)free_heap(memory,finished.value,state);complete_call(state,memory,1);return std::nullopt;}
    if(finished.continuation==Continuation::MetafileEnumeration){finish_metafile_enumeration(finished.value);(void)free_heap(memory,finished.value,state);complete_call(state,memory,static_cast<std::uint32_t>(callback_value)!=0);return std::nullopt;}
    if(finished.continuation==Continuation::SafeArrayOperation){const auto locks=memory.load(GuestAddress{finished.value+8},32);if(locks)memory.store(GuestAddress{finished.value+8},32,locks-1);complete_call(state,memory,0);return std::nullopt;}
    if(finished.continuation==Continuation::AccessibleOperation)return resume_accessible(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::OleStreamCopy)return resume_ole_stream_copy(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::OlePersistence)return resume_ole_persistence(state,memory,finished.value,static_cast<std::uint32_t>(callback_value));
    if(finished.continuation==Continuation::PrintAbort)return resume_printing(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::FiberDelete){destroy_fiber(state,memory,finished.value);complete_call(state,memory,0);return std::nullopt;}
    if(finished.continuation==Continuation::ThemeTextOperation)return finish_theme_text(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::MdiMoveOperation)return resume_mdi_move(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::ReturnCallbackFreeHeap){(void)free_heap(memory,finished.value,state);complete_call(state,memory,callback_value);return std::nullopt;}
    if(finished.continuation==Continuation::ReturnZeroFreeHeap){(void)free_heap(memory,finished.value,state);complete_call(state,memory,0);return std::nullopt;}
    if(finished.continuation==Continuation::GuiWinEvent){if(finished.value)return advance_gui_modal(state,memory,finished.value);return std::nullopt;}
    if(finished.continuation==Continuation::GuiMouseInput)return resume_gui_mouse(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::GuiMoveSize)return resume_gui_move_size(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::RegisteredWait)return resume_registered_wait(state,memory);
    if(finished.continuation==Continuation::ServiceMain || finished.continuation==Continuation::ServiceControl)return resume_guest_service(state,memory,finished.value,finished.continuation==Continuation::ServiceMain);
    if(finished.continuation==Continuation::GuiHook || finished.continuation==Continuation::GuiMsgFilterSystem)return resume_gui_hook(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::GuiCbtCreate || finished.continuation==Continuation::GuiCreationNcCreate ||
       finished.continuation==Continuation::GuiCreationNcCalc || finished.continuation==Continuation::GuiCreationCreate || finished.continuation==Continuation::GuiCreationFinish ||
       finished.continuation==Continuation::GuiCreationAbort)return resume_gui_creation(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::FileTransfer)return advance_file_transfer(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::PrintDialogOperation)return resume_print_dialog(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::PrintPropertyPageOperation)return resume_property_page(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::GuiPropertySheet)return resume_property_sheet(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::GuiBusyHook || finished.continuation==Continuation::GuiBusyResult)return resume_gui_busy(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::GuiDialogProc)return resume_gui_dialog(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::GuiCbtAction)return resume_gui_cbt_action(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::FontEnumeration)return resume_font_enumeration(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::ModuleOperation)return resume_module_operation(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::MsvcCopy || finished.continuation==Continuation::MsvcUnwind || finished.continuation==Continuation::MsvcCatch || finished.continuation==Continuation::MsvcDestroy)return resume_msvc(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::OleClipboardOperation)return resume_ole_clipboard(state,memory,finished.value,callback_value);
    if(finished.continuation==Continuation::OleVariantClear || finished.continuation==Continuation::OleVariantCopy || finished.continuation==Continuation::OleReleaseStgMedium)return resume_ole(state,memory,finished);
    if(finished.continuation==Continuation::ConsoleControl){if(!finished.value)return finish_process(state,memory,0xc000013aU);state.reg(Gpr::Rax)=0;return std::nullopt;}
    if (finished.continuation == Continuation::ThreadExit) return finish_thread(state, memory, static_cast<std::uint32_t>(finished.value));
    if (finished.continuation == Continuation::Sort) return advance_sort(state, memory, callback_value);
    if(finished.continuation==Continuation::InitOnce){
        const WindowsCall call(state,memory,4);const std::uint8_t width=x86_?32:64;
        auto& once=runtime_.once_initializations.at(call(0));
        const auto context=memory.load(GuestAddress{finished.value},width);(void)free_heap(memory,finished.value,state);
        const bool success=static_cast<std::uint32_t>(callback_value)!=0 && !(context&3);
        once.owner=0;once.complete=success;once.context=success?context:0;
        memory.store(GuestAddress{call(0)},width,success?context|2:0);
        if(success && call(3))memory.store(GuestAddress{call(3)},width,context);
        if(callback_value && (context&3))last_error_=87;
        complete_call(state,memory,success);return std::nullopt;
    }
    if (finished.continuation == Continuation::OnExitTable) return invoke_runtime(state,memory,{"crt","_execute_onexit_table",1});
    if (finished.continuation == Continuation::CrtExit) {
        auto& pending=(finished.value&(UINT64_C(1)<<33))?runtime_.quick_exit_callbacks:runtime_.exit_callbacks;
        if (!pending.empty()) {
            const auto target = pending.back(); pending.pop_back();
            start_callbacks(state, memory, {{target, {}}}, Continuation::CrtExit, finished.value); return std::nullopt;
        }
        if(!(finished.value&(UINT64_C(1)<<33)))(void)flush_crt_streams(memory);
        if (finished.value >> 32) return finish_process(state, memory, static_cast<std::uint32_t>(finished.value));
        complete_call(state,memory,0); return std::nullopt;
    }
    if (finished.continuation == Continuation::GuiModalSystemFilter || finished.continuation == Continuation::GuiModalFilter || finished.continuation == Continuation::GuiModalInit || finished.continuation == Continuation::GuiModalDispatch || finished.continuation == Continuation::GuiModalEnd || finished.continuation == Continuation::GuiDialogInit || finished.continuation == Continuation::GuiNcCreate || finished.continuation == Continuation::GuiCreate || finished.continuation == Continuation::GuiDestroy || finished.continuation == Continuation::GuiDestroyDefault)
        return resume_gui(state, memory, finished, callback_value);
    if (finished.continuation == Continuation::GuiMenuInit || finished.continuation == Continuation::GuiMenuFinish) return resume_menu(state, memory, finished, callback_value);
    if(finished.continuation==Continuation::GuiGrayString)return resume_gray_string(state,memory,finished,callback_value);
    if (finished.continuation == Continuation::GuiSentMessage) return resume_gui_send(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::GuiAsyncSend || finished.continuation==Continuation::GuiAsyncCallback)return resume_gui_async(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::GuiWaitMessage){complete_call(state,memory,1);return std::nullopt;}
    if (finished.continuation == Continuation::GuiControlOperation) return resume_gui_control(state,memory,finished,callback_value);
    if(finished.continuation==Continuation::GuiTextThunk)return resume_gui_text_thunk(state,memory,finished,callback_value);
    if (finished.continuation == Continuation::GuiWindowPos) {
        try {return finish_gui_window_pos(state,memory,finished.value);}
        catch(...) {discard_gui_window_pos(memory,state,finished.value);throw;}
    }
    if(finished.continuation==Continuation::UnhandledFilter){
        const auto value=static_cast<std::uint32_t>(callback_value);
        if(value!=0 && value!=1 && value!=UINT32_MAX)throw HostCallFault("UnhandledExceptionFilter 回傳值不合法");
        complete_call(state,memory,value?value:1);return std::nullopt;
    }
    if (finished.continuation == Continuation::Exception) return resume_exception(state, memory, callback_value);
    if (finished.continuation == Continuation::ContinueException) {
        if (exceptions_.stack.empty()) throw HostCallFault("繼續例外 callback 缺少派送狀態");
        state=read_context(memory,exceptions_.stack.back().context,finished.saved);exceptions_.stack.pop_back();return std::nullopt;
    }
    if (finished.continuation == Continuation::SehFilter) return seh_filter_result(state, memory, callback_value);
    if (finished.continuation == Continuation::Startup){(void)dispatch_user_apc(state,memory,UINT64_MAX);return std::nullopt;}
    if (finished.continuation == Continuation::Exit) {
        record_job_exit(static_cast<std::uint32_t>(finished.value));
        (void)emit_debug_event(state,memory,debugger::ExitProcess{static_cast<std::uint32_t>(finished.value)});
        return static_cast<std::uint32_t>(finished.value);
    }
    complete_call(state,memory,(finished.continuation == Continuation::ReturnCallback || finished.continuation == Continuation::ReturnUntilFailure) ? callback_value : finished.value);
    return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::finish_process(GuestState& state, GuestMemory& memory, std::uint32_t code) {
    // CRT callback 已可能從框架堆疊移除；以獨立入口保存退出碼，不能重跑 callback_return_。
    state.rip = process_exit_retry_; state.reg(Gpr::Rcx) = code;
    if (await_loader(state, memory)) return std::nullopt;
    std::vector<GuestCallback> callbacks;
    for (std::size_t i = 0; i < runtime_.fls_used.size(); ++i) {
        if (runtime_.fls_used[i] && runtime_.fls_values[i] && runtime_.fls_callbacks[i]) callbacks.push_back({runtime_.fls_callbacks[i], {runtime_.fls_values[i]}});
        runtime_.fls_values[i] = 0;
    }
    sync_current_fls_mirror(memory);
    for (const auto target : runtime_.image.tls_callbacks) callbacks.push_back({target, {runtime_.image.base, 0, 0}, true});
    for(auto it=runtime_.module_order.rbegin();it!=runtime_.module_order.rend();++it) {
        auto& module=runtime_.loaded_modules.at(*it);if(!module.attached)continue;
        for(const auto target:module.image.tls_callbacks)callbacks.push_back({target,{*it,0,1},true});
        if(module.image.entry)callbacks.push_back({module.image.entry,{*it,0,1},false,true});
        module.attached=false;
    }
    if (callbacks.empty()) {record_job_exit(code);(void)emit_debug_event(state,memory,debugger::ExitProcess{code});return code;}
    runtime_.exiting = true; start_loader_callbacks(state, memory, std::move(callbacks), Continuation::Exit, code); return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_runtime(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service) {
    // 字元家族的操作已由不可變 descriptor 索引；完整與常駐入口共用 locale／ABI 實作。
    if(crt_character_requested() && crt_character_operation(service))return invoke_crt_character(state,memory,service);
    if(service.family=="user32.dll" || service.family=="gdi32.dll")gui_.message_threads.insert(threads_.current);
    using Handler=std::optional<std::uint32_t>(WindowsServices::*)(GuestState&,GuestMemory&,const WindowsServiceDescriptor&);
    // API 的所屬 provider 在程序期間固定；以 stub 對應的 descriptor 直接索引。
    // 內部組合呼叫傳入的臨時 descriptor 仍走同一分類規則，保留原優先順序。
    const auto classify=[](const WindowsServiceDescriptor& service)->Handler {
        static const bool clock_provider=[] {const auto* setting=std::getenv("X64ARM_RESIDENT_COMPUTE_SERVICES");return !setting || std::string_view(setting)!="0";}();
        // 純時鐘查詢共用時間 provider；不可變 descriptor 只分類一次，
        // 不再逐次穿過通用相容入口的裝置、權限及環境分支。
        if(clock_provider && service.family=="kernel32.dll" &&
           (service.symbol=="QueryPerformanceCounter" || service.symbol=="QueryPerformanceFrequency" ||
            service.symbol=="GetSystemTimeAsFileTime" || service.symbol=="GetTickCount"))return &WindowsServices::invoke_time;
        static std::atomic<unsigned> observations{};
        const auto threshold=static_cast<unsigned>(2*std::bit_width(runtime_service_descriptors().size()));
        const bool reuse=observations.fetch_add(1,std::memory_order_relaxed)>=threshold;
        if(angle_module(service.family))return &WindowsServices::invoke_angle;
        if(native_library_module(service.family))return &WindowsServices::invoke_native_library;
        if(service.family=="imm32.dll")return &WindowsServices::invoke_imm;
        if(detail::service_descriptor_contains<printing_escape_service_descriptors>(service,reuse))return &WindowsServices::invoke_printing;
        if(detail::service_descriptor_contains<property_key_service_descriptors>(service,reuse))return &WindowsServices::invoke_property_keys;
        if(detail::service_descriptor_contains<shell_memory_stream_descriptors>(service,reuse))return &WindowsServices::invoke_ole_stream;
        if(detail::service_descriptor_contains<service_control_descriptors>(service,reuse))return &WindowsServices::invoke_service_control;
        if(detail::service_descriptor_contains<input_service_descriptors>(service,reuse))return &WindowsServices::invoke_input_api;
        if(detail::service_descriptor_contains<raw_input_service_descriptors>(service,reuse))return &WindowsServices::invoke_raw_input;
        if(detail::service_descriptor_contains<pointer_service_descriptors>(service,reuse))return &WindowsServices::invoke_pointer_input;
        if(detail::service_descriptor_contains<hid_service_descriptors>(service,reuse))return &WindowsServices::invoke_hid;
        if(detail::service_descriptor_contains<icon_service_descriptors>(service,reuse))return &WindowsServices::invoke_icon_api;
        if(detail::service_descriptor_contains<script_service_descriptors>(service,reuse))return &WindowsServices::invoke_script;
        if(detail::service_descriptor_contains<metafile_service_descriptors>(service,reuse))return &WindowsServices::invoke_metafile;
        if(detail::service_descriptor_contains<eventlog_service_descriptors>(service,reuse))return &WindowsServices::invoke_eventlog;
        if(detail::service_descriptor_contains<http_service_descriptors>(service,reuse))return &WindowsServices::invoke_http;
        if(detail::service_descriptor_contains<fiber_service_descriptors>(service,reuse))return &WindowsServices::invoke_fiber;
        if(detail::service_descriptor_contains<dib_transfer_service_descriptors>(service,reuse))return &WindowsServices::invoke_dib_transfer;
        if(detail::service_descriptor_contains<gdiplus_service_descriptors>(service,reuse))return &WindowsServices::invoke_gdiplus;
        if(detail::service_descriptor_contains<task_dialog_service_descriptors>(service,reuse))return &WindowsServices::invoke_task_dialog;
        if(detail::service_descriptor_contains<opencl_service_descriptors>(service,reuse))return &WindowsServices::invoke_opencl;
        if(service.family=="vulkan-1.dll")return &WindowsServices::invoke_vulkan;
        if(service.family=="directml.dll" || service.family=="internal-directml")return &WindowsServices::invoke_directml;
        if(service.family=="msvcp140_1.dll" || service.family=="internal-msvcp140_1")return &WindowsServices::invoke_msvcp_pmr;
        if(service.family=="webview2loader.dll" || service.family=="internal-webview2")return &WindowsServices::invoke_webview2;
        if(service.family=="d3d12.dll" || service.family=="internal-d3d12")return &WindowsServices::invoke_d3d12;
        if(service.family=="dxcore.dll" || service.family=="internal-dxcore")return &WindowsServices::invoke_dxcore;
        if(detail::service_descriptor_contains<print_dialog_service_descriptors>(service,reuse))return service.family=="comctl32.dll"?&WindowsServices::invoke_property_page:&WindowsServices::invoke_print_dialog;
        if(detail::service_descriptor_contains<printing_service_descriptors>(service,reuse))return &WindowsServices::invoke_printing;
        if(detail::service_descriptor_contains<gdi_path_service_descriptors>(service,reuse))return &WindowsServices::invoke_gdi_path;
        if(detail::service_descriptor_contains<gdi_state_service_descriptors>(service,reuse))return &WindowsServices::invoke_gdi_state;
        if(service.family=="oleacc.dll" || service.family=="uiautomationcore.dll" || service.family=="internal-accessible")return &WindowsServices::invoke_accessible;
        if(service.family=="dwrite.dll" || service.family=="internal-dwrite")return &WindowsServices::invoke_dwrite;
        if(detail::service_descriptor_contains<registered_wait_service_descriptors>(service,reuse))return &WindowsServices::invoke_registered_wait;
        if(service.family=="dbghelp.dll" || service.family=="dbgcore.dll")return &WindowsServices::invoke_dbghelp;
        if(detail::service_descriptor_contains<locale_service_descriptors>(service,reuse))return &WindowsServices::invoke_locale;
        if(detail::service_descriptor_contains<heap_service_descriptors>(service,reuse))return &WindowsServices::invoke_heap;
        if(detail::service_descriptor_contains<anonymous_token_service_descriptors>(service,reuse))return &WindowsServices::invoke_security;
        if(detail::service_descriptor_contains<working_set_service_descriptors>(service,reuse))return &WindowsServices::invoke_memory;
        if(detail::service_descriptor_contains<font_service_descriptors>(service,reuse))return &WindowsServices::invoke_fonts;
        if(detail::service_descriptor_contains<winstation_service_descriptors>(service,reuse))return &WindowsServices::invoke_winstation;
        if(detail::service_descriptor_contains<profile_service_descriptors>(service,reuse))return &WindowsServices::invoke_profile;
        if(detail::service_descriptor_contains<version_service_descriptors>(service,reuse))return &WindowsServices::invoke_version;
        if(detail::service_descriptor_contains<theme_service_descriptors>(service,reuse))return &WindowsServices::invoke_theme;
        if(detail::service_descriptor_contains<property_service_descriptors>(service,reuse))return &WindowsServices::invoke_properties;
        if(detail::service_descriptor_contains<activation_service_descriptors>(service,reuse))return &WindowsServices::invoke_activation;
        if(detail::service_descriptor_contains<hook_service_descriptors>(service,reuse))return &WindowsServices::invoke_hooks;
        if(detail::service_descriptor_contains<graphics_compat_service_descriptors>(service,reuse))return &WindowsServices::invoke_graphics_compat;
        if(detail::service_descriptor_contains<legacy_gdi_service_descriptors>(service,reuse))return &WindowsServices::invoke_legacy_gdi;
        if(detail::service_descriptor_contains<legacy_user_service_descriptors>(service,reuse))return &WindowsServices::invoke_legacy_user;
        if(service.family=="ntoskrnl.exe" || service.family=="hal.dll")return &WindowsServices::invoke_kernel_api;
        if(detail::service_descriptor_contains<userstring_service_descriptors>(service,reuse))return &WindowsServices::invoke_userstring;
        if(detail::service_descriptor_contains<control_service_descriptors>(service,reuse))return &WindowsServices::invoke_control_api;
        if(detail::service_descriptor_contains<msvc_service_descriptors>(service,reuse))return &WindowsServices::invoke_msvc;
        if(detail::service_descriptor_contains<resource_service_descriptors>(service,reuse))return &WindowsServices::invoke_resource;
        if(detail::service_descriptor_contains<lsa_service_descriptors>(service,reuse))return &WindowsServices::invoke_lsa;
        if(detail::service_descriptor_contains<security_service_descriptors>(service,reuse))return &WindowsServices::invoke_security;
        if (service.family == "internal-dmusic" || service.family == "dmusic.dll" || service.family == "dmime.dll" || service.family == "dmloader.dll") return &WindowsServices::invoke_directmusic;
        if (service.family == "dsound.dll" || service.family == "internal-dsound") return &WindowsServices::invoke_dsound;
        if (service.family == "mmdevapi.dll" || service.family == "audioses.dll" || service.family == "internal-wasapi") return &WindowsServices::invoke_wasapi;
        if (service.family == "mfplat.dll" || service.family == "mf.dll" || service.family == "mfreadwrite.dll" || service.family == "internal-mf") return &WindowsServices::invoke_media_foundation;
        if(detail::service_descriptor_contains<dinput_service_descriptors>(service,reuse))return &WindowsServices::invoke_dinput;
        if (service.family == "d3d9.dll" || service.family == "internal-d3d9") return &WindowsServices::invoke_d3d9;
        if(service.family=="d3dx9_42.dll" || service.family=="d3dx9_43.dll")
            if(detail::service_descriptor_contains<d3d9_service_descriptors>(service,reuse))return &WindowsServices::invoke_d3d9;
        if (service.family == "d3d10.dll" || service.family == "d3d10_1.dll" || service.family == "internal-d3d10" || service.family == "d3dx10_43.dll" || service.family == "d3d11.dll" || service.family == "dxgi.dll" || service.family == "internal-d3d11" || service.family == "d3dcompiler_47.dll" || service.family == "d3dcompiler_43.dll" || service.family == "d3dx11_42.dll" || service.family == "d3dx11_43.dll")
            return &WindowsServices::invoke_d3d11;
        constexpr std::array io_names{"_umask","_umask_s","_creat", "_wcreat", "freopen", "_wfreopen", "_chsize", "_chsize_s", "_open_osfhandle", "_get_osfhandle", "_filelength", "_filelengthi64", "fgetpos", "fsetpos", "fopen_s", "_wfopen_s", "fopen", "_wfopen", "_fsopen", "_wfsopen", "_commit", "fclose", "fread", "fwrite", "fseek", "_fseeki64", "ftell", "_ftelli64", "feof", "ferror", "clearerr", "rewind", "fgetwc", "fputwc", "ungetc", "ungetwc", "getchar", "fgetc", "getc", "fgets", "fputs", "fputc", "putc", "fflush", "setvbuf", "setbuf", "_getmaxstdio", "_setmaxstdio", "_dup2", "_locking", "_dup", "_fdopen", "_wfdopen", "_close", "_read", "_write", "_open", "_wopen", "_sopen", "_wsopen", "_sopen_s", "_wsopen_s", "_lseek", "_lseeki64", "_tell", "_telli64", "_isatty", "_setmode"};
        if (service.family == "crt" && std::find(io_names.begin(), io_names.end(), service.symbol) != io_names.end()) return &WindowsServices::invoke_crt_io;
        if(detail::service_descriptor_contains<tls_service_descriptors>(service,reuse))return &WindowsServices::invoke_tls;
        if(service.family=="internal-msxml")return &WindowsServices::invoke_msxml;
        if(service.family=="internal-ribbon")return &WindowsServices::invoke_ribbon;
        if(detail::service_descriptor_contains<wmi_service_descriptors>(service,reuse))return &WindowsServices::invoke_wmi;
        if(detail::service_descriptor_contains<safearray_service_descriptors>(service,reuse))return &WindowsServices::invoke_safearray;
        if(detail::service_descriptor_contains<ole_stream_service_descriptors>(service,reuse))return &WindowsServices::invoke_ole_stream;
        if(detail::service_descriptor_contains<storage_service_descriptors>(service,reuse))return &WindowsServices::invoke_storage;
        if(detail::service_descriptor_contains<ole_service_descriptors>(service,reuse))return &WindowsServices::invoke_ole;
        if(detail::service_descriptor_contains<gui_drag_service_descriptors>(service,reuse))return &WindowsServices::invoke_gui_drag;
        if(service.family=="internal-window-interaction")return &WindowsServices::invoke_gui_move_size;
        if(service.family=="internal-ole-drag" || (service.family=="ole32.dll" && service.symbol=="DoDragDrop"))return &WindowsServices::invoke_ole_drag;
        if(service.family=="internal-com-proxy")return &WindowsServices::invoke_com_proxy;
        if(detail::service_descriptor_contains<gui_rpc_service_descriptors>(service,reuse))return &WindowsServices::invoke_gui_rpc;
        if(detail::service_descriptor_contains<desktop_notification_service_descriptors>(service,reuse))return &WindowsServices::invoke_desktop_notification;
        if(detail::service_descriptor_contains<winrt_error_service_descriptors>(service,reuse))return &WindowsServices::invoke_winrt_error;
        if(detail::service_descriptor_contains<com_error_service_descriptors>(service,reuse))return &WindowsServices::invoke_com_error;
        if(detail::service_descriptor_contains<type_library_service_descriptors>(service,reuse))return &WindowsServices::invoke_type_library;
        if(detail::service_descriptor_contains<com_marshal_service_descriptors>(service,reuse))return &WindowsServices::invoke_com_marshal;
        if(detail::service_descriptor_contains<com_context_service_descriptors>(service,reuse))return &WindowsServices::invoke_com_context;
        if(detail::service_descriptor_contains<winrt_service_descriptors>(service,reuse))return &WindowsServices::invoke_winrt;
        if(detail::service_descriptor_contains<power_notification_service_descriptors>(service,reuse))return &WindowsServices::invoke_power_notification;
        if(detail::service_descriptor_contains<data_protection_service_descriptors>(service,reuse))return &WindowsServices::invoke_data_protection;
        if(detail::service_descriptor_contains<application_policy_service_descriptors>(service,reuse))return &WindowsServices::invoke_application_policy;
        if(detail::service_descriptor_contains<shell_notification_service_descriptors>(service,reuse))return &WindowsServices::invoke_shell_notification;
        if(detail::service_descriptor_contains<shell_item_service_descriptors>(service,reuse))return &WindowsServices::invoke_shell_item;
        if(detail::service_descriptor_contains<shell_service_descriptors>(service,reuse))return &WindowsServices::invoke_shell;
        if(detail::service_descriptor_contains<registry_service_descriptors>(service,reuse))return &WindowsServices::invoke_registry;
        if(detail::service_descriptor_contains<error_service_descriptors>(service,reuse))return &WindowsServices::invoke_error;
        if(detail::service_descriptor_contains<volume_service_descriptors>(service,reuse))return &WindowsServices::invoke_volume;
        if(detail::service_descriptor_contains<serial_service_descriptors>(service,reuse))return &WindowsServices::invoke_serial;
        if(detail::service_descriptor_contains<console_service_descriptors>(service,reuse))return &WindowsServices::invoke_console_control;
        if(detail::service_descriptor_contains<file_service_descriptors>(service,reuse))return &WindowsServices::invoke_file;
        if(detail::service_descriptor_contains<file_encryption_service_descriptors>(service,reuse))return &WindowsServices::invoke_file_encryption;
        if(detail::service_descriptor_contains<thread_service_descriptors>(service,reuse))return &WindowsServices::invoke_thread;
        if(detail::service_descriptor_contains<etw_service_descriptors>(service,reuse))return &WindowsServices::invoke_etw;
        if(detail::service_descriptor_contains<perflib_service_descriptors>(service,reuse))return &WindowsServices::invoke_perflib;
        if(service.family=="pdh.dll")return &WindowsServices::invoke_pdh;
        if(detail::service_descriptor_contains<transaction_service_descriptors>(service,reuse))return &WindowsServices::invoke_transactions;
        if(detail::service_descriptor_contains<system_object_service_descriptors>(service,reuse))return &WindowsServices::invoke_system_objects;
        if(detail::service_descriptor_contains<dde_service_descriptors>(service,reuse))return &WindowsServices::invoke_dde;
        if(detail::service_descriptor_contains<user_session_service_descriptors>(service,reuse))return &WindowsServices::invoke_user_session;
        if(detail::service_descriptor_contains<com_spy_service_descriptors>(service,reuse))return &WindowsServices::invoke_com_spy;
        if(service.family=="urlmon.dll" || service.family=="internal-url-security")return &WindowsServices::invoke_url_security;
        if(service.family=="dxva2.dll" || service.family.starts_with("internal-dxva2-"))return &WindowsServices::invoke_dxva2;
        if(service.family=="ndfapi.dll")return &WindowsServices::invoke_network_diagnostics;
        if(service.family=="credui.dll")return &WindowsServices::invoke_credential_ui;
        if(service.family=="cryptui.dll")return &WindowsServices::invoke_certificate_ui;
        if(service.family=="winusb.dll")return &WindowsServices::invoke_winusb;
        if(service.family=="fontsub.dll")return &WindowsServices::invoke_font_package;
        if(detail::service_descriptor_contains<mci_service_descriptors>(service,reuse))return &WindowsServices::invoke_mci;
        if(detail::service_descriptor_contains<reparse_service_descriptors>(service,reuse))return &WindowsServices::invoke_reparse;
        if(detail::service_descriptor_contains<environment_service_descriptors>(service,reuse))return &WindowsServices::invoke_environment;
        if(detail::service_descriptor_contains<memory_service_descriptors>(service,reuse))return &WindowsServices::invoke_memory;
        if (service.family == "iphlpapi.dll" || service.family=="winhttp.dll" || service.family=="dhcpcsvc.dll") return &WindowsServices::invoke_network_config;
        if (service.family == "ws2_32.dll" || service.family=="mswsock.dll") return &WindowsServices::invoke_network;
        if (service.family == "mpr.dll") return &WindowsServices::invoke_mpr;
        if(detail::service_descriptor_contains<dpapi_service_descriptors>(service,reuse))return &WindowsServices::invoke_dpapi;
        if(detail::service_descriptor_contains<certificate_find_service_descriptors>(service,reuse))return &WindowsServices::invoke_certificate_find;
        if(detail::service_descriptor_contains<certificate_key_service_descriptors>(service,reuse))return &WindowsServices::invoke_certificate_key;
        if(detail::service_descriptor_contains<certificate_decode_service_descriptors>(service,reuse))return &WindowsServices::invoke_certificate_decode;
        if(detail::service_descriptor_contains<certificate_service_descriptors>(service,reuse))return &WindowsServices::invoke_certificate;
        if(detail::service_descriptor_contains<crypto_service_descriptors>(service,reuse))return &WindowsServices::invoke_crypto;
        if(detail::service_descriptor_contains<opengl_service_descriptors>(service,reuse))return &WindowsServices::invoke_opengl;
        if(detail::service_descriptor_contains<process_service_descriptors>(service,reuse))return &WindowsServices::invoke_process;
        if(detail::service_descriptor_contains<nt_native_service_descriptors>(service,reuse))return &WindowsServices::invoke_nt_native;
        if(detail::service_descriptor_contains<nt_process_service_descriptors>(service,reuse))return &WindowsServices::invoke_nt_process;
        if(detail::service_descriptor_contains<mapping_service_descriptors>(service,reuse))return &WindowsServices::invoke_mapping;
        if(detail::service_descriptor_contains<time_service_descriptors>(service,reuse))return &WindowsServices::invoke_time;
        if(detail::service_descriptor_contains<path_service_descriptors>(service,reuse))return &WindowsServices::invoke_path;
        if(detail::service_descriptor_contains<crt_string_service_descriptors>(service,reuse))return &WindowsServices::invoke_crt_string;
        if(detail::service_descriptor_contains<math_service_descriptors>(service,reuse))return &WindowsServices::invoke_math;
        if(detail::service_descriptor_contains<compatibility_service_descriptors>(service,reuse) || crt_character_operation(service))return &WindowsServices::invoke_compatibility;
        if(detail::service_descriptor_contains<menu_service_descriptors>(service,reuse))return &WindowsServices::invoke_menu;
        if(service.family=="comdlg32.dll" || service.family=="oledlg.dll")return &WindowsServices::invoke_common_dialog;
        if(detail::service_descriptor_contains<gdi_service_descriptors>(service,reuse))return &WindowsServices::invoke_gdi;
        if(detail::service_descriptor_contains<gui_service_descriptors>(service,reuse))return &WindowsServices::invoke_gui_support;
        if (service.family == "user32.dll" || service.family == "gdi32.dll") return &WindowsServices::invoke_gui;
        if(service.symbol=="RtlMoveMemory")return nullptr; // 記憶體搬移走共用 CRT 核心，不是例外處理服務。
        if (service.symbol == "DebugBreak" || service.symbol == "UnhandledExceptionFilter" || service.symbol == "__crtUnhandledException" || service.symbol == "__crtCapturePreviousContext" || service.symbol == "RaiseException" || service.symbol == "RaiseFailFastException" || service.symbol == "AddVectoredExceptionHandler" || service.symbol == "RemoveVectoredExceptionHandler" || service.symbol == "AddVectoredContinueHandler" || service.symbol == "RemoveVectoredContinueHandler" || service.symbol.starts_with("Rtl") || service.symbol == "__C_specific_handler" || service.symbol.find("setjmp") != std::string_view::npos || (service.symbol == "longjmp" || service.symbol == "_longjmpex"))
            return &WindowsServices::invoke_exception;
        return nullptr;
    };
    const auto table=runtime_service_descriptors();
    // 只分類實際呼叫的 API；release/acquire 發布不可變 handler，熱路徑不取鎖。
    struct Slot { std::atomic<bool> ready{false};std::once_flag once;Handler handler{};bool format{}; };
    static const auto handlers=std::make_unique<Slot[]>(table.size());
    const auto descriptor_address=reinterpret_cast<std::uintptr_t>(&service),descriptor_begin=reinterpret_cast<std::uintptr_t>(table.data());
    const auto cached=descriptor_address>=descriptor_begin && descriptor_address-descriptor_begin<table.size_bytes() && (descriptor_address-descriptor_begin)%sizeof(WindowsServiceDescriptor)==0;
    Handler handler{};bool format_service=false;
    static const bool format_provider=[] {const auto* setting=std::getenv("X64ARM_CRT_FORMAT_PROVIDER");return !setting || std::string_view(setting)!="0";}();
    if(cached){
        auto& slot=handlers[(descriptor_address-descriptor_begin)/sizeof(WindowsServiceDescriptor)];
        if(!slot.ready.load(std::memory_order_acquire))std::call_once(slot.once,[&]{
            slot.handler=classify(service);
            slot.format=!slot.handler && crt_format_symbol(service.symbol);
            slot.ready.store(true,std::memory_order_release);
        });
        handler=slot.handler;format_service=slot.format;
    }else {handler=classify(service);format_service=!handler && crt_format_symbol(service.symbol);}
    if(handler)return (this->*handler)(state,memory,service);
    const WindowsCall arg(state, memory, service);
    // 不可變 descriptor 已判定所屬 provider，格式化不再逐次穿過 TLS、heap 等家族。
    // 關閉開關仍走原分類順序，兩條路徑共用同一次 ABI 檢查與格式化本體。
    if(format_provider && format_service)return invoke_crt_format(state,memory,service,arg);
    const auto symbol = service.symbol;
    std::uint64_t result = 0;
    const auto fail = [&](std::uint32_t error) { last_error_ = error; result = 0; };
    const auto heap_max_request=x86_?UINT64_C(0xffffffe0):UINT64_C(0xffffffffffffffe0);
    const auto retry_allocation=[&](std::uint64_t size){
        if(runtime_.new_mode && runtime_.new_handler){
            start_callbacks(state,memory,{{runtime_.new_handler,{size}}},Continuation::CrtNewRetry);return true;
        }
        memory.store(GuestAddress{runtime_.errno_cell},32,12);return false;
    };
    const auto output = [&](std::string_view text, std::uint64_t stream = 1) {
        const auto written=write_crt_stream(memory,runtime_base_.value+0x800+stream*128,std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
        result=written==text.size()?written:UINT32_MAX;
    };
    const auto tls_offset=[&](std::size_t index){const auto unit=x86_?4U:8U;return index<64?(x86_?0xe10U:0x1480U)+index*unit:0x2000+(index-64)*unit;};
    const auto tls_slot = [&](std::size_t index) { return GuestAddress{(x86_?state.fs_base:state.gs_base)+tls_offset(index)}; };
    const auto clear_tls_slot = [&](std::size_t index) {
        runtime_.tls_values[index]=0;
        for(auto& [id,thread]:threads_.items) {
            (void)id;thread.tls_values[index]=0;
            // 退出物件可由 handle 保留，但其 TEB 已在切離執行緒後回收。
            if(thread.status!=ThreadStatus::Exited)
                memory.store(GuestAddress{thread.teb+tls_offset(index)},x86_?32:64,0);
        }
    };
    if (symbol == "FlsAlloc") {
        if (arg(0)) memory.check(GuestAddress{arg(0)}, 1, Access::Execute);
        result = UINT32_MAX;
        for (std::size_t i = 0; i < runtime_.fls_used.size(); ++i) if (!runtime_.fls_used[i]) {
            runtime_.fls_used[i] = true; runtime_.fls_values[i] = 0; runtime_.fls_callbacks[i] = arg(0);for(auto& [id,thread]:threads_.items){(void)id;thread.fls_values[i]=0;}for(auto& [handle,fiber]:threads_.fibers){(void)handle;fiber.fls_values[i]=0;} result = i; sync_fls_mirrors(memory); break;
        }
        if (result == UINT32_MAX) last_error_ = 8;
    } else if (symbol == "FlsGetValue" || symbol == "FlsGetValue2" || symbol == "FlsSetValue" || symbol == "FlsFree") {
        const auto index = static_cast<std::uint32_t>(arg(0));
        if (index >= runtime_.fls_used.size() || !runtime_.fls_used[index]) {if(symbol=="FlsGetValue2")result=0;else fail(87);}
        else if (symbol == "FlsGetValue" || symbol == "FlsGetValue2") { result = runtime_.fls_values[index]; if(symbol=="FlsGetValue")last_error_ = 0; }
        else if (symbol == "FlsSetValue") { runtime_.fls_values[index] = arg(1); result = 1; if(fls_mirror_)memory.store(GuestAddress{state.gs_base+0x1840+std::uint64_t{index}*8},64,arg(1)); }
        else {
            const auto callback = runtime_.fls_callbacks[index];std::vector<GuestCallback> callbacks;
            if (callback && runtime_.fls_values[index]) callbacks.push_back({callback,{runtime_.fls_values[index]}});
            for (auto& [id,thread] : threads_.items) {
                if (id!=threads_.current && thread.status!=ThreadStatus::Exited && callback && thread.fls_values[index]) callbacks.push_back({callback,{thread.fls_values[index]}});
                thread.fls_values[index]=0;
            }
            for(auto& [handle,fiber]:threads_.fibers){(void)handle;if(!fiber.active_thread && callback && fiber.fls_values[index])callbacks.push_back({callback,{fiber.fls_values[index]}});fiber.fls_values[index]=0;}
            runtime_.fls_values[index]=0;runtime_.fls_used[index]=false;runtime_.fls_callbacks[index]=0;sync_fls_mirrors(memory);
            if (!callbacks.empty()) {start_callbacks(state,memory,std::move(callbacks),Continuation::Return,1);return std::nullopt;}
            result=1;
        }
    } else if (symbol == "TlsAlloc") {
        result = UINT32_MAX;
        for (std::size_t i = 0; i < runtime_.tls_used.size(); ++i) if (!runtime_.tls_used[i]) { runtime_.tls_used[i] = true;
            clear_tls_slot(i); result = i; break; }
        if (result == UINT32_MAX) last_error_ = 8;
    } else if (symbol == "TlsGetValue" || symbol == "TlsGetValue2" || symbol == "TlsSetValue" || symbol == "TlsFree") {
        const auto index = static_cast<std::uint32_t>(arg(0));
        const bool reading=symbol=="TlsGetValue" || symbol=="TlsGetValue2";
        if (index >= runtime_.tls_used.size() || (!reading && !runtime_.tls_used[index])) {if(symbol=="TlsGetValue2")result=0;else fail(87);}
        else if (reading) { if(symbol=="TlsGetValue")last_error_ = 0; result = runtime_.image.base ? memory.load(tls_slot(index), x86_?32:64) : runtime_.tls_values[index]; }
        else if (symbol == "TlsFree") { runtime_.tls_used[index] = false; clear_tls_slot(index); result = 1; }
        else { runtime_.tls_values[index] = arg(1); if (runtime_.image.base) memory.store(tls_slot(index), x86_?32:64, arg(1)); result = 1; }
    } else if (symbol == "Sleep") { if (arg(0)) throw HostCallFault("單執行緒模式尚不支援等待其他執行緒的 Sleep"); }
    else if (symbol == "SetUnhandledExceptionFilter" || symbol=="__crtSetUnhandledExceptionFilter") { result = runtime_.unhandled_filter; runtime_.unhandled_filter = arg(0); }
    else if(symbol=="GetProcessUserModeExceptionPolicy"){
        if(!arg(0))last_error_=87;
        else try{memory.store(GuestAddress{arg(0)},32,runtime_.user_mode_exception_policy);result=1;}catch(const MemoryFault&){last_error_=998;}
    }else if(symbol=="SetProcessUserModeExceptionPolicy"){
        const auto flags_value=static_cast<std::uint32_t>(arg(0));
        if(flags_value&~1U)last_error_=87;else {runtime_.user_mode_exception_policy=flags_value;result=1;}
    }
    else if(symbol=="__crtGetShowWindowMode" || symbol=="_get_show_window_mode")result=runtime_.startup_show==UINT32_MAX?10U:runtime_.startup_show;
    else if(symbol=="_get_narrow_winmain_command_line" || symbol=="_get_wide_winmain_command_line") {
        const bool wide=symbol=="_get_wide_winmain_command_line";const unsigned unit=wide?2U:1U;
        const auto base=wide?command_line_address_.value:runtime_.command_ansi;
        std::uint64_t offset=0;bool quoted=false;
        // CRT WinMain 尾端保留原始引數文字，僅跳過第一個程式名稱與其後空白。
        for(;;offset+=unit){const auto ch=memory.load(GuestAddress{base+offset},static_cast<std::uint8_t>(unit*8));
            if(!ch || (!quoted && ch<=32))break;if(ch=='"')quoted=!quoted;
        }
        while(const auto ch=memory.load(GuestAddress{base+offset},static_cast<std::uint8_t>(unit*8))){if(ch>32)break;offset+=unit;}
        result=base+offset;
    }
    else if(symbol=="__crtTerminateProcess"){const auto code=static_cast<std::uint32_t>(arg(0));record_job_exit(code);return code;}
    else if(symbol=="_crt_debugger_hook" || symbol=="__crt_debugger_hook") {
        // CRT 的 debugger breakpoint 標記沒有預設副作用；保留 cdecl 參數與正常返回。
    }
    else if (symbol == "GetCommandLineA") result = runtime_.command_ansi;
    else if (symbol == "GetCurrentThreadId") result = thread_identifier(threads_.current);
    else if (symbol == "GetCurrentProcessId" || symbol=="_getpid") result = process_id();
    else if (symbol == "IsDBCSLeadByte") result = 0; // 本程序的 ANSI code page 為 UTF-8，沒有 DBCS lead byte。
    else if (symbol == "GetACP") result = 65001;
    else if (symbol == "GetModuleHandleA" || symbol == "GetModuleHandleW") { if (arg(0)) fail(126); else result = runtime_.image.base; }
    else if (symbol == "GetStartupInfoA" || symbol == "GetStartupInfoW") {
        const auto size=x86_ ? 68U : 104U; memory.write(GuestAddress{arg(0)}, std::vector<std::uint8_t>(size)); memory.store(GuestAddress{arg(0)}, 32, size);
        if(runtime_.startup_show!=UINT32_MAX){memory.store(GuestAddress{arg(0)+(x86_?44U:60U)},32,1);memory.store(GuestAddress{arg(0)+(x86_?48U:64U)},16,runtime_.startup_show);}
        if(runtime_.startup_handles){const auto flags=memory.load(GuestAddress{arg(0)+(x86_?44U:60U)},32);memory.store(GuestAddress{arg(0)+(x86_?44U:60U)},32,flags|0x100U);for(unsigned i=0;i<3;++i)memory.store(GuestAddress{arg(0)+(x86_?56U:80U)+i*(x86_?4U:8U)},x86_?32:64,(*runtime_.startup_handles)[i]);}
        if(runtime_.startup_reserved2_address){memory.store(GuestAddress{arg(0)+(x86_?50U:66U)},16,runtime_.startup_reserved2.size());memory.store(GuestAddress{arg(0)+(x86_?52U:72U)},x86_?32:64,runtime_.startup_reserved2_address);}
    } else if (symbol == "__p___argc") result = runtime_.argc_cell;
    else if (symbol == "__p__acmdln") result = runtime_base_.value + 48;
    else if (symbol == "__p__wcmdln") result = runtime_base_.value + 104;
    else if (symbol == "__p___argv") result = runtime_.argv_cell;
    else if (symbol == "__p___wargv") result = runtime_.wargv_cell;
    else if (symbol == "__p__wenviron") result = runtime_.wenviron_cell;
    else if (symbol == "__p__environ") result = runtime_.environ_cell;
    else if (symbol == "__p___initenv") result = runtime_base_.value+112;
    else if (symbol == "__p___winitenv") result = runtime_base_.value+120;
    else if (symbol == "_set_fmode") {
        const auto mode=static_cast<std::uint32_t>(arg(0));
        if(mode!=0x4000 && mode!=0x8000 && mode!=0x10000){memory.store(GuestAddress{runtime_.errno_cell},32,22);result=22;}
        else memory.store(GuestAddress{runtime_.fmode_cell},32,mode);
    } else if (symbol == "_get_fmode") {
        if(!arg(0)){memory.store(GuestAddress{runtime_.errno_cell},32,22);result=22;}
        else memory.store(GuestAddress{arg(0)},32,memory.load(GuestAddress{runtime_.fmode_cell},32));
    }
    else if (symbol == "_get_stream_buffer_pointers") {
        const auto stream=crt_stream(arg(0)),base=runtime_base_.value+0x800;
        const bool standard=stream>=base && stream<=base+256 && (stream-base)%128==0 && standard_open_[(stream-base)/128];
        if(!standard && !runtime_.streams.contains(stream)){memory.store(GuestAddress{runtime_.errno_cell},32,22);result=22;}
        else {
            // 回傳同一串流的客體 base／next／count 欄位，緩衝讀寫即時更新。
            crt_buffer_fields(memory,*crt_file(stream));
            const std::uint8_t width=x86_?32:64;const unsigned unit=width/8;
            for(unsigned i=1;i<4;++i)if(arg(i))memory.check(GuestAddress{arg(i)},unit,Access::Write);
            if(arg(1))memory.store(GuestAddress{arg(1)},width,stream+unit);
            if(arg(2))memory.store(GuestAddress{arg(2)},width,stream);
            if(arg(3))memory.store(GuestAddress{arg(3)},width,stream+unit*2);
        }
    }
    else if (symbol == "__p__fmode") result = runtime_.fmode_cell;
    else if (symbol == "__p__commode") result = runtime_.commode_cell;
    else if (symbol == "__p__adjust_fdiv") result = runtime_base_.value+72;
    else if (symbol == "_errno") result = runtime_.errno_cell;
    else if (symbol == "_configure_narrow_argv" || symbol == "_configure_wide_argv") { if (static_cast<std::uint32_t>(arg(0)) != 1) throw HostCallFault("目前 CRT argv 使用不展開萬用字元的模式"); }
    else if (symbol == "_initialize_narrow_environment" || symbol == "_initialize_wide_environment" || symbol == "_get_initial_narrow_environment" || symbol == "_get_initial_wide_environment") {
        if(symbol=="_get_initial_narrow_environment")result=memory.load(GuestAddress{runtime_base_.value+112},x86_?32:64);
        else if(symbol=="_get_initial_wide_environment")result=memory.load(GuestAddress{runtime_base_.value+120},x86_?32:64);
    }
    else if (symbol == "_set_app_type") runtime_.app_type = static_cast<int>(arg(0));
    else if (symbol == "_set_new_handler") {result=runtime_.new_handler;runtime_.new_handler=arg(0);}
    else if (symbol == "_query_new_handler") result=runtime_.new_handler;
    else if (symbol == "_query_new_mode") result=static_cast<std::uint32_t>(runtime_.new_mode);
    else if (symbol == "_callnewh") {
        if(runtime_.new_handler){start_callbacks(state,memory,{{runtime_.new_handler,{arg(0)}}},Continuation::CrtNewResult);return std::nullopt;}
    }
    else if (symbol == "_set_new_mode") {
        const auto mode=static_cast<std::uint32_t>(arg(0));
        if(mode>1){
            if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::CrtInvalidNewMode);return std::nullopt;}
            return UINT32_C(0xc0000409);
        }
        result=static_cast<std::uint32_t>(runtime_.new_mode);runtime_.new_mode=static_cast<int>(mode);
    }
    else if (symbol == "_configthreadlocale") { result = static_cast<std::uint32_t>(runtime_.thread_locale); if (arg(0)) runtime_.thread_locale = static_cast<int>(arg(0)); }
    else if (symbol == "__setusermatherr") { result = runtime_.math_handler; runtime_.math_handler = arg(0); }
    else if(symbol=="_set_thread_local_invalid_parameter_handler") {auto& thread=threads_.items.at(threads_.current);result=thread.invalid_parameter_handler;thread.invalid_parameter_handler=arg(0);}
    else if(symbol=="_abs64") {const auto value=x86_?memory.load(GuestAddress{state.reg(Gpr::Rsp)+4},64):arg(0);result=(value>>63)?0-value:value;if(x86_)state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(result>>32);}
    else if(symbol=="_msize" && guest_heap_contains(arg(0)))result=guest_heap_block_size(memory,arg(0));
    else if(symbol=="_msize") {const auto found=allocations_.find(arg(0));if(found!=allocations_.end())result=found->second;else {memory.store(GuestAddress{runtime_.errno_cell},32,22);result=UINT64_MAX;}}
    else if (symbol == "_set_invalid_parameter_handler") { result = runtime_.invalid_parameter_handler; runtime_.invalid_parameter_handler = arg(0); }
    else if (symbol == "_initterm" || symbol == "_initterm_e") {
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto begin=arg(0),end=arg(1);
        if(end<begin || (end-begin)%unit || end-begin>memory.limit())throw HostCallFault("CRT initializer table 不合法");
        // 客體表格受記憶體預算約束；宿主僅保留目前一個 callback，不限制合法表格的筆數。
        for(auto at=begin;at<end;at+=unit){
            const auto target=memory.load(GuestAddress{at},bits);
            if(!target)continue;
            start_callbacks(state,memory,{{target,{}}},symbol=="_initterm_e"?Continuation::InitTermE:Continuation::InitTerm);
            auto& frame=runtime_.callbacks.back();frame.initializer_next=at+unit;frame.initializer_end=end;
            return std::nullopt;
        }
    } else if(symbol=="_initialize_onexit_table" || symbol=="_register_onexit_function" || symbol=="_execute_onexit_table") {
        const unsigned width=x86_?32:64,unit=width/8;const auto onexit_table=arg(0);
        if(!onexit_table)result=UINT32_MAX;
        else {
            memory.check(GuestAddress{onexit_table},unit*3,Access::Write);
            auto begin=memory.load(GuestAddress{onexit_table},static_cast<std::uint8_t>(width));
            auto end=memory.load(GuestAddress{onexit_table+unit},static_cast<std::uint8_t>(width));
            auto limit=memory.load(GuestAddress{onexit_table+unit*2},static_cast<std::uint8_t>(width));
            if(end<begin || limit<end || (end-begin)%unit || (limit-begin)%unit || limit-begin>4096U*unit)throw HostCallFault("CRT onexit onexit_table 範圍無效");
            if(symbol=="_register_onexit_function"){
                if(end==limit){
                    const auto old_size=end-begin,new_size=std::min<std::uint64_t>(4096U*unit,std::max<std::uint64_t>(16U*unit,old_size*2));
                    const auto allocation=new_size>old_size?allocate_heap(memory,new_size):0;
                    if(!allocation)result=UINT32_MAX;
                    else {
                        if(old_size){std::vector<std::uint8_t> data(static_cast<std::size_t>(old_size));memory.read(GuestAddress{begin},data);memory.write(GuestAddress{allocation},data);}
                        if(begin && allocations_.contains(begin))(void)free_heap(memory,begin,state);
                        begin=allocation;end=begin+old_size;limit=begin+new_size;
                        memory.store(GuestAddress{onexit_table},static_cast<std::uint8_t>(width),begin);memory.store(GuestAddress{onexit_table+unit*2},static_cast<std::uint8_t>(width),limit);
                    }
                }
                if(!result){memory.store(GuestAddress{end},static_cast<std::uint8_t>(width),arg(1));memory.store(GuestAddress{onexit_table+unit},static_cast<std::uint8_t>(width),end+unit);}
            } else if(symbol=="_execute_onexit_table"){
                while(end>begin){
                    end-=unit;const auto callback=memory.load(GuestAddress{end},static_cast<std::uint8_t>(width));memory.store(GuestAddress{onexit_table+unit},static_cast<std::uint8_t>(width),end);
                    if(callback){start_callbacks(state,memory,{{callback,{}}},Continuation::OnExitTable);return std::nullopt;}
                }
                if(begin && allocations_.contains(begin))(void)free_heap(memory,begin,state);
                memory.write(GuestAddress{onexit_table},std::vector<std::uint8_t>(unit*3));
            }
        }
    } else if(symbol=="__dllonexit") {
        memory.check(GuestAddress{arg(1)},8,Access::Write);memory.check(GuestAddress{arg(2)},8,Access::Write);
        const auto begin=memory.load(GuestAddress{arg(1)},64),end=memory.load(GuestAddress{arg(2)},64);
        if(!arg(0) || end<begin || (end-begin)%8 || end-begin>=4096*8U)result=0;
        else {
            memory.check(GuestAddress{arg(0)},1,Access::Execute);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end-begin+8));
            if(end!=begin)memory.read(GuestAddress{begin},std::span(bytes).first(bytes.size()-8));
            const auto target=allocate_heap(memory,bytes.size());
            if(target){memory.write(GuestAddress{target},bytes);memory.store(GuestAddress{target+end-begin},64,arg(0));
                if(begin && allocations_.contains(begin))(void)free_heap(memory,begin,state);
                memory.store(GuestAddress{arg(1)},64,target);memory.store(GuestAddress{arg(2)},64,target+bytes.size());result=arg(0);
            }
        }
    } else if (symbol == "_c_exit")result=0; // quick cleanup 不執行 onexit；共用 CRT 串流採直接 I/O，沒有待 flush 緩衝。
    else if (symbol == "_crt_atexit" || symbol == "atexit" || symbol == "_onexit" || symbol=="_crt_at_quick_exit" || symbol=="at_quick_exit") {
        auto& pending=symbol.ends_with("quick_exit")?runtime_.quick_exit_callbacks:runtime_.exit_callbacks;
        if (!arg(0) || pending.size() >= 4096 || runtime_.exiting) result = symbol == "_onexit" ? 0 : UINT32_MAX;
        else { memory.check(GuestAddress{arg(0)}, 1, Access::Execute); pending.push_back(arg(0)); if (symbol == "_onexit") result = arg(0); }
    } else if(symbol=="_register_thread_local_exe_atexit_callback") {
        if(runtime_.tls_exit_callback)return 3;
        if(arg(0))memory.check(GuestAddress{arg(0)},1,Access::Execute);
        runtime_.tls_exit_callback=arg(0);
    } else if (symbol == "exit" || symbol == "_cexit" || symbol=="quick_exit") {
        const bool quick=symbol=="quick_exit";
        const auto value = symbol != "_cexit" ? (UINT64_C(1) << 32) | (quick?UINT64_C(1)<<33:0) | static_cast<std::uint32_t>(arg(0)) : 0;
        auto& pending=quick?runtime_.quick_exit_callbacks:runtime_.exit_callbacks;
        std::vector<GuestCallback> callbacks;
        // UCRT 的主執行緒 TLS 解構先於一般 atexit，快速 _exit 不走此路徑。
        if(!quick && runtime_.tls_exit_callback){callbacks.push_back({runtime_.tls_exit_callback,{0,0,0}});runtime_.tls_exit_callback=0;}
        else if (!pending.empty()) { callbacks.push_back({pending.back(), {}}); pending.pop_back(); }
        start_callbacks(state, memory, std::move(callbacks), Continuation::CrtExit, value); return std::nullopt;
    } else if (symbol == "_exit") return finish_process(state, memory, static_cast<std::uint32_t>(arg(0)));
    else if (symbol == "_set_abort_behavior") {result=runtime_.crt_abort_behavior;runtime_.crt_abort_behavior=(runtime_.crt_abort_behavior&~static_cast<std::uint32_t>(arg(1)))|(static_cast<std::uint32_t>(arg(0))&static_cast<std::uint32_t>(arg(1)));}
    else if (symbol == "abort") {
        if(runtime_.crt_abort_behavior&1U){constexpr std::string_view message="This application has requested the Runtime to terminate it in an unusual way.\n";(void)write_standard_output(2,{reinterpret_cast<const std::uint8_t*>(message.data()),message.size()});}
        const auto abort_callback=runtime_.signals[22];
        if(abort_callback>1){runtime_.signals[22]=0;start_callbacks(state,memory,{{abort_callback,{22}}},Continuation::Exit,3);return std::nullopt;}
        return 3;
    }
    else if (symbol == "_aligned_msize") {
        // UCRT：pointer 必須是 _aligned_* 的起始地址，alignment／offset 與配置時相同。
        const auto found = runtime_.aligned_allocations.find(arg(0));
        std::optional<AlignedAllocation> allocation;
        if(guest_heap_contains(arg(0)))allocation=guest_heap_aligned_allocation(memory,arg(0));
        else if(found!=runtime_.aligned_allocations.end())allocation=found->second;
        if (!arg(0) || !allocation || arg(1) != allocation->alignment || arg(2) != allocation->offset) {
            memory.store(GuestAddress{runtime_.errno_cell}, 32, 22);
            if (current_invalid_parameter_handler()) {start_callbacks(state, memory, {{current_invalid_parameter_handler(), {0,0,0,0,0}}}, Continuation::Return, x86_?UINT32_MAX:UINT64_MAX);return std::nullopt;}
            return UINT32_C(0xc0000409);
        }
        result = allocation->size;
    }
    else if (symbol == "_aligned_malloc" || symbol == "_aligned_realloc" || symbol == "_aligned_free" || symbol == "_aligned_offset_malloc" ||
             symbol == "_aligned_offset_realloc" || symbol == "_aligned_recalloc" || symbol == "_aligned_offset_recalloc") {
        std::optional<std::uint64_t> failed_request;
        const bool reallocating = symbol.ends_with("realloc") || symbol.ends_with("recalloc");
        const bool clearing = symbol.ends_with("recalloc"), offsetting = symbol.starts_with("_aligned_offset_");
        const auto pointer = reallocating || symbol == "_aligned_free" ? arg(0) : 0;
        const auto found = runtime_.aligned_allocations.find(pointer);
        std::optional<AlignedAllocation> previous_allocation;
        if(guest_heap_contains(pointer))previous_allocation=guest_heap_aligned_allocation(memory,pointer);
        else if(found!=runtime_.aligned_allocations.end())previous_allocation=found->second;
        const auto release = [&](std::uint64_t address) {
            if (!address) return;
            if(guest_heap_contains(address)){
                guest_heap_free(memory,guest_heap_aligned_allocation(memory,address).base);return;
            }
            const auto allocation = runtime_.aligned_allocations.find(address);
            if (allocation == runtime_.aligned_allocations.end()) throw HostCallFault("_aligned_free 收到非對齊配置起始地址");
            if (!free_heap(memory, allocation->second.base, state)) throw HostCallFault("對齊配置的底層 heap block 已失效");
            runtime_.aligned_allocations.erase(allocation);
        };
        if (symbol == "_aligned_free") release(pointer);
        else {
            // recalloc 的元素數×大小溢位依 UCRT 回報 ENOMEM。
            const auto count = clearing ? arg(1) : 1, unit = clearing ? arg(2) : arg(reallocating ? 1 : 0);
            const bool overflow = clearing && unit && count > heap_max_request / unit;
            const auto size = overflow ? 0 : count * unit, alignment = arg(clearing ? 3 : reallocating ? 2 : 1);
            const auto offset = offsetting ? arg(clearing ? 4 : reallocating ? 3 : 2) : 0;
            if (pointer && !previous_allocation) throw HostCallFault("_aligned_realloc 不可接收 malloc 配置或非起始地址");
            if (overflow) memory.store(GuestAddress{runtime_.errno_cell}, 32, 12); // 保留原配置，回傳 NULL。
            else if (pointer && !size) release(pointer); // UCRT 先處理 size=0，再檢查 alignment。
            else if ((!size && !reallocating) || !alignment || (alignment & (alignment - 1)) || (offset && offset >= size) || (pointer && (alignment != previous_allocation->alignment || offset != previous_allocation->offset))) {
                memory.store(GuestAddress{runtime_.errno_cell}, 32, 22);
                if (current_invalid_parameter_handler()) {
                    start_callbacks(state, memory, {{current_invalid_parameter_handler(), {0,0,0,0,0}}}, Continuation::Return);
                    return std::nullopt;
                }
                // UCRT 預設 _invoke_watson 使用 FAST_FAIL_INVALID_ARG。
                // 直接終止客體；不得執行 CRT/FLS/TLS 清理或派送可攔截的 SEH/VEH。
                return UINT32_C(0xc0000409);
            } else if (pointer && size <= previous_allocation->size) {
                if(guest_heap_contains(pointer))memory.store(GuestAddress{previous_allocation->base},64,size);
                else found->second.size = size;
                result = pointer;
            } else {
                const auto allocate = [&](std::uint64_t requested) -> std::uint64_t {
                    // 與原生常式共享標頭，保存原始大小及對齊，供 msize／realloc／recalloc 使用。
                    // 額外保留至少一個 payload byte，避免 size=0 的回傳值落到下一區塊。
                    if(!offset && alignment<=16 && requested<=guest_heap_request_limit()){
                        const auto base=guest_heap_allocate(memory,std::max(UINT64_C(1),requested)+guest_heap_aligned_header);
                        if(base){
                            try{
                                memory.store(GuestAddress{base},64,requested);
                                memory.store(GuestAddress{base+8},64,alignment);
                            }catch(...){guest_heap_free(memory,base);throw;}
                            return base+guest_heap_aligned_header;
                        }
                    }
                    // 至少保留一個 pointer header，使回傳值與 malloc 的 base 必定不同。
                    // 對齊值 1/2/4 仍具有 pointer 自然對齊。
                    const auto header=x86_?UINT64_C(4):UINT64_C(8);
                    const auto effective = std::max(header, alignment);
                    const auto overhead = effective - 1 + header;
                    if (overhead > heap_max_request || requested > heap_max_request - overhead) {
                        memory.store(GuestAddress{runtime_.errno_cell}, 32, 12); return 0;
                    }
                    const auto base = allocate_heap(memory, requested + overhead);
                    if (!base) { failed_request=requested+overhead; return 0; }
                    // (address+offset) 對齊；offset 小於配置大小，位移仍落在保留的 overhead 內。
                    const auto address = ((base + header + offset + effective - 1) & ~(effective - 1)) - offset;
                    try {
                        memory.store(GuestAddress{address - header}, x86_?32:64, base);
                        runtime_.aligned_allocations.emplace(address, AlignedAllocation{base, requested, alignment, offset});
                    } catch (const std::bad_alloc&) {
                        (void)free_heap(memory, base, state);
                        memory.store(GuestAddress{runtime_.errno_cell}, 32, 12); return 0;
                    } catch (...) { (void)free_heap(memory, base, state); throw; }
                    return address;
                };
                std::vector<std::uint8_t> previous;
                try {
                    if (pointer) {
                        previous.resize(static_cast<std::size_t>(std::min(size, previous_allocation->size)));
                        memory.read(GuestAddress{pointer}, previous);
                    }
                } catch (const std::bad_alloc&) { memory.store(GuestAddress{runtime_.errno_cell}, 32, 12); }
                if (!pointer || previous.size() == std::min(size, previous_allocation->size)) {
                    result = allocate(size);
                    if (result && pointer) {
                        try { memory.write(GuestAddress{result}, previous); release(pointer); }
                        catch (...) { release(result); throw; }
                    }
                    // _aligned_recalloc：新擴充的部分清為零。
                    if (result && clearing && size > previous.size()) memory.write(GuestAddress{result + previous.size()}, std::vector<std::uint8_t>(static_cast<std::size_t>(size - previous.size())));
                }
            }
        }
        if(!result && failed_request && retry_allocation(*failed_request))return std::nullopt;
    }
    else if (symbol == "malloc" || symbol == "calloc") {
        const auto count=arg(0),unit=symbol=="calloc"?arg(1):1;
        if(unit && count>heap_max_request/unit)memory.store(GuestAddress{runtime_.errno_cell},32,12);
        else {
            const auto size=count*unit;result=guest_heap_allocate(memory,size);
            if(result){if(symbol=="calloc")memory.write(GuestAddress{result},std::vector<std::uint8_t>(static_cast<std::size_t>(std::max<std::uint64_t>(size,1)),0));}
            else result=allocate_heap(memory,size);
            if(!result && retry_allocation(size))return std::nullopt;
        }
    } else if (symbol == "free") { if (!free_heap(memory, arg(0), state)) throw HostCallFault("free 收到非配置起始地址"); }
    else if (symbol == "realloc" && guest_heap_contains(arg(0))) {
        // 配置區區塊：縮小或同級距原地保留；放大時配置新區塊、複製舊區塊內容後釋放。
        const auto old_size=guest_heap_block_size(memory,arg(0));
        if(!arg(1)){guest_heap_free(memory,arg(0));}
        else if(arg(1)>heap_max_request)memory.store(GuestAddress{runtime_.errno_cell},32,12);
        else if(arg(1)<=old_size)result=arg(0);
        else {
            result=guest_heap_allocate(memory,arg(1));if(!result)result=allocate_heap(memory,arg(1));
            if(!result && retry_allocation(arg(1)))return std::nullopt;
            if(result){std::vector<std::uint8_t> bytes(static_cast<std::size_t>(old_size));memory.read(GuestAddress{arg(0)},bytes);
                memory.write(GuestAddress{result},bytes);guest_heap_free(memory,arg(0));}
        }
    }
    else if (symbol == "realloc") {
        const auto found = allocations_.find(arg(0));
        if(arg(0) && found==allocations_.end())throw HostCallFault("realloc 收到非配置起始地址");
        if(arg(0) && !arg(1))(void)free_heap(memory,arg(0),state);
        else if(arg(1)>heap_max_request)memory.store(GuestAddress{runtime_.errno_cell},32,12);
        else {
            result=arg(0)?reallocate_heap(memory,arg(0),arg(1),state):allocate_heap(memory,arg(1));
            if(!result && retry_allocation(arg(1)))return std::nullopt;
        }
    } else if (symbol == "_recalloc") {
        // MSVC：以 count×size 重新配置；乘積溢位為 ENOMEM，NULL 視同 calloc，大小 0 釋放，擴增部分清零。
        const auto found = allocations_.find(arg(0));
        if(arg(0) && found==allocations_.end())throw HostCallFault("_recalloc 收到非配置起始地址");
        const auto count=arg(1),unit=arg(2);
        if(unit && count>heap_max_request/unit)memory.store(GuestAddress{runtime_.errno_cell},32,12);
        else {
            const std::uint64_t size=count*unit,previous=arg(0)?found->second:0;
            if(arg(0) && !size)(void)free_heap(memory,arg(0),state);
            else {
                result=arg(0)?reallocate_heap(memory,arg(0),size,state):allocate_heap(memory,size);
                if(!result && retry_allocation(size))return std::nullopt;
                if(result && size>previous)memory.fill(GuestAddress{result+previous},0,static_cast<std::size_t>(size-previous));
            }
        }
    } else if (symbol == "RtlMoveMemory" || symbol == "memcpy" || symbol == "memmove" || symbol == "memset" || symbol == "memcmp") {
        const auto count = arg(2); if (count > memory.limit()) throw HostCallFault("CRT memory operation 超過上限");
        if (symbol == "memcmp") {
            result=static_cast<std::uint32_t>(memory.compare(GuestAddress{arg(0)},GuestAddress{arg(1)},static_cast<std::size_t>(count)));
        } else {
            if(symbol=="memset")memory.fill(GuestAddress{arg(0)},static_cast<std::uint8_t>(arg(1)),static_cast<std::size_t>(count));
            else memory.move(GuestAddress{arg(0)},GuestAddress{arg(1)},static_cast<std::size_t>(count));
            result=arg(0);
        }
    } else if (symbol == "strstr" || symbol == "strchr" || symbol == "strrchr") {
        const auto text = read_ansi(memory, arg(0));
        const auto position = symbol == "strstr" ? text.find(read_ansi(memory, arg(1))) : (arg(1) & 255) == 0 ? text.size() :
            symbol == "strchr" ? text.find(static_cast<char>(arg(1) & 255)) : text.rfind(static_cast<char>(arg(1) & 255));
        result = position == std::string::npos ? 0 : arg(0) + position;
    }
    else if (symbol == "strlen") result = ansi_length(memory, arg(0));
    else if (symbol == "strcmp" || symbol == "strncmp") {
        const auto count = symbol == "strncmp" ? arg(2) : UINT64_C(1048576);
        if (count > 1048576) throw HostCallFault("CRT 字串比較超過上限");
        if(count){
            // 首字元即可決定的短呼叫留在入口，避免額外進入批次核心。
            const auto a=memory.load(GuestAddress{arg(0)},8),b=memory.load(GuestAddress{arg(1)},8);
            if(a!=b)result=static_cast<std::uint32_t>(static_cast<int>(a)-static_cast<int>(b));
            else if(a && count>1)result=static_cast<std::uint32_t>(compare_ansi(memory,arg(0)+1,arg(1)+1,static_cast<std::size_t>(count-1)));
        }
    } else if (symbol=="strcat" || symbol=="strncat" || symbol=="wcscat" || symbol=="wcsncat") {
        const bool wide=symbol.starts_with("w"),bounded=symbol=="strncat" || symbol=="wcsncat";
        const auto stride=wide?2U:1U;const auto count=bounded?arg(2):UINT64_C(1048576);
        if(count>1048576)throw HostCallFault("CRT 字串串接超過上限");
        const auto address=[&](std::uint64_t base,std::uint64_t index) {
            if(index>(UINT64_MAX-base)/stride)throw HostCallFault("CRT 字串串接地址溢位");
            return GuestAddress{base+index*stride};
        };
        std::uint64_t length=0;
        while(memory.load(address(arg(0),length),static_cast<std::uint8_t>(stride*8))) {
            if(++length>=1048576)throw HostCallFault("CRT 目的字串沒有上限內的 NUL");
        }
        std::vector<std::uint8_t> tail;bool terminated=false;
        for(std::uint64_t i=0;i<count;++i) {
            const auto value=memory.load(address(arg(1),i),static_cast<std::uint8_t>(stride*8));
            if(!value){terminated=true;break;}
            tail.push_back(static_cast<std::uint8_t>(value));if(wide)tail.push_back(static_cast<std::uint8_t>(value>>8));
        }
        if(!bounded && !terminated)throw HostCallFault("CRT 來源字串沒有上限內的 NUL");
        tail.resize(tail.size()+stride);memory.write(address(arg(0),length),tail);result=arg(0);
    } else if (symbol == "strcpy" || symbol == "strncpy") {
        std::vector<std::uint8_t> bytes;
        if (symbol == "strcpy") bytes = bytes_of(read_ansi(memory, arg(1)));
        else {
            if (arg(2) > 1048576 || arg(2) > UINT64_MAX - arg(1)) throw HostCallFault("strncpy 範圍超過上限");
            bytes.resize(static_cast<std::size_t>(arg(2)), 0);
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                const auto value = static_cast<std::uint8_t>(memory.load(GuestAddress{arg(1) + i}, 8));
                if (!value) break;
                bytes[i] = value;
            }
        }
        memory.write(GuestAddress{arg(0)}, bytes); result = arg(0);
    } else if (symbol == "__acrt_iob_func" || symbol == "__iob_func") {
        const auto index = symbol == "__iob_func" ? 0 : arg(0);
        if (index > 2) throw HostCallFault("CRT 標準串流索引不合法"); result = runtime_base_.value + 0x800 + index * 0x80;
    } else if (symbol == "fflush") result = 0;
    else if (symbol == "setvbuf") { if (crt_stream(arg(0)) < runtime_base_.value + 0x800 || crt_stream(arg(0)) > runtime_base_.value + 0x900) result = UINT32_MAX; }
    else if (symbol == "puts") { const CrtIoScope scope(*this,state,memory,runtime_base_.value+0x880);if(!scope)return std::nullopt;auto text = read_ansi(memory, arg(0)); text += '\n'; output(text); }
    else if (symbol == "putchar") { const CrtIoScope scope(*this,state,memory,runtime_base_.value+0x880);if(!scope)return std::nullopt;output(std::string(1, static_cast<char>(arg(0) & 255))); result = arg(0) & 255; }
    else if(symbol=="_invoke_watson")return 0xc0000409U;
    else if(symbol=="_invalid_parameter_noinfo" || symbol=="_invalid_parameter" || symbol=="_invalid_parameter_noinfo_noreturn"){
        if(current_invalid_parameter_handler()){
            std::vector<std::uint64_t> values(5,0);if(symbol=="_invalid_parameter")for(unsigned i=0;i<5;++i)values[i]=arg(i);
            const bool fatal=symbol=="_invalid_parameter_noinfo_noreturn";
            start_callbacks(state,memory,{{current_invalid_parameter_handler(),std::move(values)}},fatal?Continuation::Exit:Continuation::Return,fatal?0xc0000409U:0);return std::nullopt;
        }
        return 0xc0000409U;
    }
    else if(crt_format_symbol(symbol))return invoke_crt_format(state,memory,service,arg);
    else if (symbol == "signal" || symbol=="raise") {
        const auto signal=static_cast<std::uint32_t>(arg(0));
        if(signal!=2 && signal!=4 && signal!=8 && signal!=11 && signal!=15 && signal!=21 && signal!=22){
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(const auto invalid_callback=current_invalid_parameter_handler()){start_callbacks(state,memory,{{invalid_callback,{0,0,0,0,0}}},Continuation::Return,UINT32_MAX);return std::nullopt;}
            return 0xc0000409U;
        }
        auto& signal_callback=runtime_.signals[signal];
        if(symbol=="signal"){result=signal_callback;signal_callback=arg(1);}
        else if(signal_callback!=1){
            if(!signal_callback){if(signal!=15)return 3;}
            else {const auto callback=signal_callback;signal_callback=0;start_callbacks(state,memory,{{callback,signal==8?std::vector<std::uint64_t>{signal,0x8c}:std::vector<std::uint64_t>{signal}}},Continuation::Return,0);return std::nullopt;}
        }
    }
    else if (symbol == "__getmainargs" || symbol == "__wgetmainargs") {
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const bool wide=symbol=="__wgetmainargs";
        memory.check(GuestAddress{arg(0)}, 4, Access::Write); memory.check(GuestAddress{arg(1)}, unit, Access::Write); memory.check(GuestAddress{arg(2)}, unit, Access::Write);
        memory.store(GuestAddress{arg(0)}, 32, memory.load(GuestAddress{runtime_.argc_cell}, 32));
        memory.store(GuestAddress{arg(1)}, bits, memory.load(GuestAddress{wide?runtime_.wargv_cell:runtime_.argv_cell}, bits));
        memory.store(GuestAddress{arg(2)}, bits, memory.load(GuestAddress{wide?runtime_.wenviron_cell:runtime_.environ_cell}, bits));
    } else throw HostCallFault("尚未實作此執行階段服務：" + std::string(symbol));
    arg.return_to(state, result); return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_format(GuestState& state,GuestMemory& memory,
                                                          const WindowsServiceDescriptor& service,const WindowsCall& arg){
    std::uint64_t result=0;
    const auto [common,common_stream,common_buffer,stream,standard_output,counting,
                wide,secure,limited,unbounded,va,underscore,format_index]=crt_format_layout(service);
    const auto destination=stream || counting?0:arg(common_buffer?1:0),capacity=stream || counting?0:unbounded?output_limit:arg(common_buffer?2:1);
    const auto limit=limited?arg(common_buffer?3:2):capacity;const auto fmt=arg(format_index);const auto options=common?arg(0):0;
    const auto invalid_format=[&](unsigned error)->std::optional<std::uint32_t>{
        if(destination && capacity)memory.store(GuestAddress{destination},wide?16:8,0);memory.store(GuestAddress{runtime_.errno_cell},32,error);
        if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,UINT32_MAX);return std::nullopt;}return 0xc0000417U;
    };
    const auto target=stream?(standard_output?runtime_base_.value+0x880:crt_stream(common_stream?arg(1):arg(0))):0;
    const CrtIoScope scope(*this,state,memory,target);if(!scope)return std::nullopt;
    if(stream)if(auto* file=crt_file(target);file && (!import_crt_lowio(memory,*file) || (!wide && file->text && (file->lowio_extended&3U))))return invalid_format(22);
    if(!fmt)return invalid_format(22);
    if(common && ((options&~UINT64_C(0x27)) || arg(format_index+1)))throw HostCallFault("目前 CRT formatter 僅支援預設 C locale 與基本 UCRT options");
    if(limited && !destination && !capacity && !limit){arg.return_to(state,0);return std::nullopt;}
    if(!stream && !counting && ((secure && (!destination || !capacity)) || (!destination && capacity)))return invalid_format(22);
    if(limited && !limit){memory.store(GuestAddress{destination},wide?16:8,0);arg.return_to(state,UINT32_MAX);return std::nullopt;}
    unsigned index=format_index+1;std::uint64_t list=va?arg(common?format_index+2:format_index+1):0;
    std::u16string specification;if(wide)specification=read_utf16(memory,GuestAddress{fmt});else {const auto bytes=read_ansi(memory,fmt);for(const auto c:bytes)specification.push_back(static_cast<unsigned char>(c));}
    Formatted formatted;
    try {formatted=format(memory,specification,wide,wide && (!common || (options&4)),x86_?4:8,common?(options&0x20)!=0:underscore,[&](unsigned size){
        if(va){const auto value=memory.load(GuestAddress{list},static_cast<std::uint8_t>(size*8));list+=x86_?size:8;return value;}
        if(x86_){const auto value=memory.load(GuestAddress{state.reg(Gpr::Rsp)+4ULL*(index+1)},static_cast<std::uint8_t>(size*8));index+=size/4;return value;}return arg(index++);
    });}catch(const FormatError& error){return invalid_format(error.code);}
    const auto& text=formatted.text;if(formatted.encoding_error)memory.store(GuestAddress{runtime_.errno_cell},32,formatted.encoding_error);
    result=formatted.failed?UINT32_MAX:text.size();
    if(!stream && !counting){
        const bool legacy=!secure && (common_buffer?(options&1)!=0:underscore);
        if(!destination && !capacity){arg.return_to(state,result);return std::nullopt;}
        if(secure && !limited && text.size()>=capacity)return invalid_format(34);
        if(limited && limit>=(capacity) && limit!=(x86_?UINT32_MAX:UINT64_MAX) && text.size()>=capacity)return invalid_format(34);
        const auto available=capacity?capacity-(legacy?0U:1U):0;auto length=std::min<std::uint64_t>(text.size(),available);if(limited)length=std::min(length,limit);
        if(capacity){std::vector<std::uint8_t> bytes;bytes.reserve(static_cast<std::size_t>((length+1)*(wide?2:1)));for(std::uint64_t i=0;i<length;++i){bytes.push_back(static_cast<std::uint8_t>(text[i]));if(wide)bytes.push_back(static_cast<std::uint8_t>(text[i]>>8));}if(length<capacity)bytes.resize(bytes.size()+(wide?2:1));memory.write(GuestAddress{destination},bytes);}
        if(limited && length<text.size())result=UINT32_MAX;
        else if(legacy && text.size()>capacity)result=UINT32_MAX;
        else if(common_buffer && !secure && !legacy && !(options&2) && text.size()>=capacity)result=UINT32_MAX;
        if(!capacity && legacy){memory.store(GuestAddress{runtime_.errno_cell},32,34);result=UINT32_MAX;}
    }else if(stream){
        std::uint64_t written=0;
        if(wide)written=write_crt_wide_stream(state,memory,target,text);
        else {std::vector<std::uint8_t> bytes;bytes.reserve(text.size());for(auto c:text)bytes.push_back(static_cast<std::uint8_t>(c));written=write_crt_stream(memory,target,bytes);}
        if(written!=text.size())result=UINT32_MAX;
    }
    arg.return_to(state,result);return std::nullopt;
}

}
