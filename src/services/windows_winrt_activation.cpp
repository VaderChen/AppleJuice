#include "x64arm/windows_services.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_memory=0x8007000eU;
constexpr std::uint32_t reference_magic=0x52534858U;
constexpr std::uint32_t not_initialized=0x800401f0U,class_not_registered=0x80040154U,no_interface=0x80004002U;
enum Phase : unsigned { Load,FindFactory,GetFactory,QueryFactory,Activate,ReleaseFactory,FindUnload=10,CanUnload,FreeModule };
constexpr unsigned operation_size=160,header_offset=96,class_offset=160;
std::u16string fold(std::u16string value){for(auto& c:value)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return value;}
std::uint32_t hresult(std::uint32_t error){return error?0x80070000U|(error&65535U):0x80004005U;}
std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    const std::uint8_t bits=memory.execution_mode()==ExecutionMode::X86?32:64;
    const auto table=memory.load(GuestAddress{object},bits),offset=std::uint64_t{slot}*(bits/8U);
    if(!table || table>UINT64_MAX-offset)throw HostCallFault("WinRT vtable 位址不合法");
    return memory.load(GuestAddress{table+offset},bits);
}
std::u16string registry_text(const RegistryValue& value){
    if((value.type!=1 && value.type!=2) || value.bytes.size()%2 || value.bytes.size()>65536)throw std::invalid_argument("WinRT 類別登錄字串不合法");
    std::u16string result;
    for(std::size_t i=0;i<value.bytes.size();i+=2){const auto c=static_cast<char16_t>(value.bytes[i]|(std::uint16_t{value.bytes[i+1]}<<8));if(!c)break;result.push_back(c);}
    return result;
}
}
std::u16string WindowsServices::winrt_string_content(const GuestMemory& memory,std::uint64_t handle) const {
    return winrt_content(memory,winrt_string(memory,handle));
}
void WindowsServices::discard_winrt_operation(GuestState& state,GuestMemory& memory,std::uint64_t scratch){
    if(!scratch)return;
    if(memory.load(GuestAddress{scratch+12},32)){
        const auto module=memory.load(GuestAddress{scratch+16},64);const auto found=runtime_.winrt_modules.find(module);
        if(found!=runtime_.winrt_modules.end() && found->second.active)--found->second.active;
    }
    if(memory.load(GuestAddress{scratch},32)>=FindUnload){
        const auto module=memory.load(GuestAddress{scratch+16},64);const auto found=runtime_.winrt_modules.find(module);
        if(found!=runtime_.winrt_modules.end())found->second.unloading=false;
    }
    (void)free_heap(memory,scratch,state);
}
std::optional<std::uint32_t> WindowsServices::invoke_winrt_activation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool activate=service.symbol=="RoActivateInstance";
    const auto output=arg(activate?1:2);const std::uint8_t bits=x86_?32:64;
    const auto done=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    std::uint64_t scratch=0;
    try{
        if(!output || (!activate && !arg(1)))return done(pointer_error);
        memory.store(GuestAddress{output},bits,0);
        const auto apartment=com_apartment(threads_.current);if(!apartment)return done(not_initialized);
        const auto class_name=winrt_string_content(memory,arg(0));
        if(class_name.empty() || class_name.size()>32767 || class_name.find(u'\0')!=class_name.npos)return done(invalid);
        std::u16string path;
        // RoRegisterActivationFactories 登錄的程序內工廠優先，不需載入 DLL。
        const auto registered=winrt_class_factories_.find(fold(class_name));
        if(registered==winrt_class_factories_.end()){
            // 與 Reg* 共用持久檔鎖及 WOW64 視圖；不在 guest 回呼期間持有鎖。
            if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
            RegistryTransaction transaction(runtime_.registry,registry_file_);if(transaction.error)return done(hresult(transaction.error));
            initialize_registry_views(runtime_.registry);
            const auto key=registry_view_path(std::u16string(1,2)+u"\\software\\microsoft\\windowsruntime\\activatableclassid\\"+fold(class_name),registry_view(0,x86_));
            const auto found=runtime_.registry.keys.find(key);if(found==runtime_.registry.keys.end())return done(class_not_registered);
            const auto dll=found->second.find(u"dllpath");if(dll==found->second.end())return done(class_not_registered);
            path=registry_text(dll->second);if(path.empty())return done(class_not_registered);
            if(const auto model=found->second.find(u"threadingmodel");model!=found->second.end()){
                const auto threading=fold(registry_text(model->second));
                if(threading!=u"both" && threading!=u"sta" && threading!=u"apartment" && threading!=u"mta" && threading!=u"free")return done(invalid);
                // 跨 apartment 的 proxy／marshaling 尚未提供；不可直接在錯誤 apartment 執行。
                if(((threading==u"sta" || threading==u"apartment") && !*apartment) || ((threading==u"mta" || threading==u"free") && *apartment))return done(0x80004001U);
            }
            if(dll->second.type==2){
                std::u16string expanded;
                for(std::size_t at=0;at<path.size();){if(path[at]!=u'%'){expanded.push_back(path[at++]);continue;}const auto end=path.find(u'%',at+1);if(end==path.npos){expanded.append(path.substr(at));break;}
                    auto variable=path.substr(at+1,end-at-1);for(auto& c:variable)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
                    const auto value=runtime_.environment.find(variable);expanded.append(value==runtime_.environment.end()?path.substr(at,end-at+1):value->second.second);at=end+1;
                    if(expanded.size()>32767)return done(invalid);}
                path=std::move(expanded);
            }
        }
        else path=u"";
        const auto class_bytes=utf16_bytes(class_name),path_bytes=utf16_bytes(path);
        const auto path_offset=class_offset+class_bytes.size();scratch=allocate_heap(memory,path_offset+path_bytes.size());if(!scratch)return done(no_memory);
        memory.write(GuestAddress{scratch},std::vector<std::uint8_t>(operation_size));
        memory.store(GuestAddress{scratch+4},32,activate);memory.store(GuestAddress{scratch+40},64,output);
        if(!activate)memory.move(GuestAddress{scratch+64},GuestAddress{arg(1)},16);
        memory.write(GuestAddress{scratch+class_offset},class_bytes);memory.write(GuestAddress{scratch+path_offset},path_bytes);
        memory.store(GuestAddress{scratch+48},64,scratch+path_offset);
        memory.store(GuestAddress{scratch+header_offset},32,reference_magic);memory.store(GuestAddress{scratch+header_offset+4},32,class_name.size());memory.store(GuestAddress{scratch+header_offset+8},bits,scratch+class_offset);
        memory.write(GuestAddress{scratch+128},std::array<std::uint8_t,24>{'D','l','l','G','e','t','A','c','t','i','v','a','t','i','o','n','F','a','c','t','o','r','y',0});
        if(registered!=winrt_class_factories_.end()){
            memory.store(GuestAddress{scratch},32,GetFactory);memory.store(GuestAddress{scratch+80},bits,0);
            start_callbacks(state,memory,{{registered->second.first,{scratch+header_offset,scratch+80}}},Continuation::WinrtOperation,scratch);return std::nullopt;
        }
        return start_activation_module(state,memory,scratch);
    }catch(const MemoryFault&){if(scratch)discard_winrt_operation(state,memory,scratch);return done(pointer_error);}
    catch(const std::invalid_argument&){if(scratch)discard_winrt_operation(state,memory,scratch);return done(invalid);}
    catch(const std::bad_alloc&){if(scratch)discard_winrt_operation(state,memory,scratch);return done(no_memory);}
}
std::optional<std::uint32_t> WindowsServices::start_activation_module(GuestState& state,GuestMemory& memory,std::uint64_t scratch){
        // COM 與 WinRT 持有同一組 DLL lease；重入中的 activation 不得被 CoFreeUnusedLibraries 卸載。
        const auto path_pointer=memory.load(GuestAddress{scratch+48},64);
        const auto canonical=fold(read_utf16(memory,GuestAddress{path_pointer}));
        for(auto& [module,record]:runtime_.winrt_modules)if((record.path==canonical || record.aliases.contains(canonical)) && !record.unloading && module_image(module)){
            memory.store(GuestAddress{scratch},32,FindFactory);memory.store(GuestAddress{scratch+16},64,module);memory.store(GuestAddress{scratch+12},32,1);++record.active;
            start_callbacks(state,memory,{{resolve("kernel32.dll","GetProcAddress").value,{module,scratch+128}}},Continuation::WinrtOperation,scratch);
            return std::nullopt;
        }
        memory.store(GuestAddress{scratch},32,Load);
        start_callbacks(state,memory,{{resolve("kernel32.dll","LoadLibraryExW").value,{path_pointer,0,0}}},Continuation::WinrtOperation,scratch);return std::nullopt;
}
bool WindowsServices::begin_inproc_com(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool create=service.symbol=="CoCreateInstance";
    const auto done=[&](std::uint32_t value){arg.return_to(state,value);return true;};
    std::uint64_t scratch=0;
    try{
        std::array<std::uint8_t,16> clsid{};memory.read(GuestAddress{arg(0)},clsid);
        constexpr std::array order{3U,2U,1U,0U,5U,4U,7U,6U,8U,9U,10U,11U,12U,13U,14U,15U};
        constexpr std::u16string_view hex=u"0123456789abcdef";std::u16string key=u"\\clsid\\{";
        for(unsigned n=0;n<16;++n){if(n==4 || n==6 || n==8 || n==10)key+=u'-';const auto byte=clsid[order[n]];key+=hex[byte>>4];key+=hex[byte&15];}key+=u"}\\inprocserver32";
        std::u16string path,threading;std::uint32_t path_type=0;
        {
            if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return done(0x80040150U);
            // 現有 HKCR 儲存別名與 HKCU／HKLM Classes 使用相同 WOW64 轉向。
            for(const auto& root:{std::u16string(1,1)+u"\\software\\classes",std::u16string(1,0),std::u16string(1,2)+u"\\software\\classes"}){
                const auto found=runtime_.registry.keys.find(registry_view_path(root+key,registry_view(0,x86_)));
                if(found==runtime_.registry.keys.end())continue;
                const auto value=found->second.find(u"");if(value==found->second.end())return done(class_not_registered);
                path=registry_text(value->second);path_type=value->second.type;
                if(const auto model=found->second.find(u"threadingmodel");model!=found->second.end())threading=fold(registry_text(model->second));
                break;
            }
        }
        if(!path_type)return false; // 未登錄時繼續原有內建 COM 工廠。
        if(path.empty())return done(class_not_registered);
        const auto apartment=com_apartment(threads_.current);if(!apartment)return done(not_initialized);
        const auto context=static_cast<std::uint32_t>(arg(create?2:1));if(!context)return done(invalid);
        if(!(context&1U))return done(class_not_registered);
        if(!create && arg(2))return done(0x80004001U);
        // 與既有 WinRT 載入器相同：跨 apartment／Neutral 尚需 proxy，不直接在錯誤執行緒呼叫。
        if(threading!=u"both" && threading!=u"free" && threading!=u"apartment" && !threading.empty())return done(0x80004001U);
        if((threading==u"free" && *apartment) || ((threading.empty() || threading==u"apartment") && !*apartment))return done(0x80004001U);
        if(path_type==2){
            std::u16string expanded;
            for(std::size_t at=0;at<path.size();){if(path[at]!=u'%'){expanded.push_back(path[at++]);continue;}const auto end=path.find(u'%',at+1);if(end==path.npos){expanded.append(path.substr(at));break;}
                auto variable=path.substr(at+1,end-at-1);for(auto& c:variable)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
                const auto value=runtime_.environment.find(variable);expanded.append(value==runtime_.environment.end()?path.substr(at,end-at+1):value->second.second);at=end+1;}
            path=std::move(expanded);
        }
        if(path.size()>32767)return done(invalid);
        const auto bytes=utf16_bytes(path);scratch=allocate_heap(memory,operation_size+bytes.size());if(!scratch)return done(no_memory);
        memory.write(GuestAddress{scratch},std::vector<std::uint8_t>(operation_size));memory.store(GuestAddress{scratch+4},32,create?3:2);
        memory.store(GuestAddress{scratch+40},64,arg(4));memory.store(GuestAddress{scratch+48},64,scratch+operation_size);memory.store(GuestAddress{scratch+56},64,create?arg(1):0);
        memory.move(GuestAddress{scratch+64},GuestAddress{arg(3)},16);memory.write(GuestAddress{scratch+96},clsid);
        memory.write(GuestAddress{scratch+112},std::array<std::uint8_t,16>{1,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46});
        memory.write(GuestAddress{scratch+128},std::array<std::uint8_t,18>{'D','l','l','G','e','t','C','l','a','s','s','O','b','j','e','c','t',0});memory.write(GuestAddress{scratch+operation_size},bytes);
        (void)start_activation_module(state,memory,scratch);return true;
    }catch(const MemoryFault&){if(scratch)discard_winrt_operation(state,memory,scratch);return done(pointer_error);}
    catch(const std::invalid_argument&){if(scratch)discard_winrt_operation(state,memory,scratch);return done(invalid);}
    catch(const std::bad_alloc&){if(scratch)discard_winrt_operation(state,memory,scratch);return done(no_memory);}
}
std::optional<std::uint32_t> WindowsServices::resume_winrt_operation(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint64_t value){
    const auto read=[&](unsigned offset,std::uint8_t bits=64){return memory.load(GuestAddress{scratch+offset},bits);};
    const auto put=[&](unsigned offset,std::uint64_t number,std::uint8_t bits=64){memory.store(GuestAddress{scratch+offset},bits,number);};
    const std::uint8_t bits=x86_?32:64;const auto phase=static_cast<unsigned>(read(0,32));
    const auto finish=[&](std::uint32_t result){discard_winrt_operation(state,memory,scratch);complete_call(state,memory,result);return std::optional<std::uint32_t>{};};
    const auto call=[&](unsigned next,std::uint64_t target,std::vector<std::uint64_t> arguments){put(0,next,32);start_callbacks(state,memory,{{target,std::move(arguments)}},Continuation::WinrtOperation,scratch);return std::optional<std::uint32_t>{};};
    if(phase==Load){
        if(!value)return finish(read(4,32)>=2?(last_error_==126?0x800401f8U:last_error_==193?0x800401f9U:hresult(last_error_)):hresult(last_error_));put(16,value);
        auto [entry,inserted]=runtime_.winrt_modules.try_emplace(value,WinrtModule{fold(read_utf16(memory,GuestAddress{read(48)}))});
        if(!inserted)++entry->second.references;
        entry->second.aliases.insert(fold(read_utf16(memory,GuestAddress{read(48)})));
        ++entry->second.active;put(12,1,32);
        return call(FindFactory,resolve("kernel32.dll","GetProcAddress").value,{value,scratch+128});
    }
    if(phase==FindFactory){
        if(!value)return finish(read(4,32)>=2?0x800401f9U:hresult(last_error_));put(80,0,bits);
        if(read(4,32)>=2)return call(GetFactory,value,{scratch+96,scratch+(read(4,32)==3?112:64),scratch+80});
        return call(GetFactory,value,{scratch+header_offset,scratch+80});
    }
    if(phase==GetFactory){
        if(static_cast<std::uint32_t>(value)&0x80000000U)return finish(static_cast<std::uint32_t>(value));
        const auto factory=read(80,bits);if(!factory)return finish(no_interface);put(24,factory);put(88,0,bits);
        if(read(4,32)==2){memory.store(GuestAddress{read(40)},bits,factory);return finish(0);}
        if(read(4,32)==3)return call(Activate,method(memory,factory,3),{factory,read(56),scratch+64,scratch+88});
        if(read(4,32))return call(Activate,method(memory,factory,6),{factory,scratch+88});
        return call(QueryFactory,method(memory,factory,0),{factory,scratch+64,scratch+88});
    }
    if(phase==QueryFactory || phase==Activate){
        auto result=static_cast<std::uint32_t>(value);if(!(result&0x80000000U) && !read(88,bits))result=no_interface;
        put(32,result,32);return call(ReleaseFactory,method(memory,read(24),2),{read(24)});
    }
    if(phase==ReleaseFactory){const auto result=static_cast<std::uint32_t>(read(32,32));if(!(result&0x80000000U))memory.store(GuestAddress{read(40)},bits,read(88,bits));return finish(result);}
    const auto next_unload=[&]()->std::optional<std::uint32_t>{
        if(const auto current=runtime_.winrt_modules.find(read(16));current!=runtime_.winrt_modules.end())current->second.unloading=false;
        auto index=static_cast<std::uint32_t>(read(8,32));const auto count=static_cast<std::uint32_t>(read(4,32));
        while(++index<count){put(8,index,32);const auto module=read(operation_size+index*8);const auto found=runtime_.winrt_modules.find(module);
            if(found==runtime_.winrt_modules.end() || found->second.active || found->second.unloading)continue;
            found->second.unloading=true;put(16,module);return call(FindUnload,resolve("kernel32.dll","GetProcAddress").value,{module,scratch+128});
        }
        return finish(0);
    };
    if(phase==FindUnload){if(!value)return next_unload();return call(CanUnload,value,{});}
    if(phase==CanUnload){if(static_cast<std::uint32_t>(value)!=0)return next_unload();return call(FreeModule,resolve("kernel32.dll","FreeLibrary").value,{read(16)});}
    if(phase==FreeModule){
        const auto found=runtime_.winrt_modules.find(read(16));
        if(found!=runtime_.winrt_modules.end() && value){if(--found->second.references)return call(FreeModule,resolve("kernel32.dll","FreeLibrary").value,{read(16)});runtime_.winrt_modules.erase(found);}
        return next_unload();
    }
    throw HostCallFault("WinRT 啟用回呼狀態不合法");
}
std::optional<std::uint32_t> WindowsServices::release_unused_winrt_modules(GuestState& state,GuestMemory& memory){
    std::vector<std::uint64_t> modules;
    for(const auto& [module,record]:runtime_.winrt_modules)if(!record.active && !record.unloading)modules.push_back(module);
    if(modules.empty()){complete_call(state,memory,0);return std::nullopt;}
    const auto scratch=allocate_heap(memory,operation_size+modules.size()*8);
    if(!scratch){complete_call(state,memory,0);return std::nullopt;}
    memory.write(GuestAddress{scratch},std::vector<std::uint8_t>(operation_size));memory.store(GuestAddress{scratch},32,FindUnload);memory.store(GuestAddress{scratch+4},32,modules.size());memory.store(GuestAddress{scratch+16},64,modules.front());
    for(std::size_t i=0;i<modules.size();++i)memory.store(GuestAddress{scratch+operation_size+i*8},64,modules[i]);
    memory.write(GuestAddress{scratch+128},std::array<std::uint8_t,16>{'D','l','l','C','a','n','U','n','l','o','a','d','N','o','w',0});runtime_.winrt_modules.at(modules.front()).unloading=true;
    start_callbacks(state,memory,{{resolve("kernel32.dll","GetProcAddress").value,{modules.front(),scratch+128}}},Continuation::WinrtOperation,scratch);return std::nullopt;
}
}
