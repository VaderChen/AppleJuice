#include "windows_storage_state.hpp"
#include <algorithm>

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::persist_storage(GuestState& state,GuestMemory& memory,std::uint64_t view_id,std::uint64_t caller,
    std::map<std::uint64_t,compound::Document> rollback,std::uint64_t output,std::uint64_t new_object,std::uint32_t flags,std::vector<std::uint64_t> commits,std::vector<std::uint64_t> new_views,std::pair<std::uint64_t,std::uint32_t> count_output,std::optional<std::uint64_t> position){
    auto& bridge=storage_bridge();const auto& view=bridge.views.at(view_id);auto& document=bridge.documents.at(view.document);const auto& root=bridge.views.at(document.root_view);
    const bool commit=std::find(commits.begin(),commits.end(),document.root_view)!=commits.end();
    if(view_id!=document.root_view || ((root.mode&storage::transacted) && !commit)){
        try{if(output)memory.store(GuestAddress{output},x86_?32:64,new_object);if(count_output.first)memory.store(GuestAddress{count_output.first},32,count_output.second);for(auto id:commits)bridge.views.at(id).committed=bridge.views.at(id).tree;complete_call(state,memory,0);return std::nullopt;}
        catch(...){for(auto& [id,tree]:rollback)bridge.views.at(id).tree=std::move(tree);if(position)bridge.objects.at(caller).position=*position;if(new_object)(void)release_storage_object(state,memory,new_object);for(auto id:new_views)bridge.views.erase(id);throw;}
    }
    StorageState::Operation operation;operation.document=view.document;operation.view=view_id;operation.backend=document.backend;operation.caller=caller;
    operation.output=output;operation.new_object=new_object;operation.flags=flags;operation.mode=root.mode;operation.commit=commit;
    operation.rollback=std::move(rollback);operation.commit_views=std::move(commits);operation.new_views=std::move(new_views);
    operation.count_output=count_output.first;operation.count=count_output.second;operation.position_changed=position.has_value();operation.previous_position=position.value_or(0);
    if(!operation.rollback.contains(view_id))operation.rollback.emplace(view_id,view.tree);
    const auto token=bridge.next_operation++;bridge.operations.emplace(token,std::move(operation));document.busy=token;++bridge.objects.at(caller).references;
    return resume_storage(state,memory,token,0);
}
void WindowsServices::discard_storage(GuestState& state,GuestMemory& memory,std::uint64_t token){
    auto& bridge=storage_bridge();const auto found=bridge.operations.find(token);if(found==bridge.operations.end())return;
    auto operation=std::move(found->second);bridge.operations.erase(found);
    if(operation.kind==StorageState::Kind::Persist){
        if(!operation.published){for(auto& [id,tree]:operation.rollback)if(bridge.views.contains(id))bridge.views.at(id).tree=std::move(tree);if(operation.position_changed && bridge.objects.contains(operation.caller))bridge.objects.at(operation.caller).position=operation.previous_position;}
        if(bridge.documents.contains(operation.document))bridge.documents.at(operation.document).busy=0;
        if(operation.new_object && !operation.published)(void)release_storage_object(state,memory,operation.new_object);
        if(operation.caller && bridge.objects.contains(operation.caller))(void)release_storage_object(state,memory,operation.caller);
        if(!operation.published)for(auto id:operation.new_views)bridge.views.erase(id);
    }
    if(operation.scratch)(void)free_heap(memory,operation.scratch,state);if(operation.buffer)(void)free_heap(memory,operation.buffer,state);
}
std::optional<std::uint32_t> WindowsServices::resume_storage(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint32_t status){
    auto& bridge=storage_bridge();using Phase=StorageState::Phase;using Kind=StorageState::Kind;
    const auto found=bridge.operations.find(token);if(found==bridge.operations.end())throw HostCallFault("找不到複合儲存操作");auto& op=found->second;
    const auto callback=[&](Phase phase,unsigned slot,std::vector<std::uint64_t> arguments,std::uint32_t wide=0){
        const auto target=storage::method(memory,op.backend,slot);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        op.phase=phase;GuestCallback call;call.target=target;call.arguments=std::move(arguments);call.wide_arguments=wide;
        start_callbacks(state,memory,{std::move(call)},Continuation::StorageOperation,token);return std::optional<std::uint32_t>{};
    };
    const auto finish=[&](){
        auto operation=std::move(op);bridge.operations.erase(token);std::vector<GuestCallback> callbacks;
        const auto collect=[&](std::uint64_t object){auto releases=release_storage_object(state,memory,object);callbacks.insert(callbacks.end(),releases.begin(),releases.end());};
        if(operation.kind==Kind::Persist){
            auto& document=bridge.documents.at(operation.document);document.busy=0;
            if(operation.published){document.revision=bridge.views.at(operation.view).tree.transaction;
                for(auto& [id,checkpoint]:operation.checkpoints)bridge.views.at(id).committed=std::move(checkpoint);
                if(operation.output)memory.store(GuestAddress{operation.output},x86_?32:64,operation.new_object);
                if(operation.count_output)memory.store(GuestAddress{operation.count_output},32,operation.count);
            }else{for(auto& [id,tree]:operation.rollback)bridge.views.at(id).tree=std::move(tree);if(operation.position_changed)bridge.objects.at(operation.caller).position=operation.previous_position;if(operation.new_object)collect(operation.new_object);for(auto id:operation.new_views)bridge.views.erase(id);}
            collect(operation.caller);
        }else if(!(operation.status&0x80000000U) && operation.kind!=Kind::IsStorage){
            const auto document_id=bridge.next_document++,view_id=bridge.next_view++;StorageState::Document document;
            document.backend=operation.backend;document.root_view=view_id;document.revision=operation.initial.transaction;document.next_node=operation.initial.next_node;document.locks=operation.locks;
            try{StorageState::View view;view.tree=std::move(operation.initial);view.committed=view.tree;view.document=document_id;view.mode=operation.mode&~(storage::create|storage::convert);
                bridge.documents.emplace(document_id,document);bridge.views.emplace(view_id,std::move(view));const auto object=create_storage_object(memory,0,view_id,0,operation.mode);memory.store(GuestAddress{operation.output},x86_?32:64,object);operation.held=false;}
            catch(...){const auto objects=bridge.objects;for(const auto& [handle,value]:objects)if(value.view==view_id){bridge.objects.erase(handle);(void)free_heap(memory,handle,state);}bridge.views.erase(view_id);bridge.documents.erase(document_id);operation.status=storage::no_memory;}
        }
        if(operation.held)callbacks.push_back({storage::method(memory,operation.backend,2),{operation.backend}});
        if(operation.scratch)(void)free_heap(memory,operation.scratch,state);if(operation.buffer)(void)free_heap(memory,operation.buffer,state);
        const auto result=operation.status?operation.status:operation.success;
        if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::optional<std::uint32_t>{};}complete_call(state,memory,result);return std::optional<std::uint32_t>{};
    };
    const auto cleanup=[&](){if(op.locked)return callback(Phase::Unlock,8,{op.backend,0,1,2},6);return finish();};
    const auto failure=[&](std::uint32_t error){
        if(!op.status)op.status=error;
        if(op.modified && !op.published){op.offset=0;return callback(Phase::RestoreSize,6,{op.backend,op.previous.size()},2);}return cleanup();
    };
    const auto read=[&](){
        op.requested=static_cast<std::uint32_t>(std::min<std::uint64_t>(65536,op.previous.size()-op.offset));memory.store(GuestAddress{op.scratch+96},32,0);
        return callback(Phase::Read,3,{op.backend,op.offset,op.buffer,op.requested,op.scratch+96},2);
    };
    const auto write=[&](Phase phase,const std::vector<std::uint8_t>& bytes,std::size_t end){
        op.requested=static_cast<std::uint32_t>(std::min<std::uint64_t>(65536,end-op.offset));memory.write(GuestAddress{op.buffer},std::span(bytes).subspan(static_cast<std::size_t>(op.offset),op.requested));memory.store(GuestAddress{op.scratch+96},32,0);
        return callback(phase,4,{op.backend,op.offset,op.buffer,op.requested,op.scratch+96},2);
    };
    const auto prepare=[&](){
        try{
            if(op.kind==Kind::IsStorage){op.success=compound::signature(op.previous)?0U:1U;return cleanup();}
            if(op.kind==Kind::Open){if(!compound::signature(op.previous))return failure(storage::exists);op.initial=compound::parse(op.previous);
                // ILockBytes 宣告支援 locking 時，同程序的 root open 仍須遵循雙方分享模式。
                if(op.locks)for(const auto& [id,document]:bridge.documents){(void)id;if(!document.owns_backend || document.backend!=op.backend)continue;const auto existing=bridge.views.at(document.root_view).mode;
                    const auto deny=[](std::uint32_t mode,bool read,bool write){const auto sharing=mode&0x70;return sharing==0x10 || (sharing==0x20 && write) || (sharing==0x30 && read);};
                    if(deny(existing,storage::readable(op.mode),storage::writable(op.mode)) || deny(op.mode,storage::readable(existing),storage::writable(existing)))return failure(storage::lock_violation);}
                return cleanup();}
            if(op.kind==Kind::Create){if(op.mode&storage::convert){const auto node=op.initial.add(0,u"Contents",compound::Kind::Stream);op.initial.nodes.at(node).data=op.previous;op.success=0x00030200U;}op.initial.transaction=1;op.bytes=compound::serialize(op.initial);}
            else {auto& view=bridge.views.at(op.view);auto& document=bridge.documents.at(op.document);
                const auto previous_revision=[&](){if(op.previous.size()<56 || !compound::signature(op.previous))throw storage::Error{storage::invalid_header};std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=static_cast<std::uint32_t>(op.previous[52+i])<<(i*8);return value;};
                if((op.flags&2) && previous_revision()!=document.revision)return failure(storage::not_current);
                if(op.locks && !compound::signature(op.previous))return failure(storage::invalid_header);
                view.tree.transaction=document.revision+1;
                op.append=(op.mode&storage::transacted) && !(op.flags&9) && compound::signature(op.previous);
                op.bytes=op.append?compound::serialize_append(view.tree,op.previous):compound::serialize(view.tree);
                // 發布 header 之前先配置回復點；提交後只移動已配置的文件。
                for(auto id:op.commit_views)op.checkpoints.emplace(id,bridge.views.at(id).tree);
                if(!(op.mode&storage::transacted))op.checkpoints.try_emplace(op.view,view.tree);
            }
            op.modified=true;return callback(Phase::Resize,6,{op.backend,std::max(op.previous.size(),op.bytes.size())},2);
        }catch(const storage::Error& error){return failure(error.status);}catch(const compound::Error& error){return failure(storage::error(error));}catch(const std::bad_alloc&){return failure(storage::no_memory);}
    };
    const auto start_read=[&](){if(op.previous.empty())return prepare();op.offset=0;return read();};
    try{
        switch(op.phase){
        case Phase::Start:
            op.scratch=allocate_heap(memory,128);op.buffer=allocate_heap(memory,65536);if(!op.scratch || !op.buffer)return failure(storage::no_memory);
            memory.write(GuestAddress{op.scratch},std::array<std::uint8_t,128>{});
            if(op.kind!=Kind::Persist)return callback(Phase::AddRef,1,{op.backend});return callback(Phase::Stat,9,{op.backend,op.scratch,1});
        case Phase::AddRef:op.held=true;return callback(Phase::Stat,9,{op.backend,op.scratch,1});
        case Phase::Stat:
            if(status&0x80000000U)return failure(status);op.backend_size=memory.load(GuestAddress{op.scratch+(x86_?8:16)},64);op.locks=static_cast<std::uint32_t>(memory.load(GuestAddress{op.scratch+(x86_?44:52)},32));
            if(op.backend_size>compound::default_limit)return failure(storage::medium_full);op.previous.resize(static_cast<std::size_t>(op.kind==Kind::IsStorage?std::min(op.backend_size,UINT64_C(512)):op.backend_size));
            if((op.kind==Kind::Create || op.kind==Kind::Persist) && (op.locks&2))return callback(Phase::Lock,7,{op.backend,0,1,2},6);return start_read();
        case Phase::Lock:if(status&0x80000000U)return failure(status);op.locked=true;return start_read();
        case Phase::Read:
            if(status&0x80000000U)return failure(status);if(memory.load(GuestAddress{op.scratch+96},32)!=op.requested)return failure(storage::read_fault);
            memory.read(GuestAddress{op.buffer},std::span(op.previous).subspan(static_cast<std::size_t>(op.offset),op.requested));op.offset+=op.requested;
            if(op.offset<op.previous.size())return read();return prepare();
        case Phase::Resize:
            if(status&0x80000000U)return failure(status);op.offset=op.append?op.previous.size():op.bytes[26]==3?512:4096;
            if(op.offset<op.bytes.size())return write(Phase::WriteBody,op.bytes,op.bytes.size());return callback(Phase::FlushBody,5,{op.backend});
        case Phase::WriteBody:
            if(status&0x80000000U)return failure(status);if(memory.load(GuestAddress{op.scratch+96},32)!=op.requested)return failure(storage::write_fault);op.offset+=op.requested;
            if(op.offset<op.bytes.size())return write(Phase::WriteBody,op.bytes,op.bytes.size());return callback(Phase::FlushBody,5,{op.backend});
        case Phase::FlushBody:
            if(status&0x80000000U)return failure(status);op.offset=0;return write(Phase::WriteHeader,op.bytes,op.bytes[26]==3?512:4096);
        case Phase::WriteHeader:
            if(status&0x80000000U)return failure(status);if(memory.load(GuestAddress{op.scratch+96},32)!=op.requested)return failure(storage::write_fault);return callback(Phase::Flush,5,{op.backend});
        case Phase::Flush:
            if(status&0x80000000U)return failure(status);
            if(op.bytes.size()<op.previous.size())return callback(Phase::Trim,6,{op.backend,op.bytes.size()},2);op.published=true;return cleanup();
        case Phase::Trim:if(status&0x80000000U)return failure(status);op.published=true;return cleanup();
        case Phase::RestoreSize:
            if(status&0x80000000U)return cleanup();if(!op.previous.empty())return write(Phase::Restore,op.previous,op.previous.size());return callback(Phase::RestoreFlush,5,{op.backend});
        case Phase::Restore:
            if((status&0x80000000U) || memory.load(GuestAddress{op.scratch+96},32)!=op.requested)return cleanup();op.offset+=op.requested;
            if(op.offset<op.previous.size())return write(Phase::Restore,op.previous,op.previous.size());return callback(Phase::RestoreFlush,5,{op.backend});
        case Phase::RestoreFlush:return cleanup();
        case Phase::Unlock:op.locked=false;if((status&0x80000000U) && !op.status)op.status=status;return finish();
        case Phase::Release:return finish();
        }
    }catch(const MemoryFault&){return failure(storage::pointer_error);}catch(const storage::Error& error){return failure(error.status);}catch(const std::bad_alloc&){return failure(storage::no_memory);}
    throw HostCallFault("複合儲存操作階段無效");
}
}
