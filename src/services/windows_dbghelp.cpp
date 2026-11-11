#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <utility>

namespace x64arm {
namespace {
enum class WalkStep {Start,Function,BuiltinFunction,RegisteredFunction,Module,Read};
struct WalkOperation {
    WindowsServiceDescriptor service;GuestState cursor,caller;
    std::uint64_t scratch{},function{},module{},result{},read_address{},process{};unsigned read_size{};
    WalkStep step{};bool frame64{},amd64{},advance{},registered_tried{};
    std::uint32_t error{};
};
constexpr unsigned frame_size(bool wide){return wide?264U:164U;}
constexpr unsigned virtual_offset(bool wide){return wide?124U:72U;}
constexpr unsigned function_offset(bool wide){return wide?80U:48U;}
constexpr unsigned params_offset(bool wide){return wide?88U:52U;}
bool address32(std::uint64_t value){return value<=UINT32_MAX || (value>>32)==UINT32_MAX;}
}
struct StackWalkState {std::map<std::uint64_t,WalkOperation> operations;std::uint64_t next{1},resuming{};};
void WindowsServices::discard_stack_walk(std::uint64_t token,GuestMemory& memory,const GuestState& state){
    if(!stack_walk_)return;const auto found=stack_walk_->operations.find(token);if(found==stack_walk_->operations.end())return;
    if(found->second.step==WalkStep::RegisteredFunction)discard_symbol_callback(found->second.process);
    (void)free_heap(memory,found->second.scratch,state);stack_walk_->operations.erase(found);
}
std::optional<std::uint32_t> WindowsServices::resume_stack_walk(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    if(!stack_walk_ || !stack_walk_->operations.contains(token))throw HostCallFault("堆疊走訪回呼缺少狀態");
    auto& operation=stack_walk_->operations.at(token);operation.result=result;const auto descriptor=operation.service;
    stack_walk_->resuming=token;return invoke_stack_walk(state,memory,descriptor);
}
std::optional<std::uint32_t> WindowsServices::invoke_stack_walk(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);if(!stack_walk_)stack_walk_=std::make_shared<StackWalkState>();auto& bridge=*stack_walk_;
    auto token=std::exchange(bridge.resuming,0);
    const auto finish=[&](bool success)->std::optional<std::uint32_t>{
        if(token){last_error_=bridge.operations.at(token).error;discard_stack_walk(token,memory,state);}arg.return_to(state,success);return std::nullopt;
    };
    const auto callback=[&](WalkStep step,unsigned index,std::vector<std::uint64_t> arguments,bool wide_result=false)->std::optional<std::uint32_t>{
        auto& operation=bridge.operations.at(token);operation.step=step;GuestCallback call;call.target=arg(index);call.arguments=std::move(arguments);call.wide_arguments=operation.frame64?2U:0U;call.wide_result=wide_result;
        start_callbacks(state,memory,{std::move(call)},Continuation::StackWalkOperation,token);return std::nullopt;
    };
    try{
        if(!token){
            const auto machine=static_cast<std::uint32_t>(arg(0));const bool amd64=machine==0x8664,wide=service.symbol=="StackWalk64";
            if((machine!=0x14c && !amd64) || (amd64 && (x86_ || !wide || !arg(4))) || !arg(3) || (!arg(5) && !current_process_handle(arg(1))) || bridge.operations.size()>=128)return finish(false);
            memory.check(GuestAddress{arg(3)},frame_size(wide),Access::Read);memory.check(GuestAddress{arg(3)},frame_size(wide),Access::Write);
            const unsigned stride=wide?16U:12U,mode=wide?12U:8U,bits=wide?64U:32U;
            for(unsigned index:{0U,2U,3U})if(memory.load(GuestAddress{arg(3)+index*stride+mode},32)!=3)return finish(false); // 僅支援 Windows 平面位址。
            WalkOperation operation;operation.service=service;operation.frame64=wide;operation.amd64=amd64;operation.error=last_error_;operation.process=arg(1);
            operation.cursor=arg(4)?read_context(memory,arg(4),state):state;
            if(arg(4))memory.check(GuestAddress{arg(4)},amd64?1232:716,Access::Write);
            operation.cursor.rip=GuestAddress{memory.load(GuestAddress{arg(3)},static_cast<std::uint8_t>(bits))};
            operation.cursor.reg(Gpr::Rbp)=memory.load(GuestAddress{arg(3)+2*stride},static_cast<std::uint8_t>(bits));operation.cursor.reg(Gpr::Rsp)=memory.load(GuestAddress{arg(3)+3*stride},static_cast<std::uint8_t>(bits));
            if(!operation.cursor.rip.value || !operation.cursor.reg(Gpr::Rsp) || (!amd64 && (!address32(operation.cursor.rip.value) || !address32(operation.cursor.reg(Gpr::Rsp)) || !address32(operation.cursor.reg(Gpr::Rbp)))))return finish(false);
            operation.advance=memory.load(GuestAddress{arg(3)+virtual_offset(wide)},32)!=0;
            operation.scratch=allocate_heap(memory,64);if(!operation.scratch)return finish(false);
            token=bridge.next++;try{bridge.operations.emplace(token,std::move(operation));}catch(...){(void)free_heap(memory,operation.scratch,state);token=0;throw;}
        }else{
            auto& operation=bridge.operations.at(token);
            if(operation.step==WalkStep::BuiltinFunction){operation.function=operation.result;operation.module=memory.load(GuestAddress{operation.scratch+56},64);operation.step=WalkStep::Module;}
            else if(operation.step==WalkStep::RegisteredFunction){discard_symbol_callback(operation.process);operation.step=WalkStep::Function;operation.function=operation.result;}
            else if(operation.step==WalkStep::Function)operation.function=operation.result;
            else if(operation.step==WalkStep::Module)operation.module=operation.result;
            else {
                const auto count=memory.load(GuestAddress{operation.scratch+48},32);const auto required=operation.amd64?8U:8U;
                if(!static_cast<std::uint32_t>(operation.result) || count<required || count>operation.read_size)return finish(false);
                operation.caller=operation.cursor;
                if(operation.amd64){operation.caller.rip=GuestAddress{memory.load(GuestAddress{operation.scratch},64)};operation.caller.reg(Gpr::Rsp)+=8;}
                else {operation.caller.reg(Gpr::Rbp)=memory.load(GuestAddress{operation.scratch},32);operation.caller.rip=GuestAddress{memory.load(GuestAddress{operation.scratch+4},32)};operation.caller.reg(Gpr::Rsp)=static_cast<std::uint32_t>(operation.cursor.reg(Gpr::Rbp)+8);}
            }
        }
        auto& operation=bridge.operations.at(token);
        bool have_function=operation.step!=WalkStep::Start,have_module=operation.step==WalkStep::Module || operation.step==WalkStep::Read,have_caller=operation.step==WalkStep::Read;
        for(;;){
            if(!have_caller){
                if(!have_function){
                    operation.function=0;operation.module=0;
                    if(arg(6))return callback(WalkStep::Function,6,{arg(1),operation.cursor.rip.value});
                    if(operation.amd64 && function_table_callback(operation.cursor.rip.value)){
                        operation.step=WalkStep::BuiltinFunction;memory.store(GuestAddress{operation.scratch+56},64,0);
                        start_callbacks(state,memory,{{resolve("ntdll.dll","RtlLookupFunctionEntry").value,{operation.cursor.rip.value,operation.scratch+56,0}}},Continuation::StackWalkOperation,token);return {};
                    }
                    if(operation.amd64)if(const auto* image=unwind_image(memory,operation.cursor.rip.value))if(const auto* entry=lookup_function(*image,operation.cursor.rip.value))operation.function=image->function_table+static_cast<std::uint64_t>(entry-image->functions.data())*12;
                    have_function=true;
                }
                if(!operation.function && !operation.registered_tried){
                    operation.registered_tried=true;const auto session=runtime_.symbol_sessions.find(operation.process);
                    if(session!=runtime_.symbol_sessions.end() && session->second.function_callback && !session->second.function_callback_active){
                        GuestCallback call;call.target=session->second.function_callback;call.arguments={operation.process,operation.cursor.rip.value,session->second.function_context};call.wide_arguments=6;
                        operation.step=WalkStep::RegisteredFunction;session->second.function_callback_active=true;
                        start_callbacks(state,memory,{std::move(call)},Continuation::StackWalkOperation,token);return std::nullopt;
                    }
                }
                const auto symbol_image=[&]()->const RuntimeImage*{
                    const auto session=runtime_.symbol_sessions.find(operation.process);if(session==runtime_.symbol_sessions.end())return nullptr;
                    const auto found=session->second.images.upper_bound(operation.cursor.rip.value);if(found==session->second.images.begin())return nullptr;
                    const auto& image=std::prev(found)->second.image;return operation.cursor.rip.value-image.base<image.size?&image:nullptr;
                };
                if(!have_module){if(arg(7))return callback(WalkStep::Module,7,{arg(1),operation.cursor.rip.value},operation.frame64);if(const auto* image=unwind_image(memory,operation.cursor.rip.value))operation.module=image->base;else if(const auto* symbol=symbol_image())operation.module=symbol->base;have_module=true;}
                operation.caller=operation.cursor;
                if(operation.amd64 && operation.function){
                    // 共用 CPU 執行時的 AMD64 unwind 語義，包含 prolog／epilog／chained metadata。
                    // 非本程序的回呼式非 leaf unwind 尚未支援，不能改讀本程序而給出錯誤框架。
                    if(arg(5) || !current_process_handle(arg(1)))return finish(false);
                    const auto* image=unwind_image(memory,operation.cursor.rip.value,operation.function);if(!image)image=symbol_image();if(!image || image->base!=operation.module)return finish(false);
                    auto selected=*image;selected.functions={{static_cast<std::uint32_t>(memory.load(GuestAddress{operation.function},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{operation.function+4},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{operation.function+8},32))}};selected.function_table=operation.function;
                    const auto& entry=selected.functions.front();if(entry.begin>=entry.end || entry.end>selected.size || operation.cursor.rip.value-selected.base<entry.begin || operation.cursor.rip.value-selected.base>=entry.end)return finish(false);
                    operation.caller=virtual_unwind(memory,selected,operation.cursor.rip.value,operation.cursor,0).caller;
                }else{
                    operation.read_address=operation.amd64?operation.cursor.reg(Gpr::Rsp):operation.cursor.reg(Gpr::Rbp);operation.read_size=operation.amd64?8U:24U;
                    if(!operation.amd64 && (!operation.read_address || (operation.read_address&3) || static_cast<std::uint32_t>(operation.read_address)<static_cast<std::uint32_t>(operation.cursor.reg(Gpr::Rsp))))return finish(false);
                    if(arg(5)){memory.write(GuestAddress{operation.scratch},std::array<std::uint8_t,64>{});return callback(WalkStep::Read,5,{arg(1),operation.read_address,operation.scratch,operation.read_size,operation.scratch+48});}
                    const auto address=operation.amd64?operation.read_address:static_cast<std::uint32_t>(operation.read_address);
                    if(operation.amd64){operation.caller.rip=GuestAddress{memory.load(GuestAddress{address},64)};operation.caller.reg(Gpr::Rsp)+=8;}
                    else {operation.caller.reg(Gpr::Rbp)=memory.load(GuestAddress{address},32);operation.caller.rip=GuestAddress{memory.load(GuestAddress{address+4},32)};operation.caller.reg(Gpr::Rsp)=static_cast<std::uint32_t>(address+8);}
                }
                have_caller=true;
            }
            if(operation.caller.rip.value && operation.caller.reg(Gpr::Rsp)<=operation.cursor.reg(Gpr::Rsp))return finish(false);
            if(!operation.amd64 && operation.caller.reg(Gpr::Rbp) && static_cast<std::uint32_t>(operation.caller.reg(Gpr::Rbp))<=static_cast<std::uint32_t>(operation.cursor.reg(Gpr::Rbp)))return finish(false);
            if(operation.advance){
                if(!operation.caller.rip.value)return finish(false);operation.cursor=operation.caller;operation.advance=false;operation.read_size=0;operation.result=0;operation.registered_tried=false;have_function=false;have_module=false;have_caller=false;continue;
            }
            const auto at=arg(3);const unsigned stride=operation.frame64?16U:12U,mode=operation.frame64?12U:8U;const auto bits=static_cast<std::uint8_t>(operation.frame64?64:32);
            for(const auto [index,value]:{std::pair{0U,operation.cursor.rip.value},std::pair{1U,operation.caller.rip.value},std::pair{2U,operation.cursor.reg(Gpr::Rbp)},std::pair{3U,operation.cursor.reg(Gpr::Rsp)}}){memory.store(GuestAddress{at+index*stride},bits,value);memory.store(GuestAddress{at+index*stride+mode},32,3);}
            memory.store(GuestAddress{at+function_offset(operation.frame64)},x86_?32:64,operation.function);
            memory.store(GuestAddress{at+virtual_offset(operation.frame64)-4},32,0);memory.store(GuestAddress{at+virtual_offset(operation.frame64)},32,1);
            for(unsigned index=0;index<4;++index){std::uint64_t value=0;if(!operation.amd64)try{value=arg(5)?memory.load(GuestAddress{operation.scratch+8+index*4},32):memory.load(GuestAddress{static_cast<std::uint32_t>(operation.cursor.reg(Gpr::Rbp))+8+index*4},32);}catch(const MemoryFault&){}memory.store(GuestAddress{at+params_offset(operation.frame64)+index*(operation.frame64?8:4)},bits,value);}
            if(arg(4))write_context(memory,arg(4),operation.cursor);return finish(true);
        }
    }catch(const MemoryFault&){return finish(false);}catch(const HostCallFault&){return finish(false);}catch(const std::bad_alloc&){return finish(false);}
}
std::span<const WindowsServiceDescriptor> dbghelp_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"dbghelp.dll","UnDecorateSymbolName",4,0,0,12},{"dbghelp.dll","UnDecorateSymbolNameW",4,0,0,12},
        {"dbghelp.dll","StackWalk64",9,0,0,1},{"dbghelp.dll","StackWalk",9,0,0,1},
        {"dbghelp.dll","SymSetOptions",1},{"dbghelp.dll","SymGetOptions",0},{"dbghelp.dll","ImagehlpApiVersion",0},{"dbghelp.dll","ImagehlpApiVersionEx",1},
        {"dbghelp.dll","ImageNtHeader",1},{"dbghelp.dll","ImageDirectoryEntryToData",4},{"dbghelp.dll","ImageDirectoryEntryToDataEx",5},
        {"dbghelp.dll","ImageRvaToSection",3},{"dbghelp.dll","ImageRvaToVa",4},
        {"dbghelp.dll","SymInitialize",3},{"dbghelp.dll","SymInitializeW",3},
        {"dbghelp.dll","SymCleanup",1},{"dbghelp.dll","SymRefreshModuleList",1},
        {"dbghelp.dll","SymSetSearchPath",2},{"dbghelp.dll","SymSetSearchPathW",2},
        {"dbghelp.dll","SymGetSearchPath",3},{"dbghelp.dll","SymGetSearchPathW",3},
        {"dbghelp.dll","SymGetModuleBase64",2,2},{"dbghelp.dll","SymFunctionTableAccess64",2,2},
        {"dbghelp.dll","SymFromAddr",4,2},{"dbghelp.dll","SymFromAddrW",4,2},
        {"dbghelp.dll","SymGetLineFromAddr64",4,2},{"dbghelp.dll","SymGetLineFromAddrW64",4,2}
    };
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_dbghelp(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    if(name=="MiniDumpWriteDump" || name=="MiniDumpReadDumpStream")return invoke_minidump(state,memory,service);
    if(name=="UnDecorateSymbolName" || name=="UnDecorateSymbolNameW")return invoke_demangle(state,memory,service);
    if(name=="StackWalk64" || name=="StackWalk")return invoke_stack_walk(state,memory,service);
    const bool wide=name.ends_with('W') || name.ends_with("W64");
    std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    const auto text=[&](std::uint64_t address){return wide?read_utf16(memory,GuestAddress{address},32768):utf8_to_utf16(read_ansi(memory,address,32768));};
    const auto default_path=[&]{
        auto path=runtime_.current_directory;
        for(const auto* key:{u"_nt_symbol_path",u"_nt_alternate_symbol_path"}){
            const auto found=runtime_.environment.find(key);
            if(found!=runtime_.environment.end() && !found->second.second.empty()){path+=u';';path+=found->second.second;}
        }
        return path;
    };
    const auto snapshot=[&](SymbolSession& session){refresh_symbol_modules(session,memory);};
    if(name.starts_with("Image") && !name.starts_with("Imagehlp")){
        // PE 標頭工具（ImagehlpApiVersion* 是版本查詢，不屬此類）：呼叫端傳入已映射的映像或檔案 view；非 PE 回 NULL 與 ERROR_BAD_EXE_FORMAT。
        const auto nt_header=[&](std::uint64_t base)->std::uint64_t{
            if(!base || memory.load(GuestAddress{base},16)!=0x5a4dU)return 0;
            const auto offset=memory.load(GuestAddress{base+0x3c},32);
            if(offset>=UINT64_C(0x10000000) || memory.load(GuestAddress{base+offset},32)!=0x4550U)return 0;
            return base+offset;
        };
        const auto section_of=[&](std::uint64_t nt,std::uint64_t rva)->std::uint64_t{
            const auto count=memory.load(GuestAddress{nt+6},16),optional=memory.load(GuestAddress{nt+20},16);
            const auto first=nt+24+optional;
            for(std::uint64_t i=0;i<count;++i){
                const auto section=first+i*40;
                const auto start=memory.load(GuestAddress{section+12},32),virtual_size=memory.load(GuestAddress{section+8},32),raw=memory.load(GuestAddress{section+16},32);
                if(rva>=start && rva<start+std::max(virtual_size,raw))return section;
            }
            return 0;
        };
        if(name=="ImageNtHeader"){result=nt_header(arg(0));if(!result)last_error_=193;}
        else if(name=="ImageRvaToSection"){const auto nt=arg(0)?arg(0):nt_header(arg(1));result=nt?section_of(nt,static_cast<std::uint32_t>(arg(2))):0;if(!result)last_error_=193;}
        else if(name=="ImageRvaToVa"){
            const auto nt=arg(0)?arg(0):nt_header(arg(1));const auto rva=static_cast<std::uint32_t>(arg(2));
            const auto section=nt?section_of(nt,rva):0;
            if(!section)last_error_=193;
            else{
                const auto start=memory.load(GuestAddress{section+12},32),raw=memory.load(GuestAddress{section+20},32);
                result=arg(1)+raw+(rva-start);
                if(arg(3))memory.store(GuestAddress{arg(3)},x86_?32:64,section);
            }
        }else{
            const bool extended=name=="ImageDirectoryEntryToDataEx";
            const auto base=arg(0);const bool mapped_as_image=static_cast<std::uint8_t>(arg(1))!=0;const auto entry=static_cast<std::uint16_t>(arg(2));const auto size_out=arg(3);
            if(extended && arg(4))memory.store(GuestAddress{arg(4)},x86_?32:64,0);
            const auto nt=nt_header(base);
            if(!nt)last_error_=193;
            else{
                const auto magic=memory.load(GuestAddress{nt+24},16);const std::uint64_t directories=nt+24+(magic==0x20b?112U:96U);
                const auto limit=memory.load(GuestAddress{nt+24+(magic==0x20b?108U:92U)},32);
                if(entry<limit){
                    const auto rva=memory.load(GuestAddress{directories+entry*8ULL},32),size=memory.load(GuestAddress{directories+entry*8ULL+4},32);
                    if(rva && size){
                        if(size_out)memory.store(GuestAddress{size_out},32,size);
                        if(mapped_as_image)result=base+rva;
                        else if(const auto section=section_of(nt,rva)){
                            result=base+memory.load(GuestAddress{section+20},32)+(rva-memory.load(GuestAddress{section+12},32));
                            if(extended && arg(4))memory.store(GuestAddress{arg(4)},x86_?32:64,section);
                        }
                    }
                }
                if(!result)last_error_=0;
            }
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(name=="ImagehlpApiVersion" || name=="ImagehlpApiVersionEx"){
        // 回傳程式庫內固定的 API_VERSION（Major、Minor、Revision、Reserved 各 16 位元）：Windows 10 dbghelp 10.0、API_VERSION_NUMBER 12；
        // Ex 版的呼叫端版本只供程式庫參考，不改變回傳內容。結構配置一次，之後同一位址。
        static std::uint64_t version=0;
        if(!version){version=allocate_heap(memory,8);if(version){memory.store(GuestAddress{version},16,10);memory.store(GuestAddress{version+2},16,0);memory.store(GuestAddress{version+4},16,12);memory.store(GuestAddress{version+6},16,0);}}
        if(!version)fail(8);else result=version;
    }
    else if(name=="SymSetOptions")result=runtime_.symbol_options=static_cast<std::uint32_t>(arg(0));
    else if(name=="SymGetOptions")result=runtime_.symbol_options;
    else if(name=="SymInitialize" || name=="SymInitializeW"){
        if(!arg(0) || runtime_.symbol_sessions.contains(arg(0)))fail(87);
        else if(arg(2) && !current_process_handle(arg(0)))fail(50); // 僅能檢查本虛擬程序。
        else if(runtime_.symbol_sessions.size()>=64)fail(8);
        else {
            SymbolSession session;session.search_path=arg(1)?text(arg(1)):default_path();
            if(arg(2))snapshot(session);
            runtime_.symbol_sessions.emplace(arg(0),std::move(session));result=1;
        }
    }else {
        const auto found=runtime_.symbol_sessions.find(arg(0));
        if(found==runtime_.symbol_sessions.end())fail(6);
        else if(name=="SymCleanup"){runtime_.symbol_sessions.erase(found);result=1;}
        else if(name=="SymRefreshModuleList"){
            if(!current_process_handle(arg(0)))fail(50);else {snapshot(found->second);result=1;}
        }else if(name.starts_with("SymSetSearchPath")){
            found->second.search_path=arg(1)?text(arg(1)):default_path();result=1;
        }else if(name.starts_with("SymGetSearchPath")){
            const auto& path=found->second.search_path;const auto ansi=utf16_to_utf8(path);
            const auto count=wide?path.size():ansi.size();
            if(!arg(1))fail(87);
            else if(static_cast<std::uint32_t>(arg(2))<=count)fail(122);
            else {
                if(wide)memory.write(GuestAddress{arg(1)},utf16_bytes(path));
                else {std::vector<std::uint8_t> bytes(ansi.begin(),ansi.end());bytes.push_back(0);memory.write(GuestAddress{arg(1)},bytes);}
                result=1;
            }
        }else return invoke_dbghelp_symbols(state,memory,service);
    }
    arg.return_to(state,result,name=="SymGetModuleBase64");return std::nullopt;
}
}
