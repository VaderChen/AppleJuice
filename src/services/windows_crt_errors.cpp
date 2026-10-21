#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <bit>

namespace x64arm {
namespace {
std::string_view runtime_error(std::int32_t number) {
    switch(number) {
    case 0:return "stack overflow";
    case 1:return "null pointer assignment";
    case 2:return "floating point support not loaded";
    case 3:return "integer divide by zero";
    case 8:return "not enough space for arguments";
    case 9:return "not enough space for environment";
    case 10:return "abnormal program termination";
    case 16:return "not enough space for thread data";
    case 17:return "unexpected multithread lock error";
    case 18:return "unexpected heap error";
    case 19:return "unable to open console device";
    case 24:return "not enough space for _onexit/atexit table";
    case 25:return "pure virtual function call";
    case 26:return "not enough space for stdio initialization";
    case 27:return "not enough space for lowio initialization";
    case 28:return "unable to initialize heap";
    case 29:return "unable to initialize environment";
    case 30:return "CRT not initialized";
    case 31:return "attempt to initialize the CRT more than once";
    default:return "unrecognized C runtime error";
    }
}
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_error(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);
    const auto number=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(0)));
    if(service.symbol=="_set_error_mode") {
        const auto previous=runtime_.crt_error_mode;
        if(number<0 || number>3) {
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(const auto handler=current_invalid_parameter_handler()) {start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,UINT32_MAX);return std::nullopt;}
            return 0xc0000417U;
        }
        if(number!=3)runtime_.crt_error_mode=number;arg.return_to(state,static_cast<std::uint32_t>(previous));return std::nullopt;
    }
    const bool assertion=service.symbol=="_assert" || service.symbol=="_wassert";
    std::string message;
    if(assertion){
        const auto read=[&](std::uint64_t pointer){return service.symbol=="_wassert"?read_utf16(memory,GuestAddress{pointer}):cp1252_to_utf16(read_ansi(memory,pointer));};
        message="Assertion failed!\nFile: "+utf16_to_utf8(read(arg(1)))+"\nLine: "+std::to_string(static_cast<std::uint32_t>(arg(2)))+"\nExpression: "+utf16_to_utf8(read(arg(0)))+"\n";
    }else message="runtime error R"+std::to_string(INT64_C(6000)+number)+"\n"+std::string(runtime_error(number))+"\n";
    if(runtime_.crt_error_mode==2 || (!runtime_.crt_error_mode && runtime_.app_type==2)) {
        const auto text=utf16_bytes(utf8_to_utf16(message)),caption=utf16_bytes(u"Microsoft Visual C++ Runtime Library");
        const auto base=allocate_heap(memory,text.size()+caption.size());
        if(!base)return assertion?3U:255U;
        try{
            memory.write(GuestAddress{base},text);memory.write(GuestAddress{base+text.size()},caption);
            // 共用原生／headless 模態對話框；保留呼叫端堆疊，Ignore 可正常返回。
            start_callbacks(state,memory,{{resolve("user32.dll","MessageBoxW").value,{0,base,base+text.size(),assertion?0x12U:0x10U}}},assertion?Continuation::CrtAssertion:Continuation::CrtFatalDialog,base);
        }catch(...){(void)free_heap(memory,base,state);throw;}
        return std::nullopt;
    }
    (void)write_standard_output(2,{reinterpret_cast<const std::uint8_t*>(message.data()),message.size()});return assertion?3U:255U;
}
}
