#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <bit>
#include "msvcp_stream_abi.hpp"
#include "msvc_exception_abi.hpp"
#include "msvc_eh_metadata.hpp"

namespace x64arm {
std::span<const WindowsServiceDescriptor> msvc_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
#include "msvcp_yarn_descriptors.inc"
#include "msvcp100_streambuf_descriptors.inc"
#include "msvcp100_descriptors.inc"
#include "msvcp140_descriptors.inc"
        {"crt","??0exception@std@@QEAA@AEBV01@@Z",2},
        {"crt","??0exception@std@@QAE@ABV01@@Z",2},
        {"crt","??0exception@std@@QEAA@PEBD@Z",2},
        {"crt","??0exception@std@@QAE@PBD@Z",2},
        {"crt","??1exception@std@@UEAA@XZ",1},
        {"crt","??1exception@std@@UAE@XZ",1},
        {"crt","??0bad_cast@std@@QEAA@AEBV01@@Z",2},
        {"crt","??0bad_cast@std@@QAE@ABV01@@Z",2},
        {"crt","??0bad_cast@std@@QEAA@PEBD@Z",2},
        {"crt","??0bad_cast@std@@QAE@PBD@Z",2},
        {"crt","??1bad_cast@std@@UEAA@XZ",1},
        {"crt","??1bad_cast@std@@UAE@XZ",1},
        {"crt","??0bad_typeid@std@@QEAA@AEBV01@@Z",2},
        {"crt","??0bad_typeid@std@@QAE@ABV01@@Z",2},
        {"crt","??0bad_typeid@std@@QEAA@PEBD@Z",2},
        {"crt","??0bad_typeid@std@@QAE@PBD@Z",2},
        {"crt","??1bad_typeid@std@@UEAA@XZ",1},
        {"crt","??1bad_typeid@std@@UAE@XZ",1},
        {"crt","??0exception@std@@QEAA@AEBQEBD@Z",2},
        {"crt","??0exception@std@@QAE@ABQBD@Z",2},
        {"crt","??0exception@std@@QEAA@AEBQEBDH@Z",3},
        {"crt","??0exception@std@@QAE@ABQBDH@Z",3},
        {"crt","?what@exception@std@@UEBAPEBDXZ",1},
        {"crt","?what@exception@std@@UBEPBDXZ",1},
        {"crt","?_type_info_dtor_internal_method@type_info@@QEAAXXZ",1},
        {"crt","?_type_info_dtor_internal_method@type_info@@QAEXXZ",1},
        {"msvcp140.dll","?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Fiopen@std@@YAPEAU_iobuf@@PEBGHH@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Fiopen@std@@YAPAU_iobuf@@PB_WHH@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Fiopen@std@@YAPAU_iobuf@@PBDHH@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Fiopen@std@@YAPAU_iobuf@@PBGHH@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Xbad_alloc@std@@YAXXZ",0,0,0,0,true},
        {"msvcp140.dll","?_Xbad_function_call@std@@YAXXZ",0,0,0,0,true},
        {"msvcp140.dll","?_Xlength_error@std@@YAXPEBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xlength_error@std@@YAXPBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xout_of_range@std@@YAXPEBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xout_of_range@std@@YAXPBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xinvalid_argument@std@@YAXPEBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xinvalid_argument@std@@YAXPBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xruntime_error@std@@YAXPEBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xruntime_error@std@@YAXPBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xoverflow_error@std@@YAXPEBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xoverflow_error@std@@YAXPBD@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Xregex_error@std@@YAXW4error_type@regex_constants@1@@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Random_device@std@@YAIXZ",0,0,0,0,true},
        {"msvcp140.dll","?generic_category@std@@YAAEBVerror_category@1@XZ",0,0,0,0,true},{"msvcp140.dll","?generic_category@std@@YAABVerror_category@1@XZ",0,0,0,0,true},
        {"msvcp140.dll","?system_category@std@@YAAEBVerror_category@1@XZ",0,0,0,0,true},{"msvcp140.dll","?system_category@std@@YAABVerror_category@1@XZ",0,0,0,0,true},
        {"msvcp140.dll","?iostream_category@std@@YAAEBVerror_category@1@XZ",0,0,0,0,true},{"msvcp140.dll","?iostream_category@std@@YAABVerror_category@1@XZ",0,0,0,0,true},
        {"msvcp140.dll","?_Winerror_map@std@@YAHH@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Throw_future_error@std@@YAXAEBVerror_code@1@@Z",1,0,0,0,true},{"msvcp140.dll","?_Throw_future_error@std@@YAXABVerror_code@1@@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Rethrow_future_exception@std@@YAXVexception_ptr@1@@Z",1,0,0,0,true},
        {"msvcp140.dll","?_Winerror_message@std@@YAKKPEADK@Z",3,0,0,0,true},{"msvcp140.dll","?_Winerror_message@std@@YAKKPADK@Z",3,0,0,0,true},
        {"msvcp140.dll","?_Syserror_map@std@@YAPEBDH@Z",1,0,0,0,true},{"msvcp140.dll","?_Syserror_map@std@@YAPBDH@Z",1,0,0,0,true},
        {"crt","_CxxThrowException",2},{"crt","__uncaught_exception",0},{"msvcp140.dll","?uncaught_exception@std@@YA_NXZ",0},{"crt","__uncaught_exceptions",0},{"crt","__current_exception",0},{"crt","__current_exception_context",0},{"crt","__processing_throw",0},{"crt","__AdjustPointer",2},{"crt","__GetPlatformExceptionInfo",1},{"crt","_SetWinRTOutOfMemoryExceptionCallback",1},{"crt","__CxxFrameHandler",4},{"crt","__CxxFrameHandler3",4},{"vcruntime140_1.dll","__CxxFrameHandler4",4},
        {"crt","?terminate@@YAXXZ",0},{"crt","terminate",0},{"crt","__std_terminate",0},{"crt","_purecall",0},
        {"crt","??1type_info@@UEAA@XZ",1},{"crt","??1type_info@@UAE@XZ",0},{"crt","_XcptFilter",2},
        {"crt","_seh_filter_dll",2},{"crt","_seh_filter_exe",2},{"crt","_amsg_exit",1},{"crt","_assert",3},{"crt","_wassert",3},{"crt","_except_handler3",4},{"crt","_except_handler4_common",6},
        {"crt","_set_error_mode",1},{"crt","_set_purecall_handler",1},{"crt","_get_purecall_handler",0},
        {"crt","_local_unwind2",2},{"crt","_seh_longjmp_unwind",1},{"crt","_abnormal_termination",0},
        {"crt","__unDName",6},{"crt","__unDNameEx",7},{"crt","__std_type_info_destroy_list",1},
        {"crt","__std_type_info_name",2},{"crt","__std_type_info_compare",2},{"crt","__std_type_info_hash",1}
        ,{"crt","__telemetry_main_invoke_trigger",1},{"crt","__telemetry_main_return_trigger",1}
        ,{"crt","__RTtypeid",1},{"crt","__RTCastToVoid",1},{"crt","__RTDynamicCast",5}
        ,{"x64arm.msvc","ExceptionCopy",2},{"x64arm.msvc","ExceptionWhat",1},{"x64arm.msvc","ExceptionDestroy",1},{"x64arm.msvc","ExceptionDelete",2}
    };
    // VC10～VC12 的標準例外拋出入口共用既有 EH／RTTI 實作；
    // 不把不同版本 STL 物件版面或其餘匯出一概別名至 VC14。
    static const auto versions=[] {
        std::vector<WindowsServiceDescriptor> all(std::begin(table),std::end(table));
        for(const auto family:{"msvcp100.dll","msvcp110.dll","msvcp120.dll"})
            for(const auto& item:table)if(item.family=="msvcp140.dll" &&
                ((item.symbol.starts_with("?_X") && !item.symbol.starts_with("?_Xregex")) || item.symbol.starts_with("?_Fiopen@std@@"))){
                auto entry=item;entry.family=family;all.push_back(entry);
            }
        // _Fiopen 的路徑／openmode／sharemode ABI 相同，FILE 仍由共用 CRT 管理。
        // VC10 _Lockit 保留 int locktype；只共用此經核對的鎖契約，不轉送 STL 物件。
        for(const auto& item:std::array<WindowsServiceDescriptor,6>{{
            {"msvcp100.dll","??0_Lockit@std@@QEAA@H@Z",2},
            {"msvcp100.dll","??0_Lockit@std@@QEAA@XZ",1},
            {"msvcp100.dll","??1_Lockit@std@@QEAA@XZ",1},
            {"msvcp100.dll","??0_Lockit@std@@QAE@H@Z",1},
            {"msvcp100.dll","??0_Lockit@std@@QAE@XZ",0},
            {"msvcp100.dll","??1_Lockit@std@@QAE@XZ",0}}})all.push_back(item);
        return all;
    }();
    return versions;
}
namespace {
constexpr std::uint32_t cpp_code=0xe06d7363U;
std::int32_t signed32(std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
std::uint64_t add_offset(std::uint64_t base,std::int64_t offset){
    if((offset>=0 && static_cast<std::uint64_t>(offset)>UINT64_MAX-base) || (offset<0 && static_cast<std::uint64_t>(-offset)>base))throw HostCallFault("MSVC EH 位址溢位");
    return offset<0?base-static_cast<std::uint64_t>(-offset):base+static_cast<std::uint64_t>(offset);
}
std::uint64_t rva(const GuestMemory& memory,const RuntimeImage& image,std::uint64_t offset,std::size_t size){
    if(offset>image.size || size>image.size-offset)throw HostCallFault("MSVC EH RVA 超出所屬映像");
    const auto address=add_offset(image.base,static_cast<std::int64_t>(offset));memory.check(GuestAddress{address},size,Access::Read);return address;
}
std::uint32_t dword(const GuestMemory& memory,std::uint64_t at){return static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));}
using msvc_eh::CatchEntry;using msvc_eh::TryEntry;using msvc_eh::FunctionInfo;using msvc_eh::UnwindEntry;
FunctionInfo function_info(const GuestMemory& memory,const RuntimeImage& image,std::uint64_t address){
    if(address<image.base)throw HostCallFault("MSVC FuncInfo 位址不在映像內");
    const auto at=rva(memory,image,address-image.base,32);const auto magic=dword(memory,at)&0x1fffffffU;
    if(magic<0x19930520U || magic>0x19930522U)throw HostCallFault("尚未支援此 MSVC FuncInfo 版本");
    const auto states=dword(memory,at+4),tries=dword(memory,at+12),ips=dword(memory,at+20);
    if(states>65536 || tries>4096 || ips>65536)throw HostCallFault("MSVC EH metadata 數量過大");
    FunctionInfo result;result.address=address;result.help=signed32(dword(memory,at+28));
    if(magic>=0x19930521U){(void)rva(memory,image,address-image.base,36);if(dword(memory,at+32))throw HostCallFault("尚未提供 MSVC dynamic exception specification");}
    if(magic>=0x19930522U){(void)rva(memory,image,address-image.base,40);result.flags=dword(memory,at+36);}
    const auto unwind=rva(memory,image,dword(memory,at+8),static_cast<std::size_t>(states)*8);
    for(std::uint32_t i=0;i<states;++i){const auto entry=unwind+i*8;const auto next=signed32(dword(memory,entry));const auto action=dword(memory,entry+4);
        if(next<-1 || next>=static_cast<std::int32_t>(i))throw HostCallFault("MSVC unwind state 不向外層推進");
        result.unwind.push_back({next,action?rva(memory,image,action,1):0,action?UnwindEntry::Kind::Funclet:UnwindEntry::Kind::None,0});
    }
    const auto table=rva(memory,image,dword(memory,at+16),static_cast<std::size_t>(tries)*20);
    for(std::uint32_t i=0;i<tries;++i){const auto entry=table+i*20;TryEntry block;block.low=signed32(dword(memory,entry));block.high=signed32(dword(memory,entry+4));block.catch_high=signed32(dword(memory,entry+8));
        const auto count=dword(memory,entry+12);if(count>4096 || block.low<0 || block.high<block.low || block.catch_high<block.high || block.catch_high>=static_cast<std::int32_t>(states))throw HostCallFault("MSVC try state 範圍不合法");
        const auto catches=rva(memory,image,dword(memory,entry+16),static_cast<std::size_t>(count)*20);
        for(std::uint32_t j=0;j<count;++j){const auto item=catches+j*20;const auto type=dword(memory,item+4);block.handlers.push_back({dword(memory,item),type?rva(memory,image,type,17):0,rva(memory,image,dword(memory,item+12),1),signed32(dword(memory,item+8)),signed32(dword(memory,item+16))});}
        result.tries.push_back(std::move(block));
    }
    const auto ip_table=rva(memory,image,dword(memory,at+24),static_cast<std::size_t>(ips)*8);
    for(std::uint32_t i=0;i<ips;++i){const auto entry=ip_table+i*8;const auto ip=dword(memory,entry);const auto level=signed32(dword(memory,entry+4));
        if(ip>=image.size || (!result.ips.empty() && ip<result.ips.back().first) || level<-1 || level>=static_cast<std::int32_t>(states))throw HostCallFault("MSVC IP-state table 不合法");
        result.ips.emplace_back(ip,level);
    }
    return result;
}
std::int32_t ip_state(const FunctionInfo& info,const RuntimeImage& image,std::uint64_t pc){
    std::int32_t level=-1;for(const auto& [ip,state]:info.ips){if(pc<image.base+ip)break;level=state;}return level;
}
std::uint64_t adjusted_pointer(const GuestMemory& memory,std::uint64_t object,std::uint64_t type){
    if(!object)return 0;
    const auto member=signed32(dword(memory,type+8)),vbptr=signed32(dword(memory,type+12)),vbase=signed32(dword(memory,type+16));
    if(vbptr>=0){const auto at=add_offset(object,vbptr);const auto table=memory.load(GuestAddress{at},64);object=add_offset(at,signed32(dword(memory,add_offset(table,vbase))));}
    return add_offset(object,member);
}
std::string type_name(const GuestMemory& memory,std::uint64_t type){
    std::string name;for(std::uint64_t i=0;i<4096;++i){const auto ch=memory.load(GuestAddress{add_offset(type,static_cast<std::int64_t>(16+i))},8);if(!ch)return name;name.push_back(static_cast<char>(ch));}throw HostCallFault("MSVC TypeDescriptor 名稱沒有終止字元");
}
}
std::uint64_t WindowsServices::msvc_exception_cells(GuestState&,GuestMemory& memory,bool create,bool refresh){
    std::uint64_t cells=teb_base_.value+0x3fe0;bool created=false;
    if(!threads_.items.empty()){
        auto& thread=threads_.items.at(threads_.current);
        auto& storage=thread.fiber?threads_.fibers.at(thread.fiber).msvc_exception_cells:thread.msvc_exception_cells;
        // 第三格為 __processing_throw 回傳的 int 計數；前兩格為 __current_exception／context。
        if(!storage && create){storage=allocate_heap(memory,x86_?12:24);if(!storage)throw std::bad_alloc{};memory.store(GuestAddress{storage+(x86_?8:16)},32,0);created=true;}
        cells=storage;
    }
    if(cells && (created || refresh)){
        const auto active=std::find_if(exceptions_.stack.rbegin(),exceptions_.stack.rend(),[](const ExceptionDispatch& item){return item.msvc.catching;});
        const std::uint8_t bits=x86_?32:64;const unsigned unit=x86_?4:8;
        memory.store(GuestAddress{cells},bits,active==exceptions_.stack.rend()?0:active->record);
        memory.store(GuestAddress{cells+unit},bits,active==exceptions_.stack.rend()?0:active->context);
    }
    return cells;
}
std::optional<std::uint32_t> WindowsServices::invoke_msvc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.family=="msvcp100.dll" && service.symbol.find("_Lockit@std@@")!=service.symbol.npos)return invoke_nt_native(state,memory,service);
    if(service.symbol.starts_with("?_XGetLastError@") || service.symbol.starts_with("?generic_category@") || service.symbol.starts_with("?system_category@") || service.symbol.starts_with("?iostream_category@") || service.symbol.starts_with("?_Winerror_map@") || service.symbol.starts_with("?_Winerror_message@") || service.symbol.starts_with("?_Syserror_map@") || service.symbol.starts_with("?_Throw_future_error@") || service.symbol.starts_with("?_Rethrow_future_exception@"))return invoke_msvcp100(state,memory,service);
    if(service.symbol.starts_with("??0exception@") || service.symbol.starts_with("??1exception@") || service.symbol.starts_with("??0bad_") || service.symbol.starts_with("??1bad_") || service.symbol.starts_with("?what@exception@") || service.symbol.starts_with("?_type_info_dtor_internal_method@") || service.family=="x64arm.msvc" || service.symbol.starts_with("__RT") || service.symbol.starts_with("?_X") || service.symbol=="?_Random_device@std@@YAIXZ")return invoke_rtti(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
#include "msvcp_yarn_handlers.inc"
#include "msvcp100_streambuf_handlers.inc"
    if(service.family=="internal-msvcp100" || service.family=="internal-msvcp140" ||
       (service.family=="msvcp100.dll" && !symbol.starts_with("?_Fiopen@")) ||
       (service.family=="msvcp140.dll" && (symbol.find("basic_streambuf@")!=symbol.npos || symbol.find("basic_ios@")!=symbol.npos ||
        symbol.find("basic_istream@")!=symbol.npos || symbol.find("basic_ostream@")!=symbol.npos || symbol.find("basic_iostream@")!=symbol.npos ||
        symbol.find("codecvt")!=symbol.npos || symbol.find("ctype@")!=symbol.npos || symbol.find("_Locinfo@")!=symbol.npos ||
        symbol.find("num_put@")!=symbol.npos || symbol.find("numpunct@")!=symbol.npos || symbol.find("numpunct_byname@")!=symbol.npos ||
        symbol.find("@locale@std@@")!=symbol.npos || symbol.starts_with("?__ExceptionPtr") || symbol.starts_with("?setw@") || symbol.starts_with("?setprecision@") || symbol.starts_with("?_Throw_C_error@") ||
        symbol.starts_with("?_Throw_Cpp_error@") || symbol.find("ios_base@std@@")!=symbol.npos || symbol.find("time_put@")!=symbol.npos)))return invoke_msvcp100(state,memory,service);
    if(symbol=="__processing_throw"){
        // vcruntime 每執行緒的 _ProcessingThrow；FH4 在派送 catch 期間增減。
        const auto cells=msvc_exception_cells(state,memory,true);arg.return_to(state,cells+(x86_?8:16));return std::nullopt;
    }
    if(symbol=="__AdjustPointer"){
        // PMD {mdisp,pdisp,vdisp}：先依 vbtable 調整虛擬基底，再加上成員位移。
        const auto object=arg(0),pmd=arg(1);const auto value=[&](unsigned i){return static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{pmd+i*4},32))));};
        auto result=object+static_cast<std::uint64_t>(value(0));
        if(value(1)>=0){const auto table=memory.load(GuestAddress{object+static_cast<std::uint64_t>(value(1))},x86_?32:64);
            result+=static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{table+static_cast<std::uint64_t>(value(2))},32))))+value(1));}
        if(x86_)result&=0xffffffffU;arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="_SetWinRTOutOfMemoryExceptionCallback"){runtime_.winrt_oom_callback=arg(0);arg.return_to(state,0);return std::nullopt;}
    if(symbol=="__GetPlatformExceptionInfo"){
        // 目前處理中的 C++ 例外若為 WinRT 例外，回傳 WINRTEXCEPTIONINFO；std::bad_alloc 另以輸出旗標標示。
        const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
        if(arg(0))memory.store(GuestAddress{arg(0)},32,0);
        const auto active=std::find_if(exceptions_.stack.rbegin(),exceptions_.stack.rend(),[](const ExceptionDispatch& item){return item.msvc.catching;});
        std::uint64_t result=0;
        if(active!=exceptions_.stack.rend() && active->code==cpp_code && active->record){
            const auto record=active->record;const auto info=record+(x86_?20U:32U);const auto count=memory.load(GuestAddress{record+(x86_?16U:24U)},32);
            const auto magic=memory.load(GuestAddress{info},bits)&0xffffffffU;
            if(count>=3 && (magic==0x19930520U || magic==0x19930521U || magic==0x19930522U || magic==0x01994000U)){
                const auto object=memory.load(GuestAddress{info+unit},bits),throw_info=memory.load(GuestAddress{info+2*unit},bits);
                const auto image=!x86_ && count>=4?memory.load(GuestAddress{info+3*unit},bits):0;
                const auto address=[&](std::uint64_t at){const auto raw=memory.load(GuestAddress{at},32);return x86_?raw:(raw?image+raw:0);};
                if(throw_info){
                    const auto attributes=memory.load(GuestAddress{throw_info},32);
                    if(attributes&8U){const auto slot=memory.load(GuestAddress{object-unit},bits);result=slot?memory.load(GuestAddress{slot},bits):0;}
                    else if(const auto types=address(throw_info+12)){
                        const auto entries=memory.load(GuestAddress{types},32);constexpr std::string_view bad_alloc=".?AVbad_alloc@std@@";
                        for(std::uint64_t i=0;i<std::min<std::uint64_t>(entries,64);++i){
                            const auto catchable=address(types+4+i*4);if(!catchable)continue;const auto descriptor=address(catchable+4);if(!descriptor)continue;
                            if(read_ansi(memory,descriptor+2*unit,256)==bad_alloc){if(arg(0))memory.store(GuestAddress{arg(0)},32,1);break;}
                        }
                    }
                }
            }
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="__current_exception" || symbol=="__current_exception_context"){
        const auto cells=msvc_exception_cells(state,memory,true);
        arg.return_to(state,cells+(symbol=="__current_exception_context"?(x86_?4:8):0));return std::nullopt;
    }
    if(symbol=="__telemetry_main_invoke_trigger" || symbol=="__telemetry_main_return_trigger"){
        // VS2015 Update 3 已移除 CRT 啟動／DllMain 的 ETW eventing。
        // 現行 Microsoft DLL 保留的 void cdecl 入口只有 RET 0；保留暫存器、LastError 與客體資料。
        arg.return_to(state,state.reg(Gpr::Rax));return std::nullopt;
    }
    if(symbol=="__uncaught_exception" || symbol=="?uncaught_exception@std@@YA_NXZ" || symbol=="__uncaught_exceptions" || symbol=="?uncaught_exceptions@std@@YAHXZ"){
        // 使用既有執行緒／fiber 例外堆疊；catch 離開後的物件解構不再算未捕捉。
        const auto count=std::count_if(exceptions_.stack.begin(),exceptions_.stack.end(),[](const ExceptionDispatch& item){return item.code==cpp_code && !item.msvc.entered_catch;});
        arg.return_to(state,symbol=="__uncaught_exceptions" || symbol=="?uncaught_exceptions@std@@YAHXZ"?static_cast<std::uint64_t>(count):static_cast<std::uint64_t>(count!=0));return std::nullopt;
    }
    if(symbol.starts_with("?_Fiopen@std@@"))return invoke_crt_io(state,memory,service);
    if(symbol=="_except_handler3" || symbol=="_except_handler4_common" || symbol=="_local_unwind2" || symbol=="_seh_longjmp_unwind")return invoke_eh4(state,memory,service);
    if(symbol=="_abnormal_termination"){
        bool abnormal=!exceptions_.stack.empty() && exceptions_.stack.back().stage==ExceptionStage::Unwind;
        for(auto i=runtime_.callbacks.size();i-->0;)if(runtime_.callbacks[i].continuation==Continuation::Eh4Operation){
            const auto op=runtime_.eh4_operations.find(runtime_.callbacks[i].value);
            if(op!=runtime_.eh4_operations.end() && op->second.phase==Eh4Operation::Phase::Finally){abnormal=true;break;}
        }
        arg.return_to(state,abnormal);return std::nullopt;
    }
    if(symbol=="_amsg_exit" || symbol=="_set_error_mode" || symbol=="_assert" || symbol=="_wassert")return invoke_crt_error(state,memory,service);
    if(symbol=="_seh_filter_dll") {
        // DLL 啟動 filter 只委派 Microsoft C++ 例外；不將 access violation 等轉成 CRT signal。
        if(static_cast<std::uint32_t>(arg(0))!=cpp_code){arg.return_to(state,0);return std::nullopt;}
        return invoke_msvc(state,memory,WindowsServiceDescriptor{"crt","_XcptFilter",2});
    }
    if(symbol=="__unDName" || symbol=="__unDNameEx" || symbol.starts_with("__std_type_info_"))return invoke_demangle(state,memory,service);
    if(symbol=="_set_purecall_handler" || symbol=="_get_purecall_handler"){
        const auto previous=runtime_.purecall_handler;
        if(symbol=="_set_purecall_handler")runtime_.purecall_handler=arg(0);
        arg.return_to(state,previous);return std::nullopt;
    }
    if(symbol=="_purecall" && runtime_.purecall_handler){
        start_callbacks(state,memory,{{runtime_.purecall_handler,{}}},Continuation::Exit,3);return std::nullopt;
    }
    if(symbol=="?terminate@@YAXXZ" || symbol=="terminate" || symbol=="__std_terminate" || symbol=="_purecall")return 3;
    if(symbol=="??1type_info@@UEAA@XZ" || symbol=="??1type_info@@UAE@XZ"){
        // Win32 的零明確參數 thiscall 從 ECX 取得 this，只彈出返回位址。
        const auto object=symbol=="??1type_info@@UAE@XZ"?(x86_?state.reg(Gpr::Rcx)&UINT32_MAX:state.reg(Gpr::Rcx)):arg(0);
        const auto cell=add_offset(object,x86_?4:8);const std::uint8_t width=x86_?32:64;
        memory.check(GuestAddress{cell},x86_?4:8,Access::Write);
        const auto cached=memory.load(GuestAddress{cell},width);if(cached && !free_heap(memory,cached,state))throw HostCallFault("type_info cache 不是有效 CRT 配置");
        memory.store(GuestAddress{cell},width,0);arg.return_to(state,0);return std::nullopt;
    }
    if(symbol=="_XcptFilter" || symbol=="_seh_filter_exe"){
        // 未安裝對應 CRT signal handler 時，保留 Windows SEH 的繼續搜尋契約。
        const auto code=static_cast<std::uint32_t>(arg(0));const auto signal=code==0xc0000005U?11U:(code==0xc000001dU || code==0xc0000096U)?4U:(code>=0xc000008dU && code<=0xc0000095U)?8U:0U;
        const auto found=runtime_.signals.find(signal);const auto handler=found==runtime_.signals.end()?0:found->second;
        if(!signal || !handler){arg.return_to(state,0);return std::nullopt;}
        if(handler==1){arg.return_to(state,UINT32_MAX);return std::nullopt;}
        runtime_.signals[signal]=0;start_callbacks(state,memory,{{handler,{signal,0}}},Continuation::Return,UINT32_MAX);return std::nullopt;
    }
    if(symbol=="_CxxThrowException"){
        auto caller=state;arg.return_to(caller,0);
        if(!arg(0) && !arg(1)){
            const auto active=std::find_if(exceptions_.stack.rbegin(),exceptions_.stack.rend(),[](const ExceptionDispatch& item){return item.msvc.catching;});
            if(active==exceptions_.stack.rend())return 3;
            const auto record=msvc_detail::exception_record(memory,active->record,x86_);
            begin_exception(caller,memory,cpp_code,1,record.parameters);state=caller;return std::nullopt;
        }
        const auto* image=module_image(arg(1),true);if(!image || !arg(0))throw HostCallFault("MSVC ThrowInfo 沒有所屬客體映像");
        (void)rva(memory,*image,arg(1)-image->base,16);
        const std::array<std::uint64_t,4> parameters{0x19930520U,arg(0),arg(1),image->base};
        begin_exception(caller,memory,cpp_code,1,std::span(parameters).first(x86_?3U:4U));state=caller;return std::nullopt;
    }
    if(symbol!="__CxxFrameHandler" && symbol!="__CxxFrameHandler3" && symbol!="__CxxFrameHandler4")throw HostCallFault("未實作的 MSVC 執行階段服務");
    if(exceptions_.stack.empty())throw HostCallFault("MSVC personality 缺少例外派送狀態");
    auto& dispatch=exceptions_.stack.back();if(arg(0)!=dispatch.record || arg(3)!=dispatch.dispatcher)throw HostCallFault("MSVC personality DispatcherContext 不符");
    const auto* image=unwind_image(memory,memory.load(GuestAddress{dispatch.dispatcher},64),memory.load(GuestAddress{dispatch.dispatcher+16},64));if(!image)throw HostCallFault("MSVC personality 映像已不存在");
    const auto pc=memory.load(GuestAddress{dispatch.dispatcher},64);const auto frame=memory.load(GuestAddress{dispatch.dispatcher+24},64);
    const auto data=memory.load(GuestAddress{dispatch.dispatcher+56},64);const auto* function=lookup_function(*image,pc);
    if(!function)throw HostCallFault("MSVC personality 缺少函式 unwind entry");
    const auto info=symbol=="__CxxFrameHandler4"?msvc_eh::function_info4(memory,*image,rva(memory,*image,dword(memory,data),1),function->begin):function_info(memory,*image,rva(memory,*image,dword(memory,data),32));
    auto level=ip_state(info,*image,pc);std::uint64_t parent=frame;std::int32_t cleanup_limit=-1;const TryEntry* in_catch=nullptr;
    if(info.catch_funclet)parent=memory.load(GuestAddress{add_offset(frame,info.parent_offset)},64);
    if(!info.compressed)for(const auto& block:info.tries)if(level>block.high && level<=block.catch_high){
        for(const auto& caught:block.handlers)if(caught.handler==image->base+function->begin){parent=memory.load(GuestAddress{add_offset(frame,caught.parent_offset)},64);cleanup_limit=block.high;in_catch=&block;}
    }
    const auto help=info.compressed?0:add_offset(parent,info.help);if(help)memory.check(GuestAddress{help},8,Access::Write);
    if(dispatch.msvc.rethrow_state!=-2 && dispatch.msvc.parent==parent && dispatch.msvc.function_info==info.address){level=dispatch.msvc.rethrow_state;dispatch.msvc.rethrow_state=-2;}
    const bool cpp=dispatch.code==cpp_code && memory.load(GuestAddress{dispatch.record+24},32)==4;
    if((info.flags&1U) && !cpp && dispatch.code!=0x80000026U){arg.return_to(state,1);return std::nullopt;}
    if(dispatch.stage==ExceptionStage::Unwind){
        auto current=level;
        const auto target=(dispatch.index-1==dispatch.target_frame)?(dispatch.msvc.active?dispatch.msvc.target_state:level):cleanup_limit;
        std::vector<GuestCallback> callbacks;
        for(auto outer=exceptions_.stack.rbegin()+1;outer!=exceptions_.stack.rend();++outer){
            auto& abandoned=outer->msvc;if(!abandoned.catching || abandoned.parent!=parent || abandoned.function_info!=info.address)continue;
            const auto& catch_parent=outer->frames.at(outer->target_frame);
            if(catch_parent.unwind.establisher!=frame || catch_parent.unwind.function_entry!=memory.load(GuestAddress{dispatch.dispatcher+16},64))continue;
            // FH4 沒有 frame 上的 UnwindHelp；進 catch 前已展開至 tryLow。
            current=abandoned.compressed?abandoned.target_state:abandoned.catch_state;abandoned.catching=false;
            const auto old_object=memory.load(GuestAddress{outer->record+40},64),new_object=cpp?memory.load(GuestAddress{dispatch.record+40},64):0;
            if(abandoned.destructor && old_object!=new_object)callbacks.push_back({abandoned.destructor,{old_object}});
        }
        while(current>target){if(current<0 || static_cast<std::size_t>(current)>=info.unwind.size())throw HostCallFault("MSVC unwind current state 不合法");const auto& item=info.unwind[static_cast<std::size_t>(current)];
            if(item.action){
                if(item.kind==UnwindEntry::Kind::Object || item.kind==UnwindEntry::Kind::ObjectPointer){auto object=add_offset(parent,item.object);if(item.kind==UnwindEntry::Kind::ObjectPointer)object=memory.load(GuestAddress{object},64);callbacks.push_back({item.action,{object,parent,0x103}});}
                else callbacks.push_back({item.action,{item.action,parent,0x103}});
            }current=item.next;}
        if(help)memory.store(GuestAddress{help},32,static_cast<std::uint32_t>(current));
        if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::MsvcUnwind);return std::nullopt;}
        arg.return_to(state,1);return std::nullopt;
    }
    if(level<-1 || level>=static_cast<std::int32_t>(info.unwind.size()))throw HostCallFault("MSVC search state 不合法");
    if(help){memory.store(GuestAddress{help},32,static_cast<std::uint32_t>(level));memory.store(GuestAddress{help+4},32,static_cast<std::uint32_t>(level));}
    const auto object=cpp?memory.load(GuestAddress{dispatch.record+40},64):0,throw_info=cpp?memory.load(GuestAddress{dispatch.record+48},64):0;
    const auto* thrown=cpp?module_image(memory.load(GuestAddress{dispatch.record+56},64)):nullptr;
    if(cpp && (!thrown || throw_info<thrown->base))throw HostCallFault("MSVC 例外物件 metadata 沒有所屬映像");
    std::uint32_t throw_flags=0;std::uint64_t destructor=0,types=0;std::uint32_t type_count=0;
    if(cpp){(void)rva(memory,*thrown,throw_info-thrown->base,16);throw_flags=dword(memory,throw_info);const auto destroy=dword(memory,throw_info+4);destructor=destroy?rva(memory,*thrown,destroy,1):0;
        if(dword(memory,throw_info+8) || (throw_flags&~7U))throw HostCallFault("尚未提供 MSVC WinRT／forward-compatible throw metadata");
        types=rva(memory,*thrown,dword(memory,throw_info+12),4);type_count=dword(memory,types);if(!type_count || type_count>4096)throw HostCallFault("MSVC catchable type table 數量不合法");
        (void)rva(memory,*thrown,types-thrown->base+4,static_cast<std::size_t>(type_count)*4);
    }
    for(const auto& block:info.tries){
        if(level<block.low || level>block.high || (in_catch && (block.low<=in_catch->high || block.high>in_catch->catch_high)))continue;
        for(const auto& caught:block.handlers){
            std::uint64_t compatible=0;
            if(caught.type && !type_name(memory,caught.type).empty()){
                if(!cpp || ((throw_flags&3U)&~caught.flags))continue;
                for(std::uint32_t i=0;i<type_count;++i){const auto type=rva(memory,*thrown,dword(memory,types+4+i*4),28);const auto flags=dword(memory,type);
                    if((flags&2U) && !(caught.flags&8U))continue;
                    if(type_name(memory,rva(memory,*thrown,dword(memory,type+4),17))==type_name(memory,caught.type)){compatible=type;break;}
                }
                if(!compatible)continue;
            }
            MsvcCatch plan;plan.active=true;plan.handler=caught.handler;plan.parent=parent;plan.unwind_help=help;plan.function_info=info.address;
            plan.compressed=info.compressed;plan.continuations=caught.continuations;plan.continuation_count=caught.continuation_count;
            plan.destructor=destructor;plan.target_state=block.low;plan.catch_state=block.high+1;
            if(compatible && caught.object_offset){
                const auto flags=dword(memory,compatible);if(flags&~7U)throw HostCallFault("尚未提供 MSVC WinRT catchable type");
                const auto destination=add_offset(parent,caught.object_offset);const auto source=adjusted_pointer(memory,object,compatible);const auto size=dword(memory,compatible+20);
                if(caught.flags&8U)memory.store(GuestAddress{destination},64,source);
                else if(flags&1U){
                    if(size>memory.limit())throw HostCallFault("MSVC catch copy 大小超過客體容量");memory.check(GuestAddress{object},size,Access::Read);memory.check(GuestAddress{destination},size,Access::Write);std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{object},bytes);memory.write(GuestAddress{destination},bytes);
                    if(size==8)memory.store(GuestAddress{destination},64,adjusted_pointer(memory,memory.load(GuestAddress{destination},64),compatible));
                }else if(const auto copy=dword(memory,compatible+24)){
                    plan.copy_constructor=rva(memory,*thrown,copy,1);plan.copy_target=destination;plan.copy_source=source;plan.copy_virtual=(flags&4U)!=0;
                }else {if(size>memory.limit())throw HostCallFault("MSVC catch copy 大小超過客體容量");memory.check(GuestAddress{source},size,Access::Read);memory.check(GuestAddress{destination},size,Access::Write);std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{source},bytes);memory.write(GuestAddress{destination},bytes);}
            }
            dispatch.msvc=plan;
            if(plan.copy_constructor){start_callbacks(state,memory,{{plan.copy_constructor,{plan.copy_target,plan.copy_source,plan.copy_virtual?1U:0U}}},Continuation::MsvcCopy);return std::nullopt;}
            begin_unwind(state,memory,dispatch.frames.size()-1,plan.handler,0);return std::nullopt;
        }
    }
    if(cpp && (info.flags&4U) && !in_catch)return 3;
    arg.return_to(state,1);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::start_msvc_catch(GuestState& state,GuestMemory& memory){
    auto& dispatch=exceptions_.stack.back();auto& plan=dispatch.msvc;
    if(plan.unwind_help){memory.store(GuestAddress{plan.unwind_help},32,static_cast<std::uint32_t>(plan.catch_state));memory.store(GuestAddress{plan.unwind_help+4},32,static_cast<std::uint32_t>(plan.catch_state));}
    plan.catching=true;plan.entered_catch=true;state=dispatch.original;
    (void)msvc_exception_cells(state,memory,false,true);
    start_callbacks(state,memory,{{plan.handler,{plan.handler,plan.parent,0x100}}},Continuation::MsvcCatch);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_msvc(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value){
    if(frame.continuation==Continuation::MsvcUnwind){complete_call(state,memory,1);return std::nullopt;}
    if(exceptions_.stack.empty())throw HostCallFault("MSVC callback 缺少例外狀態");
    auto& dispatch=exceptions_.stack.back();auto& plan=dispatch.msvc;
    if(frame.continuation==Continuation::MsvcCopy){begin_unwind(state,memory,dispatch.frames.size()-1,plan.handler,0);return std::nullopt;}
    if(frame.continuation==Continuation::MsvcCatch){
        if(plan.compressed && value<=2){if(value>=plan.continuation_count)throw HostCallFault("FH4 catch continuation index 無效");value=plan.continuations[static_cast<std::size_t>(value)];}
        memory.check(GuestAddress{value},1,Access::Execute);plan.continuation=value;plan.catching=false;
        (void)msvc_exception_cells(state,memory,false,true);
        const auto object=memory.load(GuestAddress{dispatch.record+40},64);
        const auto retained=std::find_if(exceptions_.stack.begin(),exceptions_.stack.end()-1,[&](const ExceptionDispatch& outer){return outer.msvc.catching && memory.load(GuestAddress{outer.record+40},64)==object;});
        if(retained!=exceptions_.stack.end()-1){if(dispatch.msvc_owned_object){retained->msvc_owned_object=dispatch.msvc_owned_object;dispatch.msvc_owned_object=0;}}
        else if(plan.destructor){start_callbacks(state,memory,{{plan.destructor,{object}}},Continuation::MsvcDestroy);return std::nullopt;}
    }
    if(plan.unwind_help){memory.store(GuestAddress{plan.unwind_help},32,0xfffffffeU);memory.store(GuestAddress{plan.unwind_help+4},32,UINT32_MAX);}
    if(dispatch.msvc_owned_object){(void)free_heap(memory,dispatch.msvc_owned_object,state);dispatch.msvc_owned_object=0;}
    state=dispatch.target_context;state.reg(Gpr::Rsp)=dispatch.frames.at(dispatch.target_frame).unwind.establisher;state.rip=GuestAddress{plan.continuation};
    const auto target=dispatch.frames.at(dispatch.target_frame);discard_callbacks(target.callback_depth,state,memory);exceptions_.stack.resize(target.exception_depth);
    (void)msvc_exception_cells(state,memory,false,true);return std::nullopt;
}
}
