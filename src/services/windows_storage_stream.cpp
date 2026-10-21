#include "windows_storage_state.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::invoke_storage_stream(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto done=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    auto& bridge=storage_bridge();auto& object=bridge.object(arg(0));auto& view=bridge.views.at(object.view);auto& document=bridge.documents.at(view.document);auto& node=view.tree.nodes.at(object.node);
    if(name=="Read" || name=="Write"){
        const auto count=static_cast<std::uint32_t>(arg(2));const auto output=arg(3);if(output)memory.store(GuestAddress{output},32,0);if(!arg(1))return done(storage::pointer_error);
        if(name=="Read"){
            if(!storage::readable(object.mode))return done(storage::access_denied);const auto actual=object.position<node.data.size()?std::min<std::uint64_t>(count,node.data.size()-object.position):0;
            if(actual)memory.write(GuestAddress{arg(1)},std::span(node.data).subspan(static_cast<std::size_t>(object.position),static_cast<std::size_t>(actual)));
            object.position+=actual;if(output)memory.store(GuestAddress{output},32,actual);return done(0); // COM CFB 在 EOF 保留 S_OK，數量由 pcbRead 回覆。
        }
        if(!storage::writable(object.mode))return done(storage::access_denied);if(document.busy)return done(storage::lock_violation);
        if(!count)return done(0);if(object.position>compound::default_limit || count>compound::default_limit-object.position)return done(storage::medium_full);
        std::vector<std::uint8_t> data(count);memory.read(GuestAddress{arg(1)},data);const auto before=view.tree;auto tree=view.tree;auto& bytes=tree.nodes.at(object.node).data;
        bytes.resize(static_cast<std::size_t>(std::max<std::uint64_t>(bytes.size(),object.position+count)));std::copy(data.begin(),data.end(),bytes.begin()+static_cast<std::ptrdiff_t>(object.position));
        const auto position=object.position;view.tree=std::move(tree);object.position+=count;
        return persist_storage(state,memory,object.view,arg(0),{{object.view,before}},0,0,0,{},{},{output,count},position);
    }
    if(name=="Seek"){
        const auto origin=static_cast<std::uint32_t>(arg(2));if(origin>2)return done(storage::invalid_function);
        const auto relative=std::bit_cast<std::int64_t>(arg(1));const auto base=origin==0?0:origin==1?object.position:node.data.size();std::uint64_t result;
        if(relative<0){const auto amount=UINT64_C(0)-static_cast<std::uint64_t>(relative);if(amount>base)return done(storage::invalid_function);result=base-amount;}
        else{if(static_cast<std::uint64_t>(relative)>INT64_MAX-base)return done(storage::invalid_function);result=base+static_cast<std::uint64_t>(relative);}
        if(arg(3))memory.store(GuestAddress{arg(3)},64,result);object.position=result;return done(0);
    }
    if(name=="SetSize"){
        if(!storage::writable(object.mode))return done(storage::access_denied);if(document.busy)return done(storage::lock_violation);
        if(arg(1)>compound::default_limit)return done(storage::medium_full);const auto before=view.tree;auto tree=view.tree;tree.nodes.at(object.node).data.resize(static_cast<std::size_t>(arg(1)));view.tree=std::move(tree);
        return persist_storage(state,memory,object.view,arg(0),{{object.view,before}});
    }
    if(name=="CopyTo"){
        const auto output_read=arg(3),output_written=arg(4);if(output_read)memory.store(GuestAddress{output_read},64,0);if(output_written)memory.store(GuestAddress{output_written},64,0);
        if(!storage::readable(object.mode))return done(storage::access_denied);if(!arg(1))return done(storage::pointer_error);
        const auto count=object.position<node.data.size()?std::min<std::uint64_t>(arg(2),node.data.size()-object.position):0;
        const auto target=storage::method(memory,arg(1),4);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
        const auto scratch=allocate_heap(memory,24+count);if(!scratch)return done(storage::no_memory);
        try{memory.write(GuestAddress{scratch},std::array<std::uint8_t,24>{});memory.store(GuestAddress{scratch},64,count);memory.store(GuestAddress{scratch+8},64,output_written);
            if(count)memory.write(GuestAddress{scratch+24},std::span(node.data).subspan(static_cast<std::size_t>(object.position),static_cast<std::size_t>(count)));
            if(output_read)memory.store(GuestAddress{output_read},64,count);object.position+=count;
            start_callbacks(state,memory,{{target,{arg(1),scratch+24,count,scratch+16}}},Continuation::OleStreamCopy,scratch);
        }catch(...){(void)free_heap(memory,scratch,state);throw;}return std::nullopt;
    }
    if(name=="Commit"){if(static_cast<std::uint32_t>(arg(1))&~7U)return done(storage::invalid_flag);return done(0);} // CFB 串流只有 direct 模式，已反映到父儲存。
    if(name=="Revert")return done(0);
    if(name=="LockRegion" || name=="UnlockRegion")return done(storage::invalid_function); // COM CFB 的 IStream 不提供區域鎖。
    if(name=="Stat"){storage_stat(state,memory,arg(1),static_cast<std::uint32_t>(arg(2)),node,object.mode,false);return done(0);}
    if(name=="Clone"){
        if(!arg(1))return done(storage::pointer_error);memory.store(GuestAddress{arg(1)},x86_?32:64,0);const auto clone=create_storage_object(memory,1,object.view,object.node,object.mode);
        bridge.objects.at(clone).position=object.position;bridge.objects.at(clone).parent=object.parent;memory.store(GuestAddress{arg(1)},x86_?32:64,clone);return done(0);
    }
    throw HostCallFault("未分類的 CFB IStream 方法");
}
std::optional<std::uint32_t> WindowsServices::invoke_storage_enumerator(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;auto& bridge=storage_bridge();auto& object=bridge.object(arg(0));
    const auto done=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    if(name=="Reset"){object.position=0;return done(0);}
    if(name=="Skip"){const auto count=static_cast<std::uint32_t>(arg(1));const auto actual=std::min<std::uint64_t>(count,object.enumeration.size()-object.position);object.position+=actual;return done(actual==count?0U:1U);}
    if(name=="Clone"){
        if(!arg(1))return done(storage::pointer_error);memory.store(GuestAddress{arg(1)},x86_?32:64,0);const auto handle=create_storage_object(memory,2,object.view,object.node,0);
        try{bridge.objects.at(handle).enumeration=object.enumeration;bridge.objects.at(handle).position=object.position;bridge.objects.at(handle).parent=object.parent;memory.store(GuestAddress{arg(1)},x86_?32:64,handle);}
        catch(...){(void)release_storage_object(state,memory,handle);throw;}return done(0);
    }
    if(name=="Next"){
        const auto count=static_cast<std::uint32_t>(arg(1));if(arg(3))memory.store(GuestAddress{arg(3)},32,0);
        if(!arg(2) || (count!=1 && !arg(3)))return done(storage::pointer_error);const auto actual=std::min<std::uint64_t>(count,object.enumeration.size()-object.position);const auto extent=x86_?72U:80U;
        if(count>compound::node_limit)return done(storage::no_memory);memory.check(GuestAddress{arg(2)},static_cast<std::size_t>(count)*extent,Access::Write);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(static_cast<std::size_t>(count)*extent));
        std::vector<std::uint64_t> names;
        try{for(std::uint64_t i=0;i<actual;++i){const auto address=arg(2)+i*extent;storage_stat(state,memory,address,0,object.enumeration.at(static_cast<std::size_t>(object.position+i)),0,false);names.push_back(memory.load(GuestAddress{address},x86_?32:64));}}
        catch(...){for(auto address:names)(void)free_heap(memory,address,state);throw;}
        object.position+=actual;if(arg(3))memory.store(GuestAddress{arg(3)},32,actual);return done(actual==count?0U:1U);
    }
    throw HostCallFault("未分類的 IEnumSTATSTG 方法");
}
}
