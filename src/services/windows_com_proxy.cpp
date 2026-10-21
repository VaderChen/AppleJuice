#include "x64arm/windows_services.hpp"
#include "x64arm/com_proxy_parameters.hpp"
#include <algorithm>
#include <limits>

namespace x64arm {
namespace {
using namespace com_proxy;
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_memory=0x8007000eU;
constexpr std::uint32_t disconnected=0x80010108U,wrong_thread=0x8001010eU,no_proxy=0x80040155U,aborted=0x800703e3U;
std::uint64_t method_pointer(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    const unsigned unit=memory.execution_mode()==ExecutionMode::X86?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!object)throw HostCallFault("COM 來源物件為空");
    const auto table=memory.load(GuestAddress{object},bits);
    if(!table || slot>(UINT64_MAX-table)/unit)throw HostCallFault("COM 來源 vtable 不合法");
    const auto target=memory.load(GuestAddress{table+slot*unit},bits);
    if(!target)throw HostCallFault("COM 來源方法為空");return target;
}
std::uint32_t status_error(Status s){
    if(s==Status::Disconnected || s==Status::Missing)return disconnected;
    if(s==Status::WrongApartment)return wrong_thread;
    if(s==Status::Limit)return no_memory;return invalid;
}
std::optional<Kind> guid_kind(const GuestMemory& memory,std::uint64_t address){
    WindowsGuid id{};memory.read(GuestAddress{address},id);return kind_for(id);
}
bool succeeded(std::uint64_t result){return !(result&0x80000000U);}
}
std::span<const WindowsServiceDescriptor> com_proxy_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> entries{{"internal-com-proxy","Marshal",5},{"internal-com-proxy","Dispatch",1},{"internal-com-proxy","Retire",1},{"internal-com-proxy","ReleaseAt",2}};
        for(const auto& method:com_proxy::methods)entries.push_back({"internal-com-proxy",method.symbol,method.arguments,method.wide_arguments,0,method.narrow_arguments});
        return entries;
    }();return table;
}
std::uint64_t WindowsServices::create_com_proxy_owned(GuestState& state,GuestMemory& memory,com_proxy::Key key,std::vector<std::uint64_t>& retirements){
    auto& bridge=runtime_.com_proxies;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(key.owner==key.apartment)return key.original;
    auto table=bridge.vtables.find(key.kind);
    if(table==bridge.vtables.end()){
        const auto specification=com_proxy::methods_for(key.kind);if(specification.empty())throw std::invalid_argument("COM 代理介面種類不合法");
        const auto address=runtime_allocate(memory,{},specification.size()*unit);
        for(const auto& method:specification)memory.store(GuestAddress{address+method.slot*unit},bits,resolve("internal-com-proxy",method.symbol).value);
        table=bridge.vtables.emplace(key.kind,address).first;
    }
    const auto header=allocate_heap(memory,unit);if(!header)throw std::bad_alloc();
    try {
        memory.store(GuestAddress{header},bits,table->second);
        // 先保留 retirement 容量，避免 adopt 已取得 ownership 後因 vector 配置失敗遺失引用。
        retirements.reserve(retirements.size()+1);
        const auto result=bridge.objects.adopt(key,header);
        if(result.status!=com_proxy::Status::Ok)throw std::bad_alloc();
        if(result.address!=header)(void)free_heap(memory,header,state);
        if(result.retirement)retirements.push_back(result.retirement);
        return result.address;
    }catch(...){(void)free_heap(memory,header,state);throw;}
}
std::uint64_t WindowsServices::release_com_proxy_owned(GuestState& state,GuestMemory& memory,com_proxy::Origin origin){
    auto& objects=runtime_.com_proxies.objects;
    if(objects.get(origin.object)){
        const auto result=objects.release(origin.object);
        if(result.disposal)(void)free_heap(memory,result.disposal->header,state);
        return result.retirement;
    }
    const auto owner=threads_.items.find(origin.owner);
    if(!origin.object || owner==threads_.items.end() || owner->second.status==ThreadStatus::Exited || owner->second.forced_stop)return 0;
    return objects.retire_owned(origin).retirement;
}
void WindowsServices::queue_com_proxy_retirement(GuestMemory&,std::uint64_t retirement){
    auto& bridge=runtime_.com_proxies;const auto found=bridge.objects.retirements().find(retirement);
    if(found==bridge.objects.retirements().end())return;
    if(!found->second.held){if(!found->second.started)(void)bridge.objects.finish_retirement(retirement);return;}
    const auto owner=threads_.items.find(found->second.origin.owner);
    if(owner==threads_.items.end() || owner->second.status==ThreadStatus::Exited || owner->second.forced_stop)return;
    ComProxyOperation operation;operation.type=ComProxyOperation::Type::Retire;operation.retirement=retirement;
    operation.receiver=found->second.origin.owner;operation.object=found->second.origin.object;operation.abandoned=true;
    const auto id=bridge.next++;const auto token=gui_.next_object++;
    GuiSentMessage request;request.receiver=operation.receiver;request.proxy_operation=id;
    request.rpc=GuestCallback{resolve("internal-com-proxy","Dispatch").value,{id}};request.abandoned=true;
    operation.rpc=token;
    const auto inserted=bridge.operations.emplace(id,std::move(operation));
    try{gui_.sent_messages.emplace(token,std::move(request));}catch(...){bridge.operations.erase(inserted.first);throw;}
}
void WindowsServices::discard_com_proxy_dispatch(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& bridge=runtime_.com_proxies;const auto found=bridge.operations.find(id);if(found==bridge.operations.end())return;
    auto op=std::move(found->second);bridge.operations.erase(found);
    if(op.acquired){const auto retire=release_com_proxy_owned(state,memory,{op.receiver,op.object});if(retire)queue_com_proxy_retirement(memory,retire);}
    for(auto& input:op.inputs)if(input.acquired){
        const auto retire=release_com_proxy_owned(state,memory,{op.receiver,input.acquired});if(retire)queue_com_proxy_retirement(memory,retire);
    }
    for(const auto origin:op.exported){const auto retire=release_com_proxy_owned(state,memory,origin);if(retire)queue_com_proxy_retirement(memory,retire);}
    for(std::size_t i=op.field;i<op.fields.size();++i){
        const auto raw=memory.load(GuestAddress{op.fields[i].cell},x86_?32:64);
        if(raw){const auto retire=release_com_proxy_owned(state,memory,{op.receiver,raw});if(retire)queue_com_proxy_retirement(memory,retire);}
    }
    if(op.proxy){const auto result=bridge.objects.unpin(op.proxy);if(result.disposal)(void)free_heap(memory,result.disposal->header,state);if(result.retirement)queue_com_proxy_retirement(memory,result.retirement);}
    for(const auto retirement:op.retirements)queue_com_proxy_retirement(memory,retirement);
    if(op.retirement_started)(void)bridge.objects.finish_retirement(op.retirement);
    for(const auto address:op.scratch)(void)free_heap(memory,address,state);
    // 讓仍在等待的 caller 重試原 API；已離開的 caller 不再寫其輸出。
    if(const auto rpc=gui_.sent_messages.find(op.rpc);rpc!=gui_.sent_messages.end()){rpc->second.error=995;rpc->second.done=true;rpc->second.active=false;}
}
std::vector<GuestCallback> WindowsServices::retire_com_proxy_thread(GuestState& state,GuestMemory& memory,std::uint64_t thread,bool callbacks){
    auto& bridge=runtime_.com_proxies;std::vector<GuestCallback> result;
    const auto detached=bridge.objects.disconnect(thread);
    if(callbacks)for(const auto origin:detached.releases)result.push_back({method_pointer(memory,origin.object,2),{origin.object}});
    std::vector<std::uint64_t> discarded;
    for(auto& [id,op]:bridge.operations){
        if(op.receiver==thread){if(!callbacks || !op.active)discarded.push_back(id);}
        else if(op.sender==thread){op.abandoned=true;if(!op.active || op.stage==ComProxyOperation::Stage::Done)discarded.push_back(id);}
    }
    for(const auto id:discarded)discard_com_proxy_dispatch(state,memory,id);
    // 離開 apartment 時代理不得留下本地引用；執行中的 server pin 仍保留到回呼結束。
    std::vector<std::uint64_t> headers;
    for(const auto& [header,entry]:bridge.objects.entries())if(entry.key.apartment==thread)headers.push_back(header);
    for(const auto header:headers)while(const auto entry=bridge.objects.get(header)){
        if(!entry->references)break;
        const auto released=bridge.objects.release(header);if(released.disposal)(void)free_heap(memory,released.disposal->header,state);
        if(released.retirement)queue_com_proxy_retirement(memory,released.retirement);
    }
    return result;
}
std::optional<std::uint32_t> WindowsServices::invoke_com_proxy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace com_proxy;using Type=ComProxyOperation::Type;using Stage=ComProxyOperation::Stage;
    WindowsCall call(state,memory,service);auto& bridge=runtime_.com_proxies;
    if(service.symbol=="Dispatch"){
        const auto id=call(0);const auto found=bridge.operations.find(id);
        if(found==bridge.operations.end() || found->second.receiver!=threads_.current || found->second.active){call.return_to(state,disconnected);return std::nullopt;}
        found->second.active=true;return advance_com_proxy_dispatch(state,memory,id);
    }
    auto request=std::find_if(gui_.sent_messages.begin(),gui_.sent_messages.end(),[&](const auto& pair){const auto& q=pair.second;return q.proxy_operation && q.sender==threads_.current && q.sender_stack==state.reg(Gpr::Rsp) && q.sender_rip==state.rip.value;});
    if(request!=gui_.sent_messages.end() && request->second.done){
        const auto id=request->second.proxy_operation;const auto transport=request->second.error;gui_.sent_messages.erase(request);
        const auto found=bridge.operations.find(id);
        if(found==bridge.operations.end()){
            const auto error=transport==1400?disconnected:aborted;
            if(service.symbol=="Marshal"){if(call(4))memory.store(GuestAddress{call(4)},32,error);call.return_to(state,0);}
            else {const auto method=method_for(service.symbol);call.return_to(state,method && method->slot==2?0:error);}return std::nullopt;
        }
        auto& op=found->second;
        if(transport || op.stage!=Stage::Done){
            const auto type=op.type;const auto error_pointer=op.error_pointer;discard_com_proxy_dispatch(state,memory,id);
            const auto error=transport==1400?disconnected:aborted;if(error_pointer)memory.store(GuestAddress{error_pointer},32,error);
            call.return_to(state,type==Type::Marshal?0:error);return std::nullopt;
        }
        const auto value=op.type==Type::Marshal?(op.error?0:op.result):op.type==Type::Retire?(op.symbol.empty()?op.error:op.result):(op.error?op.error:op.result);
        try {
        // 原方法完成且 caller 還在 apartment 中才提交介面及 STGMEDIUM 輸出。
        for(const auto& output:op.outputs)if(!op.error || output.layout==ComProxyOutput::Layout::Interface){
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(output.size));memory.read(GuestAddress{output.buffer},bytes);memory.write(GuestAddress{output.destination},bytes);
        }
        if(op.error_pointer)memory.store(GuestAddress{op.error_pointer},32,op.error);
        }catch(...){discard_com_proxy_dispatch(state,memory,id);throw;}
        for(const auto address:op.scratch)(void)free_heap(memory,address,state);
        bridge.operations.erase(found);threads_.items.at(threads_.current).wait={};call.return_to(state,value);return std::nullopt;
    }
    if(request==gui_.sent_messages.end()){
        ComProxyOperation op;op.sender=threads_.current;op.frame_stack=state.reg(Gpr::Rsp);op.frame_rip=state.rip.value;
        const auto method=method_for(service.symbol);
        if(service.symbol=="Marshal"){
            op.type=Type::Marshal;op.object=call(0);op.receiver=call(1);op.marshal_recipient=call(2);op.kind=static_cast<Kind>(call(3));op.error_pointer=call(4);
            if(op.error_pointer)memory.check(GuestAddress{op.error_pointer},4,Access::Write);
            if(!op.object || !op.marshal_recipient || call(3)>=iids.size()){
                if(op.error_pointer)memory.store(GuestAddress{op.error_pointer},32,invalid);call.return_to(state,0);return std::nullopt;
            }
            if(const auto entry=bridge.objects.get(op.object)){
                if(entry->key.apartment!=op.receiver || !entry->connected){if(op.error_pointer)memory.store(GuestAddress{op.error_pointer},32,entry->connected?wrong_thread:disconnected);call.return_to(state,0);return std::nullopt;}
                if(op.marshal_recipient==entry->key.apartment && op.kind==entry->key.kind){
                    const auto added=bridge.objects.add_ref(op.object);if(op.error_pointer)memory.store(GuestAddress{op.error_pointer},32,added.status==Status::Ok?0:status_error(added.status));call.return_to(state,added.status==Status::Ok?op.object:0);return std::nullopt;
                }
                op.pin_apartment=entry->key.apartment;op.proxy=op.object;op.object=entry->key.original;op.receiver=entry->key.owner;
            }
        }else if(service.symbol=="Retire" || service.symbol=="ReleaseAt"){
            op.type=Type::Retire;
            if(service.symbol=="ReleaseAt")op.retirement=bridge.objects.retire_owned({call(0),call(1)}).retirement;else op.retirement=call(0);
            const auto retirement=bridge.objects.retirements().find(op.retirement);
            if(retirement==bridge.objects.retirements().end() || !retirement->second.held){if(retirement!=bridge.objects.retirements().end() && !retirement->second.started)(void)bridge.objects.finish_retirement(op.retirement);call.return_to(state,0);return std::nullopt;}
            op.receiver=retirement->second.origin.owner;op.object=retirement->second.origin.object;
        }else if(method){
            const auto entry=bridge.objects.get(call(0));
            if(!entry || entry->key.kind!=method->kind){call.return_to(state,method->slot==1 || method->slot==2?0:disconnected);return std::nullopt;}
            if(method->slot==1){const auto added=bridge.objects.add_ref(call(0));call.return_to(state,added.status==Status::Ok?added.references:0);return std::nullopt;}
            if(method->slot==2){
                const auto released=bridge.objects.release(call(0));if(released.disposal)(void)free_heap(memory,released.disposal->header,state);
                if(!released.retirement){call.return_to(state,released.references);return std::nullopt;}
                op.type=Type::Retire;op.retirement=released.retirement;op.symbol=std::string(service.symbol);op.result=released.references;
                const auto& retirement=bridge.objects.retirements().at(op.retirement);op.receiver=retirement.origin.owner;op.object=retirement.origin.object;
            }else {
                op.proxy=call(0);op.object=entry->key.original;op.receiver=entry->key.owner;op.kind=entry->key.kind;op.symbol=std::string(service.symbol);
                for(unsigned i=0;i<method->arguments;++i)op.arguments.push_back(call(i));op.arguments[0]=op.object;
            }
        }else {call.return_to(state,invalid);return std::nullopt;}
        const auto receiver=threads_.items.find(op.receiver),recipient=threads_.items.find(op.type==Type::Marshal?op.marshal_recipient:op.sender);
        if(receiver==threads_.items.end() || receiver->second.status==ThreadStatus::Exited || receiver->second.forced_stop || recipient==threads_.items.end() || recipient->second.status==ThreadStatus::Exited){
            if(op.error_pointer)memory.store(GuestAddress{op.error_pointer},32,disconnected);call.return_to(state,op.type==Type::Marshal?0:disconnected);return std::nullopt;
        }
        if(bridge.operations.size()>=4096 || gui_.sent_messages.size()>=8192){if(op.error_pointer)memory.store(GuestAddress{op.error_pointer},32,no_memory);call.return_to(state,op.type==Type::Marshal?0:no_memory);return std::nullopt;}
        if(op.proxy){const auto pinned=bridge.objects.pin(op.proxy,op.pin_apartment?op.pin_apartment:op.sender);if(pinned.status!=Status::Ok){if(op.error_pointer)memory.store(GuestAddress{op.error_pointer},32,status_error(pinned.status));call.return_to(state,op.type==Type::Marshal?0:status_error(pinned.status));return std::nullopt;}}
        const auto id=bridge.next++;const auto token=gui_.next_object++;op.rpc=token;
        GuiSentMessage queued;queued.sender=op.sender;queued.receiver=op.receiver;queued.sender_stack=op.frame_stack;queued.sender_rip=op.frame_rip;queued.proxy_operation=id;
        queued.rpc=GuestCallback{resolve("internal-com-proxy","Dispatch").value,{id}};
        try {bridge.operations.emplace(id,std::move(op));request=gui_.sent_messages.emplace(token,std::move(queued)).first;}
        catch(...){if(bridge.operations.contains(id))discard_com_proxy_dispatch(state,memory,id);else if(op.proxy)(void)bridge.objects.unpin(op.proxy);throw;}
    }
    const auto token=request->first;if(dispatch_gui_send(state,memory,0,false))return std::nullopt;
    auto& sender=threads_.items.at(threads_.current);sender.wait={};sender.wait.kind=ThreadWaitKind::GuiSend;sender.wait.handles={token};sender.status=ThreadStatus::Waiting;schedule(state,memory);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::advance_com_proxy_dispatch(GuestState& state,GuestMemory& memory,std::uint64_t id){
    using namespace com_proxy;using Type=ComProxyOperation::Type;using Stage=ComProxyOperation::Stage;using Layout=ComProxyOutput::Layout;
    auto& bridge=runtime_.com_proxies;const auto found=bridge.operations.find(id);
    if(found==bridge.operations.end()){complete_call(state,memory,disconnected);return std::nullopt;}
    auto& op=found->second;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto cell=[&](std::uint64_t address){return memory.load(GuestAddress{address},bits);};
    const auto scratch=[&](std::uint64_t size){const auto at=allocate_heap(memory,size);if(!at)throw std::bad_alloc();try{op.scratch.push_back(at);}catch(...){(void)free_heap(memory,at,state);throw;}memory.write(GuestAddress{at},std::vector<std::uint8_t>(static_cast<std::size_t>(size)));return at;};
    const auto retire=[&](Origin origin){const auto retirement=release_com_proxy_owned(state,memory,origin);if(retirement)op.retirements.push_back(retirement);};
    const auto invoke=[&](GuestCallback callback){start_callbacks(state,memory,{std::move(callback)},Continuation::ComProxyDispatch,id);};
    try {
        if(op.stage==Stage::Begin){
            if(op.type==Type::Retire){
                const auto retirement=bridge.objects.retirements().find(op.retirement);
                if(retirement==bridge.objects.retirements().end() || !retirement->second.held){op.stage=Stage::Done;}
                else {
                    const auto target=method_pointer(memory,op.object,2);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
                    op.stage=Stage::Invoke;invoke({target,{op.object}});
                    // 入口與 stack 準備成功後才把引用交給真正的 Release。
                    if(!bridge.objects.start_retirement(op.retirement))throw HostCallFault("COM retirement ownership 不一致");op.retirement_started=true;return std::nullopt;
                }
            }else if(op.type==Type::Marshal){
                op.exported.reserve(1);op.retirements.reserve(4);op.stage=Stage::Invoke;
                if(op.kind==Kind::Unknown){
                    const auto iid=scratch(16);memory.write(GuestAddress{iid},iids[0]);op.marshal_query_output=scratch(unit);
                    invoke({method_pointer(memory,op.object,0),{op.object,iid,op.marshal_query_output}});
                }else invoke({method_pointer(memory,op.object,1),{op.object}});
                return std::nullopt;
            }else {
                const auto method=method_for(op.symbol);if(!method)throw HostCallFault("COM proxy method 遺失");
                // 先核對來源方法，尚未準備好的介面不能留下借用引用。
                const auto target=method_pointer(memory,op.object,method->slot);if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);
                op.scratch_error=scratch(4);
                if(op.symbol.ends_with(".Next") && op.arguments.size()==4 && op.arguments[3]){
                    const auto destination=op.arguments[3];memory.check(GuestAddress{destination},4,Access::Write);
                    const auto buffer=scratch(4);op.arguments[3]=buffer;op.outputs.push_back({destination,buffer,4,1,0,Kind::Unknown,-1,Layout::Plain});
                }
                for(const auto& parameter:interface_parameters)if(parameter.symbol==op.symbol && parameter.iid_argument>=0){
                    const auto arg=static_cast<unsigned>(parameter.iid_argument);const auto buffer=scratch(16);
                    std::array<std::uint8_t,16> bytes{};memory.read(GuestAddress{op.arguments.at(arg)},bytes);memory.write(GuestAddress{buffer},bytes);op.arguments[arg]=buffer;
                }
                for(const auto& parameter:interface_parameters)if(parameter.symbol==op.symbol){
                    const auto arg=parameter.argument;const auto original=op.arguments.at(arg);
                    if(parameter.direction==Direction::In){if(original)op.inputs.push_back({original,0,0,arg,parameter.kind,false});continue;}
                    if(!original){if(parameter.direction!=Direction::InOut)op.error=pointer_error;continue;}
                    const auto count=parameter.count_argument<0?1:op.arguments.at(static_cast<unsigned>(parameter.count_argument));
                    if(count>UINT32_MAX/unit)throw std::bad_alloc();const auto size=count*unit;
                    memory.check(GuestAddress{original},static_cast<std::size_t>(size),Access::Write);
                    ComProxyOutput out{original,scratch(std::max<std::uint64_t>(unit,size)),size,count,parameter.fetched_argument<0?0:op.arguments.at(static_cast<unsigned>(parameter.fetched_argument)),parameter.kind,parameter.iid_argument,Layout::Interface};
                    op.arguments[arg]=out.buffer;op.outputs.push_back(out);
                    if(parameter.direction==Direction::InOut){const auto input=cell(original);if(input)op.inputs.push_back({input,0,out.buffer,arg,parameter.kind,true});}
                }
                if(op.symbol=="IDataObject.GetData"){
                    const auto destination=op.arguments.at(2);if(!destination)op.error=pointer_error;
                    else {memory.check(GuestAddress{destination},3*unit,Access::Write);const auto buffer=scratch(3*unit);op.outputs.push_back({destination,buffer,3*unit,1,0,Kind::Unknown,-1,Layout::Medium});op.arguments[2]=buffer;}
                }
                if(op.symbol=="IEnumSTATDATA.Next"){
                    const auto count=op.arguments.at(1),destination=op.arguments.at(2);const auto stride=unit==4?32U:56U;
                    if(count>UINT32_MAX/stride)throw std::bad_alloc();const auto size=count*stride;
                    if(!destination)op.error=pointer_error;
                    else {memory.check(GuestAddress{destination},static_cast<std::size_t>(size),Access::Write);const auto buffer=scratch(std::max<std::uint64_t>(stride,size));op.outputs.push_back({destination,buffer,size,count,op.arguments.at(3),Kind::AdviseSink,-1,Layout::StatData});op.arguments[2]=buffer;}
                }
                const int medium_arg=op.symbol=="IDataObject.GetDataHere" || op.symbol=="IDataObject.SetData"?2:op.symbol=="IAdviseSink.OnDataChange"?2:-1;
                if(medium_arg>=0 && op.arguments.at(static_cast<unsigned>(medium_arg))){
                    const auto destination=op.arguments.at(static_cast<unsigned>(medium_arg)),buffer=scratch(3*unit);std::vector<std::uint8_t> bytes(3*unit);memory.read(GuestAddress{destination},bytes);memory.write(GuestAddress{buffer},bytes);op.arguments[static_cast<unsigned>(medium_arg)]=buffer;
                    const auto tymed=memory.load(GuestAddress{buffer},32);const bool transfer=op.symbol=="IDataObject.SetData" && op.arguments.at(3)!=0;
                    if((tymed==4 || tymed==8) && cell(buffer+unit))op.inputs.push_back({cell(buffer+unit),0,buffer+unit,static_cast<unsigned>(medium_arg),tymed==4?Kind::Stream:Kind::Storage,transfer});
                    if(cell(buffer+2*unit))op.inputs.push_back({cell(buffer+2*unit),0,buffer+2*unit,static_cast<unsigned>(medium_arg),Kind::Unknown,transfer});
                    if(op.symbol=="IDataObject.GetDataHere")op.outputs.push_back({destination,buffer,3*unit,1,0,Kind::Unknown,-1,Layout::InMedium});
                }
                std::size_t capacity=0;
                for(const auto& output:op.outputs)if(output.layout==Layout::Medium)capacity+=2;else if(output.layout!=Layout::Plain && output.layout!=Layout::InMedium)capacity+=static_cast<std::size_t>(output.count);
                op.fields.reserve(capacity);op.exported.reserve(capacity);op.retirements.reserve(op.inputs.size()+capacity*3+16);
                op.stage=op.error?Stage::Release:Stage::Borrow;
            }
        }
        if(op.stage==Stage::Borrow){
            if(op.input<op.inputs.size()){
                const auto& input=op.inputs[op.input];memory.store(GuestAddress{op.scratch_error},32,0);
                invoke({resolve("internal-com-proxy","Marshal").value,{input.original,op.sender,op.receiver,static_cast<std::uint64_t>(input.kind),op.scratch_error}});return std::nullopt;
            }
            op.stage=Stage::Invoke;const auto method=method_for(op.symbol);
            invoke({method_pointer(memory,op.object,method->slot),op.arguments,false,false,method->wide_arguments});return std::nullopt;
        }
        if(op.stage==Stage::Fields){
            while(op.field<op.fields.size()){
                auto& field=op.fields[op.field];const auto raw=cell(field.cell);
                if(!raw){++op.field;continue;}
                if(!field.kind){op.error=no_proxy;retire({op.receiver,raw});memory.store(GuestAddress{field.cell},bits,0);++op.field;break;}
                if(bridge.objects.get(raw) || *field.kind==Kind::Unknown){
                    op.stage=Stage::Export;memory.store(GuestAddress{op.scratch_error},32,0);
                    invoke({resolve("internal-com-proxy","Marshal").value,{raw,op.receiver,op.sender,static_cast<std::uint64_t>(*field.kind),op.scratch_error}});return std::nullopt;
                }
                // GetData／QI 等已交來一份真正的來源引用，不額外 AddRef。
                op.exported.reserve(op.exported.size()+1);op.retirements.reserve(op.retirements.size()+1);
                const auto wrapped=create_com_proxy_owned(state,memory,{op.receiver,op.sender,raw,*field.kind},op.retirements);
                memory.store(GuestAddress{field.cell},bits,wrapped);op.exported.push_back({op.sender,wrapped});++op.field;
            }
            op.stage=Stage::Release;
        }
        if(op.stage==Stage::Release){
            if(!op.release){
                for(auto& input:op.inputs)if(input.acquired){
                    if(op.success && input.transfer){retire({op.sender,input.original});input.acquired=0;}
                    else {retire({op.receiver,input.acquired});input.acquired=0;}
                    // GetDataHere 保留 caller 原有的 medium 介面身分。
                    if(input.cell)for(const auto& output:op.outputs)if(output.layout==Layout::InMedium && input.cell>=output.buffer && input.cell<output.buffer+output.size)memory.store(GuestAddress{input.cell},bits,input.original);
                }
                if(op.error || op.abandoned){
                    for(const auto origin:op.exported)retire(origin);op.exported.clear();
                    for(std::size_t i=op.field;i<op.fields.size();++i){const auto raw=cell(op.fields[i].cell);if(raw)retire({op.receiver,raw});memory.store(GuestAddress{op.fields[i].cell},bits,0);}op.field=op.fields.size();
                    for(const auto& output:op.outputs)if(output.layout==Layout::Interface)memory.write(GuestAddress{output.buffer},std::vector<std::uint8_t>(static_cast<std::size_t>(output.size)));
                }
                if(op.proxy){const auto released=bridge.objects.unpin(op.proxy);op.proxy=0;if(released.disposal)(void)free_heap(memory,released.disposal->header,state);if(released.retirement)op.retirements.push_back(released.retirement);}
                op.release=1;
            }
            const auto index=op.release-1;
            if(index<op.retirements.size()){++op.release;invoke({resolve("internal-com-proxy","Retire").value,{op.retirements[index]}});return std::nullopt;}
            op.stage=Stage::Done;
        }
        if(op.stage==Stage::Done){
            const auto value=op.type==Type::Marshal?(op.error?0:op.result):op.type==Type::Retire?op.error:op.error?op.error:op.result;
            if(op.abandoned || !op.sender){
                for(const auto origin:op.exported){const auto retired=release_com_proxy_owned(state,memory,origin);if(retired)queue_com_proxy_retirement(memory,retired);}
                for(const auto address:op.scratch)(void)free_heap(memory,address,state);bridge.operations.erase(found);
            }
            complete_call(state,memory,value);return std::nullopt;
        }
    }catch(const std::bad_alloc&){op.error=no_memory;}
    catch(const MemoryFault&){op.error=aborted;}
    catch(const HostCallFault&){op.error=aborted;}
    // 入口失敗不回傳成功，也不能留下已取得的引用或暫存。
    const auto error=op.error;discard_com_proxy_dispatch(state,memory,id);complete_call(state,memory,error);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_com_proxy_dispatch(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t result){
    using namespace com_proxy;using Type=ComProxyOperation::Type;using Stage=ComProxyOperation::Stage;using Layout=ComProxyOutput::Layout;
    auto& bridge=runtime_.com_proxies;const auto found=bridge.operations.find(id);
    if(found==bridge.operations.end()){complete_call(state,memory,disconnected);return std::nullopt;}
    auto& op=found->second;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    try {
    if(op.stage==Stage::Borrow){
        const auto error=static_cast<std::uint32_t>(memory.load(GuestAddress{op.scratch_error},32));
        if(error || !result){op.error=error?error:no_memory;op.stage=Stage::Release;}
        else {auto& input=op.inputs[op.input++];input.acquired=result;if(input.cell)memory.store(GuestAddress{input.cell},bits,result);else op.arguments[input.argument]=result;}
    }else if(op.stage==Stage::Export){
        auto& field=op.fields[op.field];const auto raw=memory.load(GuestAddress{field.cell},bits);
        const auto error=static_cast<std::uint32_t>(memory.load(GuestAddress{op.scratch_error},32));
        const auto retired=release_com_proxy_owned(state,memory,{op.receiver,raw});if(retired)op.retirements.push_back(retired);
        memory.store(GuestAddress{field.cell},bits,error?0:result);++op.field;
        if(error || !result){op.error=error?error:no_memory;op.stage=Stage::Release;}
        else {op.exported.push_back({op.sender,result});op.stage=Stage::Fields;}
    }else if(op.stage==Stage::Invoke){
        if(op.type==Type::Retire){if(op.retirement_started)(void)bridge.objects.finish_retirement(op.retirement);op.retirement_started=false;op.stage=Stage::Release;}
        else if(op.type==Type::Marshal){
            // AddRef 的 ULONG 高位不是 HRESULT；來源實際引用已經取得。
            if(op.marshal_query_output){
                if(!succeeded(result)){op.error=static_cast<std::uint32_t>(result);op.stage=Stage::Release;return advance_com_proxy_dispatch(state,memory,id);}
                op.object=memory.load(GuestAddress{op.marshal_query_output},bits);
                if(!op.object){op.error=pointer_error;op.stage=Stage::Release;return advance_com_proxy_dispatch(state,memory,id);}
            }
            op.acquired=true;op.exported.reserve(op.exported.size()+1);
            try {op.result=create_com_proxy_owned(state,memory,{op.receiver,op.marshal_recipient,op.object,op.kind},op.retirements);op.exported.push_back({op.marshal_recipient,op.result});op.acquired=false;}
            catch(const std::bad_alloc&){op.error=no_memory;const auto retired=release_com_proxy_owned(state,memory,{op.receiver,op.object});if(retired)op.retirements.push_back(retired);op.acquired=false;}
            op.stage=Stage::Release;
        }else {
            const auto method=method_for(op.symbol);op.result=method->void_result?0:static_cast<std::uint32_t>(result);op.success=succeeded(op.result);
            // Reduce 回傳 NULL 表示左側 moniker 沒有被替換，原引用仍屬 caller。
            if(op.symbol=="IMoniker.Reduce")for(auto& input:op.inputs)if(input.argument==3 && input.cell)input.transfer=op.success && memory.load(GuestAddress{input.cell},bits)!=0;
            if(op.success)for(const auto& output:op.outputs){
                if(output.layout==Layout::InMedium || output.layout==Layout::Plain)continue;
                if(output.layout==Layout::Medium){const auto tymed=memory.load(GuestAddress{output.buffer},32);if(tymed==4 || tymed==8)op.fields.push_back({output.buffer+unit,tymed==4?Kind::Stream:Kind::Storage});op.fields.push_back({output.buffer+2*unit,Kind::Unknown});continue;}
                auto count=output.count;
                if(output.fetched)count=std::min(count,memory.load(GuestAddress{output.fetched},32));else if(output.count>1 || (op.symbol.ends_with(".Next") && op.result!=0))count=0;
                if(output.layout==Layout::StatData){const auto stride=unit==4?32:56,offset=unit==4?24:40;for(std::uint64_t i=0;i<count;++i)op.fields.push_back({output.buffer+i*static_cast<unsigned>(stride)+static_cast<unsigned>(offset),Kind::AdviseSink});}
                else {auto kind=std::optional<Kind>{output.kind};if(output.iid_argument>=0)kind=guid_kind(memory,op.arguments.at(static_cast<unsigned>(output.iid_argument)));for(std::uint64_t i=0;i<count;++i)op.fields.push_back({output.buffer+i*unit,kind});}
            }
            else {for(const auto& output:op.outputs)if(output.layout==Layout::Interface)memory.write(GuestAddress{output.buffer},std::vector<std::uint8_t>(static_cast<std::size_t>(output.size)));}
            op.stage=op.success?Stage::Fields:Stage::Release;
        }
    }
    }catch(const std::bad_alloc&){op.error=no_memory;op.stage=Stage::Release;}
    catch(const MemoryFault&){op.error=aborted;op.stage=Stage::Release;}
    return advance_com_proxy_dispatch(state,memory,id);
}
}
