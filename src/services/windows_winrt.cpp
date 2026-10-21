#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> winrt_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"combase.dll","RoGetActivationFactory",3},{"combase.dll","RoActivateInstance",2},
        {"combase.dll","RoInitialize",1},{"combase.dll","RoUninitialize",0},{"combase.dll","RoGetApartmentIdentifier",1},
        {"combase.dll","WindowsCreateString",3},{"combase.dll","WindowsCreateStringReference",4},{"combase.dll","WindowsDeleteString",1},
        {"combase.dll","WindowsDuplicateString",2},{"combase.dll","WindowsGetStringLen",1},{"combase.dll","WindowsGetStringRawBuffer",2},
        {"combase.dll","WindowsIsStringEmpty",1},{"combase.dll","WindowsStringHasEmbeddedNull",2},{"combase.dll","WindowsCompareStringOrdinal",3},
        {"combase.dll","RoRegisterActivationFactories",4},{"combase.dll","RoRevokeActivationFactories",1},
        {"combase.dll","RoRegisterForApartmentShutdown",3},{"combase.dll","RoUnregisterForApartmentShutdown",1},{"combase.dll","RoTransformError",3}
    };return table;
}
std::uint32_t WindowsServices::initialize_com_apartment(std::uint32_t mode){
    if(mode&~0xeU)return 0x80070057U;
    auto& com=gui_.com_initializations[threads_.current];
    if(com && (com&0xfU)!=(mode&2U))return 0x80010106U;
    if(com>UINT32_MAX-16)return 0x8007000eU;
    if(!com && (mode&2U) && !gui_.com_main_sta)gui_.com_main_sta=threads_.current;
    const auto result=com?1U:0U;com=(com?com:mode&2U)+16;return result;
}
std::vector<GuestCallback> WindowsServices::retire_apartment_shutdowns(GuestMemory& memory,std::uint64_t thread,std::optional<std::uint64_t> apartment){
    // apartment 結束時依登錄順序呼叫 IApartmentShutdown::OnUninitialize，再釋放參照。
    // STA 隨執行緒結束；MTA 在最後一個 MTA 執行緒離開時結束。apartment 為離開前的身分。
    std::vector<GuestCallback> callbacks;if(!apartment)return callbacks;
    if(!*apartment && std::any_of(gui_.com_initializations.begin(),gui_.com_initializations.end(),[&](const auto& row){return row.first!=thread && row.second>=16 && !(row.second&2U);}))return callbacks;
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto method=[&](std::uint64_t object,unsigned slot){const auto table=memory.load(GuestAddress{object},bits);return memory.load(GuestAddress{table+std::uint64_t{slot}*unit},bits);};
    for(auto it=apartment_shutdowns_.begin();it!=apartment_shutdowns_.end();){
        if(it->second.apartment!=*apartment){++it;continue;}
        GuestCallback notify{method(it->second.object,3),{it->second.object,it->second.identifier}};notify.wide_arguments=2;callbacks.push_back(std::move(notify));
        callbacks.push_back({method(it->second.object,2),{it->second.object}});it=apartment_shutdowns_.erase(it);
    }
    return callbacks;
}
std::optional<std::uint32_t> WindowsServices::invoke_winrt(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="RoGetActivationFactory" || service.symbol=="RoActivateInstance")return invoke_winrt_activation(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const std::uint8_t bits=x86_?32:64;
    constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_memory=0x8007000eU,reference_magic=0x52534858U;
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(name=="RoInitialize"){const auto mode=static_cast<std::uint32_t>(arg(0));return done(mode>1?invalid:initialize_com_apartment(mode?0U:2U));}
    if(name=="RoUninitialize")return invoke_gui_support(state,memory,{"ole32.dll","CoUninitialize",0});
    if(name=="RoRegisterActivationFactories" || name=="RoRevokeActivationFactories" || name=="RoRegisterForApartmentShutdown" || name=="RoUnregisterForApartmentShutdown" || name=="RoTransformError"){
        const unsigned unit=x86_?4:8;
        const auto method=[&](std::uint64_t object,unsigned slot){const auto table=memory.load(GuestAddress{object},bits);return memory.load(GuestAddress{table+std::uint64_t{slot}*unit},bits);};
        try{
            if(name=="RoTransformError"){
                // 錯誤碼沒有改變或新值為成功時不回報；否則與 RoOriginateError 共用 restricted error 流程。
                const auto old_error=static_cast<std::uint32_t>(arg(0)),new_error=static_cast<std::uint32_t>(arg(1));
                if(old_error==new_error || !(new_error&0x80000000U))return done(0);
                start_callbacks(state,memory,{{resolve("combase.dll","RoOriginateError").value,{new_error,arg(2)}}},Continuation::Return,1);return std::nullopt;
            }
            if(name=="RoRegisterActivationFactories"){
                // 本程序的 activatable class 工廠；之後 RoGetActivationFactory／RoActivateInstance 優先使用。
                const auto count=static_cast<std::uint32_t>(arg(2));
                if(!arg(0) || !arg(1) || !arg(3) || !count || count>4096)return done(invalid);
                memory.check(GuestAddress{arg(3)},unit,Access::Write);memory.store(GuestAddress{arg(3)},bits,0);
                if(!com_apartment(threads_.current))return done(0x800401f0U);
                std::vector<std::pair<std::u16string,std::uint64_t>> entries;
                for(std::uint32_t i=0;i<count;++i){
                    const auto id=winrt_string_content(memory,memory.load(GuestAddress{arg(0)+i*unit},bits));const auto callback=memory.load(GuestAddress{arg(1)+i*unit},bits);
                    if(id.empty() || !callback)return done(invalid);
                    auto key=id;for(auto& c:key)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
                    if(winrt_class_factories_.contains(key) || std::any_of(entries.begin(),entries.end(),[&](const auto& e){return e.first==key;}))return done(0x800401fcU); // CO_E_OBJISREG
                    entries.emplace_back(std::move(key),callback);
                }
                const auto cookie=runtime_.allocate_wait_handle();std::vector<std::u16string> keys;
                for(auto& [key,callback]:entries){winrt_class_factories_[key]={callback,cookie};keys.push_back(key);}
                winrt_factory_cookies_.emplace(cookie,std::move(keys));memory.store(GuestAddress{arg(3)},bits,cookie);return done(0);
            }
            if(name=="RoRevokeActivationFactories"){
                if(const auto found=winrt_factory_cookies_.find(arg(0));found!=winrt_factory_cookies_.end()){for(const auto& key:found->second)winrt_class_factories_.erase(key);winrt_factory_cookies_.erase(found);}
                return done(0);
            }
            if(name=="RoRegisterForApartmentShutdown"){
                if(!arg(0) || !arg(1) || !arg(2))return done(invalid);
                memory.check(GuestAddress{arg(1)},8,Access::Write);memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.store(GuestAddress{arg(2)},bits,0);
                const auto apartment=com_apartment(threads_.current);if(!apartment)return done(0x800401f0U);
                const auto identifier=(static_cast<std::uint64_t>(process_id())<<32)|*apartment;
                const auto cookie=runtime_.allocate_wait_handle();apartment_shutdowns_.emplace(cookie,ApartmentShutdown{arg(0),*apartment,identifier});
                memory.store(GuestAddress{arg(1)},64,identifier);memory.store(GuestAddress{arg(2)},bits,cookie);
                start_callbacks(state,memory,{{method(arg(0),1),{arg(0)}}},Continuation::Return,0);return std::nullopt; // 持有 callback 參照。
            }
            const auto found=apartment_shutdowns_.find(arg(0));if(found==apartment_shutdowns_.end())return done(invalid);
            const auto object=found->second.object;apartment_shutdowns_.erase(found);
            start_callbacks(state,memory,{{method(object,2),{object}}},Continuation::Return,0);return std::nullopt;
        }catch(const MemoryFault&){return done(pointer_error);}catch(const std::bad_alloc&){return done(no_memory);}
    }
    const auto create=[&](std::uint64_t source,std::uint32_t length)->std::uint64_t{
        if(!length)return 0;
        const auto size=std::uint64_t{length}*2;memory.check(GuestAddress{source},static_cast<std::size_t>(size),Access::Read);
        if(size+26>memory.limit())throw std::bad_alloc();
        const auto handle=allocate_heap(memory,size+26);if(!handle)throw std::bad_alloc();
        try{memory.move(GuestAddress{handle+24},GuestAddress{source},static_cast<std::size_t>(size));memory.store(GuestAddress{handle+24+size},16,0);
            runtime_.winrt_strings.emplace(handle,WinrtString{handle+24,length,1});}
        catch(...){(void)free_heap(memory,handle,state);throw;}
        return handle;
    };
    const bool scalar=name=="WindowsGetStringLen" || name=="WindowsGetStringRawBuffer" || name=="WindowsIsStringEmpty";
    try{
        if(name=="RoGetApartmentIdentifier"){
            if(!arg(0))return done(pointer_error);const auto apartment=com_apartment(threads_.current);if(!apartment)return done(0x800401f0U);
            memory.store(GuestAddress{arg(0)},64,(static_cast<std::uint64_t>(process_id())<<32)|*apartment);return done(0);
        }
        if(name=="WindowsCreateString" || name=="WindowsCreateStringReference"){
            const bool reference=name.ends_with("Reference");const auto length=static_cast<std::uint32_t>(arg(1));const auto output=arg(reference?3:2);
            if(!output || (reference && !arg(2)))return done(invalid);
            memory.store(GuestAddress{output},bits,0);if(!arg(0) && length)return done(pointer_error);
            if(!length)return done(0);
            if(std::uint64_t{length}*2>memory.limit())return done(no_memory);
            if(reference){
                memory.check(GuestAddress{arg(2)},24,Access::Write);memory.check(GuestAddress{arg(0)},std::uint64_t{length}*2+2,Access::Read);
                if(memory.load(GuestAddress{arg(0)+std::uint64_t{length}*2},16))return done(invalid);
                memory.write(GuestAddress{arg(2)},std::array<std::uint8_t,24>{});memory.store(GuestAddress{arg(2)},32,reference_magic);
                memory.store(GuestAddress{arg(2)+4},32,length);memory.store(GuestAddress{arg(2)+8},bits,arg(0));memory.store(GuestAddress{output},bits,arg(2));
            }else {const auto handle=create(arg(0),length);memory.store(GuestAddress{output},bits,handle);}return done(0);
        }
        const auto value=winrt_string(memory,arg(0));
        if(name=="WindowsGetStringLen")return done(value.length);
        if(name=="WindowsIsStringEmpty")return done(value.length?0:1);
        if(name=="WindowsGetStringRawBuffer"){
            if(arg(1))memory.store(GuestAddress{arg(1)},32,value.length);
            if(value.length)return done(value.data);
            if(!runtime_.winrt_empty_string){runtime_.winrt_empty_string=allocate_heap(memory,2);if(!runtime_.winrt_empty_string)throw std::bad_alloc();memory.store(GuestAddress{runtime_.winrt_empty_string},16,0);}
            return done(runtime_.winrt_empty_string);
        }
        if(name=="WindowsDeleteString"){
            if(const auto found=runtime_.winrt_strings.find(arg(0));found!=runtime_.winrt_strings.end()){
                if(found->second.references>1)--found->second.references;
                else {const auto handle=found->first;runtime_.winrt_strings.erase(found);(void)free_heap(memory,handle,state);}
            }return done(0); // fast-pass 來源及 header 都由呼叫者持有。
        }
        if(name=="WindowsDuplicateString"){
            if(!arg(1))return done(invalid);memory.store(GuestAddress{arg(1)},bits,0);
            if(!value.length)return done(0);
            const auto owned=runtime_.winrt_strings.find(arg(0));std::uint64_t handle=arg(0);
            if(owned!=runtime_.winrt_strings.end()){if(owned->second.references==UINT32_MAX)return done(no_memory);++owned->second.references;}
            else handle=create(value.data,value.length);
            memory.store(GuestAddress{arg(1)},bits,handle);return done(0);
        }
        if(name=="WindowsStringHasEmbeddedNull"){
            if(!arg(1))return done(invalid);const auto data=winrt_content(memory,value);
            memory.store(GuestAddress{arg(1)},32,data.find(u'\0')!=data.npos);return done(0);
        }
        if(name=="WindowsCompareStringOrdinal"){
            if(!arg(2))return done(invalid);const auto a=winrt_content(memory,value),b=winrt_content(memory,winrt_string(memory,arg(1)));
            memory.store(GuestAddress{arg(2)},32,a<b?UINT32_MAX:a>b?1:0);return done(0);
        }
    }catch(const MemoryFault&){if(scalar)throw;return done(pointer_error);}catch(const std::bad_alloc&){if(scalar)throw;return done(no_memory);}
    throw HostCallFault("未知的 WinRT 字串／apartment 服務");
}
}
