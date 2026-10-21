#include "windows_storage_state.hpp"
#include <algorithm>

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::begin_storage_copy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);auto& bridge=storage_bridge();const auto& source=bridge.object(arg(0));const auto& view=bridge.views.at(source.view);const bool moving=service.symbol=="MoveElementTo";
    const auto destination=arg(moving?2:4);if(!destination)throw storage::Error{storage::pointer_error};
    if(!storage::readable(source.mode) || (moving && !storage::writable(source.mode)))throw storage::Error{storage::access_denied};
    if(const auto target=bridge.objects.find(destination);target!=bridge.objects.end()){
        const auto& object=bridge.object(destination);if(object.type!=StorageState::Type::Storage)throw storage::Error{storage::pointer_error};
        if(bridge.views.at(object.view).document==view.document){const auto& target_tree=bridge.views.at(object.view).tree;for(auto parent=object.node;parent!=compound::no_node;parent=target_tree.nodes.at(parent).parent)if(parent==source.node)throw storage::Error{storage::access_denied};}
    }
    StorageState::Copy copy;copy.source=arg(0);copy.destination=destination;copy.merge=!moving;
    bool skip_storage=false,skip_stream=false;std::vector<std::u16string> exclusions;std::optional<std::uint32_t> selected;
    const auto text=[&](std::uint64_t address){return storage::name(memory,address);};
    if(moving){copy.flags=static_cast<std::uint32_t>(arg(4));if(copy.flags>2)throw storage::Error{storage::invalid_flag};copy.move=copy.flags==0;
        selected=view.tree.find(source.node,text(arg(1)));if(!selected)throw storage::Error{storage::missing};copy.move_node=*selected;
        if(std::any_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& item){const auto& object=item.second;return !object.invalidated && object.type!=StorageState::Type::Enumerator && object.node==*selected && bridge.views.at(object.view).document==view.document && bridge.valid_view(object.view) && object.generation==bridge.views.at(object.view).generation;}))throw storage::Error{storage::access_denied};
    }else{
        if(arg(2)){const auto count=static_cast<std::uint32_t>(arg(1));if(count>1024)throw storage::Error{storage::invalid_parameter};
            constexpr std::array<std::uint8_t,16> iid_storage{11,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},iid_stream{12,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            for(unsigned i=0;i<count;++i){std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(2)+i*16ULL},iid);skip_storage|=iid==iid_storage;skip_stream|=iid==iid_stream;}}
        if(arg(3) && !skip_storage){const auto unit=x86_?4U:8U;for(unsigned i=0;;++i){if(i>=compound::node_limit)throw storage::Error{storage::invalid_parameter};const auto pointer=memory.load(GuestAddress{arg(3)+i*unit},x86_?32:64);if(!pointer)break;exclusions.push_back(text(pointer));}}
    }
    StorageState::Copy::Frame root;root.node=view.tree.nodes.at(source.node);root.root=true;root.object=destination;copy.frames.push_back(std::move(root));
    if(selected){StorageState::Copy::Frame frame;frame.node=view.tree.nodes.at(*selected);frame.node.name=text(arg(3));frame.parent=0;frame.stream=frame.node.kind==compound::Kind::Stream;copy.frames.push_back(std::move(frame));}
    std::vector<std::uint32_t> source_nodes{source.node};if(selected)source_nodes.push_back(*selected);
    for(std::size_t i=selected?1:0;i<copy.frames.size();++i){
        if(copy.frames[i].stream || (moving && copy.flags==2 && i==1))continue;
        for(auto id:view.tree.children(source_nodes[i])){const auto& node=view.tree.nodes.at(id);
            if((node.kind==compound::Kind::Storage && skip_storage) || (node.kind==compound::Kind::Stream && skip_stream) || (!i && std::any_of(exclusions.begin(),exclusions.end(),[&](const auto& name){return compound::compare_name(name,node.name)==0;})))continue;
            StorageState::Copy::Frame child;child.node=node;child.parent=i;child.stream=node.kind==compound::Kind::Stream;copy.frames.push_back(std::move(child));source_nodes.push_back(id);}
    }
    const auto token=bridge.next_operation++;copy.scratch=allocate_heap(memory,256);copy.buffer=allocate_heap(memory,65536);
    if(!copy.scratch || !copy.buffer){if(copy.scratch)(void)free_heap(memory,copy.scratch,state);if(copy.buffer)(void)free_heap(memory,copy.buffer,state);throw storage::Error{storage::no_memory};}
    try{memory.write(GuestAddress{copy.scratch},std::array<std::uint8_t,256>{});bridge.copies.emplace(token,std::move(copy));++bridge.objects.at(arg(0)).references;return resume_storage_copy(state,memory,token,0);}
    catch(...){if(bridge.copies.contains(token))discard_storage_copy(state,memory,token);else {if(copy.scratch)(void)free_heap(memory,copy.scratch,state);if(copy.buffer)(void)free_heap(memory,copy.buffer,state);}throw;}
}
void WindowsServices::discard_storage_copy(GuestState& state,GuestMemory& memory,std::uint64_t token){
    auto& bridge=storage_bridge();const auto found=bridge.copies.find(token);if(found==bridge.copies.end())return;auto copy=std::move(found->second);bridge.copies.erase(found);
    // SEH 跨越客體 COM 方法時不再進入已離開的客體 frame；釋放本端操作資源。
    if(bridge.objects.contains(copy.source))(void)release_storage_object(state,memory,copy.source);
    if(copy.scratch)(void)free_heap(memory,copy.scratch,state);if(copy.buffer)(void)free_heap(memory,copy.buffer,state);
}
std::optional<std::uint32_t> WindowsServices::resume_storage_copy(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint32_t status){
    auto& bridge=storage_bridge();using Phase=StorageState::Copy::Phase;auto& copy=bridge.copies.at(token);
    const auto callback=[&](Phase phase,std::uint64_t object,unsigned slot,std::vector<std::uint64_t> arguments){
        const auto target=storage::method(memory,object,slot);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        copy.phase=phase;start_callbacks(state,memory,{{target,std::move(arguments)}},Continuation::StorageCopyOperation,token);return std::optional<std::uint32_t>{};
    };
    const auto finish=[&](){
        auto operation=std::move(copy);bridge.copies.erase(token);(void)free_heap(memory,operation.scratch,state);(void)free_heap(memory,operation.buffer,state);
        if(operation.move && !operation.status){
            try{const auto& source=bridge.object(operation.source);auto& view=bridge.views.at(source.view);if(bridge.documents.at(view.document).busy)throw storage::Error{storage::lock_violation};const auto before=view.tree;
                auto tree=view.tree;tree.erase(static_cast<std::uint32_t>(operation.move_node));view.tree=std::move(tree);const auto saved=state;
                auto result=persist_storage(state,memory,source.view,operation.source,{{source.view,before}});auto callbacks=release_storage_object(state,memory,operation.source);
                if(!callbacks.empty()){state=saved;start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);}return result;
            }catch(const storage::Error& error){operation.status=error.status;}catch(const compound::Error& error){operation.status=storage::error(error);}
        }
        auto callbacks=release_storage_object(state,memory,operation.source);
        if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,operation.status);return std::optional<std::uint32_t>{};}
        complete_call(state,memory,operation.status);return std::optional<std::uint32_t>{};
    };
    const auto cleanup=[&](){
        if(!copy.owned.empty()){const auto object=copy.owned.back();copy.owned.pop_back();return callback(Phase::ReleaseOwned,object,2,{object});}
        if(copy.held){copy.held=false;return callback(Phase::ReleaseDestination,copy.destination,2,{copy.destination});}return finish();
    };
    const auto fail=[&](std::uint32_t error){if(!copy.status)copy.status=error;return cleanup();};
    const auto create=[&](){
        auto& frame=copy.frames.at(copy.index);const auto parent=copy.frames.at(static_cast<std::size_t>(frame.parent)).object;
        memory.write(GuestAddress{copy.scratch},utf16_bytes(frame.node.name));memory.store(GuestAddress{copy.scratch+112},x86_?32:64,0);
        return callback(Phase::Create,parent,frame.stream?3:5,{parent,copy.scratch,(frame.stream && copy.merge?storage::create:0U)|0x12U,0,0,copy.scratch+112});
    };
    const auto advance=[&](){++copy.index;copy.offset=0;if(copy.index>=copy.frames.size())return cleanup();return create();};
    const auto stream_write=[&](){const auto& frame=copy.frames.at(copy.index);copy.requested=static_cast<std::uint32_t>(std::min<std::uint64_t>(65536,frame.node.data.size()-copy.offset));
        memory.write(GuestAddress{copy.buffer},std::span(frame.node.data).subspan(static_cast<std::size_t>(copy.offset),copy.requested));memory.store(GuestAddress{copy.scratch+104},32,0);
        return callback(Phase::Write,frame.object,4,{frame.object,copy.buffer,copy.requested,copy.scratch+104});};
    const auto metadata=[&](){const auto& frame=copy.frames.at(copy.index);if(frame.stream){if(!frame.node.data.empty())return stream_write();copy.owned.pop_back();return callback(Phase::ReleaseElement,frame.object,2,{frame.object});}
        memory.write(GuestAddress{copy.scratch+64},frame.node.clsid);return callback(Phase::Class,frame.object,15,{frame.object,copy.scratch+64});};
    try{
        switch(copy.phase){
        case Phase::AddRef:
            if(!copy.held){copy.held=true;return callback(Phase::Finish,copy.destination,1,{copy.destination});}return metadata();
        case Phase::Finish:copy.phase=Phase::AddRef;if(!copy.merge)return advance();return metadata();
        case Phase::Create:
            if(status==storage::exists && !copy.frames.at(copy.index).stream && copy.merge){auto& frame=copy.frames.at(copy.index);const auto parent=copy.frames.at(static_cast<std::size_t>(frame.parent)).object;return callback(Phase::Open,parent,6,{parent,copy.scratch,0,0x12,0,0,copy.scratch+112});}
            [[fallthrough]];
        case Phase::Open:
            if(status&0x80000000U)return fail(status);copy.frames.at(copy.index).object=memory.load(GuestAddress{copy.scratch+112},x86_?32:64);
            if(!copy.frames.at(copy.index).object)return fail(storage::pointer_error);copy.owned.push_back(copy.frames.at(copy.index).object);return metadata();
        case Phase::Class:
            if(status&0x80000000U)return fail(status);return callback(Phase::State,copy.frames.at(copy.index).object,16,{copy.frames.at(copy.index).object,copy.frames.at(copy.index).node.state_bits,UINT32_MAX});
        case Phase::State:{
            if(status&0x80000000U)return fail(status);const auto& frame=copy.frames.at(copy.index);memory.store(GuestAddress{copy.scratch+80},64,frame.node.created);memory.store(GuestAddress{copy.scratch+88},64,frame.node.modified);
            const auto parent=frame.root?frame.object:copy.frames.at(static_cast<std::size_t>(frame.parent)).object;
            if(!frame.root)memory.write(GuestAddress{copy.scratch},utf16_bytes(frame.node.name));
            return callback(Phase::Times,parent,14,{parent,frame.root?0:copy.scratch,copy.scratch+80,0,copy.scratch+88});}
        case Phase::Times:if(status&0x80000000U)return fail(status);return advance();
        case Phase::Write:
            if(status&0x80000000U)return fail(status);if(memory.load(GuestAddress{copy.scratch+104},32)!=copy.requested)return fail(storage::write_fault);copy.offset+=copy.requested;
            if(copy.offset<copy.frames.at(copy.index).node.data.size())return stream_write();copy.owned.pop_back();return callback(Phase::ReleaseElement,copy.frames.at(copy.index).object,2,{copy.frames.at(copy.index).object});
        case Phase::ReleaseElement:return advance();
        case Phase::ReleaseOwned:return cleanup();
        case Phase::ReleaseDestination:return finish();
        }
    }catch(const storage::Error& error){return fail(error.status);}catch(const MemoryFault&){return fail(storage::pointer_error);}catch(const std::bad_alloc&){return fail(storage::no_memory);}
    throw HostCallFault("複合儲存複製階段無效");
}
}
