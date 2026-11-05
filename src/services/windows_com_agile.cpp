#include "x64arm/windows_services.hpp"
#include "x64arm/pe_loader.hpp"
#include "x64arm/startup_profile.hpp"
#include "com_agile_layout.h"
#include "guest_com_agile.hpp"
#include "guest_com_agile_code.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_memory=0x8007000eU,
    no_interface=0x80004002U,disconnected=0x80010108U,not_initialized=0x800401f0U;
WindowsGuid id_from(const unsigned char* data){WindowsGuid id{};std::copy_n(data,16,id.begin());return id;}
bool failed(std::uint64_t value){return (value&0x80000000U)!=0;}
std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    const auto bits=static_cast<std::uint8_t>(memory.execution_mode()==ExecutionMode::X86?32:64);
    const auto table=memory.load(GuestAddress{object},bits);return memory.load(GuestAddress{table+slot*(bits/8U)},bits);
}
}

void WindowsServices::initialize_guest_com_agile(GuestMemory& memory){
    const auto* setting=std::getenv("X64ARM_GUEST_COM_AGILE");
    if((setting && std::string_view(setting)=="0") || !trace_services_.empty() || startup_profile::enabled())return;
    const auto bytes=x86_?std::span<const std::uint8_t>{com_agile::image_x86}:std::span<const std::uint8_t>{com_agile::image_x64};
    const auto base=callback_return_.value+com_agile::image_offset;
    auto parsed=parse_pe_image(bytes,GuestAddress{base},true,memory.limit());
    if(!parsed.imports.empty() || !parsed.delay_imports.empty() || parsed.metadata.image_size>0x10000)
        throw std::logic_error("COM agile 常式包含外部相依或超出映像範圍");
    for(const auto& [address,image]:runtime_.builtin_images)if(base<address+image.size && address<base+parsed.metadata.image_size)
        throw std::logic_error("COM agile 常式與既有常駐映像重疊");
    auto image=parsed.runtime;image.path=u"[guest-com-agile]";image.export_names.clear();image.export_ordinals.clear();
    for(const auto& [name,ordinal]:parsed.runtime.export_names)image.export_names.emplace(name,parsed.runtime.export_ordinals.at(ordinal));
    for(auto& mapping:parsed.mappings)mapping.permissions=(mapping.permissions&permission(Access::Execute))?read_execute:permission(Access::Read);
    map_pe_image(memory,parsed);runtime_.builtin_images.emplace(base,std::move(image));
}

std::uint64_t WindowsServices::com_agile_owner(std::uint64_t context,std::uint64_t preferred)const{
    const auto* entry=runtime_.com_contexts.contexts.find(context);if(!entry || !entry->active)return 0;
    const auto live=[&](std::uint64_t thread){const auto found=threads_.items.find(thread);
        const auto apartment=com_apartment(thread);return found!=threads_.items.end() && found->second.status!=ThreadStatus::Exited &&
            !found->second.forced_stop && apartment && *apartment==entry->owner;};
    if(live(threads_.current))return threads_.current;
    if(live(preferred))return preferred;
    if(!entry->owner)for(const auto& [thread,value]:threads_.items){(void)value;if(live(thread))return thread;}
    return 0;
}

void WindowsServices::discard_com_agile(GuestState& state,GuestMemory& memory,std::uint64_t id){
    auto& bridge=runtime_.com_agile;const auto found=bridge.operations.find(id);if(found==bridge.operations.end())return;
    auto op=std::move(found->second);bridge.operations.erase(found);
    const auto release=[&](std::uint64_t owner,std::uint64_t object){if(!object)return;
        const auto retirement=release_com_proxy_owned(state,memory,{owner,object});if(retirement)queue_com_proxy_retirement(memory,retirement);};
    if(op.temporary)release(threads_.current,op.temporary);
    if(op.source_owned)release(op.agile?threads_.current:op.owner,op.source);
    if(op.context_owned)release(threads_.current,op.context);
    if(op.scratch)(void)free_heap(memory,op.scratch,state);
    if(!op.stream_owned)return;
    if(!op.packet_owned){release(threads_.current,op.stream);return;}
    // 異常解開 callback 時，封送資料仍需在原公寓釋放；保存獨立清理工作。
    const auto owner=com_agile_owner(op.context,op.owner);
    if(!owner){release(threads_.current,op.stream);return;}
    ComAgileOperation cleanup;cleanup.kind=ComAgileOperation::Kind::Cleanup;cleanup.stream=op.stream;
    cleanup.stream_owned=true;cleanup.packet_owned=true;cleanup.owner=owner;cleanup.context=op.context;
    const auto token=bridge.next++;bridge.operations.emplace(token,cleanup);
    GuiSentMessage queued;queued.receiver=owner;queued.abandoned=true;
    queued.rpc=GuestCallback{resolve("internal-com-agile","Cleanup").value,{token}};
    gui_.sent_messages.emplace(gui_.next_object++,std::move(queued));
}

std::optional<std::uint32_t> WindowsServices::resume_com_agile(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t returned){
    using Kind=ComAgileOperation::Kind;auto& bridge=runtime_.com_agile;auto& op=bridge.operations.at(token);
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=bits/8U;
    const auto cell=[&](unsigned offset){return memory.load(GuestAddress{op.scratch+offset},bits);};
    const auto callback=[&](GuestCallback call,unsigned step){op.step=step;start_callbacks(state,memory,{std::move(call)},Continuation::ComAgileOperation,token);return std::optional<std::uint32_t>{};};
    const auto api=[&](std::string_view name,std::vector<std::uint64_t> args,unsigned step){return callback({resolve("ole32.dll",name).value,std::move(args)},step);};
    const auto seek=[&](unsigned step){GuestCallback call{method(memory,op.stream,5),{op.stream,0,0,0}};call.wide_arguments=2;return callback(std::move(call),step);};
    const auto finish=[&](std::uint32_t result){discard_com_agile(state,memory,token);complete_call(state,memory,result);return std::optional<std::uint32_t>{};};
    const auto cleanup=[&](std::uint32_t result){
        op.result=result;
        if(op.temporary){const auto object=op.temporary;op.temporary=0;return callback({method(memory,object,2),{object}},90);}
        if(op.stream_owned && op.packet_owned){
            const auto owner=com_agile_owner(op.context,op.owner);
            if(owner && owner!=threads_.current)return callback({resolve("internal-com-proxy","CallAt").value,
                {owner,resolve("internal-com-agile","ReleasePacket").value,token}},90);
            op.packet_owned=false;
            return seek(91);
        }
        if(op.stream_owned){op.stream_owned=false;return callback({method(memory,op.stream,2),{op.stream}},90);}
        if(op.source_owned){op.source_owned=false;
            if(op.agile || op.owner==threads_.current)return callback({method(memory,op.source,2),{op.source}},90);
            const auto owner=com_agile_owner(op.context,op.owner);
            if(owner)return callback({resolve("internal-com-proxy","ReleaseAt").value,{owner,op.source}},90);
        }
        if(op.context_owned){op.context_owned=false;return callback({method(memory,op.context,2),{op.context}},90);}
        return finish(result);
    };
    try{
        if(op.step==90 || op.step==92)return cleanup(op.result);
        if(op.step==91){if(failed(returned))return cleanup(op.result);return api("CoReleaseMarshalData",{op.stream},92);}
        if(op.kind==Kind::Cleanup)return cleanup(0);
        if(op.kind==Kind::Dispose)return cleanup(0);
        if(op.kind==Kind::Create){
            if(op.step==0){memory.write(GuestAddress{op.scratch+32},op.iid);return callback({method(memory,op.source,0),{op.source,op.scratch+32,op.scratch}},1);}
            if(op.step==1){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));op.temporary=cell(0);if(!op.temporary)return cleanup(no_interface);
                const auto object=op.temporary;op.temporary=0;return callback({method(memory,object,2),{object}},2);}
            if(op.step==2){memory.write(GuestAddress{op.scratch+32},id_from(com_agile_inomarshal));memory.store(GuestAddress{op.scratch},bits,0);
                return callback({method(memory,op.source,0),{op.source,op.scratch+32,op.scratch}},3);}
            if(op.step==3){if(!failed(returned)){op.temporary=cell(0);return cleanup(0x80004021U);} // CO_E_NOT_SUPPORTED
                if(returned!=no_interface)return cleanup(static_cast<std::uint32_t>(returned));
                memory.write(GuestAddress{op.scratch+32},id_from(com_agile_iagile));return callback({method(memory,op.source,0),{op.source,op.scratch+32,op.scratch}},4);}
            if(op.step==4){if(!failed(returned)){op.agile=true;op.temporary=cell(0);if(!op.temporary)return cleanup(no_interface);
                    const auto object=op.temporary;op.temporary=0;return callback({method(memory,object,2),{object}},5);}
                if(returned!=no_interface)return cleanup(static_cast<std::uint32_t>(returned));op.step=5;}
            if(op.step==5){memory.write(GuestAddress{op.scratch+32},id_from(com_agile_iunknown));return api("CoGetObjectContext",{op.scratch+32,op.scratch},6);}
            if(op.step==6){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));op.context=cell(0);op.context_owned=true;
                if(op.agile || op.options){return callback({method(memory,op.source,1),{op.source}},7);}
                return api("CreateStreamOnHGlobal",{0,1,op.scratch},8);}
            if(op.step==7){op.source_owned=true;op.step=10;}
            if(op.step==8){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));op.stream=cell(0);op.stream_owned=true;
                memory.write(GuestAddress{op.scratch+32},op.iid);return api("CoMarshalInterface",{op.stream,op.scratch+32,op.source,3,0,1},9);}
            if(op.step==9){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));op.packet_owned=true;op.step=10;}
            if(op.step==10){
                if(!bridge.table){const auto table=runtime_allocate(memory,{},unit*4);unsigned slot=0;
                    for(const auto name:{"QueryInterface","AddRef","Release","Resolve"})memory.store(GuestAddress{table+slot++*unit},bits,resolve("internal-com-agile",name).value);
                    bridge.table=table;}
                if(bridge.objects.size()>=4096)return cleanup(no_memory);
                const auto address=allocate_heap(memory,COM_AGILE_BYTES);if(!address)return cleanup(no_memory);
                try{
                    memory.fill(GuestAddress{address},0,COM_AGILE_BYTES);memory.store(GuestAddress{address},bits,bridge.table);
                    memory.store(GuestAddress{address+COM_AGILE_REFS},32,1);memory.store(GuestAddress{address+COM_AGILE_FLAGS},32,op.agile?1:0);
                    memory.store(GuestAddress{address+COM_AGILE_TARGET},bits,op.agile?op.source:0);
                    memory.store(GuestAddress{address+COM_AGILE_DESTROY},bits,resolve("internal-com-agile","Destroy").value);
                    // 慢路徑必須指向服務 gateway，不能再解析回同一個 lowered Resolve。
                    memory.store(GuestAddress{address+COM_AGILE_RESOLVE},bits,resolve("internal-com-agile","ResolveHost").value);
                    bridge.objects.emplace(address,ComAgileObject{op.source_owned?op.source:0,op.stream,op.context,op.owner,op.iid,op.agile,op.options!=0});
                    memory.store(GuestAddress{op.output},bits,address);
                }catch(...){bridge.objects.erase(address);(void)free_heap(memory,address,state);throw;}
                op.source_owned=false;op.stream_owned=false;op.packet_owned=false;op.context_owned=false;return finish(0);
            }
        }
        if(op.kind==Kind::Prepare){
            // 此工作由原公寓執行；每次建立獨立 stream，沒有跨執行緒共用游標。
            if(op.step==0)return callback({method(memory,op.source,1),{op.source}},1);
            if(op.step==1){op.source_owned=true;return api("CreateStreamOnHGlobal",{0,1,op.scratch},2);}
            if(op.step==2){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));op.stream=cell(0);op.stream_owned=true;
                memory.write(GuestAddress{op.scratch+32},op.iid);return api("CoMarshalInterface",{op.stream,op.scratch+32,op.source,3,0,1},3);}
            if(op.step==3){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));op.packet_owned=true;
                const auto parent=bridge.operations.find(op.parent);if(parent==bridge.operations.end())return cleanup(disconnected);
                parent->second.stream=op.stream;parent->second.stream_owned=true;parent->second.packet_owned=true;
                op.stream_owned=false;op.packet_owned=false;return cleanup(0);}
        }
        if(op.kind==Kind::Resolve){
            if(op.step==0){
                const auto found=bridge.objects.find(op.object);if(found==bridge.objects.end())return cleanup(disconnected);const auto entry=found->second;
                if(entry.agile)return callback({method(memory,entry.source,0),{entry.source,op.scratch+32,op.output}},20);
                // 取得目前 context 同時確認 COM 初始化；保留 input IID 快照。
                return api("CoGetContextToken",{op.scratch},1);
            }
            if(op.step==1){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));
                const auto found=bridge.objects.find(op.object);if(found==bridge.objects.end())return cleanup(disconnected);const auto entry=found->second;
                if(entry.delayed){const auto owner=com_agile_owner(entry.context,entry.owner);if(!owner || !entry.source)return cleanup(disconnected);
                    if(cell(0)==entry.context)return callback({method(memory,entry.source,0),{entry.source,op.scratch+32,op.output}},20);
                    return callback({resolve("internal-com-proxy","CallAt").value,{owner,resolve("internal-com-agile","Prepare").value,token}},3);
                }
                return callback({method(memory,entry.stream,13),{entry.stream,op.scratch}},2);
            }
            if(op.step==2){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));op.stream=cell(0);op.stream_owned=true;op.step=3;}
            if(op.step==3){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));if(!op.stream)return cleanup(pointer_error);return seek(4);}
            if(op.step==4){if(failed(returned))return cleanup(static_cast<std::uint32_t>(returned));return api("CoUnmarshalInterface",{op.stream,op.scratch+32,op.output},20);}
            if(op.step==20)return cleanup(static_cast<std::uint32_t>(returned));
        }
        return cleanup(invalid);
    }catch(const MemoryFault&){return cleanup(pointer_error);}catch(const std::bad_alloc&){return cleanup(no_memory);}
}

std::optional<std::uint32_t> WindowsServices::invoke_com_agile(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using Kind=ComAgileOperation::Kind;auto& bridge=runtime_.com_agile;const WindowsCall arg(state,memory,service);
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto name=service.symbol;
    const auto done=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto begin=[&](ComAgileOperation op){
        if(bridge.operations.size()>=4096)throw std::bad_alloc();
        if(op.kind!=Kind::Dispose && op.kind!=Kind::Cleanup){op.scratch=allocate_heap(memory,64);if(!op.scratch)throw std::bad_alloc();}
        const auto token=bridge.next++;try{if(op.scratch){memory.fill(GuestAddress{op.scratch},0,64);memory.write(GuestAddress{op.scratch+32},op.iid);}bridge.operations.emplace(token,op);}
        catch(...){if(op.scratch)(void)free_heap(memory,op.scratch,state);throw;}return resume_com_agile(state,memory,token);
    };
    try{
        if(name=="RoGetAgileReference"){
            if(arg(0)>1)return done(invalid);if(!arg(1) || !arg(2) || !arg(3))return done(pointer_error);
            ComAgileOperation op;op.kind=Kind::Create;op.options=static_cast<std::uint32_t>(arg(0));op.source=arg(2);op.output=arg(3);op.owner=threads_.current;
            memory.read(GuestAddress{arg(1)},op.iid);memory.store(GuestAddress{op.output},bits,0);
            if(!com_apartment(threads_.current) && !std::any_of(gui_.com_initializations.begin(),gui_.com_initializations.end(),[](const auto& row){return row.second>=16 && !(row.second&2U);}))return done(not_initialized);
            return begin(op);
        }
        if(name=="Cleanup"){
            const auto it=bridge.operations.find(arg(0));if(it==bridge.operations.end())return done(disconnected);
            return resume_com_agile(state,memory,arg(0));
        }
        if(name=="ReleasePacket"){
            const auto parent=bridge.operations.find(arg(0));if(parent==bridge.operations.end())return done(disconnected);
            // 封送 stream 是本層建立的內建 IStream。先取得獨立引用再轉移 packet，
            // 避免外層 caller 在回呼期間取消，提早釋放正在讀取的 stream。
            ComAgileOperation release;release.kind=Kind::Cleanup;release.stream=parent->second.stream;release.context=parent->second.context;release.owner=threads_.current;
            (void)call_guest_nested(state,memory,method(memory,release.stream,1),{release.stream});
            release.stream_owned=true;release.packet_owned=true;parent->second.packet_owned=false;
            try{return begin(release);}catch(...){parent->second.packet_owned=true;(void)call_guest_nested(state,memory,method(memory,release.stream,2),{release.stream});throw;}
        }
        if(name=="Prepare"){
            const auto parent=bridge.operations.find(arg(0));if(parent==bridge.operations.end())return done(disconnected);
            const auto found=bridge.objects.find(parent->second.object);if(found==bridge.objects.end() || !found->second.source)return done(disconnected);
            const auto entry=found->second;if(com_agile_owner(entry.context,entry.owner)!=threads_.current)return done(disconnected);
            ComAgileOperation op;op.kind=Kind::Prepare;op.parent=arg(0);op.source=entry.source;op.context=entry.context;op.owner=threads_.current;op.iid=entry.iid;return begin(op);
        }
        const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end())return done(name=="Release" || name=="AddRef" || name=="Destroy"?0:disconnected);
        const auto address=arg(0);const auto entry=found->second;
        if(name=="AddRef")return done(static_cast<std::uint32_t>(memory.atomic_rmw(GuestAddress{address+COM_AGILE_REFS},32,GuestMemory::AtomicRmw::Add,1)+1));
        if(name=="QueryInterface"){
            if(!arg(2))return done(pointer_error);WindowsGuid iid{};if(arg(1))memory.read(GuestAddress{arg(1)},iid);
            memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return done(invalid);
            if(iid!=id_from(com_agile_iunknown) && iid!=id_from(com_agile_iagile) && iid!=id_from(com_agile_ireference))return done(no_interface);
            (void)memory.atomic_rmw(GuestAddress{address+COM_AGILE_REFS},32,GuestMemory::AtomicRmw::Add,1);memory.store(GuestAddress{arg(2)},bits,address);return done(0);
        }
        if(name=="Release" || name=="Destroy"){
            if(name=="Release"){const auto refs=static_cast<std::uint32_t>(memory.atomic_rmw(GuestAddress{address+COM_AGILE_REFS},32,GuestMemory::AtomicRmw::Add,UINT32_MAX)-1);if(refs)return done(refs);}
            else if(memory.load(GuestAddress{address+COM_AGILE_REFS},32))return done(invalid);
            ComAgileOperation op;op.kind=Kind::Dispose;op.source=entry.source;op.stream=entry.stream;op.context=entry.context;op.owner=entry.owner;op.agile=entry.agile;
            op.source_owned=op.source!=0;op.stream_owned=op.packet_owned=op.stream!=0;op.context_owned=true;
            bridge.objects.erase(found);(void)free_heap(memory,address,state);return begin(op);
        }
        if(name=="Resolve" || name=="ResolveHost"){
            if(!arg(2))return done(pointer_error);ComAgileOperation op;op.kind=Kind::Resolve;op.object=address;op.output=arg(2);op.owner=entry.owner;op.context=entry.context;
            if(arg(1))memory.read(GuestAddress{arg(1)},op.iid);memory.store(GuestAddress{op.output},bits,0);if(!arg(1))return done(invalid);return begin(op);
        }
        return done(invalid);
    }catch(const MemoryFault&){return done(pointer_error);}catch(const std::bad_alloc&){return done(no_memory);}
}
}
