#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_error_support.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_memory=0x8007000eU,no_interface=0x80004002U;
constexpr WindowsGuid unknown_iid{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr WindowsGuid error_iid{0x20,0xb1,0xf2,0x1c,0x7d,0x54,0x1b,0x10,0x8e,0x65,8,0,0x2b,0x2b,0xd1,0x19};
constexpr WindowsGuid create_iid{0x40,0x33,0xf0,0x22,0x7d,0x54,0x1b,0x10,0x8e,0x65,8,0,0x2b,0x2b,0xd1,0x19};
constexpr WindowsGuid support_iid{0x60,0x3d,0x0b,0xdf,0x8f,0x54,0x1b,0x10,0x8e,0x65,8,0,0x2b,0x2b,0xd1,0x19};
using winrt_error_detail::method;
}
std::span<const WindowsServiceDescriptor> com_error_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"oleaut32.dll","CreateErrorInfo",1},{"oleaut32.dll","GetErrorInfo",2},{"oleaut32.dll","SetErrorInfo",2},
        {"ole32.dll","CreateErrorInfo",1},{"ole32.dll","GetErrorInfo",2},{"ole32.dll","SetErrorInfo",2},
        {"combase.dll","CreateErrorInfo",1},{"combase.dll","GetErrorInfo",2},{"combase.dll","SetErrorInfo",2},
        {"internal-com-error-info","QueryInterface",3},{"internal-com-error-info","AddRef",1},{"internal-com-error-info","Release",1},
        {"internal-com-error-info","GetGUID",2},{"internal-com-error-info","GetSource",2},{"internal-com-error-info","GetDescription",2},
        {"internal-com-error-info","GetHelpFile",2},{"internal-com-error-info","GetHelpContext",2},
        {"internal-com-error-create","QueryInterface",3},{"internal-com-error-create","AddRef",1},{"internal-com-error-create","Release",1},
        {"internal-com-error-create","SetGUID",2},{"internal-com-error-create","SetSource",2},{"internal-com-error-create","SetDescription",2},
        {"internal-com-error-create","SetHelpFile",2},{"internal-com-error-create","SetHelpContext",2},
        {"internal-com-error-support","QueryInterface",3},{"internal-com-error-support","AddRef",1},{"internal-com-error-support","Release",1},
        {"internal-com-error-support","InterfaceSupportsErrorInfo",2},
        {"internal-com-error","CleanupThread",0}
    };return table;
}
std::uint64_t WindowsServices::create_com_error(GuestState& state,GuestMemory& memory,com_error::Object value){
    auto& bridge=runtime_.com_error_bridge;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(runtime_.com_errors.object_count()>=4096)throw std::bad_alloc();
    const auto table=[&](std::string_view family,std::uint64_t& slot,unsigned count){
        if(slot)return;
        constexpr unsigned arena_limit=1024U*1024U;
        if(runtime_.arena_next>arena_limit || count*unit>arena_limit-runtime_.arena_next)throw std::bad_alloc();
        const auto pointer=runtime_allocate(memory,{},count*unit);unsigned index=0;
        for(const auto& row:com_error_service_descriptors())if(row.family==family)
            memory.store(GuestAddress{pointer+unit*index++},bits,resolve(row.family,row.symbol).value);
        slot=pointer;
    };
    table("internal-com-error-info",bridge.info_table,8);table("internal-com-error-create",bridge.create_table,8);table("internal-com-error-support",bridge.support_table,4);
    const bool restricted=value.restricted;
    if(restricted && !bridge.restricted_table){
        constexpr unsigned arena_limit=1024U*1024U;
        if(runtime_.arena_next>arena_limit || 5*unit>arena_limit-runtime_.arena_next)throw std::bad_alloc();
        const auto target=runtime_allocate(memory,{},5*unit);unsigned index=0;
        for(const auto& row:winrt_error_service_descriptors())if(row.family=="internal-restricted-error")
            memory.store(GuestAddress{target+unit*index++},bits,resolve(row.family,row.symbol).value);
        bridge.restricted_table=target;
    }
    const auto object=allocate_heap(memory,(restricted?4:3)*unit);if(!object)throw std::bad_alloc();
    try{
        memory.store(GuestAddress{object},bits,bridge.info_table);memory.store(GuestAddress{object+unit},bits,bridge.create_table);
        memory.store(GuestAddress{object+2*unit},bits,bridge.support_table);
        bridge.views.emplace(object,ComErrorView{object,ComErrorViewKind::Info});
        bridge.views.emplace(object+unit,ComErrorView{object,ComErrorViewKind::Create});
        bridge.views.emplace(object+2*unit,ComErrorView{object,ComErrorViewKind::Support});
        if(restricted){memory.store(GuestAddress{object+3*unit},bits,bridge.restricted_table);bridge.views.emplace(object+3*unit,ComErrorView{object,ComErrorViewKind::Restricted});}
        if(runtime_.com_errors.create(object,std::move(value))!=com_error::Status::Ok)throw std::bad_alloc();
    }catch(...){for(unsigned i=0;i<4;++i)bridge.views.erase(object+i*unit);(void)free_heap(memory,object,state);throw;}
    return object;
}
std::uint32_t WindowsServices::release_com_error(GuestState& state,GuestMemory& memory,std::uint64_t pointer){
    auto& bridge=runtime_.com_error_bridge;const auto view=bridge.views.at(pointer);const auto result=runtime_.com_errors.release(view.object);
    if(result.destroyed){for(unsigned i=0;i<4;++i)bridge.views.erase(view.object+i*(x86_?4:8));(void)free_heap(memory,view.object,state);}
    return result.count;
}
std::vector<GuestCallback> WindowsServices::retire_com_error(GuestState& state,GuestMemory& memory,std::uint64_t thread,bool callbacks){
    const auto object=runtime_.com_errors.take(thread);if(!object)return {};
    if(runtime_.com_error_bridge.views.contains(object)){(void)release_com_error(state,memory,object);return {};}
    if(!callbacks)return {}; // 強制結束與離開 callback frame 後不能重新執行客體 Release。
    const auto target=method(memory,object,2);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
    return {{target,{object}}};
}
void WindowsServices::discard_com_error_set(std::uint64_t id){runtime_.com_error_bridge.operations.erase(id);}
std::optional<std::uint32_t> WindowsServices::resume_com_error_set(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& bridge=runtime_.com_error_bridge;const auto operation=bridge.operations.at(id);bridge.operations.erase(id);
    std::uint64_t previous{};std::uint32_t status{};
    try{previous=runtime_.com_errors.replace(operation.thread,operation.object);}
    catch(const std::bad_alloc&){previous=operation.object;status=no_memory;}
    if(previous){
        const auto target=method(memory,previous,2);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        start_callbacks(state,memory,{{target,{previous}}},Continuation::Return,status);
    }else complete_call(state,memory,status);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_com_error_retire(GuestState& state,GuestMemory& memory){
    auto callbacks=retire_com_error(state,memory,threads_.current);
    // 正常 thread detach 的 Release 可以重入 SetErrorInfo；直到 slot 空了才回到 ThreadExit。
    if(!callbacks.empty())start_callbacks(state,memory,std::move(callbacks),Continuation::ComErrorRetire);
    else complete_call(state,memory,0);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_com_error(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;auto& bridge=runtime_.com_error_bridge;
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto pointer=[&](std::uint64_t address,std::size_t size){if(!address)throw HostCallFault("COM 錯誤輸出指標為空");memory.check(GuestAddress{address},size,Access::Write);};
    try{
        if(name=="CleanupThread")return resume_com_error_retire(state,memory);
        if(name=="CreateErrorInfo"){
            pointer(arg(0),unit);memory.store(GuestAddress{arg(0)},bits,0);
            const auto object=create_com_error(state,memory,{});memory.store(GuestAddress{arg(0)},bits,object+unit);return done(0);
        }
        if(name=="GetErrorInfo"){
            if(static_cast<std::uint32_t>(arg(0)))return done(invalid);
            pointer(arg(1),unit);const auto object=runtime_.com_errors.take(threads_.current);
            memory.store(GuestAddress{arg(1)},bits,object);return done(object?0:1); // 轉移 channel 已持有的引用。
        }
        if(name=="SetErrorInfo"){
            if(static_cast<std::uint32_t>(arg(0)))return done(invalid);
            const auto object=arg(1);
            if(const auto view=bridge.views.find(object);view!=bridge.views.end() && runtime_.com_errors.find(view->second.object)->references==UINT32_MAX)return done(no_memory);
            if(object)for(const auto slot:{1U,2U}){const auto target=method(memory,object,slot);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);}
            if(bridge.operations.size()>=4096)return done(no_memory);
            auto id=bridge.next_operation;while(!id || bridge.operations.contains(id))++id;
            bridge.operations.emplace(id,ComErrorOperation{threads_.current,object});bridge.next_operation=id+1;
            if(!object)return resume_com_error_set(state,memory,id);
            try{start_callbacks(state,memory,{{method(memory,object,1),{object}}},Continuation::ComErrorSet,id);}
            catch(...){discard_com_error_set(id);throw;}return std::nullopt;
        }
        const auto found=bridge.views.find(arg(0));if(found==bridge.views.end())return done(invalid);
        const auto view=found->second;auto& value=*runtime_.com_errors.find(view.object);
        if(name=="AddRef")return done(runtime_.com_errors.add_ref(view.object).count);
        if(name=="Release")return done(release_com_error(state,memory,arg(0)));
        if(name=="QueryInterface"){
            pointer(arg(2),unit);WindowsGuid iid{};if(arg(1))memory.read(GuestAddress{arg(1)},iid);
            memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(pointer_error);
            const auto result=iid==unknown_iid || iid==error_iid?view.object:
                value.restricted?(iid==winrt_error_detail::restricted_iid?view.object+3*unit:0):
                iid==create_iid?view.object+unit:iid==support_iid?view.object+2*unit:0;
            if(!result)return done(no_interface);
            if(runtime_.com_errors.add_ref(view.object).status!=com_error::Status::Ok)return done(no_memory);
            memory.store(GuestAddress{arg(2)},bits,result);return done(0);
        }
        if(view.kind==ComErrorViewKind::Support){
            if(name!="InterfaceSupportsErrorInfo")throw HostCallFault("未知的 ISupportErrorInfo 方法");
            if(!arg(1))return done(pointer_error);WindowsGuid iid{};memory.read(GuestAddress{arg(1)},iid);
            return done(iid==value.guid?0:1);
        }
        if(view.kind==ComErrorViewKind::Create){
            if(name=="SetGUID"){if(!arg(1))return done(pointer_error);WindowsGuid guid{};memory.read(GuestAddress{arg(1)},guid);value.guid=guid;return done(0);}
            if(name=="SetHelpContext"){value.help_context=static_cast<std::uint32_t>(arg(1));return done(0);}
            auto text=arg(1)?read_utf16(memory,GuestAddress{arg(1)},static_cast<std::size_t>(std::min<std::uint64_t>(UINT32_MAX/2,memory.limit()/2))):std::u16string{};
            if(name=="SetSource"){value.source=std::move(text);value.source_present=bool(arg(1));}
            else if(name=="SetDescription"){value.description=std::move(text);value.description_present=bool(arg(1));}
            else if(name=="SetHelpFile"){value.help_file=std::move(text);value.help_file_present=bool(arg(1));}
            else throw HostCallFault("未知的 ICreateErrorInfo 方法");return done(0);
        }
        if(name=="GetGUID"){pointer(arg(1),16);memory.write(GuestAddress{arg(1)},value.guid);return done(0);}
        if(name=="GetHelpContext"){pointer(arg(1),4);memory.store(GuestAddress{arg(1)},32,value.help_context);return done(0);}
        pointer(arg(1),unit);memory.store(GuestAddress{arg(1)},bits,0);
        const auto* text=name=="GetSource"?&value.source:name=="GetDescription"?&value.description:name=="GetHelpFile"?&value.help_file:nullptr;
        if(!text)throw HostCallFault("未知的 IErrorInfo 方法");
        const auto present=name=="GetSource"?value.source_present:name=="GetDescription"?value.description_present:value.help_file_present;
        if(!present)return done(0);
        if(text->size()>UINT32_MAX/2)return done(no_memory);
        std::vector<std::uint8_t> bytes(text->size()*2);for(std::size_t i=0;i<text->size();++i){bytes[2*i]=static_cast<std::uint8_t>((*text)[i]);bytes[2*i+1]=static_cast<std::uint8_t>((*text)[i]>>8);}
        const auto result=allocate_bstr(memory,bytes.size(),bytes);if(!result)return done(no_memory);
        memory.store(GuestAddress{arg(1)},bits,result);return done(0);
    }catch(const MemoryFault&){return done(pointer_error);}catch(const std::bad_alloc&){return done(no_memory);}catch(const HostCallFault&){return done(pointer_error);}
}
}
