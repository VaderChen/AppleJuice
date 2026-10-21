#include "x64arm/windows_services.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/com_proxy_schema.hpp"
#include "shared_rot.hpp"
#include <algorithm>
#include <array>
namespace x64arm {
namespace {
constexpr std::uint32_t pointer_error=0x80004003U,invalid=0x80070057U,no_interface=0x80004002U,no_memory=0x8007000eU,disconnected=0x800401fdU,read_fault=0x8003001eU,write_fault=0x8003001dU;
WindowsGuid iid_value(std::uint32_t value){WindowsGuid id{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};for(unsigned i=0;i<4;++i)id[i]=static_cast<std::uint8_t>(value>>(8*i));return id;}
std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned slot){const auto bits=static_cast<std::uint8_t>(memory.execution_mode()==ExecutionMode::X86?32:64);if(!object)throw MemoryFault(GuestAddress{object},Access::Read,bits/8,"COM 物件為空");const auto table=memory.load(GuestAddress{object},bits);return memory.load(GuestAddress{table+slot*(bits/8)},bits);}
bool failed(std::uint64_t value){return (value&0x80000000U)!=0;}
bool local_context(std::uint32_t value){return value==3 || value==4;}
bool bad_flags(std::uint32_t flags){return !com_marshal::valid_flags(flags);}
}
std::span<const WindowsServiceDescriptor> com_marshal_service_descriptors(){
    static constexpr WindowsServiceDescriptor rows[]{
        {"ole32.dll","CoCreateFreeThreadedMarshaler",2},{"combase.dll","CoCreateFreeThreadedMarshaler",2},
        {"ole32.dll","CoAllowSetForegroundWindow",2},{"ole32.dll","CoGetStandardMarshal",6},{"combase.dll","CoGetStandardMarshal",6},
        {"ole32.dll","CoMarshalInterface",6},{"combase.dll","CoMarshalInterface",6},
        {"ole32.dll","CoUnmarshalInterface",3},{"combase.dll","CoUnmarshalInterface",3},
        {"ole32.dll","CoReleaseMarshalData",1},{"combase.dll","CoReleaseMarshalData",1},
        {"ole32.dll","CoDisconnectObject",2},{"combase.dll","CoDisconnectObject",2},
        {"ole32.dll","CoGetMarshalSizeMax",6},{"combase.dll","CoGetMarshalSizeMax",6},
        {"ole32.dll","CoMarshalInterThreadInterfaceInStream",3},{"combase.dll","CoMarshalInterThreadInterfaceInStream",3},
        {"ole32.dll","CoGetInterfaceAndReleaseStream",3},{"combase.dll","CoGetInterfaceAndReleaseStream",3},
        {"internal-com-marshal-unknown","QueryInterface",3},{"internal-com-marshal-unknown","AddRef",1},{"internal-com-marshal-unknown","Release",1},
        {"internal-com-marshal","QueryInterface",3},{"internal-com-marshal","AddRef",1},{"internal-com-marshal","Release",1},
        {"internal-com-marshal","GetUnmarshalClass",7},{"internal-com-marshal","GetMarshalSizeMax",7},{"internal-com-marshal","MarshalInterface",7},
        {"internal-com-marshal","UnmarshalInterface",4},{"internal-com-marshal","ReleaseMarshalData",2},{"internal-com-marshal","DisconnectObject",2}
    };return rows;
}
std::uint64_t WindowsServices::create_com_marshaler(GuestState& state,GuestMemory& memory,std::uint64_t outer,bool standard){
    auto& bridge=runtime_.com_marshal;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);if(bridge.objects.size()>=4096)throw std::bad_alloc();
    const auto table=[&](std::uint64_t& cache,std::string_view family){if(!cache){const auto rows=com_marshal_service_descriptors();const auto count=std::count_if(rows.begin(),rows.end(),[&](const auto& row){return row.family==family;});const auto address=runtime_allocate(memory,{},static_cast<std::uint64_t>(count)*unit);unsigned slot=0;for(const auto& row:rows)if(row.family==family)memory.store(GuestAddress{address+slot++*unit},bits,resolve(family,row.symbol).value);cache=address;}return cache;};
    const auto unknown=table(bridge.unknown_table,"internal-com-marshal-unknown"),marshal=table(bridge.marshal_table,"internal-com-marshal");
    const auto object=allocate_heap(memory,2*unit);if(!object)throw std::bad_alloc();try{memory.store(GuestAddress{object},bits,unknown);memory.store(GuestAddress{object+unit},bits,marshal);bridge.objects.emplace(object,ComMarshalObject{outer,1,0,standard});}catch(...){(void)free_heap(memory,object,state);throw;}return object;
}
std::vector<GuestCallback> WindowsServices::retire_com_marshal(GuestMemory& memory,std::uint64_t thread,bool callbacks){
    const auto initialized=gui_.com_initializations.find(thread);
    if(initialized!=gui_.com_initializations.end() && !(initialized->second&2U)){
        for(const auto& [other,count]:gui_.com_initializations)if(other!=thread && count>=16 && !(count&2U)){
            runtime_.com_marshal.packets.move_standard_owner(thread,other);
            runtime_.com_marshal.exports.move_mta_owner(thread,other);
            if(com_running_table_)com_running_table_->move_thread(thread,other);return {};
        }
    }
    std::vector<GuestCallback> result;
    if(com_running_table_)com_running_table_->retire_thread(thread);
    auto& bridge=runtime_.com_marshal;(void)bridge.exports.disconnect_owner(thread);
    const auto packets=bridge.packets.retire_standard(thread,[&](std::uint64_t token){return bridge.exports.owns_packet(token);});
    if(callbacks)for(const auto& packet:packets)if(!com_marshal::weak_flags(packet.flags))
        result.push_back({method(memory,packet.object,2),{packet.object}});
    // 遠端封送 packet 的 pin 屬 exporter；不可在公寓離開時繞過它直接 Release。
    if(callbacks)for(const auto& [id,action]:bridge.exports.retirements())if(!action.started)
        result.push_back({resolve("internal-com-rpc","Retire").value,{id}});
    const auto apartment=initialized==gui_.com_initializations.end()?std::optional<std::uint64_t>{}:com_apartment(thread);
    const auto key_owner=apartment && !*apartment?0:thread;
    std::erase_if(bridge.object_generations,[&](const auto& entry){return entry.first.first==key_owner;});
    return result;
}
void WindowsServices::discard_com_marshal(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& bridge=runtime_.com_marshal;const auto found=bridge.operations.find(id);if(found==bridge.operations.end())return;auto op=std::move(found->second);bridge.operations.erase(found);
    const auto retire=[&](std::uint64_t object,std::uint64_t owner){if(object){const auto retirement=release_com_proxy_owned(state,memory,{owner,object});if(retirement)queue_com_proxy_retirement(memory,retirement);}};
    if(op.export_operation){(void)bridge.exports.abort(op.export_operation);queue_com_exporter_retirements(threads_.current);}
    if(op.token && op.packet_owned){
        if(op.exported){(void)bridge.exports.release_packet(op.token);queue_com_exporter_retirements(threads_.current);}
        else{const auto packet=bridge.packets.release(op.token);if(packet && !com_marshal::weak_flags(packet->flags))retire(packet->object,packet->owner);}
    }
    if(op.held)retire(op.reference,op.owner);
    if(op.interface_owned)retire(op.interface,threads_.current);
    if(op.delivered_owned)retire(op.delivered,threads_.current);
    if(op.temporary_owned)retire(op.temporary,threads_.current);
    if(op.canonical_owned)retire(op.canonical,op.owner);
    if(op.stream_owned)retire(op.stream,threads_.current);
    if(op.scratch)(void)free_heap(memory,op.scratch,state);
    if(op.marshal){const auto object=bridge.objects.find(op.marshal);if(object!=bridge.objects.end()){if(op.factory_owned)object->second.references=0;if(object->second.pins)--object->second.pins;if(!object->second.references && !object->second.pins){if(object->second.standard)retire(object->second.outer,op.owner);bridge.objects.erase(object);(void)free_heap(memory,op.marshal,state);}}}
}
std::optional<std::uint32_t> WindowsServices::resume_com_marshal(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t returned){
    using Kind=ComMarshalOperation::Kind;auto& bridge=runtime_.com_marshal;auto& op=bridge.operations.at(id);const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(op.kind>=Kind::PublicMarshal || op.symbol=="StandardDelegate" || op.symbol=="DisconnectPublic")return resume_com_marshal_public(state,memory,id,returned);
    if(op.remote)return resume_com_marshal_remote(state,memory,id,returned);
    const auto callback=[&](GuestCallback call,unsigned next){op.step=next;op.awaiting=true;start_callbacks(state,memory,{std::move(call)},Continuation::ComMarshalOperation,id);return std::optional<std::uint32_t>{};};
    const auto finish=[&](std::uint64_t value){op.token=0;op.held=false;discard_com_marshal(state,memory,id);complete_call(state,memory,value);return std::optional<std::uint32_t>{};};
    const auto release=[&](std::uint64_t object,unsigned next){return callback({method(memory,object,2),{object}},next);};
    try{
        if(op.step==99)return finish(op.error);
        if(op.kind==Kind::Forward){
            if(op.symbol=="CreateStandard"){if(op.step==0)return callback({method(memory,op.reference,1),{op.reference}},1);bridge.objects.at(op.marshal).outer=op.reference;memory.store(GuestAddress{op.output},bits,op.marshal+unit);op.factory_owned=false;return finish(0);}
            if(op.step==0){if(op.symbol=="QueryOuter"){memory.write(GuestAddress{op.scratch+80},op.iid);return callback({op.result,{op.reference,op.scratch+80,op.output}},1);}return callback({op.result,{op.reference}},1);}return finish(op.symbol=="DropStandard" || op.symbol=="QueryMarshal"?0:returned);
        }
        if(op.kind==Kind::Foreground){
            if(op.step==0){memory.write(GuestAddress{op.scratch+80},iid_value(0x145));return callback({method(memory,op.object,0),{op.object,op.scratch+80,op.scratch+64}},1);}
            if(op.step==1){if(failed(returned))return finish(returned);op.reference=memory.load(GuestAddress{op.scratch+64},bits);if(!op.reference)return finish(no_interface);op.held=true;return callback({method(memory,op.reference,3),{op.reference,0}},2);}
            if(op.step==2){op.result=returned;op.held=false;return release(op.reference,3);}return finish(op.result);
        }
        if(op.kind==Kind::Marshal){
            if(op.step==0 && !com_marshal::weak_flags(op.flags))return callback({method(memory,op.reference,1),{op.reference}},1);
            if(op.step<=1){if(!com_marshal::weak_flags(op.flags))op.held=true;op.token=bridge.packets.publish({0,op.reference,op.owner,op.iid,op.flags,bridge.objects.at(op.marshal).standard});op.packet_owned=true;op.held=false;const auto bytes=com_marshal::encode(com_marshal::Wire{op.token,op.reference,process_id(),op.flags});memory.write(GuestAddress{op.scratch},bytes);memory.store(GuestAddress{op.scratch+64},32,0);return callback({method(memory,op.stream,4),{op.stream,op.scratch,bytes.size(),op.scratch+64}},2);}
            if(op.step==2){if(!failed(returned) && memory.load(GuestAddress{op.scratch+64},32)==32){op.token=0;return finish(0);}op.error=failed(returned)?static_cast<std::uint32_t>(returned):write_fault;const auto packet=bridge.packets.release(op.token);op.token=0;if(packet && !com_marshal::weak_flags(packet->flags)){op.held=false;return release(packet->object,3);}return finish(op.error);}return finish(op.error);
        }
        if(op.kind==Kind::Unmarshal || op.kind==Kind::ReleaseData){
            if(op.step==0){memory.store(GuestAddress{op.scratch+64},32,0);return callback({method(memory,op.stream,3),{op.stream,op.scratch,32,op.scratch+64}},1);}
            if(op.step==1){
                if(failed(returned))return finish(returned);if(memory.load(GuestAddress{op.scratch+64},32)!=32)return finish(read_fault);std::array<std::uint8_t,32> data{};memory.read(GuestAddress{op.scratch},data);
                if(com_marshal::remote_prefix(data)){op.remote=true;op.step=10;return resume_com_marshal_remote(state,memory,id,returned);}
                const auto wire=com_marshal::decode(data);if(!wire || wire->pid!=process_id())return finish(disconnected);const auto* packet=bridge.packets.find(wire->token);if(!packet || packet->flags!=wire->flags || packet->object!=wire->object)return finish(disconnected);
                if(op.kind==Kind::Unmarshal && packet->iid!=op.iid)return finish(no_interface);op.reference=packet->object;op.flags=packet->flags;op.owner=packet->owner;
                if(op.kind==Kind::ReleaseData){const auto removed=bridge.packets.release(wire->token);if(!com_marshal::weak_flags(removed->flags)){if(!removed->standard)return release(removed->object,3);return callback({resolve("internal-com-proxy","ReleaseAt").value,{removed->owner,removed->object}},3);}return finish(0);}
                if(packet->standard){const auto kind=com_proxy::kind_for(packet->iid);if(!kind)return finish(no_interface);op.token=wire->token;memory.store(GuestAddress{op.scratch+68},32,0);const auto source=com_apartment(packet->owner),destination=com_apartment(threads_.current);const auto recipient=source && destination && !*source && !*destination?packet->owner:threads_.current;return callback({resolve("internal-com-proxy","Marshal").value,{packet->object,packet->owner,recipient,static_cast<std::uint64_t>(*kind),op.scratch+68}},4);}
                (void)bridge.packets.consume(wire->token);if(com_marshal::mode_flags(op.flags)==0){op.held=true;memory.store(GuestAddress{op.output},bits,op.reference);return finish(0);}return callback({method(memory,op.reference,1),{op.reference}},2);
            }
            if(op.step==2){op.held=true;memory.store(GuestAddress{op.output},bits,op.reference);return finish(0);}
            if(op.step==4){const auto error=memory.load(GuestAddress{op.scratch+68},32);if(error || !returned){op.token=0;return finish(error?error:disconnected);}op.interface=returned;op.interface_owned=true;const auto packet=bridge.packets.consume(op.token);op.token=0;if(!packet){op.error=disconnected;op.interface_owned=false;return release(op.interface,99);}if(com_marshal::mode_flags(packet->flags)==0){return callback({resolve("internal-com-proxy","ReleaseAt").value,{packet->owner,packet->object}},5);}memory.store(GuestAddress{op.output},bits,op.interface);op.interface_owned=false;return finish(0);}
            if(op.step==5){memory.store(GuestAddress{op.output},bits,op.interface);op.interface_owned=false;return finish(0);}return finish(0);
        }
        return finish(invalid);
    }catch(const MemoryFault&){op.error=pointer_error;}catch(const std::bad_alloc&){op.error=no_memory;}catch(const std::invalid_argument&){op.error=invalid;}
    if(op.token && op.packet_owned){const auto packet=bridge.packets.release(op.token);op.token=0;if(packet && !com_marshal::weak_flags(packet->flags)){op.reference=packet->object;op.owner=packet->owner;op.held=true;}}
    if(op.interface_owned){op.interface_owned=false;return release(op.interface,99);}
    if(op.held){op.held=false;return callback({resolve("internal-com-proxy","ReleaseAt").value,{op.owner,op.reference}},99);}return finish(op.error);
}
std::optional<std::uint32_t> WindowsServices::invoke_com_marshal(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);auto& bridge=runtime_.com_marshal;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto output=[&](std::uint64_t p,std::size_t size){if(!p)throw MemoryFault(GuestAddress{p},Access::Write,size,"COM 輸出為空");memory.check(GuestAddress{p},size,Access::Write);};
    const auto guid=[&](std::uint64_t p){WindowsGuid id{};if(!p)throw MemoryFault(GuestAddress{p},Access::Read,16,"COM IID 為空");memory.read(GuestAddress{p},id);return id;};
    const auto begin=[&](ComMarshalOperation op){if(bridge.operations.size()>=4096)throw std::bad_alloc();op.owner=threads_.current;op.scratch=allocate_heap(memory,512);if(!op.scratch)throw std::bad_alloc();const auto id=bridge.next++;try{memory.write(GuestAddress{op.scratch},std::vector<std::uint8_t>(512));bridge.operations.emplace(id,std::move(op));}catch(...){(void)free_heap(memory,op.scratch,state);throw;}auto& saved=bridge.operations.at(id);if(saved.marshal)++bridge.objects.at(saved.marshal).pins;return resume_com_marshal(state,memory,id);};
    const auto error=[&](std::uint32_t value){
        if(name=="CoGetInterfaceAndReleaseStream" && arg(0)){
            start_callbacks(state,memory,{{method(memory,arg(0),2),{arg(0)}}},Continuation::Return,value);
            return std::optional<std::uint32_t>{};
        }
        return done(value);
    };
    try{
        if(name=="CoCreateFreeThreadedMarshaler"){output(arg(1),unit);memory.store(GuestAddress{arg(1)},bits,0);const auto object=create_com_marshaler(state,memory,arg(0));memory.store(GuestAddress{arg(1)},bits,object);return done(0);}
        if(name=="CoAllowSetForegroundWindow"){if(arg(1))return done(invalid);if(!arg(0))return done(pointer_error);if(gui_.foreground_locked || !interactive_station() || gui_.input_desktop!=thread_desktop_object(threads_.current) || !gui_.menu_tracks.empty())return done(0x80070005U);ComMarshalOperation op;op.kind=ComMarshalOperation::Kind::Foreground;op.object=arg(0);return begin(op);}
        using Kind=ComMarshalOperation::Kind;
        const auto initialized=gui_.com_initializations.find(threads_.current);
        const bool ready=initialized!=gui_.com_initializations.end() && initialized->second>=16;
        if(name=="CoDisconnectObject"){
            if(arg(1))return done(invalid);if(!arg(0))return done(pointer_error);if(!ready)return done(0x800401f0U);
            ComMarshalOperation op;op.symbol="DisconnectPublic";op.object=arg(0);return begin(op);
        }
        if(name=="CoGetInterfaceAndReleaseStream"){
            if(!arg(0))return done(pointer_error);
            ComMarshalOperation op;op.kind=Kind::GetAndRelease;op.stream=arg(0);op.stream_owned=true;
            op.context_pointer=arg(1);op.output=arg(2);return begin(op);
        }
        if(name=="CoMarshalInterface" || name=="CoGetMarshalSizeMax" || name=="CoUnmarshalInterface" ||
           name=="CoReleaseMarshalData" || name=="CoMarshalInterThreadInterfaceInStream"){
            if(!ready)return done(0x800401f0U);
            ComMarshalOperation op;op.symbol=std::string(name);
            if(name=="CoMarshalInterface" || name=="CoGetMarshalSizeMax"){
                op.kind=Kind::PublicMarshal;op.iid=guid(arg(1));op.object=arg(2);
                op.context=static_cast<std::uint32_t>(arg(3));op.flags=static_cast<std::uint32_t>(arg(5));
                if(!op.object || (!arg(0) && name=="CoMarshalInterface"))return done(pointer_error);
                if(arg(4) || op.context>4 || bad_flags(op.flags))return done(invalid);
                if(name=="CoGetMarshalSizeMax"){op.output=arg(0);output(op.output,4);memory.store(GuestAddress{op.output},32,0);}
                else op.stream=arg(0);
            }else if(name=="CoUnmarshalInterface"){
                op.kind=Kind::PublicUnmarshal;op.iid=guid(arg(1));op.output=arg(2);
                output(op.output,unit);memory.store(GuestAddress{op.output},bits,0);op.stream=arg(0);
                if(!op.stream)return done(pointer_error);
            }else if(name=="CoReleaseMarshalData"){
                op.kind=Kind::PublicRelease;op.stream=arg(0);if(!op.stream)return done(pointer_error);
            }else{
                op.kind=Kind::InterThread;op.iid=guid(arg(0));op.object=arg(1);op.output=arg(2);
                output(op.output,unit);memory.store(GuestAddress{op.output},bits,0);
                if(!op.object)return done(pointer_error);
            }
            return begin(op);
        }
        if(name=="CoGetStandardMarshal"){
            output(arg(5),unit);memory.store(GuestAddress{arg(5)},bits,0);(void)guid(arg(0));
            if(!ready)return done(0x800401f0U);
            if(arg(3) || bad_flags(static_cast<std::uint32_t>(arg(4))) || static_cast<std::uint32_t>(arg(2))>4)return done(invalid);
            const auto object=create_com_marshaler(state,memory,0,true);
            if(!arg(1)){memory.store(GuestAddress{arg(5)},bits,object+unit);return done(0);}
            ComMarshalOperation op;op.kind=Kind::Forward;op.symbol="CreateStandard";
            op.marshal=object;op.factory_owned=true;op.reference=arg(1);op.output=arg(5);
            try{return begin(op);}catch(...){bridge.objects.erase(object);(void)free_heap(memory,object,state);throw;}
        }
        const bool unknown=service.family=="internal-com-marshal-unknown";const auto root=unknown?arg(0):arg(0)-unit;const auto found=bridge.objects.find(root);if(found==bridge.objects.end())return done(disconnected);auto& object=found->second;
        if(name=="QueryInterface"){
            const auto id=guid(arg(1));output(arg(2),unit);memory.store(GuestAddress{arg(2)},bits,0);if(!unknown && object.outer && !object.standard){ComMarshalOperation op;op.kind=ComMarshalOperation::Kind::Forward;op.symbol="QueryOuter";op.marshal=root;op.reference=object.outer;op.result=method(memory,object.outer,0);op.output=arg(2);op.iid=id;return begin(op);}
            if(id!=iid_value(0) && id!=iid_value(3))return done(no_interface);const auto address=id==iid_value(0)?root:root+unit;
            if(id==iid_value(3) && object.outer && !object.standard){memory.store(GuestAddress{arg(2)},bits,address);ComMarshalOperation op;op.kind=ComMarshalOperation::Kind::Forward;op.marshal=root;op.reference=object.outer;op.result=method(memory,object.outer,1);op.symbol="QueryMarshal";op.output=arg(2);return begin(op);}if(object.references==UINT32_MAX)return done(no_memory);++object.references;memory.store(GuestAddress{arg(2)},bits,address);return done(0);
        }
        if(name=="AddRef" || name=="Release"){
            if(!unknown && object.outer && !object.standard){ComMarshalOperation op;op.kind=ComMarshalOperation::Kind::Forward;op.marshal=root;op.reference=object.outer;op.result=method(memory,object.outer,name=="AddRef"?1:2);return begin(op);}
            if(name=="AddRef"){if(object.references==UINT32_MAX)return done(no_memory);return done(++object.references);}if(!object.references)return done(disconnected);const auto count=--object.references;
            if(!count && object.standard && object.outer){ComMarshalOperation op;op.kind=ComMarshalOperation::Kind::Forward;op.symbol="DropStandard";op.marshal=root;op.reference=object.outer;op.result=method(memory,object.outer,2);object.outer=0;return begin(op);}if(!count && !object.pins){bridge.objects.erase(found);(void)free_heap(memory,root,state);}return done(count);
        }
        if(name=="DisconnectObject"){
            if(arg(1))return done(invalid);
            // FTM 沒有自己的遠端連線；standard marshaler 綁定的是 outer 所代表的物件。
            if(!object.standard)return done(0x8000ffffU);
            if(!object.outer)return done(0x80004005U);
            ComMarshalOperation op;op.marshal=root;op.kind=Kind::Disconnect;op.reference=object.outer;op.remote=true;op.step=20;return begin(op);
        }
        ComMarshalOperation op;op.marshal=root;
        if(name=="GetUnmarshalClass" || name=="GetMarshalSizeMax" || name=="MarshalInterface"){
            const bool write=name=="MarshalInterface";op.iid=guid(arg(write?2:1));op.reference=arg(write?3:2);op.context=static_cast<std::uint32_t>(arg(write?4:3));op.flags=static_cast<std::uint32_t>(arg(write?6:5));if(arg(write?5:4) || op.context>4 || bad_flags(op.flags))return done(invalid);
            if(!local_context(op.context) && !object.standard){
                if(!write)output(arg(6),name=="GetUnmarshalClass"?16:4);
                else if(!op.reference || !arg(1))return done(pointer_error);
                op.kind=Kind::Forward;op.symbol="StandardDelegate";op.stream=write?arg(1):0;
                op.output=write?0:arg(6);op.context_pointer=write?5:(name=="GetUnmarshalClass"?3:4);
                return begin(op);
            }
            if(object.standard && !com_proxy::kind_for(op.iid))return done(no_interface);
            const bool remote=object.standard && op.context<=1;
            if(!local_context(op.context) && !remote)return done(0x800706baU);
            if(!write){output(arg(6),name=="GetUnmarshalClass"?16:4);if(name=="GetUnmarshalClass")memory.write(GuestAddress{arg(6)},iid_value(object.standard?0x17:0x33a));else memory.store(GuestAddress{arg(6)},32,remote?com_marshal::remote_wire_size:32);return done(0);}
            if(!op.reference || !arg(1))return done(pointer_error);(void)method(memory,op.reference,1);op.kind=ComMarshalOperation::Kind::Marshal;op.stream=arg(1);op.remote=remote;return begin(op);
        }
        if(name=="UnmarshalInterface"){op.kind=ComMarshalOperation::Kind::Unmarshal;op.stream=arg(1);op.iid=guid(arg(2));op.output=arg(3);output(op.output,unit);memory.store(GuestAddress{op.output},bits,0);if(!op.stream)return done(pointer_error);return begin(op);}
        if(name=="ReleaseMarshalData"){op.kind=ComMarshalOperation::Kind::ReleaseData;op.stream=arg(1);if(!op.stream)return done(pointer_error);return begin(op);}
        return done(invalid);
    }catch(const MemoryFault&){return error(pointer_error);}catch(const std::bad_alloc&){return error(no_memory);}catch(const std::invalid_argument&){return error(invalid);}
}
}
