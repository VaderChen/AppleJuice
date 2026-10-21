#include "x64arm/pe_loader.hpp"
#include "x64arm/unicode.hpp"
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
void materialize(SymbolModule& module){
    if(!module.pending)return;
    // 先在副本完成，壞的 COFF record 不留下半套符號。
    auto candidate=module;candidate.symbols.clear();exports(candidate);const auto before=candidate.symbols.size();coff(candidate,*candidate.pending);
    candidate.symbol_type=candidate.symbols.size()>before?1U:!candidate.symbols.empty()?4U:0U;candidate.pending.reset();module=std::move(candidate);
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
        {"dbghelp.dll","SymGetModuleInfo64",3,2},{"dbghelp.dll","SymGetModuleInfoW64",3,2},
        // Size／Flags 是 DWORD：x64 堆疊 slot 的高 32 位元可能殘留舊值，必須只取低位。
        {"dbghelp.dll","SymAddSymbol",6,10,0,0x30},{"dbghelp.dll","SymAddSymbolW",6,10,0,0x30},
        {"dbghelp.dll","SymDeleteSymbol",5,10,0,0x10},{"dbghelp.dll","SymDeleteSymbolW",5,10,0,0x10},
        {"dbghelp.dll","SymLoadModule",6,0,0,0x30},{"dbghelp.dll","SymLoadModule64",6,16,0,0x20},
        {"dbghelp.dll","SymGetSymFromAddr",4},{"dbghelp.dll","SymGetSymFromAddr64",4,2}
    };return table;
}
void WindowsServices::refresh_symbol_modules(SymbolSession& session,const GuestMemory& memory){
    std::map<std::uint64_t,const RuntimeImage*> images;if(runtime_.image.base)images.emplace(runtime_.image.base,&runtime_.image);
    for(const auto& [base,module]:runtime_.loaded_modules)images.emplace(base,&module.image);
    for(const auto& [base,image]:runtime_.builtin_images)images.emplace(base,&image);
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
std::optional<std::uint32_t> WindowsServices::invoke_dbghelp_symbols(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W') || name.ends_with("W64");
    const auto text=[&](std::uint64_t pointer){return wide?read_utf16(memory,GuestAddress{pointer},32768):utf8_to_utf16(read_ansi(memory,pointer,32768));};
    std::uint64_t result=0;const auto fail=[&](std::uint32_t error){last_error_=error;};
    const auto found=runtime_.symbol_sessions.find(arg(0));
    if(found==runtime_.symbol_sessions.end())fail(6);
    else try{
        auto& session=found->second;
        if(name=="SymRegisterFunctionEntryCallback64"){
            session.function_callback=arg(1);session.function_context=arg(2);result=1;
        }else if(name=="SymLoadModuleEx" || name=="SymLoadModuleExW" || name=="SymLoadModule" || name=="SymLoadModule64"){
            // SymLoadModule／SymLoadModule64 等同不帶 MODLOAD_DATA、旗標為 0 的 SymLoadModuleEx。
            const bool legacy=name=="SymLoadModule" || name=="SymLoadModule64";
            const auto flags=legacy?0U:static_cast<std::uint32_t>(arg(7));
            if(flags&~5U)throw SymbolFailure{87};if(!legacy && arg(6))throw SymbolFailure{50};
            SymbolModule module;module.image.base=name=="SymLoadModule"?static_cast<std::uint32_t>(arg(4)):arg(4);module.image.size=static_cast<std::uint32_t>(arg(5));
            module.image_name=arg(2)?text(arg(2)):std::u16string{};module.name=arg(3)?text(arg(3)):module_name(module.image_name);
            module.no_symbols=(flags&4)!=0;module.virtual_module=(flags&1)!=0;
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
                }else throw SymbolFailure{87};
                if(bytes){
                    if(bytes->size()>maximum_file)throw SymbolFailure{8};
                    PeInventory inventory;try{inventory=inspect_pe_image(*bytes);}catch(const std::bad_alloc&){throw;}catch(const std::exception&){throw SymbolFailure{193};}
                    if(!module.image.base)module.image.base=inventory.metadata.preferred_base.value;if(!module.image.size)module.image.size=inventory.metadata.image_size;
                    module.machine=static_cast<std::uint32_t>(inventory.metadata.architecture);const auto pe=static_cast<std::size_t>(integer(*bytes,60,4));
                    module.timestamp=static_cast<std::uint32_t>(integer(*bytes,pe+8,4));module.checksum=static_cast<std::uint32_t>(integer(*bytes,pe+24+64,4));
                    module.image.export_names=std::move(inventory.export_names);module.image.export_ordinals=std::move(inventory.export_ordinals);module.image.export_forwarders=std::move(inventory.export_forwarders);
                    if(!module.no_symbols){module.pending=std::move(bytes);module.symbol_type=5;if(!(runtime_.symbol_options&4))materialize(module);}
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
            if(!symbol)throw SymbolFailure{487};
            const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+(wide_address?24:16)},32));
            const auto count=capacity?std::min<std::size_t>(symbol->name.size(),capacity-1):0;
            memory.check(GuestAddress{arg(3)},header+std::max<std::size_t>(count+1,1),Access::Write);if(arg(2))memory.check(GuestAddress{arg(2)},wide_address?8:4,Access::Write);
            if(wide_address){memory.store(GuestAddress{arg(3)+8},64,symbol->address);memory.store(GuestAddress{arg(3)+16},32,symbol->size);memory.store(GuestAddress{arg(3)+20},32,symbol->flags);}
            else {memory.store(GuestAddress{arg(3)+4},32,symbol->address);memory.store(GuestAddress{arg(3)+8},32,symbol->size);memory.store(GuestAddress{arg(3)+12},32,symbol->flags);}
            std::vector<std::uint8_t> bytes(symbol->name.begin(),symbol->name.begin()+static_cast<std::ptrdiff_t>(count));bytes.push_back(0);
            if(capacity)memory.write(GuestAddress{arg(3)+header},bytes);
            if(arg(2))memory.store(GuestAddress{arg(2)},wide_address?64:32,address-symbol->address);result=1;
        }else if(name=="SymUnloadModule64"){
            if(!session.images.erase(arg(1)))fail(487);else {session.modules.erase(arg(1));result=1;}
        }else if(name=="SymFunctionTableAccess64"){
            // 真正可讀取的已映射／動態 CPU unwind table 優先；離線符號檔沒有客體 function table。
            if(auto* module=containing(session,arg(1));module && current_process_handle(arg(0)))if(const auto* image=unwind_image(memory,arg(1)))if(const auto* entry=lookup_function(*image,arg(1));entry && image->function_table)
                result=image->function_table+static_cast<std::uint64_t>(entry-image->functions.data())*12;
            if(!result && session.function_callback && !session.function_callback_active){
                GuestCallback callback;callback.target=session.function_callback;callback.arguments={arg(0),arg(1),session.function_context};callback.wide_arguments=6;
                session.function_callback_active=true;try{start_callbacks(state,memory,{std::move(callback)},Continuation::SymbolFunctionEntry,arg(0));}catch(...){discard_symbol_callback(arg(0));throw;}return std::nullopt;
            }
            if(!result)fail(487);
        }else {
            auto* module=containing(session,arg(1));if(!module)throw SymbolFailure{487};
            if(name=="SymGetModuleBase64")result=module->image.base;
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
                put(wide?3236:1656,32,module->symbol_type==1);
                put(wide?3248:1668,32,std::any_of(module->symbols.begin(),module->symbols.end(),[](const auto& symbol){return symbol.flags&0x200;}));put(wide?3252:1672,32,module->machine);result=1;
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
                if(!symbol)throw SymbolFailure{487};const auto capacity=memory.load(GuestAddress{arg(3)+80},32);
                auto bytes=wide?utf16_bytes(utf8_to_utf16(symbol->name)):std::vector<std::uint8_t>(symbol->name.begin(),symbol->name.end());if(!wide)bytes.push_back(0);
                const auto length=bytes.size()/(wide?2U:1U)-1;if(capacity>32768)throw SymbolFailure{87};if(capacity && capacity<=length)throw SymbolFailure{122};
                memory.check(GuestAddress{arg(3)},std::max<std::size_t>(88,capacity?84+bytes.size():88),Access::Write);if(arg(2))memory.check(GuestAddress{arg(2)},8,Access::Write);
                memory.write(GuestAddress{arg(3)},std::array<std::uint8_t,84>{});memory.store(GuestAddress{arg(3)},32,88);memory.store(GuestAddress{arg(3)+24},32,symbol->index);memory.store(GuestAddress{arg(3)+28},32,symbol->size);
                memory.store(GuestAddress{arg(3)+32},64,module->image.base);memory.store(GuestAddress{arg(3)+40},32,symbol->flags);memory.store(GuestAddress{arg(3)+56},64,symbol->address);memory.store(GuestAddress{arg(3)+72},32,symbol->tag);
                memory.store(GuestAddress{arg(3)+76},32,length);memory.store(GuestAddress{arg(3)+80},32,capacity);if(capacity)memory.write(GuestAddress{arg(3)+84},bytes);if(arg(2))memory.store(GuestAddress{arg(2)},64,arg(1)-symbol->address);result=1;
            }else if(name.starts_with("SymGetLineFromAddr"))throw SymbolFailure{487};
            else throw HostCallFault("尚未實作 DbgHelp 符號服務："+std::string(name));
        }
    }catch(const SymbolFailure& error){result=0;fail(error.error);}catch(const std::bad_alloc&){result=0;fail(8);}
    arg.return_to(state,result,name=="SymGetModuleBase64" || name=="SymLoadModuleEx" || name=="SymLoadModuleExW" || name=="SymLoadModule64");return std::nullopt;
}
}
