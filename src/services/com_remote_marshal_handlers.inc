std::optional<std::uint32_t> WindowsServices::resume_com_marshal_remote(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t returned){
    auto& bridge=runtime_.com_marshal;auto& op=bridge.operations.at(id);const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto callback=[&](GuestCallback call,unsigned next){op.step=next;start_callbacks(state,memory,{std::move(call)},Continuation::ComMarshalOperation,id);return std::optional<std::uint32_t>{};};
    const auto export_error=[](com_exporter::Status status){
        switch(status){case com_exporter::Status::Ok:return 0U;
        case com_exporter::Status::Denied:return 0x80070005U;
        case com_exporter::Status::Invalid:return 0x80070057U;
        case com_exporter::Status::Busy:return 0x8001010aU;
        case com_exporter::Status::Limit:return 0x8007000eU;
        default:return 0x800401fdU;}
    };
    const auto cleanup=[&](std::uint32_t result){
        op.error=result;
        if(op.export_operation){(void)bridge.exports.abort(op.export_operation);op.export_operation=0;}
        if(op.exported && op.token && op.packet_owned){(void)bridge.exports.release_packet(op.token);op.packet_owned=false;op.token=0;}
        else if(op.token && op.packet_owned){const auto packet=bridge.packets.release(op.token);op.packet_owned=false;op.token=0;
            if(packet && !com_marshal::weak_flags(packet->flags)){op.reference=packet->object;op.owner=packet->owner;op.held=true;}}
        if(op.canonical_owned){const auto object=op.canonical;auto next=callback({context_method(memory,object,2),{object}},90);op.canonical_owned=false;return next;}
        if(op.held){auto next=callback({resolve("internal-com-proxy","ReleaseAt").value,{op.owner,op.reference}},90);op.held=false;return next;}
        if(op.interface_owned){auto next=callback({context_method(memory,op.interface,2),{op.interface}},90);op.interface_owned=false;return next;}
        auto calls=com_exporter_retirements(state,memory);
        if(!calls.empty())return callback({resolve("internal-com-rpc","Retire").value,calls.front().arguments},91);
        discard_com_marshal(state,memory,id);complete_call(state,memory,result);return std::optional<std::uint32_t>{};
    };
    try{
        if(op.step==90 || op.step==91)return cleanup(op.error);
        if(op.kind==ComMarshalOperation::Kind::Disconnect){
            if(op.step==20){memory.write(GuestAddress{op.scratch+160},context_iid(0));memory.store(GuestAddress{op.scratch+176},bits,0);
                return callback({context_method(memory,op.reference,0),{op.reference,op.scratch+160,op.scratch+176}},21);}
            if(op.step==21){
                op.canonical=memory.load(GuestAddress{op.scratch+176},bits);op.canonical_owned=op.canonical!=0;
                if(returned&0x80000000U)return cleanup(static_cast<std::uint32_t>(returned));if(!op.canonical)return cleanup(0x80004002U);
                op.generation=com_object_generation(memory,op.canonical,op.owner);
                (void)bridge.exports.disconnect({op.owner,op.canonical,com_apartment(op.owner)},op.generation);
                const auto apartment=com_apartment(op.owner);bridge.object_generations.erase({apartment && !*apartment?0:op.owner,op.canonical});
                return cleanup(0);
            }
            throw HostCallFault("COM 中斷連線延續狀態不合法");
        }
        if(op.kind==ComMarshalOperation::Kind::Unmarshal || op.kind==ComMarshalOperation::Kind::ReleaseData){
            if(op.step==10){memory.store(GuestAddress{op.scratch+128},32,0);
                return callback({context_method(memory,op.stream,3),{op.stream,op.scratch+32,com_marshal::remote_wire_size-32,op.scratch+128}},11);}
            if(op.step==11){
                if(returned&0x80000000U)return cleanup(static_cast<std::uint32_t>(returned));
                if(memory.load(GuestAddress{op.scratch+128},32)!=com_marshal::remote_wire_size-32)return cleanup(0x8003001eU);
                std::array<std::uint8_t,com_marshal::remote_wire_size> bytes{};memory.read(GuestAddress{op.scratch},bytes);
                op.remote_wire=com_marshal::decode_remote(bytes);if(!op.remote_wire)return cleanup(0x80070057U);
                const auto& wire=*op.remote_wire;
                // 跨程序來源由後續 RPC dispatch 取得；任何情況都不能把 interface ID 當客體指標。
                if(wire.interface.endpoint!=com_rpc_endpoint())return cleanup(0x800706baU);
                const auto inspected=bridge.exports.inspect_packet(wire.interface,wire.packet,wire.flags);
                if(inspected.status!=com_exporter::Status::Ok)return cleanup(export_error(inspected.status));
                const auto* packet=bridge.packets.find(wire.packet);
                if(!packet || !packet->standard || packet->object!=inspected.source.interface.object ||
                   packet->owner!=inspected.source.interface.owner || packet->iid!=wire.interface.iid || packet->flags!=wire.flags)return cleanup(0x800401fdU);
                if(op.kind==ComMarshalOperation::Kind::ReleaseData){
                    const auto released=bridge.exports.release_packet(wire.packet);return cleanup(export_error(released.status));
                }
                if(op.iid!=wire.interface.iid)return cleanup(0x80004002U);
                const auto owner=threads_.items.find(inspected.source.interface.owner);
                if(owner==threads_.items.end() || owner->second.status==ThreadStatus::Exited || owner->second.forced_stop)return cleanup(0x800401fdU);
                try{if(com_object_generation(memory,inspected.source.canonical.object,inspected.source.canonical.owner)!=inspected.source.generation)return cleanup(0x800401fdU);}
                catch(const MemoryFault&){return cleanup(0x800401fdU);}
                const auto kind=com_proxy::kind_for(wire.interface.iid);if(!kind)return cleanup(0x80004002U);
                const auto acquired=bridge.exports.begin_acquire(wire.interface,wire.packet,{wire.interface.endpoint,id});
                if(acquired.status!=com_exporter::Status::Ok)return cleanup(export_error(acquired.status));op.export_operation=acquired.operation;
                op.owner=acquired.source.interface.owner;op.reference=acquired.source.interface.object;op.flags=wire.flags;
                memory.store(GuestAddress{op.scratch+128},32,0);
                const auto source=com_apartment(op.owner),destination=com_apartment(threads_.current);
                const auto recipient=source && destination && !*source && !*destination?op.owner:threads_.current;
                return callback({resolve("internal-com-proxy","Marshal").value,{op.reference,op.owner,recipient,static_cast<std::uint64_t>(*kind),op.scratch+128}},12);
            }
            if(op.step==12){
                const auto error=static_cast<std::uint32_t>(memory.load(GuestAddress{op.scratch+128},32));
                op.interface=returned;op.interface_owned=op.interface!=0;
                if(error || !op.interface)return cleanup(error?error:0x800401fdU);
                const auto completed=bridge.exports.complete_local(op.export_operation);op.export_operation=0;
                if(completed.status!=com_exporter::Status::Ok)return cleanup(export_error(completed.status));
                op.step=14;
            }
            if(op.step==14){
                auto calls=com_exporter_retirements(state,memory);
                if(!calls.empty())return callback({resolve("internal-com-rpc","Retire").value,calls.front().arguments},14);
                memory.store(GuestAddress{op.output},bits,op.interface);op.interface_owned=false;return cleanup(0);
            }
            throw HostCallFault("遠端封送讀取延續狀態不合法");
        }
        if(op.kind!=ComMarshalOperation::Kind::Marshal)throw HostCallFault("遠端封送操作種類不合法");
        if(op.step==0){(void)com_rpc_endpoint();memory.write(GuestAddress{op.scratch+160},context_iid(0));memory.store(GuestAddress{op.scratch+176},bits,0);
            return callback({context_method(memory,op.reference,0),{op.reference,op.scratch+160,op.scratch+176}},1);}
        if(op.step==1){op.canonical=memory.load(GuestAddress{op.scratch+176},bits);op.canonical_owned=op.canonical!=0;
            if(returned&0x80000000U)return cleanup(static_cast<std::uint32_t>(returned));if(!op.canonical)return cleanup(0x80004002U);
            op.generation=com_object_generation(memory,op.canonical);
            if(!com_marshal::weak_flags(op.flags))return callback({context_method(memory,op.reference,1),{op.reference}},2);
            op.step=2;
        }
        if(op.step==2){
            if(!com_marshal::weak_flags(op.flags))op.held=true;
            const auto token=bridge.packets.publish({0,op.reference,op.owner,op.iid,op.flags,true});op.token=token;op.packet_owned=true;op.held=false;
            std::array<std::uint8_t,16> capability{};if(RAND_bytes(capability.data(),static_cast<int>(capability.size()))!=1)return cleanup(0x8007000eU);
            const auto apartment=com_apartment(op.owner);com_exporter::Source source{{op.owner,op.canonical,apartment},{op.owner,op.reference,apartment},op.generation};
            const auto published=bridge.exports.publish(token,source,op.iid,op.flags,capability);
            if(published.status!=com_exporter::Status::Ok)return cleanup(published.status==com_exporter::Status::Limit?0x8007000eU:0x80004005U);op.exported=true;
            const auto bytes=com_marshal::encode(com_marshal::RemoteWire{published.wire,token,op.flags,op.context});
            memory.write(GuestAddress{op.scratch},bytes);memory.store(GuestAddress{op.scratch+128},32,0);
            return callback({context_method(memory,op.stream,4),{op.stream,op.scratch,bytes.size(),op.scratch+128}},3);
        }
        if(op.step==3){if(returned&0x80000000U)return cleanup(static_cast<std::uint32_t>(returned));if(memory.load(GuestAddress{op.scratch+128},32)!=com_marshal::remote_wire_size)return cleanup(0x8003001dU);
            op.packet_owned=false;op.token=0;return cleanup(0);
        }
        throw HostCallFault("遠端封送延續狀態不合法");
    }catch(const MemoryFault&){return cleanup(0x80004003U);}catch(const std::bad_alloc&){return cleanup(0x8007000eU);}
    catch(const com_rpc::TransportError& error){return cleanup(0x80070000U|(error.code&0xffffU));}
    catch(const rot::Error& error){return cleanup(0x80070000U|(error.code&0xffffU));}
    catch(const SecurityDescriptorError& error){return cleanup(0x80070000U|(error.code&0xffffU));}
    catch(...){discard_com_marshal(state,memory,id);throw;}
}
