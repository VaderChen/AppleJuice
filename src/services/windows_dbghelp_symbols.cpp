#include "x64arm/pe_loader.hpp"
#include "x64arm/unicode.hpp"
#include "pdb_reader.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <limits>

namespace x64arm {
namespace {
struct SymbolFailure {std::uint32_t error;};
constexpr std::size_t maximum_file=128U*1024U*1024U,maximum_symbols=65536;
std::u16string leaf(std::u16string_view path){return std::u16string(path.substr(path.find_last_of(u"\\/")+1));}
std::u16string module_name(std::u16string_view path){auto name=leaf(path);const auto dot=name.find_last_of(u'.');if(dot!=name.npos)name.resize(dot);return name;}
std::uint64_t integer(std::span<const std::uint8_t> bytes,std::size_t at,unsigned size){
    if(at>bytes.size() || size>bytes.size()-at)throw SymbolFailure{193};
    std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{bytes[at+i]}<<(8*i);return value;
}
void exports(SymbolModule& module){
    for(const auto& [name,index]:module.image.export_names){
        const auto found=module.image.export_ordinals.find(index);
        if(found==module.image.export_ordinals.end() || module.image.export_forwarders.contains(index) || !found->second || found->second>=module.image.size)continue;
        if(module.symbols.size()>=maximum_symbols)throw SymbolFailure{8};
        module.symbols.push_back({name,module.image.base+found->second,0,index,0x200,10});
    }
}
// COFF 的 section-relative 位址及 auxiliary record 由檔案解析，不映射或執行 DLL。
void coff(SymbolModule& module,std::span<const std::uint8_t> bytes){
    const auto pe=static_cast<std::size_t>(integer(bytes,60,4)),table=static_cast<std::size_t>(integer(bytes,pe+12,4));
    const auto count=integer(bytes,pe+16,4);if(!table && !count)return;
    if(!table || count>maximum_symbols || table>bytes.size() || count>(bytes.size()-table)/18)throw SymbolFailure{193};
    const auto strings=table+static_cast<std::size_t>(count)*18;const auto length=integer(bytes,strings,4);
    if(length<4 || length>bytes.size()-strings)throw SymbolFailure{193};
    const auto sections=integer(bytes,pe+6,2);const auto section_table=pe+24+static_cast<std::size_t>(integer(bytes,pe+20,2));
    for(std::uint64_t i=0;i<count;){
        const auto at=table+static_cast<std::size_t>(i)*18;const auto aux=integer(bytes,at+17,1);
        if(aux>=count-i)throw SymbolFailure{193};
        const auto section=integer(bytes,at+12,2),storage=integer(bytes,at+16,1),type=integer(bytes,at+14,2);
        if(section>0 && section<=sections && (storage==2 || storage==3)){
            const auto section_at=section_table+static_cast<std::size_t>(section-1)*40;
            const auto rva=integer(bytes,section_at+12,4),offset=integer(bytes,at+8,4);
            const auto extent=std::max(integer(bytes,section_at+8,4),integer(bytes,section_at+16,4));
            std::string name;
            if(integer(bytes,at,4)==0){
                const auto index=integer(bytes,at+4,4);if(index<4 || index>=length)throw SymbolFailure{193};
                auto cursor=strings+static_cast<std::size_t>(index);while(cursor<strings+length && bytes[cursor]){if(name.size()>=2000)throw SymbolFailure{193};name+=static_cast<char>(bytes[cursor++]);}
                if(cursor==strings+length)throw SymbolFailure{193};
            }else for(unsigned n=0;n<8 && bytes[at+n];++n)name+=static_cast<char>(bytes[at+n]);
            // .file／section 定義和越界 absolute 符號不是可查詢的映像位址。
            if(!name.empty() && name.front()!='.' && offset<extent && rva<module.image.size && offset<module.image.size-rva){
                if(module.symbols.size()>=maximum_symbols)throw SymbolFailure{8};
                const auto address=module.image.base+rva+offset;
                if(std::none_of(module.symbols.begin(),module.symbols.end(),[&](const auto& symbol){return symbol.name==name && symbol.address==address;})){
                    const bool function=(type&0x30)==0x20;
                    std::uint32_t size=0;if(function && storage==2 && aux)size=static_cast<std::uint32_t>(integer(bytes,at+18+4,4));
                    if(size>module.image.size-(rva+offset))throw SymbolFailure{193};
                    module.symbols.push_back({std::move(name),address,size,static_cast<std::uint32_t>(i),static_cast<std::uint32_t>(function?0x800:0),static_cast<std::uint32_t>(function?5:7)});
                }
            }
        }
        i+=1+aux;
    }
}
// SymRegisterCallback：invoke_dbghelp_symbols 執行期間收集符號載入事件（只在該工作階段登記了回呼時設定）。
struct SymbolEventSink {std::vector<SymbolCallbackEvent> events;bool debug{};};
thread_local SymbolEventSink* symbol_event_sink=nullptr;
struct SymbolEventScope {
    SymbolEventSink* previous;
    explicit SymbolEventScope(SymbolEventSink& sink):previous(symbol_event_sink){symbol_event_sink=&sink;}
    ~SymbolEventScope(){symbol_event_sink=previous;}
    SymbolEventScope(const SymbolEventScope&)=delete;SymbolEventScope& operator=(const SymbolEventScope&)=delete;
};
SymbolCallbackEvent module_event(const SymbolModule& module,std::uint32_t action){
    return {0,action,module.image.base,0,module.checksum,module.timestamp,module.loaded_image_name.empty()?module.image_name:module.loaded_image_name};
}
// CBA_DEFERRED_SYMBOL_LOAD_START，SYMOPT_DEBUG 時的 CBA_DEBUG_INFO，再送 COMPLETE 或 FAILURE。
void report_load(const SymbolModule& module,std::uint32_t error){
    if(!symbol_event_sink)return;auto& events=symbol_event_sink->events;events.push_back(module_event(module,1));
    if(symbol_event_sink->debug){
        auto text=u"DBGHELP: "+module.name;
        if(error)text+=u" - symbol load failed, error "+utf8_to_utf16(std::to_string(error))+u"\n";
        else text+=module.symbol_type==3?u" - private symbols & lines\n":module.symbol_type==1?u" - coff symbols\n":module.symbol_type==4?u" - export symbols\n":u" - no symbols loaded\n";
        events.push_back({0,0x10000000U,module.image.base,0,0,0,std::move(text)});
    }
    events.push_back(module_event(module,error?3U:2U));
}
void materialize(SymbolModule& module){
    if(!module.pending)return;
    // 先在副本完成，壞的 COFF record 不留下半套符號。
    try{
        auto candidate=module;candidate.symbols.clear();exports(candidate);const auto before=candidate.symbols.size();coff(candidate,*candidate.pending);
        candidate.symbol_type=candidate.symbols.size()>before?1U:!candidate.symbols.empty()?4U:0U;
        // 相符的 PDB 取代匯出與 COFF 符號：程序符號（SymTagFunction，帶大小）與公開符號（SymTagPublicSymbol）。
        if(candidate.pdb){
            candidate.symbols.clear();
            for(const auto& item:candidate.pdb->symbols){
                if(item.rva>=candidate.image.size)continue;const auto address=candidate.image.base+item.rva;
                if(candidate.symbols.size()>=maximum_symbols)throw SymbolFailure{8};
                candidate.symbols.push_back({item.name,address,item.size,static_cast<std::uint32_t>(candidate.symbols.size()+1),static_cast<std::uint32_t>(item.function && !item.pub?0x800:0),static_cast<std::uint32_t>(item.pub?10:5)});
            }
            candidate.symbol_type=3; // SymPdb
        }
        candidate.pending.reset();module=std::move(candidate);
    }catch(const SymbolFailure& error){
        // 載入失敗只回報一次；模組保留延遲狀態，之後的查詢仍回同一錯誤。
        if(!module.load_reported){module.load_reported=true;report_load(module,error.error);}
        throw;
    }
    report_load(module,0);
}
// SymMatchString：* 任意長度、? 一個字元、[a-z] 集合（[! 為排除）、\ 跳脫；可選擇不分大小寫。
bool symbol_match(std::u16string_view text,std::u16string_view pattern,bool exact_case){
    const auto fold=[&](char16_t c){return exact_case?c:unicode_case_unit(c,true);};
    if(pattern.empty())return text.empty();
    if(pattern[0]==u'*'){for(std::size_t i=0;i<=text.size();++i)if(symbol_match(text.substr(i),pattern.substr(1),exact_case))return true;return false;}
    if(text.empty())return false;
    if(pattern[0]==u'?')return symbol_match(text.substr(1),pattern.substr(1),exact_case);
    if(pattern[0]==u'['){
        const auto end=pattern.find(u']',1);if(end==std::u16string_view::npos)return fold(text[0])==fold(u'[') && symbol_match(text.substr(1),pattern.substr(1),exact_case);
        auto set=pattern.substr(1,end-1);const bool negate=!set.empty() && set[0]==u'!';if(negate)set.remove_prefix(1);bool hit=false;
        for(std::size_t i=0;i<set.size();++i){if(i+2<set.size() && set[i+1]==u'-'){if(fold(text[0])>=fold(set[i]) && fold(text[0])<=fold(set[i+2]))hit=true;i+=2;}else if(fold(text[0])==fold(set[i]))hit=true;}
        return hit!=negate && symbol_match(text.substr(1),pattern.substr(end+1),exact_case);
    }
    std::size_t width=1;char16_t literal=pattern[0];if(literal==u'\\' && pattern.size()>1){literal=pattern[1];width=2;}
    return fold(text[0])==fold(literal) && symbol_match(text.substr(1),pattern.substr(width),exact_case);
}
SymbolModule* containing(SymbolSession& session,std::uint64_t address){
    const auto found=session.images.upper_bound(address);if(found==session.images.begin())return nullptr;
    auto& module=std::prev(found)->second;return address-module.image.base<module.image.size?&module:nullptr;
}
SymbolModule live_module(const RuntimeImage& image,const GuestMemory& memory){
    SymbolModule module;module.image=image;module.image_name=module.loaded_image_name=image.path;module.name=module_name(image.path);module.invaded=true;
    exports(module);module.symbol_type=module.symbols.empty()?0:4;
    try{
        if(memory.load(GuestAddress{image.base},16)==0x5a4d){const auto pe=image.base+memory.load(GuestAddress{image.base+60},32);
            if(memory.load(GuestAddress{pe},32)==0x4550){module.machine=static_cast<std::uint32_t>(memory.load(GuestAddress{pe+4},16));module.timestamp=static_cast<std::uint32_t>(memory.load(GuestAddress{pe+8},32));module.checksum=static_cast<std::uint32_t>(memory.load(GuestAddress{pe+24+64},32));}}
    }catch(const MemoryFault&){} // 內建合成模組未必有磁碟 PE header。
    return module;
}
}
std::span<const WindowsServiceDescriptor> dbghelp_symbol_service_descriptors(){
    // 附加到全域 descriptor 尾端，保留舊 stub 位址。
    static constexpr WindowsServiceDescriptor table[]{
        {"dbghelp.dll","SymLoadModuleEx",8,16},{"dbghelp.dll","SymLoadModuleExW",8,16},
        {"dbghelp.dll","SymRegisterFunctionEntryCallback64",3,4},
        {"dbghelp.dll","SymUnloadModule64",2,2},
        // 行號與原始檔（windows_dbghelp_lines.inc）。
        {"dbghelp.dll","SymGetLineFromAddr",4},{"dbghelp.dll","SymGetLineFromName",6,0,0,0x8},{"dbghelp.dll","SymGetLineFromName64",6,0,0,0x8},{"dbghelp.dll","SymGetLineFromNameW64",6,0,0,0x8},
        {"dbghelp.dll","SymGetLineNext",2},{"dbghelp.dll","SymGetLineNext64",2},{"dbghelp.dll","SymGetLineNextW64",2},
        {"dbghelp.dll","SymGetLinePrev",2},{"dbghelp.dll","SymGetLinePrev64",2},{"dbghelp.dll","SymGetLinePrevW64",2},
        {"dbghelp.dll","SymGetLineFromInlineContext",6,0xa,0,0x4},{"dbghelp.dll","SymGetLineFromInlineContextW",6,0xa,0,0x4},
        {"dbghelp.dll","SymEnumLines",6,2},{"dbghelp.dll","SymEnumLinesW",6,2},{"dbghelp.dll","SymEnumSourceLines",8,2,0,0x30},{"dbghelp.dll","SymEnumSourceLinesW",8,2,0,0x30},
        {"dbghelp.dll","SymEnumSourceFiles",5,2},{"dbghelp.dll","SymEnumSourceFilesW",5,2},{"dbghelp.dll","SymQueryInlineTrace",7,0x1a,0,0x4},
        {"dbghelp.dll","SymGetSourceFileToken",5,2},{"dbghelp.dll","SymGetSourceFileTokenW",5,2},
        {"dbghelp.dll","ExtensionApiVersion",0},{"dbghelp.dll","WinDbgExtensionDllInit",3,0,0,0x6},
        {"dbghelp.dll","SymGetModuleInfo64",3,2},{"dbghelp.dll","SymGetModuleInfoW64",3,2},
        // Size／Flags 是 DWORD：x64 堆疊 slot 的高 32 位元可能殘留舊值，必須只取低位。
        {"dbghelp.dll","SymAddSymbol",6,10,0,0x30},{"dbghelp.dll","SymAddSymbolW",6,10,0,0x30},
        {"dbghelp.dll","SymDeleteSymbol",5,10,0,0x10},{"dbghelp.dll","SymDeleteSymbolW",5,10,0,0x10},
        {"dbghelp.dll","SymLoadModule",6,0,0,0x30},{"dbghelp.dll","SymLoadModule64",6,16,0,0x20},
        {"dbghelp.dll","SymGetSymFromAddr",4},{"dbghelp.dll","SymGetSymFromAddr64",4,2},
        {"dbghelp.dll","EnumerateLoadedModules",3},{"dbghelp.dll","EnumerateLoadedModules64",3},{"dbghelp.dll","EnumerateLoadedModulesW64",3},
        {"dbghelp.dll","EnumerateLoadedModulesEx",3},{"dbghelp.dll","EnumerateLoadedModulesExW",3},
        // 名稱查詢、前後符號、32 位元舊版介面、比對字串、延伸選項、範圍設定與行內框架查詢。
        {"dbghelp.dll","SymFromName",3},{"dbghelp.dll","SymFromNameW",3},{"dbghelp.dll","SymGetSymFromName",3},{"dbghelp.dll","SymGetSymFromName64",3},
        {"dbghelp.dll","SymGetSymNext",2},{"dbghelp.dll","SymGetSymNext64",2},{"dbghelp.dll","SymGetSymPrev",2},{"dbghelp.dll","SymGetSymPrev64",2},
        {"dbghelp.dll","SymGetModuleBase",2,0,0,0x2},{"dbghelp.dll","SymGetModuleInfo",3,0,0,0x2},{"dbghelp.dll","SymGetModuleInfoW",3,0,0,0x2},
        {"dbghelp.dll","SymFunctionTableAccess",2,0,0,0x2},{"dbghelp.dll","SymUnloadModule",2,0,0,0x2},
        {"dbghelp.dll","SymUnDName",3,0,0,0x4},{"dbghelp.dll","SymUnDName64",3,0,0,0x4},
        {"dbghelp.dll","SymMatchString",3,0,0,0x4},{"dbghelp.dll","SymMatchStringA",3,0,0,0x4},{"dbghelp.dll","SymMatchStringW",3,0,0,0x4},
        {"dbghelp.dll","SymGetExtendedOption",1,0,0,0x1},{"dbghelp.dll","SymSetExtendedOption",2,0,0,0x3},{"dbghelp.dll","SymSetParentWindow",1},
        {"dbghelp.dll","SymSetContext",3},{"dbghelp.dll","SymSetScopeFromAddr",2,2},{"dbghelp.dll","SymSetScopeFromIndex",3,2,0,0x4},{"dbghelp.dll","SymSetScopeFromInlineContext",3,2,0,0x4},
        {"dbghelp.dll","SymAddrIncludeInlineTrace",2,2},{"dbghelp.dll","SymFromInlineContext",5,2,0,0x4},{"dbghelp.dll","SymFromInlineContextW",5,2,0,0x4},
        {"dbghelp.dll","SymEnumSymbols",5,2},{"dbghelp.dll","SymEnumSymbolsW",5,2},{"dbghelp.dll","SymSearch",9,0x22,0,0x10c},{"dbghelp.dll","SymSearchW",9,0x22,0,0x10c},
        {"dbghelp.dll","SymEnumerateSymbols",4,0,0,0x2},{"dbghelp.dll","SymEnumerateSymbols64",4,2},
        {"dbghelp.dll","SymEnumerateModules",3},{"dbghelp.dll","SymEnumerateModules64",3},{"dbghelp.dll","SymEnumerateModulesW64",3},
        {"dbghelp.dll","MakeSureDirectoryPathExists",1},{"dbghelp.dll","GetTimestampForLoadedLibrary",1},{"dbghelp.dll","SearchTreeForFile",3},{"dbghelp.dll","SearchTreeForFileW",3},
        // 檔案工具（windows_dbghelp_files.cpp）
        {"dbghelp.dll","SymMatchFileName",4},{"dbghelp.dll","SymMatchFileNameW",4},{"dbghelp.dll","SymSetHomeDirectory",2},{"dbghelp.dll","SymSetHomeDirectoryW",2},
        {"dbghelp.dll","SymGetHomeDirectory",3,0,0,0x1},{"dbghelp.dll","SymGetHomeDirectoryW",3,0,0,0x1},{"dbghelp.dll","EnumDirTree",6},{"dbghelp.dll","EnumDirTreeW",6},
        {"dbghelp.dll","SymSrvGetFileIndexInfo",3,0,0,0x4},{"dbghelp.dll","SymSrvGetFileIndexInfoW",3,0,0,0x4},
        {"dbghelp.dll","SymSrvGetFileIndexes",5,0,0,0x10},{"dbghelp.dll","SymSrvGetFileIndexesW",5,0,0,0x10},
        {"dbghelp.dll","SymSrvGetFileIndexString",6,0,0,0x20},{"dbghelp.dll","SymSrvGetFileIndexStringW",6,0,0,0x20},
        {"dbghelp.dll","SymFindFileInPath",10,0,0,0x70},{"dbghelp.dll","SymFindFileInPathW",10,0,0,0x70},
        // 符號索引與型別查詢（匯出表／COFF 符號模型）
        {"dbghelp.dll","SymFromIndex",4,2,0,0x4},{"dbghelp.dll","SymFromIndexW",4,2,0,0x4},{"dbghelp.dll","SymGetTypeInfo",5,2,0,0xc},
        {"dbghelp.dll","SymEnumTypes",4,2},{"dbghelp.dll","SymEnumTypesW",4,2},{"dbghelp.dll","SymEnumTypesByName",5,2},{"dbghelp.dll","SymEnumTypesByNameW",5,2},
        {"dbghelp.dll","SymGetTypeFromName",4,2},{"dbghelp.dll","SymGetTypeFromNameW",4,2},
        // 事件回呼（UserContext 在 64 版是 ULONG64）與 32 位元位址的函式入口回呼。
        {"dbghelp.dll","SymRegisterCallback",3},{"dbghelp.dll","SymRegisterCallback64",3,4},{"dbghelp.dll","SymRegisterCallbackW64",3,4},
        {"dbghelp.dll","SymRegisterFunctionEntryCallback",3}
    };return table;
}
void WindowsServices::refresh_symbol_modules(SymbolSession& session,const GuestMemory& memory){
    std::map<std::uint64_t,const RuntimeImage*> images;if(runtime_.image.base)images.emplace(runtime_.image.base,&runtime_.image);
    for(const auto& [base,module]:runtime_.loaded_modules)images.emplace(base,&module.image);
    for(const auto& [base,image]:runtime_.builtin_images)if(!image.private_code)images.emplace(base,&image);
    for(auto at=session.images.begin();at!=session.images.end();)if(at->second.invaded && !images.contains(at->first)){session.modules.erase(at->first);at=session.images.erase(at);}else ++at;
    for(const auto& [base,image]:images){if(session.images.contains(base))continue;
        const auto end=base+image->size;
        if(std::any_of(session.images.begin(),session.images.end(),[&](const auto& item){return base<item.first+item.second.image.size && item.first<end;}))continue;
        session.images.emplace(base,live_module(*image,memory));session.modules.insert(base);
    }
}
void WindowsServices::discard_symbol_callback(std::uint64_t process){const auto found=runtime_.symbol_sessions.find(process);if(found!=runtime_.symbol_sessions.end())found->second.function_callback_active=false;}
std::optional<std::uint32_t> WindowsServices::resume_symbol_callback(GuestState& state,GuestMemory& memory,std::uint64_t process,std::uint64_t result){
    discard_symbol_callback(process);if(!result)last_error_=487;complete_call(state,memory,result);return std::nullopt;
}
// SearchTreeForFile 與 FindExecutableImage／FindDebugInfoFile 共用：在 root 下深度優先尋找與 file 檔名部分同名的檔案（不分大小寫，最多 32 層、
// 十萬個目錄），先比對同層檔案再進入子目錄。
std::optional<std::u16string> WindowsServices::search_tree_for_file(std::u16string root,std::u16string_view file){
    auto found=find_tree_files(std::move(root),file,1);if(found.empty())return std::nullopt;return std::move(found.front());
}
std::vector<std::u16string> WindowsServices::find_tree_files(std::u16string root,std::u16string_view file,std::size_t limit){
    const auto wanted=leaf(file);while(root.size()>3 && (root.back()==u'\\' || root.back()==u'/'))root.pop_back();
    const auto fold=[](std::u16string value){for(auto& c:value)c=unicode_case_unit(c,true);return value;};const auto target=fold(wanted);std::vector<std::u16string> found;std::size_t visited=0;
    const std::function<void(const std::u16string&,unsigned)> walk=[&](const std::u16string& directory,unsigned depth){
        if(found.size()>=limit || depth>32 || ++visited>100000)return;const auto listing=filesystem_.list(utf16_to_utf8(directory));if(listing.error)return;
        const auto separator=directory.ends_with(u"\\") || directory.ends_with(u"/")?u"":u"\\";
        for(const auto& entry:listing.entries)if(!(entry.metadata.attributes&16) && fold(utf8_to_utf16_lossy(entry.name))==target){found.push_back(directory+separator+utf8_to_utf16_lossy(entry.name));if(found.size()>=limit)return;}
        for(const auto& entry:listing.entries)if((entry.metadata.attributes&16) && entry.name!="." && entry.name!=".."){walk(directory+separator+utf8_to_utf16_lossy(entry.name),depth+1);if(found.size()>=limit)return;}
    };
    if(!wanted.empty() && !root.empty() && limit)walk(root,0);
    return found;
}
// SymRegisterCallback 的事件：先執行原本的操作並收集事件，再依序呼叫回呼。操作已同步完成時，回呼結束後還原原本的回傳值與錯誤碼；
// 操作已開始自己的回呼（列舉等唯讀查詢）時撤銷那些回呼，先送事件再從 API 入口重新執行（模組已載入，不會再產生事件）。
std::optional<std::uint32_t> WindowsServices::invoke_dbghelp_symbols(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto process=arg(0);const auto name=service.symbol;
    const auto registered=runtime_.symbol_sessions.find(process);
    if(registered==runtime_.symbol_sessions.end() || !registered->second.event_callback)return invoke_dbghelp_symbol_core(state,memory,service);
    const GuestState entry=state;const auto depth=runtime_.callbacks.size();const auto saved_error=last_error_;
    SymbolEventSink sink;sink.debug=(runtime_.symbol_options&0x80000000U)!=0;std::optional<std::uint32_t> outcome;
    {const SymbolEventScope scope(sink);outcome=invoke_dbghelp_symbol_core(state,memory,service);}
    const auto current=runtime_.symbol_sessions.find(process);
    if(sink.events.empty() || current==runtime_.symbol_sessions.end() || !current->second.event_callback)return outcome;
    for(auto& event:sink.events)event.process=process;
    if(runtime_.callbacks.size()>depth){
        for(auto i=depth;i<runtime_.callbacks.size();++i)if(runtime_.callbacks[i].continuation==Continuation::SymbolEnumeration)(void)free_heap(memory,runtime_.callbacks[i].value,state);
        discard_callbacks(depth,state,memory);state=entry;last_error_=saved_error;
        if(start_symbol_events(state,memory,sink.events,std::nullopt))return std::nullopt;
        return invoke_dbghelp_symbol_core(state,memory,service);
    }
    const bool wide_result=name=="SymGetModuleBase64" || name=="SymLoadModuleEx" || name=="SymLoadModuleExW" || name=="SymLoadModule64";
    const auto result=x86_?(state.reg(Gpr::Rax)&UINT32_MAX)|(wide_result?(state.reg(Gpr::Rdx)&UINT32_MAX)<<32:0):state.reg(Gpr::Rax);
    const auto error=last_error_;
    // 模組已存在、範圍重疊等載入失敗時，先前讀到的符號沒有加入工作階段：只保留符號載入失敗本身的事件。
    if(!result && name.starts_with("SymLoadModule") && std::none_of(sink.events.begin(),sink.events.end(),[](const auto& event){return event.action==3;}))return outcome;
    GuestState returned=state;state=entry;
    if(start_symbol_events(state,memory,sink.events,std::array<std::uint64_t,3>{result,error,wide_result?1U:0U}))return std::nullopt;
    state=returned;last_error_=error;return outcome;
}
bool WindowsServices::start_symbol_events(GuestState& state,GuestMemory& memory,const std::vector<SymbolCallbackEvent>& events,std::optional<std::array<std::uint64_t,3>> result){
    // IMAGEHLP_DEFERRED_SYMBOL_LOAD（BaseOfImage 為 DWORD）、IMAGEHLP_DEFERRED_SYMBOL_LOAD64（ANSI）、IMAGEHLP_DEFERRED_SYMBOL_LOADW64（FileName[MAX_PATH+1]）。
    const auto layout=[&](std::uint8_t kind){
        struct Layout{unsigned size,base,base_bits,checksum,timestamp,name,name_units,reparse,file;};
        if(kind==1)return Layout{x86_?284U:288U,4,32,8,12,16,260,276,280};
        if(kind==2)return Layout{x86_?296U:304U,8,64,16,20,24,260,284,288};
        return Layout{x86_?560U:568U,8,64,16,20,24,261,546,x86_?548U:552U};
    };
    std::vector<std::pair<std::size_t,std::vector<std::uint8_t>>> payloads;std::vector<GuestCallback> calls;std::vector<std::optional<std::size_t>> offsets;std::size_t size=0;
    for(const auto& event:events){
        const auto session=runtime_.symbol_sessions.find(event.process);if(session==runtime_.symbol_sessions.end() || !session->second.event_callback)continue;
        const auto kind=session->second.event_kind;std::vector<std::uint8_t> bytes;bool pointer=true;
        if(event.action>=1 && event.action<=4){
            const auto at=layout(kind);bytes.assign(at.size,0);
            const auto put=[&](unsigned offset,unsigned width,std::uint64_t value){for(unsigned i=0;i<width/8;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*i));};
            put(0,32,at.size);put(at.base,at.base_bits,at.base_bits==32?event.base&UINT32_MAX:event.base);put(at.checksum,32,event.checksum);put(at.timestamp,32,event.timestamp);
            const auto name=event.text.substr(0,at.name_units-1);
            if(kind==3)for(std::size_t i=0;i<name.size();++i)put(at.name+static_cast<unsigned>(i)*2,16,name[i]);
            else{const auto ansi=utf16_to_utf8(name);for(std::size_t i=0;i<ansi.size() && i<at.name_units-1;++i)bytes[at.name+i]=static_cast<std::uint8_t>(ansi[i]);}
            (void)at.reparse;(void)at.file; // Reparse、hFile、Flags：沒有重新剖析、沒有呼叫端檔案 handle、沒有不相符的符號檔。
        }else if(event.action==0x10000000U){
            if(kind==3){for(const auto c:event.text){bytes.push_back(static_cast<std::uint8_t>(c));bytes.push_back(static_cast<std::uint8_t>(c>>8));}bytes.insert(bytes.end(),{0,0});}
            else{const auto ansi=utf16_to_utf8(event.text);bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}
        }else pointer=false;
        // CallbackData 指向區塊內的結構或字串（配置後才知道位址）；CBA_SET_OPTIONS 直接帶值。
        if(pointer){offsets.emplace_back(size);size+=(bytes.size()+7)&~std::size_t{7};payloads.emplace_back(*offsets.back(),std::move(bytes));}else offsets.emplace_back();
        GuestCallback call;call.target=session->second.event_callback;
        call.arguments={event.process,event.action,event.value,session->second.event_context};
        if(kind==1){if(x86_)call.arguments[3]&=UINT32_MAX;}else call.wide_arguments=0xc;
        calls.push_back(std::move(call));
    }
    if(calls.empty())return false;
    const auto block=allocate_heap(memory,std::max<std::size_t>(size,8));if(!block)return false;
    for(const auto& [offset,bytes]:payloads)memory.write(GuestAddress{block+offset},bytes);
    for(std::size_t i=0;i<calls.size();++i)if(offsets[i])calls[i].arguments[2]=block+*offsets[i];
    runtime_.symbol_event_returns[block]=result.value_or(std::array<std::uint64_t,3>{});
    try{start_callbacks(state,memory,std::move(calls),result?Continuation::SymbolEvent:Continuation::SymbolEventRetry,block);}
    catch(...){runtime_.symbol_event_returns.erase(block);(void)free_heap(memory,block,state);throw;}
    return true;
}
std::optional<std::uint32_t> WindowsServices::finish_symbol_events(GuestState& state,GuestMemory& memory,std::uint64_t block,bool retry){
    std::array<std::uint64_t,3> result{};
    if(const auto found=runtime_.symbol_event_returns.find(block);found!=runtime_.symbol_event_returns.end()){result=found->second;runtime_.symbol_event_returns.erase(found);}
    (void)free_heap(memory,block,state);
    if(retry)return std::nullopt; // 狀態已回到 API 入口，重新執行原本的查詢。
    last_error_=static_cast<std::uint32_t>(result[1]);complete_call(state,memory,result[0]);
    if(x86_ && result[2])state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(result[0]>>32);
    return std::nullopt;
}
namespace {bool dbghelp_line_service(std::string_view name);}
std::optional<std::uint32_t> WindowsServices::invoke_dbghelp_symbol_core(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    if(dbghelp_line_service(name))return invoke_dbghelp_lines(state,memory,service);const bool wide=name.ends_with('W') || name.ends_with("W64");
    const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer},32768):utf8_to_utf16(read_ansi(memory,pointer,32768));};
    std::uint64_t result=0;const auto fail=[&](std::uint32_t error){last_error_=error;};
    if(name.starts_with("EnumerateLoadedModules")){
        // 不需 SymInitialize：依載入器清單（主程式、外部 DLL、內建系統 DLL）回呼完整路徑、基底與大小，回呼回 FALSE 即停止。
        const bool unicode=name.ends_with("W64") || name.ends_with("ExW"),legacy=name=="EnumerateLoadedModules";
        if(!arg(1)){last_error_=87;arg.return_to(state,0);return std::nullopt;}
        std::vector<const RuntimeImage*> images;if(runtime_.image.base)images.push_back(&runtime_.image);
        for(const auto& [base,module]:runtime_.loaded_modules)images.push_back(&module.image);
        for(const auto& [base,image]:runtime_.builtin_images)if(!image.private_code)images.push_back(&image);
        std::vector<GuestCallback> calls;
        for(const auto* image:images){
            auto cached=runtime_.loaded_module_names.find(image->path);
            if(cached==runtime_.loaded_module_names.end()){
                const auto ansi=utf16_to_utf8(image->path);std::vector<std::uint8_t> narrow(ansi.begin(),ansi.end());narrow.push_back(0);
                std::vector<std::uint8_t> wide_text;for(const auto c:image->path){wide_text.push_back(static_cast<std::uint8_t>(c));wide_text.push_back(static_cast<std::uint8_t>(c>>8));}wide_text.insert(wide_text.end(),{0,0});
                cached=runtime_.loaded_module_names.emplace(image->path,std::array<std::uint64_t,2>{runtime_allocate(memory,narrow),runtime_allocate(memory,wide_text)}).first;
            }
            const auto text_address=cached->second[unicode?1:0];
            if(x86_ && !legacy)calls.push_back({arg(1),{text_address,image->base&UINT32_MAX,image->base>>32,image->size,arg(2)}});
            else calls.push_back({arg(1),{text_address,image->base,image->size,arg(2)}});
        }
        if(calls.empty()){arg.return_to(state,1);return std::nullopt;}
        start_callbacks(state,memory,std::move(calls),Continuation::ReturnUntilFalse,1);return std::nullopt;
    }
    // 不需要符號工作階段的輔助函式。
    if(name=="SymUnDName" || name=="SymUnDName64"){
        // (PIMAGEHLP_SYMBOL(64), UnDecName, UnDecNameLength)：以 UnDecorateSymbolName（UNDNAME_COMPLETE）解開結構內的 Name。
        const unsigned header=name.ends_with("64")?28U:20U;
        if(!arg(0) || !arg(1) || !static_cast<std::uint32_t>(arg(2)))fail(87);
        else result=(nested_service(state,memory,"dbghelp.dll","UnDecorateSymbolName",{arg(0)+header,arg(1),static_cast<std::uint32_t>(arg(2)),0})&UINT32_MAX)?1U:0U;
        arg.return_to(state,result);return std::nullopt;
    }
    if(name=="SymMatchString" || name=="SymMatchStringA" || name=="SymMatchStringW"){
        if(!arg(0) || !arg(1))fail(87);else result=symbol_match(text(arg(0)),text(arg(1)),static_cast<std::uint32_t>(arg(2))!=0)?1U:0U;
        arg.return_to(state,result);return std::nullopt;
    }
    if(name=="SymGetExtendedOption" || name=="SymSetExtendedOption"){
        // SYMOPT_EX_DISABLEACCESSTIMEUPDATE、LASTVALIDDEBUGDIRECTORY、NOIMPLICITPATTERNSEARCH、NEVERLOADSYMBOLS；設定時回傳先前的值。
        const auto option=static_cast<std::uint32_t>(arg(0));
        if(option<runtime_.symbol_extended_options.size()){result=runtime_.symbol_extended_options[option];if(name=="SymSetExtendedOption")runtime_.symbol_extended_options[option]=static_cast<std::uint32_t>(arg(1))!=0;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(name=="SymSetParentWindow"){runtime_.symbol_parent_window=arg(0);arg.return_to(state,1);return std::nullopt;}
    if(name=="GetTimestampForLoadedLibrary"){
        // 從記憶體中的 PE 標頭讀 TimeDateStamp；不是映像時為 0。
        try{if(arg(0) && memory.load(GuestAddress{arg(0)},16)==0x5a4d){const auto pe=arg(0)+memory.load(GuestAddress{arg(0)+60},32);if(memory.load(GuestAddress{pe},32)==0x4550)result=memory.load(GuestAddress{pe+8},32);}}catch(const MemoryFault&){result=0;}
        arg.return_to(state,result);return std::nullopt;
    }
    if(name=="MakeSureDirectoryPathExists"){
        // 依序建立路徑中的每一層目錄；最後一段不以分隔符號結尾時視為檔名不建立。沿用 CreateDirectoryW 的安全性與錯誤語義。
        if(!arg(0)){fail(87);arg.return_to(state,0);return std::nullopt;}
        const auto path=utf8_to_utf16(read_ansi(memory,arg(0),32768));std::size_t start=0;
        if(path.size()>=3 && path[1]==u':' && (path[2]==u'\\' || path[2]==u'/'))start=3;
        else if(path.starts_with(u"\\\\")){start=path.find_first_of(u"\\/",2);if(start!=path.npos)start=path.find_first_of(u"\\/",start+1);start=start==path.npos?path.size():start+1;}
        result=1;GuestScratch scratch(*this,memory,state);
        for(auto at=path.find_first_of(u"\\/",start);at!=path.npos && result;at=path.find_first_of(u"\\/",at+1)){
            if(at==start)continue;const auto prefix=path.substr(0,at);const auto buffer=scratch.allocate((prefix.size()+1)*2);if(!buffer){fail(8);result=0;break;}
            memory.write(GuestAddress{buffer},utf16_bytes(prefix));
            if(!(nested_service(state,memory,"kernel32.dll","CreateDirectoryW",{buffer,0})&UINT32_MAX) && last_error_!=183)result=0;
        }
        if(result)last_error_=0;arg.return_to(state,result);return std::nullopt;
    }
    if(name=="SearchTreeForFile" || name=="SearchTreeForFileW"){
        // (RootPath, InputPathName, OutputPathBuffer[MAX_PATH])：在根目錄下深度優先尋找同名檔案（不分大小寫，最多 32 層）。
        if(!arg(0) || !arg(1) || !arg(2)){fail(87);arg.return_to(state,0);return std::nullopt;}
        const auto found_path=search_tree_for_file(text(arg(0)),text(arg(1)));
        if(!found_path || found_path->size()>=260){fail(2);arg.return_to(state,0);return std::nullopt;}
        if(wide)memory.write(GuestAddress{arg(2)},utf16_bytes(*found_path));else{auto narrow=utf16_to_utf8(*found_path);narrow.push_back(0);memory.write(GuestAddress{arg(2)},std::span(reinterpret_cast<const std::uint8_t*>(narrow.data()),narrow.size()));}
        arg.return_to(state,1);return std::nullopt;
    }
    const auto found=runtime_.symbol_sessions.find(arg(0));
    if(found==runtime_.symbol_sessions.end())fail(6);
    else try{
        auto& session=found->second;
        // SYMBOL_INFO(W)：SizeOfStruct 88、MaxNameLen 為字元數；與 SymFromAddr 共用填寫。
        const auto symbol_info=[&](const SymbolModule& module,const SymbolEntry& symbol,std::uint64_t out){
            const auto capacity=memory.load(GuestAddress{out+80},32);
            auto bytes=wide?utf16_bytes(utf8_to_utf16(symbol.name)):std::vector<std::uint8_t>(symbol.name.begin(),symbol.name.end());if(!wide)bytes.push_back(0);
            const auto length=bytes.size()/(wide?2U:1U)-1;if(capacity>32768)throw SymbolFailure{87};if(capacity && capacity<=length)throw SymbolFailure{122};
            memory.check(GuestAddress{out},std::max<std::size_t>(88,capacity?84+bytes.size():88),Access::Write);
            memory.write(GuestAddress{out},std::array<std::uint8_t,84>{});memory.store(GuestAddress{out},32,88);memory.store(GuestAddress{out+24},32,symbol.index);memory.store(GuestAddress{out+28},32,symbol.size);
            memory.store(GuestAddress{out+32},64,module.image.base);memory.store(GuestAddress{out+40},32,symbol.flags);memory.store(GuestAddress{out+56},64,symbol.address);memory.store(GuestAddress{out+72},32,symbol.tag);
            memory.store(GuestAddress{out+76},32,length);memory.store(GuestAddress{out+80},32,capacity);if(capacity)memory.write(GuestAddress{out+84},bytes);
        };
        // IMAGEHLP_SYMBOL(64)：SizeOfStruct、Address、Size、Flags、MaxNameLength、Name；名稱超過緩衝時截斷。
        const auto imagehlp_symbol=[&](const SymbolEntry& symbol,std::uint64_t out,bool wide_address){
            const unsigned header=wide_address?28U:20U;const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{out+(wide_address?24:16)},32));
            const auto count=capacity?std::min<std::size_t>(symbol.name.size(),capacity-1):0;memory.check(GuestAddress{out},header+std::max<std::size_t>(count+1,1),Access::Write);
            if(wide_address){memory.store(GuestAddress{out+8},64,symbol.address);memory.store(GuestAddress{out+16},32,symbol.size);memory.store(GuestAddress{out+20},32,symbol.flags);}
            else {memory.store(GuestAddress{out+4},32,symbol.address);memory.store(GuestAddress{out+8},32,symbol.size);memory.store(GuestAddress{out+12},32,symbol.flags);}
            std::vector<std::uint8_t> bytes(symbol.name.begin(),symbol.name.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);if(capacity)memory.write(GuestAddress{out+header},bytes);
        };
        const auto visible=[&](const SymbolEntry& symbol){return !(runtime_.symbol_options&0x8000) || !(symbol.flags&0x200);};
        // 名稱查詢：「模組!名稱」只找該模組；SYMOPT_CASE_INSENSITIVE（1）時名稱不分大小寫，模組名稱一律不分大小寫。
        const auto by_name=[&](std::u16string_view requested)->std::pair<SymbolModule*,const SymbolEntry*>{
            const auto fold=[](std::u16string value){for(auto& c:value)c=unicode_case_unit(c,true);return value;};std::u16string module_part,symbol_part(requested);
            if(const auto bang=requested.find(u'!');bang!=std::u16string_view::npos){module_part=fold(std::u16string(requested.substr(0,bang)));symbol_part=std::u16string(requested.substr(bang+1));}
            const bool exact=!(runtime_.symbol_options&1U);const auto wanted=exact?symbol_part:fold(symbol_part);
            for(auto& [base,module]:session.images){(void)base;if(!module_part.empty() && fold(module.name)!=module_part)continue;materialize(module);
                for(const auto& symbol:module.symbols){if(!visible(symbol))continue;const auto candidate=utf8_to_utf16_lossy(symbol.name);if((exact?candidate:fold(candidate))==wanted)return {&module,&symbol};}}
            return {nullptr,nullptr};
        };
        // 列舉：名稱字串與 SYMBOL_INFO 放在同一塊程序堆積，回呼回 FALSE 即停止（函式仍回傳 TRUE），結束後釋放。
        const auto enumerate=[&](std::vector<std::vector<std::uint8_t>> payloads,const std::function<std::vector<std::uint64_t>(std::uint64_t,std::size_t)>& arguments,std::uint64_t target)->bool{
            if(payloads.empty())return false;std::size_t size=0;for(const auto& payload:payloads)size+=(payload.size()+7)&~std::size_t{7};
            const auto block=allocate_heap(memory,size);if(!block)throw std::bad_alloc{};std::vector<GuestCallback> calls;calls.reserve(payloads.size());
            try{auto at=block;for(std::size_t i=0;i<payloads.size();++i){memory.write(GuestAddress{at},payloads[i]);calls.push_back({target,arguments(at,i)});at+=(payloads[i].size()+7)&~std::size_t{7};}
                start_callbacks(state,memory,std::move(calls),Continuation::SymbolEnumeration,block);}catch(...){(void)free_heap(memory,block,state);throw;}
            return true;
        };
        const auto narrow_name=[](const std::string& text){std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);return bytes;};
        // SymEnumSymbols／SymSearch 的符號挑選：「模組!符號」比對兩段，指定 BaseOfDll 時只看該模組；沒有模組部分且未指定模組時為目前範圍的區域符號（匯出表與 COFF 沒有區域符號）。
        const auto select=[&](std::uint64_t base,std::u16string mask,std::uint32_t index,std::uint32_t tag,std::uint64_t address){
            std::vector<std::pair<SymbolModule*,const SymbolEntry*>> out;if(mask.empty())mask=u"*";std::u16string module_mask=u"*",symbol_mask=mask;
            if(const auto bang=mask.find(u'!');bang!=std::u16string::npos){module_mask=mask.substr(0,bang);symbol_mask=mask.substr(bang+1);}
            else if(!base)return out;
            const bool exact=!(runtime_.symbol_options&1U);
            for(auto& [module_base,module]:session.images){
                if(base?module_base!=base:!symbol_match(module.name,module_mask,false))continue;materialize(module);
                for(const auto& symbol:module.symbols){
                    if(!visible(symbol) || (index && symbol.index!=index) || (tag && symbol.tag!=tag) || (address && symbol.address!=address))continue;
                    if(symbol_match(utf8_to_utf16_lossy(symbol.name),symbol_mask,exact))out.emplace_back(&module,&symbol);
                }
            }
            if(base && !session.images.contains(base))throw SymbolFailure{126};
            return out;
        };
        if(name=="SymEnumSymbols" || name=="SymEnumSymbolsW" || name=="SymSearch" || name=="SymSearchW"){
            // SymEnumSymbols(hProcess, BaseOfDll, Mask, Callback, UserContext)；SymSearch(hProcess, BaseOfDll, Index, SymTag, Mask, Address, Callback, UserContext, Options)。
            const bool search=name.starts_with("SymSearch");const auto callback=arg(search?6:3),context=arg(search?7:4),mask_pointer=arg(search?4:2);if(!callback)throw SymbolFailure{87};
            const auto chosen=select(arg(1),mask_pointer?text(mask_pointer):std::u16string{},search?static_cast<std::uint32_t>(arg(2)):0U,search?static_cast<std::uint32_t>(arg(3)):0U,search?arg(5):0U);
            std::vector<std::vector<std::uint8_t>> payloads;std::vector<std::uint32_t> sizes;
            for(const auto& [module,symbol]:chosen){
                const auto length=wide?utf8_to_utf16(symbol->name).size():symbol->name.size();std::vector<std::uint8_t> bytes(std::max<std::size_t>(88,84+(length+1)*(wide?2U:1U)));
                bytes[0]=88;for(unsigned i=0;i<4;++i)bytes[80+i]=static_cast<std::uint8_t>((length+1)>>(8*i));payloads.push_back(std::move(bytes));sizes.push_back(symbol->size);
            }
            // 先配置並填好每一個 SYMBOL_INFO，再依序回呼（每個回呼收到自己的結構）。
            std::vector<std::uint64_t> addresses;
            if(!enumerate(payloads,[&](std::uint64_t at,std::size_t i){addresses.push_back(at);return std::vector<std::uint64_t>{at,sizes[i],context};},callback))result=1;
            else{for(std::size_t i=0;i<chosen.size();++i)symbol_info(*chosen[i].first,*chosen[i].second,addresses[i]);return std::nullopt;}
        }else if(name=="SymEnumerateSymbols" || name=="SymEnumerateSymbols64"){
            // 舊版：Callback(SymbolName, SymbolAddress, SymbolSize, UserContext)；64 位元版的位址在 x86 佔兩個堆疊格。
            const bool legacy=name=="SymEnumerateSymbols";const auto base=legacy?static_cast<std::uint32_t>(arg(1)):arg(1);if(!arg(2))throw SymbolFailure{87};
            auto* module=session.images.contains(base)?&session.images.at(base):nullptr;if(!module)throw SymbolFailure{126};materialize(*module);
            std::vector<std::vector<std::uint8_t>> payloads;std::vector<const SymbolEntry*> symbols;
            for(const auto& symbol:module->symbols)if(visible(symbol) && (!legacy || symbol.address<=UINT32_MAX)){payloads.push_back(narrow_name(symbol.name));symbols.push_back(&symbol);}
            if(!enumerate(std::move(payloads),[&](std::uint64_t at,std::size_t i){const auto address=symbols[i]->address;
                if(x86_ && !legacy)return std::vector<std::uint64_t>{at,address&UINT32_MAX,address>>32,symbols[i]->size,arg(3)};return std::vector<std::uint64_t>{at,address,symbols[i]->size,arg(3)};},arg(2)))result=1;
            else return std::nullopt;
        }else if(name.starts_with("SymEnumerateModules")){
            // Callback(ModuleName, BaseOfDll, UserContext)：依基底位址順序；W64 傳 UTF-16 名稱。
            const bool legacy=name=="SymEnumerateModules",unicode=name.ends_with("W64");if(!arg(1))throw SymbolFailure{87};
            std::vector<std::vector<std::uint8_t>> payloads;std::vector<std::uint64_t> bases;
            for(const auto& [base,module]:session.images){if(legacy && base>UINT32_MAX)continue;payloads.push_back(unicode?utf16_bytes(module.name):narrow_name(utf16_to_utf8(module.name)));bases.push_back(base);}
            if(!enumerate(std::move(payloads),[&](std::uint64_t at,std::size_t i){const auto base=bases[i];
                if(x86_ && !legacy)return std::vector<std::uint64_t>{at,base&UINT32_MAX,base>>32,arg(2)};return std::vector<std::uint64_t>{at,base,arg(2)};},arg(1)))result=1;
            else return std::nullopt;
        }else if(name=="SymFromName" || name=="SymFromNameW"){
            if(!arg(1) || !arg(2) || memory.load(GuestAddress{arg(2)},32)!=88)throw SymbolFailure{87};
            const auto [module,symbol]=by_name(text(arg(1)));if(!symbol)throw SymbolFailure{1168};symbol_info(*module,*symbol,arg(2));result=1;
        }else if(name=="SymGetSymFromName" || name=="SymGetSymFromName64"){
            const bool wide_address=name.ends_with("64");if(!arg(1) || !arg(2) || memory.load(GuestAddress{arg(2)},32)<(wide_address?32U:24U))throw SymbolFailure{87};
            const auto [module,symbol]=by_name(utf8_to_utf16(read_ansi(memory,arg(1),32768)));(void)module;if(!symbol)throw SymbolFailure{1168};imagehlp_symbol(*symbol,arg(2),wide_address);result=1;
        }else if(name.starts_with("SymGetSymNext") || name.starts_with("SymGetSymPrev")){
            // 依結構中的 Address 在同一模組找位址較大（Next）或較小（Prev）的最近符號。
            const bool wide_address=name.ends_with("64"),next=name.starts_with("SymGetSymNext");if(!arg(1) || memory.load(GuestAddress{arg(1)},32)<(wide_address?32U:24U))throw SymbolFailure{87};
            const auto address=wide_address?memory.load(GuestAddress{arg(1)+8},64):memory.load(GuestAddress{arg(1)+4},32);
            auto* module=containing(session,address);if(!module)throw SymbolFailure{487};materialize(*module);const SymbolEntry* best=nullptr;
            for(const auto& candidate:module->symbols){if(!visible(candidate))continue;
                if(next?candidate.address>address && (!best || candidate.address<best->address):candidate.address<address && (!best || candidate.address>best->address))best=&candidate;}
            if(!best)throw SymbolFailure{487};imagehlp_symbol(*best,arg(1),wide_address);result=1;
        }else if(name=="SymSetContext"){
            // (hProcess, PIMAGEHLP_STACK_FRAME, Context)：與目前框架相同時只更新 InstructionOffset，回傳 FALSE 且錯誤為 0。
            if(!arg(1))throw SymbolFailure{87};std::array<std::uint64_t,4> frame{};for(unsigned i=0;i<4;++i)frame[i]=memory.load(GuestAddress{arg(1)+i*8},64);
            if(!containing(session,frame[0]))throw SymbolFailure{126};
            if(frame[1]==session.scope[1] && frame[2]==session.scope[2] && frame[3]==session.scope[3] && session.scope[0]){session.scope[0]=frame[0];last_error_=0;result=0;}
            else{session.scope=frame;result=1;}
        }else if(name=="SymSetScopeFromAddr" || name=="SymSetScopeFromInlineContext"){
            if(!containing(session,arg(1)))throw SymbolFailure{126};session.scope={arg(1),0,0,0};result=1;
        }else if(name=="SymSetScopeFromIndex"){
            auto* module=session.images.contains(arg(1))?&session.images.at(arg(1)):nullptr;if(!module)throw SymbolFailure{126};materialize(*module);
            const auto index=static_cast<std::uint32_t>(arg(2));const auto symbol=std::find_if(module->symbols.begin(),module->symbols.end(),[&](const auto& item){return item.index==index;});
            if(symbol==module->symbols.end())throw SymbolFailure{1168};session.scope={symbol->address,0,0,0};result=1;
        }else if(name=="SymFromIndex" || name=="SymFromIndexW"){
            // (hProcess, BaseOfDll, Index, PSYMBOL_INFO)：模組內的符號索引（匯出表為序數索引、COFF 為符號表索引），與 SYMBOL_INFO 的 Index 相同。
            auto* module=session.images.contains(arg(1))?&session.images.at(arg(1)):nullptr;if(!module)throw SymbolFailure{126};materialize(*module);
            if(!arg(3) || memory.load(GuestAddress{arg(3)},32)!=88)throw SymbolFailure{87};
            const auto index=static_cast<std::uint32_t>(arg(2));
            const auto symbol=std::find_if(module->symbols.begin(),module->symbols.end(),[&](const auto& item){return visible(item) && item.index==index;});
            if(symbol==module->symbols.end())throw SymbolFailure{1168};symbol_info(*module,*symbol,arg(3));result=1;
        }else if(name=="SymGetTypeInfo"){
            // (hProcess, ModBase, TypeId, GetType, pInfo)：TypeId 為符號索引。匯出表與 COFF 只有符號、沒有型別記錄，提供 TI_GET_SYMTAG（0）、
            // TI_GET_SYMNAME（1，LocalAlloc 配置、LocalFree 釋放）、TI_GET_LENGTH（2）、TI_FINDCHILDREN（7，沒有子項）、TI_GET_CHILDRENCOUNT（13）、
            // TI_GET_SYMINDEX（20）與 TI_GET_ADDRESS（22）；其他屬性（型別、資料種類、偏移……）在這些符號上不存在，回 FALSE。
            auto* module=session.images.contains(arg(1))?&session.images.at(arg(1)):nullptr;if(!module)throw SymbolFailure{126};materialize(*module);
            const auto index=static_cast<std::uint32_t>(arg(2));const auto out=arg(4);if(!out)throw SymbolFailure{87};
            const auto symbol=std::find_if(module->symbols.begin(),module->symbols.end(),[&](const auto& item){return item.index==index;});
            if(symbol==module->symbols.end())throw SymbolFailure{1168};
            switch(static_cast<std::uint32_t>(arg(3))){
            case 0:memory.store(GuestAddress{out},32,symbol->tag);break;
            case 1:{
                const auto bytes=utf16_bytes(utf8_to_utf16_lossy(symbol->name));const auto block=allocate_heap(memory,bytes.size());if(!block)throw std::bad_alloc{};
                memory.write(GuestAddress{block},bytes);runtime_.local_allocations.insert(block);memory.store(GuestAddress{out},x86_?32:64,block);break;}
            case 2:memory.store(GuestAddress{out},64,symbol->size);break;
            case 7:if(memory.load(GuestAddress{out},32))throw SymbolFailure{87};break; // TI_FINDCHILDREN_PARAMS.Count 須等於子項數（0）
            case 13:memory.store(GuestAddress{out},32,0);break;
            case 20:memory.store(GuestAddress{out},32,symbol->index);break;
            case 22:memory.store(GuestAddress{out},64,symbol->address);break;
            default:throw SymbolFailure{1168};
            }
            result=1;
        }else if(name.starts_with("SymEnumTypes")){
            // SymEnumTypes(hProcess, BaseOfDll, Callback, Context)、SymEnumTypesByName(hProcess, BaseOfDll, mask, Callback, Context)：
            // 匯出表與 COFF 符號沒有型別記錄，模組存在即成功且不呼叫回呼。
            const bool by_type_name=name.starts_with("SymEnumTypesByName");if(!arg(by_type_name?3:2))throw SymbolFailure{87};
            if(!session.images.contains(arg(1)))throw SymbolFailure{126};result=1;
        }else if(name=="SymGetTypeFromName" || name=="SymGetTypeFromNameW"){
            // (hProcess, BaseOfDll, Name, PSYMBOL_INFO)：沒有型別記錄，任何名稱都找不到。
            if(!arg(2) || !arg(3) || memory.load(GuestAddress{arg(3)},32)!=88)throw SymbolFailure{87};
            if(!session.images.contains(arg(1)))throw SymbolFailure{126};throw SymbolFailure{1168};
        }else if(name=="SymAddrIncludeInlineTrace"){
            // 匯出表與 COFF 符號沒有行內框架資訊：位址所在模組不含行內呼叫。
            result=0;if(!containing(session,arg(1)))last_error_=126;
        }else if(name=="SymFromInlineContext" || name=="SymFromInlineContextW"){
            // (hProcess, Address, InlineContext, Displacement, Symbol)：沒有行內框架時與 SymFromAddr 相同。
            if(!arg(4) || memory.load(GuestAddress{arg(4)},32)!=88)throw SymbolFailure{87};auto* module=containing(session,arg(1));if(!module)throw SymbolFailure{487};materialize(*module);const SymbolEntry* symbol=nullptr;
            for(const auto& candidate:module->symbols)if(candidate.address<=arg(1) && visible(candidate) && (!symbol || candidate.address>symbol->address))symbol=&candidate;
            if(!symbol)throw SymbolFailure{487};if(arg(3))memory.check(GuestAddress{arg(3)},8,Access::Write);symbol_info(*module,*symbol,arg(4));if(arg(3))memory.store(GuestAddress{arg(3)},64,arg(1)-symbol->address);result=1;
        }else if(name=="SymRegisterFunctionEntryCallback64" || name=="SymRegisterFunctionEntryCallback"){
            // 32 位元版的回呼收到 DWORD 位址與 PVOID 內容。
            const bool legacy=name=="SymRegisterFunctionEntryCallback";
            session.function_callback=arg(1);session.function_context=legacy && x86_?arg(2)&UINT32_MAX:arg(2);session.function_callback_legacy=legacy;result=1;
        }else if(name=="SymRegisterCallback" || name=="SymRegisterCallback64" || name=="SymRegisterCallbackW64"){
            // 新的登記取代先前的回呼（不論哪一版）；回呼不得為 NULL。
            if(!arg(1))throw SymbolFailure{87};
            session.event_callback=arg(1);session.event_context=name=="SymRegisterCallback" && x86_?arg(2)&UINT32_MAX:arg(2);
            session.event_kind=name=="SymRegisterCallback"?1:name=="SymRegisterCallbackW64"?3:2;result=1;
        }else if(name=="SymLoadModuleEx" || name=="SymLoadModuleExW" || name=="SymLoadModule" || name=="SymLoadModule64"){
            // SymLoadModule／SymLoadModule64 等同不帶 MODLOAD_DATA、旗標為 0 的 SymLoadModuleEx。
            const bool legacy=name=="SymLoadModule" || name=="SymLoadModule64";
            const auto flags=legacy?0U:static_cast<std::uint32_t>(arg(7));
            if(flags&~5U)throw SymbolFailure{87};if(!legacy && arg(6))throw SymbolFailure{50};
            SymbolModule module;module.image.base=name=="SymLoadModule"?static_cast<std::uint32_t>(arg(4)):arg(4);module.image.size=static_cast<std::uint32_t>(arg(5));
            module.image_name=arg(2)?text(arg(2)):std::u16string{};module.name=arg(3)?text(arg(3)):module_name(module.image_name);
            module.no_symbols=(flags&4)!=0 || runtime_.symbol_extended_options[3];module.virtual_module=(flags&1)!=0;
            if(module.image.base && session.images.contains(module.image.base)){last_error_=0;arg.return_to(state,0,true);return std::nullopt;}
            if(module.virtual_module){if(module.name.empty() || !module.image.base || !module.image.size)throw SymbolFailure{87};module.symbol_type=8;}
            else {
                std::shared_ptr<const std::vector<std::uint8_t>> bytes;
                if(arg(1)){
                    const auto file=files_.find(arg(1));if(file==files_.end())throw SymbolFailure{6};
                    if(file->second.pipe || file->second.serial || file->second.hid || (file->second.metadata.attributes&16))throw SymbolFailure{87};
                    if(!(file->second.access&(0x80000000U|1U)))throw SymbolFailure{5};
                    if(file->second.native){
                        const auto info=file->second.native->information();if(info.error)throw SymbolFailure{info.error};if(info.metadata.size>maximum_file)throw SymbolFailure{8};
                        auto buffer=std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(info.metadata.size));
                        const auto read=file->second.native->read(*buffer,0);if(read.error)throw SymbolFailure{read.error};if(read.bytes!=buffer->size())throw SymbolFailure{38};bytes=std::move(buffer);
                    }else bytes=file->second.bytes;
                    if(module.image_name.empty())module.image_name=utf8_to_utf16(file->second.path);
                    module.loaded_image_name=utf8_to_utf16(file->second.path);
                }else if(!module.image_name.empty()){
                    auto source=filesystem_.open_library(utf16_to_utf8(module.image_name),runtime_.application_source,utf16_to_utf8(runtime_.application_directory));
                    if(source.error){const auto filename=leaf(module.image_name);std::size_t at=0;
                        while(at<=session.search_path.size()){
                            const auto end=session.search_path.find(u';',at);auto directory=session.search_path.substr(at,end==session.search_path.npos?end:end-at);
                            if(!directory.empty() && directory.back()!=u'\\' && directory.back()!=u'/')directory+=u'\\';
                            auto candidate=filesystem_.open(utf16_to_utf8(directory+filename));if(!candidate.error){source=std::move(candidate);break;}
                            if(end==session.search_path.npos)break;at=end+1;
                        }
                    }
                    if(source.error)throw SymbolFailure{source.error};bytes=source.bytes;module.loaded_image_name=utf8_to_utf16(source.key);
                }else if(!current_process_handle(arg(0)))throw SymbolFailure{50};
                else if(const auto* image=module_image(module.image.base,false)){
                    const auto base=module.image.base,size=module.image.size;module=live_module(*image,memory);module.invaded=false;module.no_symbols=(flags&4)!=0;
                    if(size)module.image.size=size;module.image.base=base;if(arg(3))module.name=text(arg(3));if(module.no_symbols){module.symbols.clear();module.symbol_type=0;}
                    if(!module.no_symbols)report_load(module,0); // 由記憶體中的映像讀取匯出，與檔案載入相同送出載入事件。
                }else throw SymbolFailure{87};
                if(bytes){
                    if(bytes->size()>maximum_file)throw SymbolFailure{8};
                    PeInventory inventory;try{inventory=inspect_pe_image(*bytes);}catch(const std::bad_alloc&){throw;}catch(const std::exception&){throw SymbolFailure{193};}
                    if(!module.image.base)module.image.base=inventory.metadata.preferred_base.value;if(!module.image.size)module.image.size=inventory.metadata.image_size;
                    module.machine=static_cast<std::uint32_t>(inventory.metadata.architecture);const auto pe=static_cast<std::size_t>(integer(*bytes,60,4));
                    module.timestamp=static_cast<std::uint32_t>(integer(*bytes,pe+8,4));module.checksum=static_cast<std::uint32_t>(integer(*bytes,pe+24+64,4));
                    module.image.export_names=std::move(inventory.export_names);module.image.export_ordinals=std::move(inventory.export_ordinals);module.image.export_forwarders=std::move(inventory.export_forwarders);
                    if(!module.no_symbols){
                        // CodeView（RSDS）指出的 PDB：依序找記錄的路徑、映像所在目錄與符號搜尋路徑；GUID 與 Age 相符才採用。
                        if(const auto view=pdb::image_codeview(*bytes)){
                            const auto pdb_path=utf8_to_utf16(view->path);const auto pdb_leaf=leaf(pdb_path);std::vector<std::u16string> candidates;
                            if(pdb_path.size()>2 && pdb_path[1]==u':')candidates.push_back(pdb_path);
                            const auto image_path=module.loaded_image_name.empty()?module.image_name:module.loaded_image_name;
                            if(const auto slash=image_path.find_last_of(u"\\/");slash!=image_path.npos)candidates.push_back(image_path.substr(0,slash+1)+pdb_leaf);
                            for(std::size_t at=0;at<=session.search_path.size();){
                                const auto end=session.search_path.find(u';',at);auto directory=session.search_path.substr(at,end==session.search_path.npos?end:end-at);
                                if(!directory.empty()){if(directory.back()!=u'\\' && directory.back()!=u'/')directory+=u'\\';candidates.push_back(directory+pdb_leaf);}
                                if(end==session.search_path.npos)break;at=end+1;
                            }
                            for(const auto& path:candidates){
                                auto source=filesystem_.open(utf16_to_utf8(path));if(source.error || !source.bytes || source.bytes->size()>maximum_file)continue;
                                auto database=pdb::parse(*source.bytes);if(!database || database->guid!=view->guid || database->age!=view->age)continue;
                                module.pdb=std::make_shared<const pdb::Database>(std::move(*database));module.pdb_name=utf8_to_utf16(source.key);break;
                            }
                        }
                        module.pending=std::move(bytes);module.symbol_type=5;if(!(runtime_.symbol_options&4))materialize(module);
                    }
                }
                if(module.name.empty())module.name=module_name(module.image_name);
            }
            if(!module.image.base || !module.image.size || module.image.size>UINT32_MAX || module.image.size>UINT64_MAX-module.image.base)throw SymbolFailure{87};
            if(session.images.contains(module.image.base)){last_error_=0;arg.return_to(state,0,true);return std::nullopt;}
            if(session.images.size()>=4096)throw SymbolFailure{8};
            if(std::any_of(session.images.begin(),session.images.end(),[&](const auto& item){return module.image.base<item.first+item.second.image.size && item.first<module.image.base+module.image.size;}))throw SymbolFailure{487};
            result=module.image.base;session.images.emplace(result,std::move(module));session.modules.insert(result);
        }else if(name=="SymGetSymFromAddr" || name=="SymGetSymFromAddr64"){
            // IMAGEHLP_SYMBOL(64)：SizeOfStruct、Address、Size、Flags、MaxNameLength、Name；名稱超過緩衝時截斷。
            const bool wide_address=name.ends_with("64");const auto address=wide_address?arg(1):static_cast<std::uint32_t>(arg(1));
            const unsigned header=wide_address?28U:20U,structure=wide_address?32U:24U;
            if(!arg(3))throw SymbolFailure{87};memory.check(GuestAddress{arg(3)},header,Access::Read);
            if(memory.load(GuestAddress{arg(3)},32)<structure)throw SymbolFailure{87};
            auto* module=containing(session,address);if(!module)throw SymbolFailure{487};materialize(*module);const SymbolEntry* symbol=nullptr;
            for(const auto& candidate:module->symbols)if(candidate.address<=address && (!(runtime_.symbol_options&0x8000) || !(candidate.flags&0x200)) && (!symbol || candidate.address>symbol->address))symbol=&candidate;
            if(!symbol)throw SymbolFailure{487};if(arg(2))memory.check(GuestAddress{arg(2)},wide_address?8:4,Access::Write);
            imagehlp_symbol(*symbol,arg(3),wide_address);if(arg(2))memory.store(GuestAddress{arg(2)},wide_address?64:32,address-symbol->address);result=1;
        }else if(name=="SymUnloadModule64" || name=="SymUnloadModule"){
            const auto module=session.images.find(arg(1));
            if(module==session.images.end())fail(487);
            else {if(symbol_event_sink)symbol_event_sink->events.push_back(module_event(module->second,4));for(const auto* cache:{&module->second.line_text,&module->second.line_text_wide})for(const auto& [index,block]:*cache){(void)index;(void)free_heap(memory,block,state);}session.images.erase(module);session.modules.erase(arg(1));result=1;}
        }else if(name=="SymFunctionTableAccess64" || name=="SymFunctionTableAccess"){
            if(current_process_handle(arg(0)) && function_table_callback(arg(1))){
                const auto scratch=allocate_heap(memory,8);if(!scratch)throw std::bad_alloc{};
                try{start_callbacks(state,memory,{{resolve("ntdll.dll","RtlLookupFunctionEntry").value,{arg(1),scratch,0}}},Continuation::ReturnCallbackFreeHeap,scratch);}catch(...){(void)free_heap(memory,scratch,state);throw;}return {};
            }
            // 真正可讀取的已映射／動態 CPU unwind table 優先；離線符號檔沒有客體 function table。
            if(auto* module=containing(session,arg(1));module && current_process_handle(arg(0)))if(const auto* image=unwind_image(memory,arg(1)))if(const auto* entry=lookup_function(*image,arg(1));entry && image->function_table)
                result=image->function_table+static_cast<std::uint64_t>(entry-image->functions.data())*12;
            if(!result && session.function_callback && !session.function_callback_active){
                GuestCallback callback;callback.target=session.function_callback;callback.arguments={arg(0),arg(1),session.function_context};callback.wide_arguments=6;
                if(session.function_callback_legacy){callback.arguments[1]&=UINT32_MAX;callback.wide_arguments=0;}
                session.function_callback_active=true;try{start_callbacks(state,memory,{std::move(callback)},Continuation::SymbolFunctionEntry,arg(0));}catch(...){discard_symbol_callback(arg(0));throw;}return std::nullopt;
            }
            if(!result)fail(487);
        }else {
            auto* module=containing(session,arg(1));if(!module)throw SymbolFailure{487};
            if(name=="SymGetModuleBase64" || name=="SymGetModuleBase")result=module->image.base;
            else if(name=="SymGetModuleInfo" || name=="SymGetModuleInfoW"){
                // IMAGEHLP_MODULE(W)：SizeOfStruct、BaseOfImage、ImageSize、TimeDateStamp、CheckSum、NumSyms、SymType（DWORD）、ModuleName[32]、ImageName[256]、LoadedImageName[256]。
                const unsigned size=wide?1116U:572U,unit=wide?2U:1U;if(!arg(2) || memory.load(GuestAddress{arg(2)},32)!=size || module->image.base>UINT32_MAX)throw SymbolFailure{87};
                memory.check(GuestAddress{arg(2)},size,Access::Write);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(size));
                const auto put=[&](unsigned offset,std::uint64_t value){memory.store(GuestAddress{arg(2)+offset},32,value);};
                const auto string=[&](unsigned offset,unsigned count,std::u16string_view value){
                    if(wide)memory.write(GuestAddress{arg(2)+offset},utf16_bytes(std::u16string(value.substr(0,count-1))));
                    else {auto ansi=utf16_to_utf8(value);ansi.resize(std::min<std::size_t>(ansi.size(),count-1));memory.write(GuestAddress{arg(2)+offset},std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(ansi.data()),ansi.size()});}
                };
                put(0,size);put(4,module->image.base);put(8,module->image.size);put(12,module->timestamp);put(16,module->checksum);put(20,module->pending?0:module->symbols.size());put(24,module->symbol_type);
                string(28,32,module->name);string(28+32*unit,256,module->image_name);string(28+288*unit,256,module->loaded_image_name);result=1;
            }
            else if(name=="SymGetModuleInfo64" || name=="SymGetModuleInfoW64"){
                const unsigned size=wide?3264:1680,unit=wide?2:1;if(!arg(2) || memory.load(GuestAddress{arg(2)},32)!=size)throw SymbolFailure{87};
                memory.check(GuestAddress{arg(2)},size,Access::Write);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(size));
                const auto put=[&](unsigned offset,unsigned bits,std::uint64_t value){memory.store(GuestAddress{arg(2)+offset},static_cast<std::uint8_t>(bits),value);};
                const auto string=[&](unsigned offset,unsigned count,std::u16string_view value){
                    if(wide)memory.write(GuestAddress{arg(2)+offset},utf16_bytes(std::u16string(value.substr(0,count-1))));
                    else {auto ansi=utf16_to_utf8(value);ansi.resize(std::min<std::size_t>(ansi.size(),count-1));memory.write(GuestAddress{arg(2)+offset},std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(ansi.data()),ansi.size()});}
                };
                put(0,32,size);put(8,64,module->image.base);put(16,32,module->image.size);put(20,32,module->timestamp);put(24,32,module->checksum);put(28,32,module->pending?0:module->symbols.size());put(32,32,module->symbol_type);
                string(36,32,module->name);string(36+32*unit,256,module->image_name);string(36+288*unit,256,module->loaded_image_name);
                put(wide?3236:1656,32,module->symbol_type==1 || (module->pdb && !module->pdb->symbols.empty()));
                if(module->pdb){
                    // PDB 7.0：LoadedPdbName、CVSig（RSDS）、CVData（PDB 路徑）、PdbSig70、PdbAge、LineNumbers、SourceIndexed。
                    string(36+544*unit,256,module->pdb_name);put(36+800*unit,32,0x53445352U);string(40+800*unit,780,module->pdb_name);
                    memory.write(GuestAddress{arg(2)+(wide?3204U:1624U)},module->pdb->guid);put(wide?3220:1640,32,module->pdb->age);
                    put(wide?3232:1652,32,!module->pdb->lines.empty());put(wide?3244:1664,32,!module->pdb->source_server.empty());
                }
                put(wide?3248:1668,32,std::any_of(module->symbols.begin(),module->symbols.end(),[](const auto& symbol){return symbol.flags&0x200;}) || (module->pdb && std::any_of(module->pdb->symbols.begin(),module->pdb->symbols.end(),[](const auto& item){return item.pub;})));put(wide?3252:1672,32,module->machine);result=1;
            }else if(name=="SymAddSymbol" || name=="SymAddSymbolW"){
                if(arg(1)!=module->image.base || !arg(2))throw SymbolFailure{87};const auto symbol=text(arg(2));const auto offset=arg(3)-module->image.base;
                if(symbol.empty() || symbol.size()>2000 || arg(3)<module->image.base || offset>=module->image.size || arg(4)>module->image.size-offset)throw SymbolFailure{87};
                materialize(*module);if(module->symbols.size()>=maximum_symbols)throw SymbolFailure{8};const auto ansi=utf16_to_utf8(symbol);
                if(std::any_of(module->symbols.begin(),module->symbols.end(),[&](const auto& item){return item.name==ansi && item.address==arg(3);}))throw SymbolFailure{183};
                module->symbols.push_back({ansi,arg(3),static_cast<std::uint32_t>(arg(4)),static_cast<std::uint32_t>(module->symbols.size()+1),0x1000,10});result=1;
            }else if(name=="SymDeleteSymbol" || name=="SymDeleteSymbolW"){
                if(arg(1)!=module->image.base)throw SymbolFailure{87};materialize(*module);const auto symbol=arg(2)?utf16_to_utf8(text(arg(2))):std::string{};
                const auto item=std::find_if(module->symbols.begin(),module->symbols.end(),[&](const auto& candidate){return (candidate.flags&0x1000) && candidate.address==arg(3) && (!arg(2) || candidate.name==symbol);});
                if(item==module->symbols.end())throw SymbolFailure{487};module->symbols.erase(item);result=1;
            }else if(name=="SymFromAddr" || name=="SymFromAddrW"){
                if(!arg(3) || memory.load(GuestAddress{arg(3)},32)!=88)throw SymbolFailure{87};materialize(*module);const SymbolEntry* symbol=nullptr;
                for(const auto& candidate:module->symbols)if(candidate.address<=arg(1) && (!(runtime_.symbol_options&0x8000) || !(candidate.flags&0x200)) && (!symbol || candidate.address>symbol->address))symbol=&candidate;
                if(!symbol)throw SymbolFailure{487};if(arg(2))memory.check(GuestAddress{arg(2)},8,Access::Write);
                symbol_info(*module,*symbol,arg(3));if(arg(2))memory.store(GuestAddress{arg(2)},64,arg(1)-symbol->address);result=1;
            }
            else throw HostCallFault("尚未實作 DbgHelp 符號服務："+std::string(name));
        }
    }catch(const SymbolFailure& error){result=0;fail(error.error);}catch(const std::bad_alloc&){result=0;fail(8);}
    arg.return_to(state,result,name=="SymGetModuleBase64" || name=="SymLoadModuleEx" || name=="SymLoadModuleExW" || name=="SymLoadModule64");return std::nullopt;
}
#include "windows_dbghelp_lines.inc"
}
