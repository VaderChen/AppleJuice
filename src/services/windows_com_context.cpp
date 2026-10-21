#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/com_proxy_schema.hpp"
#include "x64arm/file_security.hpp"
#include "windows_sddl.hpp"
#include "registry_text_view.hpp"
#include "shared_rot.hpp"
#include "shared_com_mailbox.hpp"
#include <openssl/rand.h>
#include <algorithm>
#include <limits>

namespace x64arm {
namespace {
constexpr std::uint32_t context_invalid=0x80070057U,context_pointer_error=0x80004003U,context_no_memory=0x8007000eU;
constexpr std::uint32_t context_no_interface=0x80004002U,context_failed=0x80004005U,context_unexpected=0x8000ffffU;
WindowsGuid context_iid(std::uint32_t value){
    WindowsGuid id{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
    for(unsigned i=0;i<4;++i)id[i]=static_cast<std::uint8_t>(value>>(i*8));return id;
}
std::uint64_t context_method(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    const std::uint8_t bits=memory.execution_mode()==ExecutionMode::X86?32:64;
    const auto table=memory.load(GuestAddress{object},bits);
    if(!object || !table || slot>(UINT64_MAX-table)/(bits/8))throw HostCallFault("context IUnknown vtable 不合法");
    const auto target=memory.load(GuestAddress{table+slot*(bits/8)},bits);
    if(!target)throw HostCallFault("context IUnknown 方法為空");return target;
}
std::uint32_t context_status(com_context::Status status){
    switch(status){
    case com_context::Status::Ok:return 0;
    case com_context::Status::Limit:return context_no_memory;
    case com_context::Status::Frozen:return context_unexpected;
    case com_context::Status::Missing:return context_failed;
    default:return context_invalid;
    }
}
}
std::span<const WindowsServiceDescriptor> com_context_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","CoGetContextToken",1},{"combase.dll","CoGetContextToken",1},{"ole32.dll","CoGetObjectContext",2},{"combase.dll","CoGetObjectContext",2},
        {"ole32.dll","CoGetApartmentType",2},{"combase.dll","CoGetApartmentType",2},
        {"ole32.dll","CreateBindCtx",2},
        {"internal-com-rpc","Retire",1},{"internal-com-rpc","FinishRetire",1},
        {"ole32.dll","GetRunningObjectTable",2},{"combase.dll","GetRunningObjectTable",2},
        {"internal-com-rot","QueryInterface",3},{"internal-com-rot","AddRef",1},{"internal-com-rot","Release",1},
        {"internal-com-rot","Register",5},{"internal-com-rot","Revoke",2},{"internal-com-rot","IsRunning",2},
        {"internal-com-rot","GetObject",3},{"internal-com-rot","NoteChangeTime",3},{"internal-com-rot","GetTimeOfLastChange",3},{"internal-com-rot","EnumRunning",2},
        {"internal-com-rot-enum","QueryInterface",3},{"internal-com-rot-enum","AddRef",1},{"internal-com-rot-enum","Release",1},
        {"internal-com-rot-enum","Next",4},{"internal-com-rot-enum","Skip",2},{"internal-com-rot-enum","Reset",1},{"internal-com-rot-enum","Clone",2},
        {"internal-com-bindctx","QueryInterface",3},{"internal-com-bindctx","AddRef",1},{"internal-com-bindctx","Release",1},
        {"internal-com-bindctx","RegisterObjectBound",2},{"internal-com-bindctx","RevokeObjectBound",2},{"internal-com-bindctx","ReleaseBoundObjects",1},
        {"internal-com-bindctx","SetBindOptions",2},{"internal-com-bindctx","GetBindOptions",2},{"internal-com-bindctx","GetRunningObjectTable",2},
        {"internal-com-bindctx","RegisterObjectParam",3},{"internal-com-bindctx","GetObjectParam",3},{"internal-com-bindctx","EnumObjectParam",2},{"internal-com-bindctx","RevokeObjectParam",2},
        {"internal-com-bind-enum","QueryInterface",3},{"internal-com-bind-enum","AddRef",1},{"internal-com-bind-enum","Release",1},
        {"internal-com-bind-enum","Next",4},{"internal-com-bind-enum","Skip",2},{"internal-com-bind-enum","Reset",1},{"internal-com-bind-enum","Clone",2},
        {"internal-com-context","QueryInterface",3},{"internal-com-context","AddRef",1},{"internal-com-context","Release",1},
        {"internal-com-context","SetProperty",4},{"internal-com-context","RemoveProperty",2},
        {"internal-com-context","GetProperty",4},{"internal-com-context","EnumContextProps",2},
        {"internal-com-context-enum","QueryInterface",3},{"internal-com-context-enum","AddRef",1},{"internal-com-context-enum","Release",1},
        {"internal-com-context-enum","Next",4},{"internal-com-context-enum","Skip",2},{"internal-com-context-enum","Reset",1},
        {"internal-com-context-enum","Clone",2},{"internal-com-context-enum","Count",2}
    };return table;
}
std::vector<GuestCallback> WindowsServices::release_context_properties(GuestMemory& memory,std::span<const com_context::Property> properties){
    auto& bridge=runtime_.com_contexts;std::vector<GuestCallback> result;result.reserve(properties.size());
    for(const auto& property:properties){
        if(bridge.borrowed.contains(property.object)){++bridge.deferred_releases[property.object];continue;}
        const auto target=context_method(memory,property.object,2);
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        result.push_back({target,{property.object}});
    }return result;
}
std::vector<GuestCallback> WindowsServices::release_com_context(GuestState& state,GuestMemory& memory,std::uint64_t token,bool callbacks){
    auto result=runtime_.com_contexts.contexts.release(token);
    if(!result.disposal)return {};
    (void)free_heap(memory,token,state);
    return callbacks?release_context_properties(memory,result.disposal->properties):std::vector<GuestCallback>{};
}
std::vector<GuestCallback> WindowsServices::retire_com_context(GuestState& state,GuestMemory& memory,std::uint64_t thread,bool callbacks){
    const auto initialized=gui_.com_initializations.find(thread);if(initialized==gui_.com_initializations.end())return {};
    const auto owner=(initialized->second&2U)?thread:0;
    if(!owner && std::any_of(gui_.com_initializations.begin(),gui_.com_initializations.end(),
        [&](const auto& row){return row.first!=thread && row.second>=16 && !(row.second&2U);}))return {};
    auto result=runtime_.com_contexts.contexts.retire(owner);if(!result.disposal)return {};
    (void)free_heap(memory,result.disposal->token,state);
    return callbacks?release_context_properties(memory,result.disposal->properties):std::vector<GuestCallback>{};
}
std::vector<GuestCallback> WindowsServices::release_context_enumerator(GuestState& state,GuestMemory& memory,std::uint64_t object){
    auto& enums=runtime_.com_contexts.enumerators;const auto found=enums.find(object);if(found==enums.end())return {};
    if(--found->second.references)return {};
    auto properties=found->second.snapshot.properties();enums.erase(found);(void)free_heap(memory,object,state);
    return release_context_properties(memory,properties);
}
std::uint64_t WindowsServices::create_context_enumerator(GuestState& state,GuestMemory& memory,com_context::Enumeration snapshot){
    auto& bridge=runtime_.com_contexts;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(bridge.enumerators.size()>=4096)throw std::bad_alloc();
    if(!bridge.enum_table){
        const auto table=runtime_allocate(memory,{},8*unit);unsigned i=0;
        for(const auto& row:com_context_service_descriptors())if(row.family=="internal-com-context-enum")
            memory.store(GuestAddress{table+unit*i++},bits,resolve(row.family,row.symbol).value);
        bridge.enum_table=table;
    }
    const auto object=allocate_heap(memory,unit);if(!object)throw std::bad_alloc();
    try{memory.store(GuestAddress{object},bits,bridge.enum_table);bridge.enumerators.emplace(object,ComContextEnumerator{std::move(snapshot)});}
    catch(...){(void)free_heap(memory,object,state);throw;}return object;
}
std::uint64_t WindowsServices::begin_context_operation(ComContextOperation operation){
    auto& bridge=runtime_.com_contexts;
    if(bridge.operations.size()>=4096)throw std::bad_alloc();
    // 配置失敗不能留下半份 pin；pin 保住來源已持有的引用，並不替代客體 AddRef。
    auto borrowed=bridge.borrowed;
    for(const auto& property:operation.properties)++borrowed[property.object];
    if(operation.context){const auto context=bridge.contexts.find(operation.context);if(!context || context->references==UINT32_MAX)throw std::bad_alloc();}
    if(operation.enumerator && bridge.enumerators.at(operation.enumerator).references==UINT32_MAX)throw std::bad_alloc();
    auto id=bridge.next_operation;while(!id || bridge.operations.contains(id))++id;
    const auto inserted=bridge.operations.emplace(id,std::move(operation)).first;
    bridge.next_operation=id+1;bridge.borrowed=std::move(borrowed);
    if(inserted->second.context)(void)bridge.contexts.add_ref(inserted->second.context);
    if(inserted->second.enumerator)++bridge.enumerators.at(inserted->second.enumerator).references;
    return id;
}
void WindowsServices::discard_com_context(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& bridge=runtime_.com_contexts;const auto found=bridge.operations.find(id);if(found==bridge.operations.end())return;
    auto operation=std::move(found->second);bridge.operations.erase(found);
    for(const auto& property:operation.properties){
        const auto pin=bridge.borrowed.find(property.object);
        if(pin!=bridge.borrowed.end() && !--pin->second)bridge.borrowed.erase(pin);
    }
    if(operation.result_object){bridge.enumerators.erase(operation.result_object);(void)free_heap(memory,operation.result_object,state);}
    if(operation.context)(void)release_com_context(state,memory,operation.context,false);
    if(operation.enumerator){auto& row=bridge.enumerators.at(operation.enumerator);if(!--row.references){bridge.enumerators.erase(operation.enumerator);(void)free_heap(memory,operation.enumerator,state);}}
    // 強制終止／非 COM 契約內的 callback 拋出例外時，不再次呼叫已離開的客體 frame。
}
std::optional<std::uint32_t> WindowsServices::resume_com_context(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& bridge=runtime_.com_contexts;auto& pending=bridge.operations.at(id);
    if(pending.awaiting_reference){++pending.acquired;pending.awaiting_reference=false;}
    if(pending.acquired<pending.properties.size()){
        const auto object=pending.properties[pending.acquired].object,target=context_method(memory,object,1);
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        pending.awaiting_reference=true;
        try{start_callbacks(state,memory,{{target,{object}}},Continuation::ComContextOperation,id);}
        catch(...){discard_com_context(state,memory,id);throw;}return std::nullopt;
    }
    auto operation=std::move(pending);bridge.operations.erase(id);
    std::uint32_t status=0;bool transferred=false;std::vector<GuestCallback> callbacks;
    const auto append=[&](std::vector<GuestCallback> list){callbacks.insert(callbacks.end(),list.begin(),list.end());};
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    try{
        if(operation.kind==ComContextOperationKind::Set){
            const auto changed=bridge.contexts.set(operation.context,operation.property);status=context_status(changed.status);
            transferred=!status;
            if(!status && changed.previous)append(release_context_properties(memory,std::span{&*changed.previous,1}));
        }else if(operation.kind==ComContextOperationKind::Get){
            memory.check(GuestAddress{operation.output},bits/8,Access::Write);
            memory.check(GuestAddress{operation.flags_output},4,Access::Write);
            memory.store(GuestAddress{operation.flags_output},32,operation.property.flags);
            memory.store(GuestAddress{operation.output},bits,operation.property.object);
        }else if(operation.kind==ComContextOperationKind::Next){
            const auto size=x86_?24U:32U;
            memory.check(GuestAddress{operation.output},operation.properties.size()*size,Access::Write);
            if(operation.fetched_output)memory.check(GuestAddress{operation.fetched_output},4,Access::Write);
            for(std::size_t i=0;i<operation.properties.size();++i){const auto encoded=com_context::encode_property(operation.properties[i],x86_);memory.write(GuestAddress{operation.output+i*size},std::span{encoded.bytes}.first(encoded.size));}
            if(operation.fetched_output)memory.store(GuestAddress{operation.fetched_output},32,operation.properties.size());
            status=operation.properties.size()==operation.requested?0:1;
        }else memory.store(GuestAddress{operation.output},bits,operation.result_object);
    }catch(const MemoryFault&){status=context_pointer_error;}
    catch(const std::bad_alloc&){status=context_no_memory;}
    if(status&0x80000000U){
        if(!transferred)append(release_context_properties(memory,operation.properties));
        if(operation.result_object){bridge.enumerators.erase(operation.result_object);(void)free_heap(memory,operation.result_object,state);}
    }
    // 全部 AddRef 已完成；之後才交付 source 被重入移除時延後的 Release。
    for(const auto& property:operation.properties){auto pin=bridge.borrowed.find(property.object);if(!--pin->second)bridge.borrowed.erase(pin);}
    for(auto it=bridge.deferred_releases.begin();it!=bridge.deferred_releases.end();){
        if(bridge.borrowed.contains(it->first)){++it;continue;}
        const com_context::Property property{{},0,it->first};
        for(std::size_t i=0;i<it->second;++i)append(release_context_properties(memory,std::span{&property,1}));
        it=bridge.deferred_releases.erase(it);
    }
    if(operation.context)append(release_com_context(state,memory,operation.context));
    if(operation.enumerator)append(release_context_enumerator(state,memory,operation.enumerator));
    if(!callbacks.empty())start_callbacks(state,memory,std::move(callbacks),Continuation::Return,status);
    else complete_call(state,memory,status);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_com_context(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.family=="internal-com-rpc")return invoke_com_rpc(state,memory,service);
    if((service.symbol=="GetRunningObjectTable" && (service.family=="ole32.dll" || service.family=="combase.dll")) || service.family.starts_with("internal-com-rot"))return invoke_com_rot(state,memory,service);
    if(service.symbol=="CreateBindCtx" || service.family.starts_with("internal-com-bind"))return invoke_com_bind(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;auto& bridge=runtime_.com_contexts;
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto with_callbacks=[&](std::vector<GuestCallback> callbacks,std::uint32_t status){
        if(callbacks.empty())return done(status);start_callbacks(state,memory,std::move(callbacks),Continuation::Return,status);return std::optional<std::uint32_t>{};
    };
    const auto pointer=[&](std::uint64_t address,std::size_t size){if(!address)throw HostCallFault("context 輸出指標為空");memory.check(GuestAddress{address},size,Access::Write);};
    const auto begin=[&](ComContextOperation operation){
        for(const auto& property:operation.properties)for(const auto slot:{1U,2U}){const auto target=context_method(memory,property.object,slot);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);}
        const auto id=begin_context_operation(std::move(operation));return resume_com_context(state,memory,id);
    };
    try{
        if(name=="CoGetApartmentType"){
            if(!arg(0) || !arg(1))return done(context_invalid);
            memory.check(GuestAddress{arg(0)},4,Access::Write);memory.check(GuestAddress{arg(1)},4,Access::Write);
            const auto initialized=gui_.com_initializations.find(threads_.current);
            std::uint32_t type=UINT32_MAX,qualifier=0;
            if(initialized!=gui_.com_initializations.end() && initialized->second>=16)
                type=(initialized->second&2U)?(gui_.com_main_sta==threads_.current?3U:0U):1U;
            else if(std::any_of(gui_.com_initializations.begin(),gui_.com_initializations.end(),[](const auto& row){return row.second>=16 && !(row.second&2U);})){
                type=1;qualifier=1; // 尚未明確初始化的執行緒可加入現存 implicit MTA。
            }
            if(type==UINT32_MAX)return done(0x800401f0U);
            memory.store(GuestAddress{arg(0)},32,type);memory.store(GuestAddress{arg(1)},32,qualifier);return done(0);
        }
        // CoGetContextToken 與 CoGetObjectContext 共用：取得（必要時建立）目前 apartment 的預設 context 物件。
        std::uint32_t failure=0;
        const auto current_context=[&]()->std::optional<std::uint64_t>{
            auto apartment=com_apartment(threads_.current);
            if(!apartment && std::any_of(gui_.com_initializations.begin(),gui_.com_initializations.end(),[](const auto& row){return row.second>=16 && !(row.second&2U);}))apartment=0;
            if(!apartment){failure=0x800401f0U;return std::nullopt;}
            auto token=bridge.contexts.current(*apartment);
            if(!token){
                if(!bridge.table){
                    // SDK 公開 IContext 七個方法；IObjContext 的七個 reserved 槽不提供未公開行為。
                    const auto table=runtime_allocate(memory,{},14*unit);unsigned i=0;
                    memory.write(GuestAddress{table},std::vector<std::uint8_t>(14*unit));
                    for(const auto& row:com_context_service_descriptors())if(row.family=="internal-com-context")memory.store(GuestAddress{table+unit*i++},bits,resolve(row.family,row.symbol).value);
                    bridge.table=table;
                }
                const auto object=allocate_heap(memory,unit);if(!object){failure=context_no_memory;return std::nullopt;}
                try{memory.store(GuestAddress{object},bits,bridge.table);const auto status=bridge.contexts.create(*apartment,object);if(status!=com_context::Status::Ok){(void)free_heap(memory,object,state);failure=context_status(status);return std::nullopt;}}
                catch(...){(void)free_heap(memory,object,state);throw;}token=object;
            }
            return token;
        };
        if(name=="CoGetContextToken"){
            pointer(arg(0),unit);const auto token=current_context();if(!token)return done(failure);
            memory.store(GuestAddress{arg(0)},bits,*token);return done(0); // 借用，沒有 AddRef。
        }
        if(name=="CoGetObjectContext"){
            // 依 QueryInterface 規則回傳新增參照：IUnknown、IContext、IObjContext。
            pointer(arg(1),unit);memory.store(GuestAddress{arg(1)},bits,0);if(!arg(0))return done(context_invalid);
            WindowsGuid iid{};memory.read(GuestAddress{arg(0)},iid);
            const auto token=current_context();if(!token)return done(failure);
            if(iid!=context_iid(0) && iid!=context_iid(0x1c0) && iid!=context_iid(0x1c6))return done(context_no_interface);
            if(bridge.contexts.add_ref(*token).status!=com_context::Status::Ok)return done(context_no_memory);
            memory.store(GuestAddress{arg(1)},bits,*token);return done(0);
        }
        const auto object=arg(0);const bool enumerator=service.family=="internal-com-context-enum";
        if(enumerator?!bridge.enumerators.contains(object):!bridge.contexts.find(object))return done(context_invalid);
        if(name=="AddRef"){
            if(enumerator){auto& refs=bridge.enumerators.at(object).references;return done(refs==UINT32_MAX?refs:++refs);}
            const auto result=bridge.contexts.add_ref(object);const auto context=bridge.contexts.find(object);return done(result.count-(context->active?1U:0U));
        }
        if(name=="Release"){
            if(enumerator){const auto count=bridge.enumerators.at(object).references-1;return with_callbacks(release_context_enumerator(state,memory,object),count);}
            const auto context=bridge.contexts.find(object);if(context->active && context->references==1)return done(0);
            const auto count=context->references-1-(context->active?1U:0U);return with_callbacks(release_com_context(state,memory,object),count);
        }
        if(name=="QueryInterface"){
            pointer(arg(2),unit);if(!arg(1)){memory.store(GuestAddress{arg(2)},bits,0);return done(context_pointer_error);}
            WindowsGuid iid{};
            try{memory.read(GuestAddress{arg(1)},iid);}catch(...){memory.store(GuestAddress{arg(2)},bits,0);throw;}
            memory.store(GuestAddress{arg(2)},bits,0); // 先擷取 IID，容許輸入與輸出緩衝重疊。
            if(iid!=context_iid(0) && (enumerator?iid!=context_iid(0x1c1):iid!=context_iid(0x1c0) && iid!=context_iid(0x1c6)))return done(context_no_interface);
            if(enumerator){if(bridge.enumerators.at(object).references==UINT32_MAX)return done(context_no_memory);++bridge.enumerators.at(object).references;}
            else if(bridge.contexts.add_ref(object).status!=com_context::Status::Ok)return done(context_no_memory);
            memory.store(GuestAddress{arg(2)},bits,object);return done(0);
        }
        if(enumerator){
            auto& entry=bridge.enumerators.at(object);auto& snapshot=entry.snapshot;
            if(name=="Count"){pointer(arg(1),4);memory.store(GuestAddress{arg(1)},32,snapshot.properties().size());return done(0);}
            if(name=="Reset"){snapshot.reset();return done(0);}
            if(name=="Skip"){const auto count=static_cast<std::uint32_t>(arg(1));return done(snapshot.skip(count)==count?0:1);}
            if(name=="Clone"){
                pointer(arg(1),unit);memory.store(GuestAddress{arg(1)},bits,0);
                ComContextOperation operation;operation.kind=ComContextOperationKind::Clone;operation.enumerator=object;operation.output=arg(1);operation.properties=snapshot.properties();
                operation.result_object=create_context_enumerator(state,memory,snapshot);const auto result=operation.result_object;
                try{return begin(std::move(operation));}catch(...){bridge.enumerators.erase(result);(void)free_heap(memory,result,state);throw;}
            }
            const auto count=static_cast<std::uint32_t>(arg(1)),size=x86_?24U:32U;
            if(!arg(3) && count>1)return done(context_pointer_error);
            pointer(arg(2),static_cast<std::size_t>(count)*size);if(arg(3)){pointer(arg(3),4);memory.store(GuestAddress{arg(3)},32,0);}
            auto next=snapshot;const auto rows=next.next(count);
            ComContextOperation operation;operation.kind=ComContextOperationKind::Next;operation.enumerator=object;operation.requested=count;operation.output=arg(2);operation.fetched_output=arg(3);operation.properties.assign(rows.begin(),rows.end());
            for(const auto& property:operation.properties)for(const auto slot:{1U,2U}){const auto target=context_method(memory,property.object,slot);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);}
            const auto id=begin_context_operation(std::move(operation));snapshot=std::move(next);return resume_com_context(state,memory,id);
        }
        if(name=="EnumContextProps"){
            pointer(arg(1),unit);memory.store(GuestAddress{arg(1)},bits,0);auto snapshot=bridge.contexts.enumerate(object);
            ComContextOperation operation;operation.kind=ComContextOperationKind::Enumerate;operation.context=object;operation.output=arg(1);operation.properties=snapshot->properties();
            operation.result_object=create_context_enumerator(state,memory,std::move(*snapshot));const auto result=operation.result_object;
            try{return begin(std::move(operation));}catch(...){bridge.enumerators.erase(result);(void)free_heap(memory,result,state);throw;}
        }
        if(!arg(1))return done(context_invalid);WindowsGuid id{};memory.read(GuestAddress{arg(1)},id);
        if(name=="RemoveProperty"){
            const auto changed=bridge.contexts.remove(object,id);const auto status=context_status(changed.status);
            return changed.previous?with_callbacks(release_context_properties(memory,std::span{&*changed.previous,1}),status):done(status);
        }
        ComContextOperation operation;operation.context=object;
        if(name=="SetProperty"){
            if(static_cast<std::uint32_t>(arg(2)) || !arg(3))return done(context_invalid);
            operation.kind=ComContextOperationKind::Set;operation.property={id,0,arg(3)};operation.properties.push_back(operation.property);
        }else{
            pointer(arg(2),4);pointer(arg(3),unit);memory.store(GuestAddress{arg(2)},32,0);memory.store(GuestAddress{arg(3)},bits,0);
            const auto property=bridge.contexts.get(object,id);if(!property)return done(context_failed);
            operation.kind=ComContextOperationKind::Get;operation.output=arg(3);operation.flags_output=arg(2);operation.property=*property;operation.properties.push_back(*property);
        }return begin(std::move(operation));
    }catch(const MemoryFault&){return done(context_pointer_error);}
    catch(const HostCallFault&){return done(context_pointer_error);}
    catch(const std::bad_alloc&){return done(context_no_memory);}
}
#include "com_bind_handlers.inc"
#include "com_rot_handlers.inc"
#include "com_rpc_handlers.inc"
#include "com_remote_marshal_handlers.inc"
}
