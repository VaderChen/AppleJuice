if(name=="MFCreateMediaSession"){
    out(arg(1));require(!bridge.startups.empty(),0xc00d36b0U);
    if(arg(0))require(mf::attributes(entry(arg(0)).object->kind),no_interface);
    const auto p=make(mf::Session);object(p,mf::Session)->session=std::make_shared<SessionState>();put(arg(1),p,bits);
}else if(name=="MFCreateTopology"){
    out(arg(0));const auto p=make(mf::Topology);object(p,mf::Topology)->topo_id=bridge.next_topology_id++;put(arg(0),p,bits);
}else if(name=="MFCreateTopologyNode"){
    out(arg(1));require(d(0)<=3);const auto p=make(mf::TopologyNode);auto node=object(p,mf::TopologyNode);node->node_type=d(0);node->topo_id=bridge.next_topology_id++;put(arg(1),p,bits);
}else if(name=="MFCreateCollection"){
    out(arg(0));put(arg(0),make(mf::Collection),bits);
}else if(name=="MFCreateSampleGrabberSinkActivate"){
    out(arg(2));require(arg(0)!=0 && arg(1)!=0,pointer_error);(void)object(arg(0),mf::MediaType);
    const auto type=copy_media_type(arg(0));const auto p=make(mf::Activate);auto activate=object(p,mf::Activate);
    activate->identifier=4;activate->output_type=type;activate->callback=arg(1);hold(arg(1));put(arg(2),p,bits);
}else if(name=="MFCreateAudioRendererActivate"){
    out(arg(0));const auto p=make(mf::Activate);object(p,mf::Activate)->identifier=5;put(arg(0),p,bits);
}else if(name=="MFPutWorkItem" || name=="MFPutWorkItem2" || name=="MFPutWorkItemEx" || name=="MFPutWorkItemEx2"){
    // 工作佇列共用 MF 工作執行緒，由 poll_media_foundation 派送；Invoke 不在呼叫端執行緒重入。
    const auto queue=d(0);require(queue<=8 || (queue&0xffff0000U)!=0,0xc00d36ffU);require(queue!=UINT32_MAX,0xc00d36ffU);
    const unsigned shift=name.ends_with('2')?1U:0U;std::uint64_t result_pointer=0;
    if(name.starts_with("MFPutWorkItemEx")){
        auto completion=object(arg(1+shift),mf::Async);require(completion->callback!=0);hold(arg(1+shift));result_pointer=arg(1+shift);
    }else {
        require(arg(1+shift)!=0,pointer_error);(void)foreign_method(arg(1+shift),4);
        result_pointer=make(mf::Async);auto completion=object(result_pointer,mf::Async);hold(arg(1+shift));hold(arg(2+shift));completion->callback=arg(1+shift);completion->state=arg(2+shift);
    }
    const auto callback=object(result_pointer,mf::Async)->callback;bridge.work_items.push_back({result_pointer,foreign_method(callback,4),callback});
}else if(name=="SessionPump"){
    session_pump(object(arg(0),mf::Session),arg(0));
}else if(name=="SessionPumpDone"){
    auto target=object(arg(0),mf::Session);target->poll_queued=false;if(target->closed)session_teardown(target);session_sync_job(target);
}else if(name=="SessionDeliver"){
    auto target=object(arg(0),mf::Session);target->event_dispatching=false;if(!target->closed)deliver(arg(0),target);session_sync_job(target);
}
