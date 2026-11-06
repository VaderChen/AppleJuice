else if(name.starts_with("TN_")){
    require(item->kind==mf::TopologyNode,no_interface);const auto method=name.substr(3);
    const auto pref=[&](std::map<std::uint32_t,std::uint64_t>& table,bool setting){
        if(setting){if(arg(2))(void)object(arg(2),mf::MediaType);hold(arg(2));if(const auto old=table.find(d(1));old!=table.end())release(old->second);table.insert_or_assign(d(1),arg(2));}
        else {out(arg(2));const auto found=table.find(d(1));require(found!=table.end() && found->second,0xc00d36d5U);hold(found->second);put(arg(2),found->second,bits);}
    };
    if(method=="SetObject"){hold(arg(1));release(item->result_object);item->result_object=arg(1);}
    else if(method=="GetObject"){out(arg(1));require(item->result_object!=0,0x80004005U);hold(item->result_object);put(arg(1),item->result_object,bits);}
    else if(method=="GetNodeType")put(arg(1),item->node_type);
    else if(method=="GetTopoNodeID")put(arg(1),item->topo_id,64);
    else if(method=="SetTopoNodeID")item->topo_id=arg(1);
    else if(method=="GetInputCount")put(arg(1),item->inputs.size());
    else if(method=="GetOutputCount")put(arg(1),item->outputs.size());
    else if(method=="ConnectOutput")node_connect(arg(0),d(1),arg(2),d(3));
    else if(method=="DisconnectOutput")require(node_disconnect(*item,d(1)),0xc00d36d5U);
    else if(method=="GetInput" || method=="GetOutput"){
        out(arg(2));put(arg(3),0);const auto& links=method=="GetInput"?item->inputs:item->outputs;require(d(1)<links.size(),invalid_index);
        const auto link=links[d(1)];require(link.node!=0,0xc00d36d5U);hold(link.node);put(arg(2),link.node,bits);put(arg(3),link.index);
    }
    else if(method=="SetOutputPrefType")pref(item->output_prefs,true);else if(method=="GetOutputPrefType")pref(item->output_prefs,false);
    else if(method=="SetInputPrefType")pref(item->input_prefs,true);else if(method=="GetInputPrefType")pref(item->input_prefs,false);
    else if(method=="CloneFrom"){auto from=object(arg(1),mf::TopologyNode);require(from->node_type==item->node_type,0xc00d36b2U);if(from!=item)node_copy(*item,*from);}
    else throw Failure{no_interface};
}else if(name.starts_with("TP_")){
    require(item->kind==mf::Topology,no_interface);const auto method=name.substr(3);
    if(method=="GetTopologyID")put(arg(1),item->topo_id,64);
    else if(method=="AddNode"){(void)object(arg(1),mf::TopologyNode);require(std::find(item->children.begin(),item->children.end(),arg(1))==item->children.end() && item->children.size()<UINT16_MAX);hold(arg(1));item->children.push_back(arg(1));}
    else if(method=="RemoveNode"){const auto found=std::find(item->children.begin(),item->children.end(),arg(1));require(found!=item->children.end());item->children.erase(found);release(arg(1));}
    else if(method=="GetNodeCount")put(arg(1),item->children.size(),16);
    else if(method=="GetNode"){out(arg(2));require(d(1)<item->children.size(),invalid_index);hold(item->children[d(1)]);put(arg(2),item->children[d(1)],bits);}
    else if(method=="Clear")topology_clear(*item);
    else if(method=="GetNodeByID"){
        out(arg(2));const auto found=std::find_if(item->children.begin(),item->children.end(),[&](std::uint64_t p){return object(p,mf::TopologyNode)->topo_id==arg(1);});
        require(found!=item->children.end(),0xc00d36d5U);hold(*found);put(arg(2),*found,bits);
    }
    else if(method=="GetSourceNodeCollection" || method=="GetOutputNodeCollection"){out(arg(1));put(arg(1),topology_collection(*item,method=="GetSourceNodeCollection"?1U:0U),bits);}
    else if(method=="CloneFrom"){
        auto from=object(arg(1),mf::Topology);
        if(from!=item){
            topology_clear(*item);for(const auto& [key,value]:item->attributes){(void)key;if(value.type==13)release(value.scalar);}item->attributes.clear();
            for(const auto& [key,value]:from->attributes)set(*item,key,value);
            std::map<std::uint64_t,std::uint64_t> copies;
            for(const auto p:from->children){const auto original=object(p,mf::TopologyNode);const auto copy=make(mf::TopologyNode);auto node=object(copy,mf::TopologyNode);node->node_type=original->node_type;node_copy(*node,*original);item->children.push_back(copy);copies.emplace(p,copy);}
            for(const auto p:from->children){
                const auto original=object(p,mf::TopologyNode);
                for(std::uint32_t n=0;n<original->outputs.size();++n){const auto link=original->outputs[n];if(!link.node)continue;const auto target=copies.find(link.node);if(target!=copies.end())node_connect(copies.at(p),n,target->second,link.index);}
            }
            item->topo_id=from->topo_id;
        }
    }else throw Failure{no_interface};
}else if(name.starts_with("CL_")){
    require(item->kind==mf::Collection,no_interface);const auto method=name.substr(3);
    if(method=="GetElementCount")put(arg(1),item->children.size());
    else if(method=="GetElement"){out(arg(2));require(d(1)<item->children.size());require(item->children[d(1)]!=0,0x8000ffffU);hold(item->children[d(1)]);put(arg(2),item->children[d(1)],bits);}
    else if(method=="AddElement"){require(item->children.size()<65536,0x8007000eU);hold(arg(1));item->children.push_back(arg(1));}
    else if(method=="RemoveElement"){out(arg(2));require(d(1)<item->children.size());const auto p=item->children[d(1)];item->children.erase(item->children.begin()+d(1));put(arg(2),p,bits);}
    else if(method=="InsertElementAt"){require(d(1)<=item->children.size() && item->children.size()<65536);hold(arg(2));item->children.insert(item->children.begin()+d(1),arg(2));}
    else if(method=="RemoveAllElements"){auto all=std::move(item->children);item->children.clear();for(const auto p:all)release(p);}
    else throw Failure{no_interface};
}else if(name.starts_with("MS_")){
    require(item->kind==mf::Session && bool(item->session),no_interface);require(!item->closed,shutdown);auto& s=*item->session;
    const auto enqueue=[&](SessionCommand command){require(s.commands.size()<4096,0x8007000eU);s.commands.push_back(std::move(command));session_sync_job(item);};
    if(name=="MS_SetTopology"){
        require(!(d(1)&~7U));require(arg(2) || (d(1)&4U),pointer_error);if(arg(2))(void)object(arg(2),mf::Topology);
        SessionCommand command;command.kind=SessionCommand::SetTopology;command.flags=d(1);command.topology=arg(2);hold(arg(2));
        try{enqueue(std::move(command));}catch(const Failure&){release(arg(2));throw;}
    }else if(name=="MS_ClearTopologies"){SessionCommand command;command.kind=SessionCommand::ClearTopologies;enqueue(std::move(command));}
    else if(name=="MS_Start"){
        if(arg(1))require(guid(arg(1))==Guid{},0xc00d36c5U);
        SessionCommand command;command.kind=SessionCommand::Start;command.position=read_value(arg(2));
        require(command.position.type==0 || (command.position.type==20 && std::bit_cast<std::int64_t>(command.position.scalar)>=0));enqueue(std::move(command));
    }else if(name=="MS_Pause" || name=="MS_Stop" || name=="MS_Close"){SessionCommand command;command.kind=name=="MS_Pause"?SessionCommand::Pause:name=="MS_Stop"?SessionCommand::Stop:SessionCommand::Close;enqueue(std::move(command));}
    else if(name=="MS_Shutdown"){
        item->closed=true;for(const auto& command:s.commands)release(command.topology);s.commands.clear();
        for(const auto p:item->events)release(p);item->events.clear();
        // 等候中的 BeginGetEvent 以工作項目通知；EndGetEvent 回傳 MF_E_SHUTDOWN。
        if(item->callback){
            const auto p=make(mf::Async);auto notice=object(p,mf::Async);notice->queue_owner=item->pointers.at(mf::Session);notice->callback=item->callback;notice->state=item->state;
            bridge.work_items.push_back({p,foreign_method(item->callback,4),item->callback});item->callback=item->state=0;
        }
        if(!item->poll_queued)session_teardown(item);
        session_sync_job(item);
    }else if(name=="MS_GetClock"){out(arg(1));const auto p=create(item,mf::Clock);hold(p);put(arg(1),p,bits);}
    else if(name=="MS_GetSessionCapabilities")put(arg(1),s.topology?0x57U:0U);
    else if(name=="MS_GetFullTopology"){
        out(arg(3));std::uint64_t found=0;
        if(d(1)&1U)found=s.topology;
        else {if(s.topology && object(s.topology,mf::Topology)->topo_id==arg(2))found=s.topology;for(const auto p:s.queued)if(object(p,mf::Topology)->topo_id==arg(2))found=p;}
        require(found!=0,0xc00d36b2U);hold(found);put(arg(3),found,bits);
    }else throw Failure{no_interface};
}else if(name.starts_with("CK_")){
    require(item->kind==mf::Session && bool(item->session),no_interface);require(!item->closed,shutdown);auto& s=*item->session;const auto now=runtime_.clock_ticks;
    if(name=="CK_GetClockCharacteristics")put(arg(1),2);
    else if(name=="CK_GetCorrelatedTime"){put(arg(2),std::bit_cast<std::uint64_t>(s.clock_time(now)),64);put(arg(3),now,64);}
    else if(name=="CK_GetContinuityKey")put(arg(1),s.continuity);
    else if(name=="CK_GetState")put(arg(2),s.clock_state);
    else if(name=="CK_GetProperties"){check(arg(1),48);memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(48));put(arg(1),10000000,64);put(arg(1)+32,10000000,64);put(arg(1)+40,0);put(arg(1)+44,1);}
    else if(name=="CK_SetTimeSource"){hold(arg(1));release(s.time_source);s.time_source=arg(1);}
    else if(name=="CK_GetTimeSource"){out(arg(1));require(s.time_source!=0,0xc00d9c41U);hold(s.time_source);put(arg(1),s.time_source,bits);}
    else if(name=="CK_GetTime")put(arg(1),std::bit_cast<std::uint64_t>(s.clock_time(now)),64);
    else if(name=="CK_AddClockStateSink"){
        require(arg(1)!=0,pointer_error);require(std::find(s.clock_sinks.begin(),s.clock_sinks.end(),arg(1))==s.clock_sinks.end());hold(arg(1));s.clock_sinks.push_back(arg(1));
        // 時鐘已執行時立即通知新 sink；起點以 PRESENTATION_CURRENT_POSITION 表示。
        if(s.clock_state==1){GuestCallback call{foreign_method(arg(1),mf::sink_clock_start),{arg(1),now,UINT64_C(0x7fffffffffffffff)}};call.wide_arguments=6;callbacks.push_back(std::move(call));}
    }
    else if(name=="CK_RemoveClockStateSink"){const auto found=std::find(s.clock_sinks.begin(),s.clock_sinks.end(),arg(1));if(found!=s.clock_sinks.end()){s.clock_sinks.erase(found);release(arg(1));}}
    else if(name=="CK_Start"){
        const auto position=arg(1);
        if(position==UINT64_C(0x7fffffffffffffff)){require(s.clock_state!=1,0xc00d9c42U);if(s.clock_state==3)session_clock_restart(s);else session_clock_start(s,s.anchor_time);}
        else session_clock_start(s,std::bit_cast<std::int64_t>(position));
        session_sync_job(item);
    }else if(name=="CK_Stop"){require(s.clock_state==1 || s.clock_state==3,0xc00d9c42U);session_clock_stop(s);session_sync_job(item);}
    else if(name=="CK_Pause"){require(s.clock_state==1,0xc00d9c42U);session_clock_pause(s);session_sync_job(item);}
    else throw Failure{no_interface};
}else if(name.starts_with("RC_")){
    require(item->kind==mf::Session && bool(item->session),no_interface);require(!item->closed,shutdown);auto& s=*item->session;
    if(name=="RC_SetRate"){
        const auto rate=float_argument(2);require(!d(1),0xc00d36d1U);require(rate==rate && rate>=0,0xc00d36d2U);require(rate<=8,0xc00d36d0U);
        SessionCommand command;command.kind=SessionCommand::SetRate;command.rate=rate;require(s.commands.size()<4096,0x8007000eU);s.commands.push_back(std::move(command));session_sync_job(item);
    }else {if(arg(1))put(arg(1),0);if(arg(2))put(arg(2),std::bit_cast<std::uint32_t>(s.rate));else require(arg(1)!=0,pointer_error);}
}else if(name.starts_with("RS_")){
    require(item->kind==mf::Session,no_interface);require(!item->closed,shutdown);
    if(name=="RS_IsRateSupported"){
        const auto rate=float_argument(2);if(arg(3))put(arg(3),std::bit_cast<std::uint32_t>(std::clamp(rate,0.F,8.F)));
        require(!d(1),0xc00d36d1U);require(rate==rate && rate>=0,0xc00d36d2U);require(rate<=8,0xc00d36d0U);
    }else {put(arg(3),0);require(d(1)==0,0xc00d36d2U);require(!d(2),0xc00d36d1U);put(arg(3),std::bit_cast<std::uint32_t>(name=="RS_GetSlowestRate"?0.F:8.F));}
}else if(name=="GS_GetService"){
    require(item->kind==mf::Session,no_interface);require(!item->closed,shutdown);out(arg(3));const auto service_id=guid(arg(1)),iid=guid(arg(2));mf::Kind kind{};
    if(service_id==mf::MF_RATE_CONTROL_SERVICE && iid==mf::iid_RateControl)kind=mf::RateControl;
    else if(service_id==mf::MF_RATE_CONTROL_SERVICE && iid==mf::iid_RateSupport)kind=mf::RateSupport;
    else if(service_id==mf::MR_STREAM_VOLUME_SERVICE && iid==mf::iid_StreamVolume)kind=mf::StreamVolume;
    else if(service_id==mf::MR_POLICY_VOLUME_SERVICE && iid==mf::iid_SimpleVolume)kind=mf::SimpleVolume;
    else if(iid==mf::iid_PresentationClock || iid==mf::iid_Clock)kind=mf::Clock;
    else throw Failure{0xc00d36baU};
    const auto p=create(item,kind);hold(p);put(arg(3),p,bits);
}else if(name.starts_with("SV_") || name.starts_with("AV_")){
    require(item->kind==mf::Session && bool(item->session),no_interface);require(!item->closed,shutdown);auto& s=*item->session;
    const auto valid=[](float value){return value>=0 && value<=1;};
    if(name=="SV_SetMasterVolume"){const auto value=float_argument(1);require(valid(value));s.master_volume=value;session_apply_volume(s);}
    else if(name=="SV_GetMasterVolume")put(arg(1),std::bit_cast<std::uint32_t>(s.master_volume));
    else if(name=="SV_SetMute"){s.muted=d(1)!=0;session_apply_volume(s);}
    else if(name=="SV_GetMute")put(arg(1),s.muted);
    else if(name=="AV_GetChannelCount")put(arg(1),2);
    else if(name=="AV_SetChannelVolume"){require(d(1)<2);const auto value=float_argument(2);require(valid(value));s.channel_volume[d(1)]=value;session_apply_volume(s);}
    else if(name=="AV_GetChannelVolume"){require(d(1)<2);put(arg(2),std::bit_cast<std::uint32_t>(s.channel_volume[d(1)]));}
    else if(name=="AV_SetAllVolumes"){require(d(1)==2);const std::array<float,2> values{std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32))),std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4},32)))};require(valid(values[0]) && valid(values[1]));s.channel_volume=values;session_apply_volume(s);}
    else if(name=="AV_GetAllVolumes"){require(d(1)==2);put(arg(2),std::bit_cast<std::uint32_t>(s.channel_volume[0]));put(arg(2)+4,std::bit_cast<std::uint32_t>(s.channel_volume[1]));}
    else throw Failure{no_interface};
}
