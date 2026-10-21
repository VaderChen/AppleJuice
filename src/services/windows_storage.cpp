#include "windows_storage_state.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> storage_service_descriptors(){
    static constexpr WindowsServiceDescriptor descriptors[]{
        {"ole32.dll","StgCreateDocfileOnILockBytes",4},{"ole32.dll","StgOpenStorageOnILockBytes",6},{"ole32.dll","StgIsStorageILockBytes",1},
        {"internal-storage","QueryInterface",3},{"internal-storage","AddRef",1},{"internal-storage","Release",1},
        {"internal-storage","CreateStream",6},{"internal-storage","OpenStream",6},{"internal-storage","CreateStorage",6},{"internal-storage","OpenStorage",7},
        {"internal-storage","CopyTo",5},{"internal-storage","MoveElementTo",5},{"internal-storage","Commit",2},{"internal-storage","Revert",1},
        {"internal-storage","EnumElements",5},{"internal-storage","DestroyElement",2},{"internal-storage","RenameElement",3},
        {"internal-storage","SetElementTimes",5},{"internal-storage","SetClass",2},{"internal-storage","SetStateBits",3},{"internal-storage","Stat",3},
        {"internal-storage-stream","QueryInterface",3},{"internal-storage-stream","AddRef",1},{"internal-storage-stream","Release",1},
        {"internal-storage-stream","Read",4},{"internal-storage-stream","Write",4},{"internal-storage-stream","Seek",4,2},{"internal-storage-stream","SetSize",2,2},
        {"internal-storage-stream","CopyTo",5,4},{"internal-storage-stream","Commit",2},{"internal-storage-stream","Revert",1},
        {"internal-storage-stream","LockRegion",4,6},{"internal-storage-stream","UnlockRegion",4,6},{"internal-storage-stream","Stat",3},{"internal-storage-stream","Clone",2},
        {"internal-storage-enum","QueryInterface",3},{"internal-storage-enum","AddRef",1},{"internal-storage-enum","Release",1},
        {"internal-storage-enum","Next",4},{"internal-storage-enum","Skip",2},{"internal-storage-enum","Reset",1},{"internal-storage-enum","Clone",2}
    };return descriptors;
}
StorageState& WindowsServices::storage_bridge(){if(!storage_)storage_=std::make_shared<StorageState>();else if(storage_.use_count()>1)storage_=std::make_shared<StorageState>(*storage_);return *storage_;}
bool WindowsServices::same_storage(const WindowsServices& other)const{return !storage_ || !other.storage_?storage_==other.storage_:*storage_==*other.storage_;}
std::uint64_t WindowsServices::create_storage_object(GuestMemory& memory,unsigned type,std::uint64_t view,std::uint32_t node,std::uint32_t mode){
    auto& bridge=storage_bridge();if(type>=bridge.tables.size() || bridge.objects.size()>=65536)throw storage::Error{storage::no_memory};
    const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);auto& table=bridge.tables[type];
    if(!table){constexpr std::array families{"internal-storage","internal-storage-stream","internal-storage-enum"};std::vector<WindowsServiceDescriptor> methods;
        for(const auto& entry:storage_service_descriptors())if(entry.family==families[type])methods.push_back(entry);
        const auto address=runtime_allocate(memory,{},methods.size()*unit);for(std::size_t i=0;i<methods.size();++i)memory.store(GuestAddress{address+i*unit},bits,resolve(methods[i].family,methods[i].symbol).value);table=address;}
    const auto handle=allocate_heap(memory,unit);if(!handle)throw storage::Error{storage::no_memory};
    try{memory.store(GuestAddress{handle},bits,table);StorageState::Object object;object.type=static_cast<StorageState::Type>(type);object.view=view;object.generation=bridge.views.at(view).generation;object.node=node;object.mode=mode&~(storage::create|storage::convert);bridge.objects.emplace(handle,std::move(object));}
    catch(...){GuestState unused;(void)free_heap(memory,handle,unused);throw;}return handle;
}
std::vector<GuestCallback> WindowsServices::release_storage_object(GuestState& state,GuestMemory& memory,std::uint64_t handle){
    auto& bridge=storage_bridge();const auto found=bridge.objects.find(handle);if(found==bridge.objects.end())throw storage::Error{storage::pointer_error};
    auto& object=found->second;if(--object.references)return {};
    const auto document=bridge.views.at(object.view).document;auto& record=bridge.documents.at(document);std::vector<GuestCallback> callbacks;
    if(object.type==StorageState::Type::Storage){std::vector<std::uint64_t> parents{handle};for(std::size_t i=0;i<parents.size();++i)for(auto& [child,value]:bridge.objects)if(child!=handle && value.parent==parents[i] && bridge.views.at(value.view).document==document && !value.invalidated){value.invalidated=true;parents.push_back(child);}
        if(!object.parent && record.owns_backend){callbacks.push_back({storage::method(memory,record.backend,2),{record.backend}});record.owns_backend=false;}}
    bridge.objects.erase(found);(void)free_heap(memory,handle,state);
    if(std::any_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& entry){return bridge.views.at(entry.second.view).document==document;}))return callbacks;
    if(record.owns_backend)callbacks.push_back({storage::method(memory,record.backend,2),{record.backend}});bridge.documents.erase(document);
    std::erase_if(bridge.views,[&](const auto& entry){return entry.second.document==document;});return callbacks;
}
void WindowsServices::storage_stat(GuestState& state,GuestMemory& memory,std::uint64_t address,std::uint32_t flags,const compound::Node& node,std::uint32_t mode,bool root){
    if(!address)throw storage::Error{storage::pointer_error};if(flags>2)throw storage::Error{storage::invalid_flag};
    const auto unit=x86_?4U:8U,extent=x86_?72U:80U,base=x86_?8U:16U;const auto bits=static_cast<std::uint8_t>(unit*8);
    memory.check(GuestAddress{address},extent,Access::Write);std::uint64_t name=0;
    if(!root && !(flags&1)){const auto bytes=utf16_bytes(node.name);name=allocate_heap(memory,bytes.size());if(!name)throw storage::Error{storage::no_memory};try{memory.write(GuestAddress{name},bytes);}catch(...){(void)free_heap(memory,name,state);throw;}}
    try{memory.write(GuestAddress{address},std::vector<std::uint8_t>(extent));memory.store(GuestAddress{address},bits,name);
        memory.store(GuestAddress{address+unit},32,node.kind==compound::Kind::Stream?2:1);memory.store(GuestAddress{address+base},64,node.data.size());
        if(node.kind!=compound::Kind::Stream){memory.store(GuestAddress{address+base+8},64,node.modified);memory.store(GuestAddress{address+base+16},64,root?0:node.created);memory.write(GuestAddress{address+base+40},node.clsid);}
        memory.store(GuestAddress{address+base+32},32,mode);memory.store(GuestAddress{address+base+56},32,node.state_bits);
    }catch(...){if(name)(void)free_heap(memory,name,state);throw;}
}
std::optional<std::uint32_t> WindowsServices::invoke_storage(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto done=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    try{auto& bridge=storage_bridge();
        if(service.family=="ole32.dll"){
            const bool creating=name=="StgCreateDocfileOnILockBytes",checking=name=="StgIsStorageILockBytes";const auto output=checking?0:arg(creating?3:5);
            if(!checking && !output)return done(storage::pointer_error);if(output)memory.store(GuestAddress{output},bits,0);
            if(!arg(0))return done(storage::pointer_error);if(!checking){if(arg(creating?2:4) || (!creating && (arg(1) || arg(3))))return done(storage::invalid_parameter);storage::mode(d(creating?1:2),true,false,creating);}
            if(creating && !(d(1)&(storage::create|storage::convert)))return done(storage::exists);
            if(bridge.operations.size()>=4096)return done(storage::no_memory);
            StorageState::Operation operation;operation.kind=checking?StorageState::Kind::IsStorage:creating?StorageState::Kind::Create:StorageState::Kind::Open;
            operation.backend=arg(0);operation.output=output;operation.mode=checking?0:d(creating?1:2);
            const auto token=bridge.next_operation++;bridge.operations.emplace(token,std::move(operation));return resume_storage(state,memory,token,0);
        }
        const auto raw=bridge.objects.find(arg(0));if(raw==bridge.objects.end())return done(storage::pointer_error);
        constexpr std::array families{"internal-storage","internal-storage-stream","internal-storage-enum"};
        if(service.family!=families[static_cast<unsigned>(raw->second.type)])return done(storage::pointer_error);
        if(name=="Release"){
            const auto references=raw->second.references-1;auto callbacks=release_storage_object(state,memory,arg(0));
            if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,references);return std::nullopt;}return done(references);
        }
        auto& object=bridge.object(arg(0));
        if(name=="AddRef")return done(++object.references);
        if(name=="QueryInterface"){
            if(!arg(2))return done(0x80004003U);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(0x80004003U);std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);
            constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            auto expected=unknown;expected[0]=object.type==StorageState::Type::Storage?11:object.type==StorageState::Type::Stream?12:13;
            if(iid!=unknown && iid!=expected)return done(0x80004002U);
            ++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return done(0);
        }
        if(object.type==StorageState::Type::Stream)return invoke_storage_stream(state,memory,service);
        if(object.type==StorageState::Type::Enumerator)return invoke_storage_enumerator(state,memory,service);
        auto& view=bridge.views.at(object.view);auto& document=bridge.documents.at(view.document);
        if(name=="Stat"){storage_stat(state,memory,arg(1),d(2),view.tree.nodes.at(object.node),object.mode,object.node==0);return done(0);}
        if(name=="CopyTo" || name=="MoveElementTo")return begin_storage_copy(state,memory,service);
        if(name=="EnumElements"){
            if(!arg(4))return done(storage::pointer_error);memory.store(GuestAddress{arg(4)},bits,0);if(arg(1) || arg(2) || arg(3))return done(storage::invalid_parameter);
            const auto handle=create_storage_object(memory,2,object.view,object.node,0);bridge.objects.at(handle).parent=arg(0);auto& enumeration=bridge.objects.at(handle).enumeration;
            try{for(auto child:view.tree.children(object.node))enumeration.push_back(view.tree.nodes.at(child));memory.store(GuestAddress{arg(4)},bits,handle);}
            catch(...){(void)release_storage_object(state,memory,handle);throw;}return done(0);
        }
        if(document.busy)return done(storage::lock_violation);
        if(name=="Revert"){
            if(!(object.mode&storage::transacted))return done(0);
            auto restored=view.committed;restored.next_node=document.next_node;view.tree=std::move(restored);++view.generation;object.generation=view.generation;return done(0);
        }
        if(name=="Commit"){
            if(d(1)&~15U)return done(storage::invalid_flag);if((d(1)&8) && (object.node || !(object.mode&storage::transacted)))return done(storage::invalid_flag);
            if(!storage::writable(object.mode))return done(storage::access_denied);
            if(object.node!=view.root || !(object.mode&storage::transacted)){if(object.node || view.parent)return done(0);return persist_storage(state,memory,object.view,arg(0),{},0,0,d(1));}
            if(!view.parent)return persist_storage(state,memory,object.view,arg(0),{},0,0,d(1),{object.view});
            auto& parent=bridge.views.at(view.parent);const auto before=parent.tree;auto merged=parent.tree;
            const auto parent_id=merged.nodes.at(view.root).parent;std::vector<std::uint32_t> descendants{view.root};
            for(std::size_t i=0;i<descendants.size();++i){const auto children=view.tree.children(descendants[i]);descendants.insert(descendants.end(),children.begin(),children.end());}
            merged.erase(view.root);for(auto id:descendants)merged.nodes.emplace(id,view.tree.nodes.at(id));merged.nodes.at(view.root).parent=parent_id;merged.next_node=document.next_node;merged.validate();parent.tree=std::move(merged);
            return persist_storage(state,memory,view.parent,arg(0),{{view.parent,before}},0,0,d(1),{object.view});
        }
        if(!storage::writable(object.mode) && name!="OpenStream" && name!="OpenStorage")return done(storage::access_denied);
        const auto before=view.tree;
        const auto changed=[&](std::uint64_t output=0,std::uint64_t child=0,std::vector<std::uint64_t> views=std::vector<std::uint64_t>{}){return persist_storage(state,memory,object.view,arg(0),{{object.view,before}},output,child,0,{},std::move(views));};
        const auto element_name=[&](std::uint64_t address){return storage::name(memory,address);};
        if(name=="CreateStream" || name=="OpenStream" || name=="CreateStorage" || name=="OpenStorage"){
            const bool stream=name=="CreateStream" || name=="OpenStream",creating=name=="CreateStream" || name=="CreateStorage";
            const auto output=arg(name=="OpenStorage"?6:5);if(!output)return done(storage::pointer_error);memory.store(GuestAddress{output},bits,0);
            const auto text=element_name(arg(1));const auto mode=d(name=="OpenStream"?3:name=="OpenStorage"?3:2);
            if((name=="OpenStorage" && (arg(2) || arg(4) || arg(5))) || (name=="OpenStream" && (arg(2) || arg(4))) || (creating && (arg(3) || arg(4))))return done(storage::invalid_parameter);
            storage::mode(mode,false,stream,creating);if(!storage::child_access(object.mode,mode))return done(storage::access_denied);
            auto existing=view.tree.find(object.node,text);const auto type=stream?compound::Kind::Stream:compound::Kind::Storage;
            if(existing && !creating && view.tree.nodes.at(*existing).kind!=type)return done(storage::missing);
            if(existing && std::any_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& pair){const auto& value=pair.second;return !value.invalidated && value.type!=StorageState::Type::Enumerator && value.node==*existing && bridge.views.at(value.view).document==view.document && bridge.valid_view(value.view) && value.generation==bridge.views.at(value.view).generation;}))return done(storage::access_denied);
            if(creating && existing && !(mode&storage::create))return done(storage::exists);if(!creating && !existing)return done(storage::missing);
            auto tree=view.tree;
            if(creating){if(existing)tree.erase(*existing);tree.next_node=document.next_node;existing=tree.add(object.node,text,type);document.next_node=tree.next_node;}
            auto target_view=object.view;std::vector<std::uint64_t> created_views;
            if(!stream && (mode&storage::transacted)){target_view=bridge.next_view++;StorageState::View child;child.tree=tree;child.committed=tree;child.document=view.document;child.parent=object.view;child.parent_generation=view.generation;child.root=*existing;child.mode=mode&~storage::create;bridge.views.emplace(target_view,std::move(child));created_views.push_back(target_view);}
            std::uint64_t handle=0;
            try{handle=create_storage_object(memory,stream?1:0,target_view,*existing,mode);bridge.objects.at(handle).parent=arg(0);if(creating){view.tree=std::move(tree);return changed(output,handle,std::move(created_views));}memory.store(GuestAddress{output},bits,handle);return done(0);}
            catch(...){if(handle)(void)release_storage_object(state,memory,handle);for(auto id:created_views)bridge.views.erase(id);view.tree=before;throw;}
        }
        auto tree=view.tree;
        if(name=="SetClass"){if(!arg(1))return done(storage::pointer_error);memory.read(GuestAddress{arg(1)},tree.nodes.at(object.node).clsid);}
        else if(name=="SetStateBits"){auto& value=tree.nodes.at(object.node).state_bits;value=(value&~d(2))|(d(1)&d(2));}
        else if(name=="DestroyElement" || name=="RenameElement"){
            const auto child=tree.find(object.node,element_name(arg(1)));if(!child)return done(storage::missing);
            if(name=="DestroyElement")tree.erase(*child);else tree.rename(*child,element_name(arg(2)));
        }else if(name=="SetElementTimes"){
            const auto child=arg(1)?tree.find(object.node,element_name(arg(1))):std::optional<std::uint32_t>{object.node};if(!child)return done(storage::missing);
            std::array<std::uint64_t,3> times{};for(unsigned i=0;i<3;++i)if(arg(2+i))times[i]=memory.load(GuestAddress{arg(2+i)},64);
            auto& node=tree.nodes.at(*child);if(node.kind!=compound::Kind::Stream){if(arg(2) && *child)node.created=times[0];if(arg(4))node.modified=times[2];}
        }else throw HostCallFault("未分類的 IStorage 方法");
        view.tree=std::move(tree);return changed();
    }catch(const storage::Error& error){return done(error.status);}catch(const compound::Error& error){return done(storage::error(error));}
    catch(const MemoryFault&){return done(storage::pointer_error);}catch(const std::bad_alloc&){return done(storage::no_memory);}
}
}
