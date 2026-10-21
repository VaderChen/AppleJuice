#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "mf_interfaces.hpp"
#include "mf_h264.hpp"
#include "pcm_wave_file.hpp"
#include "mp4_video.hpp"
#include "x64arm/audio.hpp"
#include <future>
#include <chrono>
#include "d3d11_guids.hpp"
#include "x64arm/media_surface.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <set>

namespace x64arm {
namespace {
using mf::Guid;
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_interface=0x80004002U,
    missing=0xc00d36e6U,type_error=0xc00d36bdU,small=0xc00d36b1U,not_initialized=0xc00d36b6U,
    shutdown=0xc00d3e85U,no_events=0xc00d3e80U,invalid_index=0xc00d36bfU;
struct Failure {std::uint32_t code;};
struct Value {
    std::uint32_t type{};std::uint64_t scalar{};Guid id{};std::u16string text;std::vector<std::uint8_t> blob;
    bool operator==(const Value&)const=default;
};
struct ByteStreamState {
    std::shared_ptr<HostFile> file;
    std::uint64_t position{};std::uint32_t capabilities{};bool closed{};
};
struct SinkWriterState {
    std::shared_ptr<ByteStreamState> stream;
    std::uint64_t base{};
    std::optional<PcmWaveFormat> output,input;
    std::uint64_t bytes{},samples{},ticks{},last_time{},last_tick{},last_request{},started_at{};
    std::uint32_t header_size{},failure{};
    bool begun{},finalized{};
    bool mp4{},encode{};unsigned stride{};
    std::optional<video::EncoderConfig> video;
    std::unique_ptr<video::HardwareEncoder> encoder;
    std::vector<std::vector<std::uint8_t>> parameters;
    std::vector<mp4::Sample> video_samples;
    std::uint64_t encoded{},last_encoded{};
    struct Notice {bool final;std::uint64_t context;std::uint32_t status;};
    std::deque<Notice> notices;
};
struct Object {
    mf::Kind kind{};std::uint32_t refs{1};std::map<mf::Kind,std::uint64_t> pointers;std::map<Guid,Value> attributes;
    std::uint64_t allocation{},data{},parent{},state{},callback{},result_object{},queue_owner{};
    std::uint32_t maximum{},length{},locks{},flags{},status{},identifier{};
    std::optional<std::uint64_t> time,duration;
    Guid extended{};Value value;
    std::vector<std::uint64_t> children;std::vector<bool> selected;std::uint64_t current_type{};
    std::deque<std::uint64_t> events;bool closed{};
    std::optional<MediaSurface> surface;bool bottom_up{};std::uint32_t locks_2d{},lock_flags{},reset_token{};
    std::uint64_t generation{},lock_owner{},lock_handle{};
    std::map<std::uint64_t,std::uint64_t> device_handles;
    std::uint64_t attribute_store{},input_attributes{},output_attributes{},output_type{};
    std::shared_ptr<video::HardwareDecoder> decoder;unsigned nal_length_size{4};
    std::future<std::vector<video::DecodedFrame>> decoding;std::deque<video::DecodedFrame> frames;
    std::shared_ptr<video::HardwareEncoder> encoder;
    std::future<std::vector<video::EncodedPacket>> encoding;std::deque<video::EncodedPacket> packets;
    bool streaming{},draining{},drain_pending{},codec_drained{},job_held{},poll_queued{},request_pending{},disconnect_source{};
    unsigned input_credit{},output_credit{};std::int64_t lower_bound{INT64_MIN},upper_bound{INT64_MAX};
    std::shared_ptr<audio::Buffer> capture;std::unique_ptr<audio::Input> input;std::uint64_t read_frames{},pending_token{};
    std::shared_ptr<SinkWriterState> writer;
    std::shared_ptr<ByteStreamState> byte_stream;

};
struct Entry {std::shared_ptr<Object> object;mf::Kind kind;};
}
struct MediaFoundationState {
    std::map<std::uint64_t,Entry> objects;std::map<mf::Kind,std::uint64_t> tables;
    std::vector<std::uint32_t> startups;
    std::uint64_t shared_manager{},next_device_handle{0x10000},next_query{1};std::uint32_t manager_locks{},next_reset_token{1};
    struct Query {std::shared_ptr<Object> manager;std::uint64_t handle{},generation{},output{},owner{};bool locking{};std::uint64_t scratch{},service_pointer{};unsigned service_stage{};std::uint32_t service_status{};};
    std::map<std::uint64_t,Query> queries;
};
bool WindowsServices::media_foundation_ready(std::uint64_t pointer,std::uint64_t thread) const {
    (void)thread;
    if(!media_foundation_)return true;
    const auto found=media_foundation_->objects.find(pointer);
    if(found==media_foundation_->objects.end())return true;const auto& item=*found->second.object;
    if(item.kind==mf::Reader){const auto source=media_foundation_->objects.find(item.parent);if(source==media_foundation_->objects.end() || source->second.object->closed)return true;const auto& capture=source->second.object->capture;if(!capture)return true;std::lock_guard lock(capture->mutex);return capture->capture_failed || capture->captured_frames>item.read_frames;}
    return item.kind==mf::Manager?!item.lock_handle:item.closed || !item.events.empty();
}
bool WindowsServices::media_foundation_waiting() const {
    if(!media_foundation_)return false;for(const auto& [id,thread]:threads_.items){(void)id;if(thread.status==ThreadStatus::Waiting && thread.wait.kind==ThreadWaitKind::MediaFoundation && !thread.wait.handles.empty()){const auto found=media_foundation_->objects.find(thread.wait.handles.front());if(found!=media_foundation_->objects.end() && found->second.object->kind==mf::Reader)return true;}}for(const auto& [pointer,entry]:media_foundation_->objects){(void)pointer;if(entry.kind==entry.object->kind && entry.object->job_held)return true;}return false;
}
void WindowsServices::poll_media_foundation(GuestMemory& memory){
    if(!media_foundation_ || threads_.items.empty() || registered_waits_.size()>=64)return;
    std::vector<std::uint64_t> ready;
    for(const auto& [pointer,entry]:media_foundation_->objects){
        const auto& item=*entry.object;if(entry.kind!=item.kind || !item.job_held || item.poll_queued)continue;
        bool complete=item.closed || !item.request_pending;
        if(item.kind==mf::Transform)complete=(!item.decoding.valid() || item.decoding.wait_for(std::chrono::seconds(0))==std::future_status::ready) && (!item.encoding.valid() || item.encoding.wait_for(std::chrono::seconds(0))==std::future_status::ready);
        else if(item.request_pending){const auto found=media_foundation_->objects.find(item.parent);if(found==media_foundation_->objects.end() || found->second.object->closed)complete=true;
            else if(const auto capture=found->second.object->capture){std::lock_guard lock(capture->mutex);complete=capture->capture_failed || capture->captured_frames>item.read_frames;}}
        if(complete)ready.push_back(pointer);
    }
    for(const auto pointer:ready){
        if(registered_waits_.size()>=64)break;const auto saved_error=last_error_;
        const auto worker=acquire_wait_worker(threads_.items.at(threads_.current).context,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));last_error_=saved_error;if(!worker)break;
        RegisteredWait work;work.worker=worker;work.callback_batch.push_back({resolve("internal-mf","PipelinePoll").value,{pointer}});work.armed=false;work.ready=true;work.closing=true;work.work_item=true;
        const auto id=next_registered_wait_++;registered_waits_.emplace(id,std::move(work));wait_workers_[worker]=id;media_foundation_->objects.at(pointer).object->poll_queued=true;
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
    }
}
void WindowsServices::discard_media_foundation_query(std::uint64_t token,GuestMemory& memory,GuestState& state){
    if(!media_foundation_)return;const auto found=media_foundation_->queries.find(token);if(found==media_foundation_->queries.end())return;
    const auto query=found->second;media_foundation_->queries.erase(found);
    if(query.scratch)(void)free_heap(memory,query.scratch,state);
    if(query.locking && query.manager->generation==query.generation && query.manager->lock_handle==query.handle && query.manager->lock_owner==query.owner)query.manager->lock_owner=query.manager->lock_handle=0;
}
std::optional<std::uint32_t> WindowsServices::resume_media_foundation_query(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint32_t status){
    const auto found=media_foundation_->queries.find(token);if(found==media_foundation_->queries.end())throw HostCallFault("MF 裝置查詢續行不存在");const auto query=found->second;
    if(query.scratch){
        const auto bits=static_cast<std::uint8_t>(x86_?32:64),unit=static_cast<std::uint8_t>(bits/8);
        const auto method=[&](std::uint64_t pointer,unsigned slot){const auto table=memory.load(GuestAddress{pointer},bits);return memory.load(GuestAddress{table+slot*unit},bits);};
        try {
            if(query.service_stage==0 && !(status&0x80000000U)){
                const auto pointer=memory.load(GuestAddress{query.scratch+16},bits);
                if(pointer){auto& pending=found->second;pending.service_pointer=pointer;pending.service_stage=1;
                    start_callbacks(state,memory,{{method(pointer,3),{pointer,query.scratch+24,query.scratch+40,query.output}}},Continuation::MediaFoundationQuery,token);return std::nullopt;}
                status=0x80004002U;
            }else if(query.service_stage==1){
                found->second.service_status=status;found->second.service_stage=2;
                start_callbacks(state,memory,{{method(query.service_pointer,2),{query.service_pointer}}},Continuation::MediaFoundationQuery,token);return std::nullopt;
            }else if(query.service_stage==2)status=query.service_status;
        }catch(...){discard_media_foundation_query(token,memory,state);throw;}
        discard_media_foundation_query(token,memory,state);complete_call(state,memory,status);return std::nullopt;
    }
    if(!(status&0x80000000U) && query.manager->generation!=query.generation){
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto pointer=memory.load(GuestAddress{query.output},bits);memory.store(GuestAddress{query.output},bits,0);status=0x80041001U;discard_media_foundation_query(token,memory,state);
        if(pointer){const auto table=memory.load(GuestAddress{pointer},bits),release=memory.load(GuestAddress{table+2*(bits/8U)},bits);start_callbacks(state,memory,{{release,{pointer}}},Continuation::Return,status);return std::nullopt;}
    }else if(status&0x80000000U)discard_media_foundation_query(token,memory,state);
    else media_foundation_->queries.erase(found);
    complete_call(state,memory,status);return std::nullopt;
}
std::span<const WindowsServiceDescriptor> media_foundation_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> result{
            {"mfplat.dll","MFStartup",2,0,0,3},{"mfplat.dll","MFShutdown",0},
            {"mfplat.dll","MFGetSystemTime",0},{"mfplat.dll","MFFrameRateToAverageTimePerFrame",3,0,0,3},
            {"mfplat.dll","MFCreateAttributes",2,0,0,2},{"mfplat.dll","MFCreateMediaType",1},
            {"mfplat.dll","MFCreateSample",1},{"mfplat.dll","MFCreateMemoryBuffer",2,0,0,1},
            {"mfplat.dll","MFCreateAlignedMemoryBuffer",3,0,0,3},{"mfplat.dll","MFCreateMediaBufferWrapper",4,0,0,6},
            {"mfplat.dll","MFCreateAsyncResult",4},{"mfplat.dll","MFInvokeCallback",1},
            {"mfplat.dll","MFCreateMediaEvent",5,0,0,5},{"mfplat.dll","MFCreateEventQueue",1},
            {"mfplat.dll","MFCreateStreamDescriptor",4,0,0,3},{"mfplat.dll","MFCreatePresentationDescriptor",3,0,0,1},
            {"mfplat.dll","MFInitMediaTypeFromWaveFormatEx",3,0,0,4},{"mfplat.dll","MFCreateWaveFormatExFromMFMediaType",4,0,0,8},
            {"mfplat.dll","MFCreateDXGIDeviceManager",2},{"mfplat.dll","MFLockDXGIDeviceManager",2},{"mfplat.dll","MFUnlockDXGIDeviceManager",0},
            {"mfplat.dll","MFCreateDXGISurfaceBuffer",5,0,0,12},
            {"mf.dll","MFGetService",4},{"mfplat.dll","MFTEnum",7,0,1,2},{"mfplat.dll","MFTEnumEx",6,0,1,2},{"mf.dll","MFEnumDeviceSources",3},{"mf.dll","MFCreateDeviceSource",2},
            {"mfreadwrite.dll","MFCreateSourceReaderFromMediaSource",3},{"mfreadwrite.dll","MFCreateSinkWriterFromURL",4},{"mfplat.dll","MFCreateFile",5,0,0,7},{"internal-mf","PipelinePoll",1}};
        std::set<std::string_view> names;const auto append=[&](const auto& methods){for(const auto& m:methods)if(names.insert(m.name).second)result.push_back({"internal-mf",m.name,m.args,m.wide,m.aggregate,m.narrow});};
        append(mf::common);for(unsigned k=0;k<=mf::ByteStream;++k)append(mf::methods(static_cast<mf::Kind>(k)));
        return result;
    }();return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_media_foundation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!media_foundation_)media_foundation_=std::make_shared<MediaFoundationState>();auto& bridge=*media_foundation_;
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=bits/8U;
    const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto require=[](bool test,std::uint32_t code=invalid){if(!test)throw Failure{code};};
    const auto check=[&](std::uint64_t p,std::size_t size){require(p!=0,pointer_error);memory.check(GuestAddress{p},size,Access::Write);};
    const auto put=[&](std::uint64_t p,std::uint64_t v,std::uint8_t width=32){check(p,width/8);memory.store(GuestAddress{p},width,v);};
    const auto out=[&](std::uint64_t p){put(p,0,bits);};
    const auto guid=[&](std::uint64_t p){require(p!=0,pointer_error);Guid id{};memory.read(GuestAddress{p},id);return id;};
    const auto data=[&](std::uint64_t p,std::size_t size){require(size<=64U*1024U*1024U);std::vector<std::uint8_t> bytes(size);if(size){require(p!=0,pointer_error);memory.read(GuestAddress{p},bytes);}return bytes;};
    const auto allocate=[&](std::size_t size){require(size<=256U*1024U*1024U);const auto p=allocate_heap(memory,std::max<std::size_t>(size,1));if(!p)throw std::bad_alloc{};return p;};
    const auto entry=[&](std::uint64_t p)->Entry&{const auto found=bridge.objects.find(p);require(found!=bridge.objects.end(),no_interface);return found->second;};
    const auto object=[&](std::uint64_t p,mf::Kind kind){auto item=entry(p).object;require(item->kind==kind,no_interface);return item;};
    std::vector<GuestCallback> callbacks;
    const auto foreign_method=[&](std::uint64_t p,unsigned slot){require(p!=0,pointer_error);const auto table=memory.load(GuestAddress{p},bits);return memory.load(GuestAddress{table+slot*unit},bits);};
    const auto hold=[&](std::uint64_t p){if(!p)return;if(const auto found=bridge.objects.find(p);found!=bridge.objects.end()){require(found->second.object->refs<UINT32_MAX);++found->second.object->refs;}else callbacks.push_back({foreign_method(p,1),{p}});};
    std::function<void(std::uint64_t)> release;
    std::function<void(const std::shared_ptr<Object>&)> close_source;
    release=[&](std::uint64_t p){
        if(!p)return;const auto found=bridge.objects.find(p);if(found==bridge.objects.end()){callbacks.push_back({foreign_method(p,2),{p}});return;}
        auto item=found->second.object;if(--item->refs)return;
        if(item->kind==mf::Reader && !item->disconnect_source && item->parent && bridge.objects.contains(item->parent) && close_source)close_source(bridge.objects.at(item->parent).object);
        for(const auto& [kind,pointer]:item->pointers){(void)kind;bridge.objects.erase(pointer);runtime_.critical_sections.erase(pointer);(void)free_heap(memory,pointer,state);}
        for(const auto& [key,value]:item->attributes){(void)key;if(value.type==13)release(value.scalar);}
        if(item->value.type==13)release(item->value.scalar);
        release(item->attribute_store);release(item->input_attributes);release(item->output_attributes);release(item->output_type);release(item->pending_token);
        release(item->parent);release(item->state);release(item->callback);release(item->result_object);release(item->current_type);
        for(const auto child:item->children)release(child);for(const auto event:item->events)release(event);
        if(item->allocation)(void)free_heap(memory,item->allocation,state);
    };
    const auto create=[&](const std::shared_ptr<Object>& item,mf::Kind kind){
        if(item->pointers.contains(kind))return item->pointers.at(kind);
        require(bridge.objects.size()<16384,0x8007000eU);
        if(!bridge.tables.contains(kind)){
            std::vector<mf::Method> methods(mf::common,mf::common+3);
            if(mf::attributes(kind) && kind!=mf::Attributes)methods.insert(methods.end(),std::begin(mf::methods_Attributes),std::end(mf::methods_Attributes));
            const auto extra=mf::methods(kind);methods.insert(methods.end(),extra.begin(),extra.end());
            const auto table=runtime_allocate(memory,{},methods.size()*unit);
            for(std::size_t n=0;n<methods.size();++n)put(table+n*unit,resolve("internal-mf",methods[n].name).value,bits);
            bridge.tables.emplace(kind,table);
        }
        const auto p=allocate(unit);put(p,bridge.tables.at(kind),bits);item->pointers.emplace(kind,p);bridge.objects.emplace(p,Entry{item,kind});return p;
    };
    const auto make=[&](mf::Kind kind){auto item=std::make_shared<Object>();item->kind=kind;return create(item,kind);};
    const auto query=[&](std::uint64_t p,const Guid& iid,std::uint64_t output){
        out(output);const auto& source=entry(p);auto item=source.object;std::uint64_t pointer=0;
        if(iid==mf::unknown || iid==mf::iid(item->kind) || (iid==mf::iid_Attributes && mf::attributes(item->kind)))pointer=item->pointers.at(item->kind);
        else if(iid==mf::iid_Generator && (item->kind==mf::Transform || item->kind==mf::Source || item->kind==mf::MediaStream))pointer=create(item,mf::Generator);
        else if(iid==mf::iid_Shutdown && item->kind==mf::Transform)pointer=create(item,mf::Shutdown);
        else if(item->kind==mf::Stream && iid==mf::iid_TypeHandler)pointer=create(item,mf::TypeHandler);
        else if(item->surface && (iid==mf::iid_Surface || iid==mf::iid_TwoD || iid==mf::iid_TwoD2))pointer=create(item,iid==mf::iid_Surface?mf::Surface:iid==mf::iid_TwoD?mf::TwoD:mf::TwoD2);
        else throw Failure{no_interface};hold(pointer);put(output,pointer,bits);
    };
    const auto read_value=[&](std::uint64_t p){
        require(p!=0,pointer_error);memory.check(GuestAddress{p},x86_?16:24,Access::Read);Value v;v.type=static_cast<std::uint32_t>(memory.load(GuestAddress{p},16));
        switch(v.type){case 0:break;case 19:v.scalar=memory.load(GuestAddress{p+8},32);break;case 20:case 21:case 5:v.scalar=memory.load(GuestAddress{p+8},64);break;
        case 13:v.scalar=memory.load(GuestAddress{p+8},bits);break;case 72:v.id=guid(memory.load(GuestAddress{p+8},bits));break;
        case 31:v.text=read_utf16(memory,GuestAddress{memory.load(GuestAddress{p+8},bits)});break;
        case 0x1011:{const auto size=memory.load(GuestAddress{p+8},32);v.blob=data(memory.load(GuestAddress{p+(x86_?12:16)},bits),size);break;}
        default:throw Failure{type_error};}return v;
    };
    const auto write_value=[&](std::uint64_t p,const Value& v){
        check(p,x86_?16:24);memory.write(GuestAddress{p},std::vector<std::uint8_t>(x86_?16:24));put(p,v.type,16);
        switch(v.type){case 0:break;case 19:put(p+8,v.scalar);break;case 20:case 21:case 5:put(p+8,v.scalar,64);break;case 13:hold(v.scalar);put(p+8,v.scalar,bits);break;
        case 72:{const auto id=allocate(16);memory.write(GuestAddress{id},v.id);put(p+8,id,bits);break;}
        case 31:{const auto bytes=utf16_bytes(v.text);const auto text=allocate(bytes.size());memory.write(GuestAddress{text},bytes);put(p+8,text,bits);break;}
        case 0x1011:{const auto blob=allocate(v.blob.size());memory.write(GuestAddress{blob},v.blob);put(p+8,v.blob.size());put(p+(x86_?12:16),blob,bits);break;}
        default:throw Failure{type_error};}
    };
    const auto set=[&](Object& item,const Guid& key,Value value){
        require(value.type==19 || value.type==21 || value.type==5 || value.type==72 || value.type==31 || value.type==0x1011 || value.type==13,type_error);
        require(item.attributes.size()<65536 || item.attributes.contains(key),0x8007000eU);
        if(value.type==13)hold(value.scalar);if(const auto old=item.attributes.find(key);old!=item.attributes.end() && old->second.type==13)release(old->second.scalar);
        item.attributes.insert_or_assign(key,std::move(value));
    };
    const auto get=[&](const Object& item,const Guid& key,std::uint32_t type=0)->const Value&{const auto found=item.attributes.find(key);require(found!=item.attributes.end(),missing);require(!type || type==found->second.type,type_error);return found->second;};
    const auto memory_buffer=[&](std::uint32_t size,std::uint32_t mask){
        require(mask<=8191 && (mask&(mask+1))==0);const auto p=make(mf::Buffer);auto item=object(p,mf::Buffer);item->allocation=allocate(static_cast<std::size_t>(size)+mask);item->data=(item->allocation+mask)&~static_cast<std::uint64_t>(mask);item->maximum=size;return p;
    };
    const auto flip=[](std::vector<std::uint8_t>& bytes,const Object& item){if(item.bottom_up && item.surface->format!=103){const auto pitch=item.surface->row_pitch,height=item.surface->height;for(unsigned y=0;y<height/2;++y)for(unsigned x=0;x<pitch;++x)std::swap(bytes[static_cast<std::size_t>(y)*pitch+x],bytes[static_cast<std::size_t>(height-y-1)*pitch+x]);}};
    std::function<void(Object&,bool,bool)> map_buffer;
    map_buffer=[&](Object& item,bool linear,bool write_only){
        if(item.surface){
            if(!item.allocation){item.allocation=allocate(item.maximum);item.data=item.allocation;}
            if(!item.locks && !item.locks_2d && !write_only){auto bytes=item.surface->read();require(bytes.size()==item.maximum,0x80004005U);if(linear)flip(bytes,item);memory.write(GuestAddress{item.data},bytes);}
        }else if(item.parent && bridge.objects.contains(item.parent) && entry(item.parent).object->kind==mf::Buffer){auto parent=entry(item.parent).object;map_buffer(*parent,linear,write_only);item.data=parent->data+item.identifier;}
    };
    std::function<void(Object&,bool)> flush_buffer;
    flush_buffer=[&](Object& item,bool linear){if(item.surface){auto bytes=data(item.data,item.maximum);if(linear)flip(bytes,item);item.surface->write(bytes);}else if(item.parent && bridge.objects.contains(item.parent) && entry(item.parent).object->kind==mf::Buffer)flush_buffer(*entry(item.parent).object,linear);};
    const auto sample_bytes=[&](const Object& sample){std::vector<std::uint8_t> result;for(const auto p:sample.children){const auto buffer=object(p,mf::Buffer);map_buffer(*buffer,true,false);require(result.size()+buffer->length<=256U*1024U*1024U);const auto part=data(buffer->data,buffer->length);result.insert(result.end(),part.begin(),part.end());}return result;};
    const auto event=[&](std::uint32_t type,const Guid& extended,std::uint32_t status,Value value){const auto p=make(mf::Event);auto item=object(p,mf::Event);item->identifier=type;item->extended=extended;item->status=status;item->value=std::move(value);if(item->value.type==13)hold(item->value.scalar);return p;};
    const auto deliver=[&](std::uint64_t queue_pointer,const std::shared_ptr<Object>& queue){
        if(!queue->callback || queue->events.empty())return;
        const auto p=make(mf::Async);auto result=object(p,mf::Async);result->queue_owner=queue_pointer;result->result_object=queue->events.front();queue->events.pop_front();result->state=queue->state;result->callback=queue->callback;queue->state=queue->callback=0;
        callbacks.push_back({foreign_method(result->callback,4),{result->callback,p}});
        // callback 期間保留 result；callback 結束後才釋放佇列持有的引用。
        callbacks.push_back({resolve("internal-mf","Release").value,{p}});
    };
    #include "mf_pipeline_helpers.inc"
    #include "mf_sink_helpers.inc"
    std::uint64_t result=0;
    try {
        #include "mf_bytestream_exports.inc"
        else
        #include "mf_sink_exports.inc"
        else
        #include "mf_pipeline_exports.inc"
        else
        if(name=="MFGetService"){
            out(arg(3));require(arg(0)!=0,pointer_error);const auto service_id=guid(arg(1)),iid=guid(arg(2));
            const auto scratch=allocate(56);const auto token=bridge.next_query++;
            MediaFoundationState::Query pending;pending.output=arg(3);pending.scratch=scratch;bridge.queries.emplace(token,pending);
            try {
                memory.write(GuestAddress{scratch},mf::guid(0xfa993888,0x4383,0x415a,{0xa9,0x30,0xdd,0x47,0x2a,0x8c,0xf6,0xf7}));
                put(scratch+16,0,bits);memory.write(GuestAddress{scratch+24},service_id);memory.write(GuestAddress{scratch+40},iid);
                start_callbacks(state,memory,{{foreign_method(arg(0),0),{arg(0),scratch,scratch+16}}},Continuation::MediaFoundationQuery,token);
            }catch(...){discard_media_foundation_query(token,memory,state);throw;}return std::nullopt;
        }else if(name=="MFStartup"){
            require(d(0)==0x20070U || d(0)==0x10070U,0xc00d36e3U);require(d(1)<=1);require(bridge.startups.size()<65536,0x8007000eU);
            if(!d(1)){require(runtime_.network.startups<UINT32_MAX,0x8007000eU);++runtime_.network.startups;}
            bridge.startups.push_back(d(1));
        }else if(name=="MFShutdown"){
            require(!bridge.startups.empty(),not_initialized);const auto flags=bridge.startups.back();bridge.startups.pop_back();if(!flags && runtime_.network.startups)--runtime_.network.startups;
        }else if(name=="MFGetSystemTime")result=runtime_.clock_ticks;
        else if(name=="MFFrameRateToAverageTimePerFrame"){require(d(0)!=0 && d(1)!=0);put(arg(2),(UINT64_C(10000000)*d(1)+d(0)/2)/d(0),64);}
        else if(name=="MFCreateAttributes" || name=="MFCreateMediaType" || name=="MFCreateSample" || name=="MFCreateEventQueue"){
            out(arg(0));if(name=="MFCreateAttributes")require(d(1)<=65536);
            put(arg(0),make(name=="MFCreateAttributes"?mf::Attributes:name=="MFCreateMediaType"?mf::MediaType:name=="MFCreateSample"?mf::Sample:mf::Queue),bits);
        }else if(name=="MFCreateMemoryBuffer" || name=="MFCreateAlignedMemoryBuffer"){
            const auto output=arg(name=="MFCreateMemoryBuffer"?1:2);out(output);put(output,memory_buffer(d(0),name=="MFCreateMemoryBuffer"?0:d(1)),bits);
        }else if(name=="MFCreateMediaBufferWrapper"){
            out(arg(3));auto parent=object(arg(0),mf::Buffer);require(d(1)<=parent->length && d(2)<=parent->length-d(1));map_buffer(*parent,true,false);const auto p=make(mf::Buffer);auto item=object(p,mf::Buffer);hold(arg(0));item->parent=arg(0);item->identifier=d(1);item->data=parent->data+d(1);item->length=item->maximum=d(2);put(arg(3),p,bits);
        }else if(name=="MFCreateAsyncResult"){
            out(arg(3));const auto p=make(mf::Async);auto item=object(p,mf::Async);hold(arg(0));hold(arg(1));hold(arg(2));item->result_object=arg(0);item->callback=arg(1);item->state=arg(2);put(arg(3),p,bits);
        }else if(name=="MFInvokeCallback"){
            auto item=object(arg(0),mf::Async);require(item->callback!=0);callbacks.push_back({foreign_method(item->callback,4),{item->callback,arg(0)}});
        }else if(name=="MFCreateMediaEvent"){out(arg(4));put(arg(4),event(d(0),guid(arg(1)),d(2),arg(3)?read_value(arg(3)):Value{}),bits);}
        else if(name=="MFCreateStreamDescriptor" || name=="MFCreatePresentationDescriptor"){
            const bool stream=name=="MFCreateStreamDescriptor";const auto count=d(stream?1:0);const auto list=arg(stream?2:1),output=arg(stream?3:2);out(output);require(count && count<=65536);memory.check(GuestAddress{list},count*unit,Access::Read);
            std::vector<std::uint64_t> children;for(unsigned n=0;n<count;++n){const auto child=memory.load(GuestAddress{list+n*unit},bits);(void)object(child,stream?mf::MediaType:mf::Stream);children.push_back(child);}
            const auto p=make(stream?mf::Stream:mf::Presentation);auto item=entry(p).object;item->identifier=stream?d(0):0;item->children=children;item->selected.resize(count,false);for(const auto child:children)hold(child);if(stream){item->current_type=children.front();hold(item->current_type);}put(output,p,bits);
        }else if(name=="MFInitMediaTypeFromWaveFormatEx"){
            auto item=object(arg(0),mf::MediaType);require(d(2)>=16);const auto bytes=data(arg(1),d(2));const auto load=[&](unsigned at,unsigned width){require(at+width<=bytes.size());std::uint32_t v=0;for(unsigned n=0;n<width;++n)v|=static_cast<std::uint32_t>(bytes[at+n])<<(8*n);return v;};
            const auto tag=load(0,2),channels=load(2,2),rate=load(4,4),avg=load(8,4),align=load(12,2),depth=load(14,2);Guid subtype=mf::guid(tag,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71});std::uint32_t mask=0;
            require(channels && rate && align);if(tag==0xfffe){require(bytes.size()>=40 && load(16,2)>=22);std::copy_n(bytes.begin()+24,16,subtype.begin());mask=load(20,4);}else if(bytes.size()>=18)require(18+load(16,2)<=bytes.size());
            const bool pcm=subtype==mf::pcm || subtype==mf::ieee;if(pcm)require(depth && align==(channels*depth+7)/8 && avg==static_cast<std::uint64_t>(rate)*align);
            Value v;v.type=72;v.id=mf::MFMediaType_Audio;set(*item,mf::MF_MT_MAJOR_TYPE,v);v.id=subtype;set(*item,mf::MF_MT_SUBTYPE,v);v={};v.type=19;
            for(const auto& pair:std::array<std::pair<Guid,std::uint32_t>,9>{{{mf::MF_MT_AUDIO_NUM_CHANNELS,channels},{mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,rate},{mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,avg},{mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,align},{mf::MF_MT_AUDIO_BITS_PER_SAMPLE,depth},{mf::MF_MT_AUDIO_CHANNEL_MASK,mask},{mf::MF_MT_COMPRESSED,pcm?0U:1U},{mf::MF_MT_FIXED_SIZE_SAMPLES,pcm?1U:0U},{mf::MF_MT_SAMPLE_SIZE,align}}}){v.scalar=pair.second;set(*item,pair.first,v);}
        }else if(name=="MFCreateWaveFormatExFromMFMediaType"){
            out(arg(1));put(arg(2),0);auto item=object(arg(0),mf::MediaType);require(d(3)<=1);require(get(*item,mf::MF_MT_MAJOR_TYPE,72).id==mf::MFMediaType_Audio,0xc00d36b4U);const auto subtype=get(*item,mf::MF_MT_SUBTYPE,72).id;
            require(subtype==mf::pcm || subtype==mf::ieee,0xc00d36b4U);const auto channels=get(*item,mf::MF_MT_AUDIO_NUM_CHANNELS,19).scalar,rate=get(*item,mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,19).scalar,align=get(*item,mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,19).scalar,depth=get(*item,mf::MF_MT_AUDIO_BITS_PER_SAMPLE,19).scalar,avg=get(*item,mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,19).scalar;
            require(channels && channels<=UINT16_MAX && depth && depth<=UINT16_MAX && align<=UINT16_MAX && avg==rate*align);const bool extended=!d(3);const auto size=extended?40U:18U;const auto p=allocate(size);memory.write(GuestAddress{p},std::vector<std::uint8_t>(size));put(p,extended?0xfffe:subtype==mf::pcm?1:3,16);put(p+2,channels,16);put(p+4,rate);put(p+8,avg);put(p+12,align,16);put(p+14,depth,16);put(p+16,extended?22:0,16);if(extended){put(p+18,depth,16);const auto mask=item->attributes.find(mf::MF_MT_AUDIO_CHANNEL_MASK);put(p+20,mask==item->attributes.end()?0:mask->second.scalar);memory.write(GuestAddress{p+24},subtype);}put(arg(1),p,bits);put(arg(2),size);
        }else if(name=="MFCreateDXGIDeviceManager" || name=="MFLockDXGIDeviceManager"){
            check(arg(0),4);out(arg(1));const bool shared=name=="MFLockDXGIDeviceManager";require(!shared || bridge.manager_locks<UINT32_MAX,0x8007000eU);
            auto p=shared?bridge.shared_manager:0;if(!p){require(bridge.next_reset_token!=0,0x8007000eU);p=make(mf::Manager);object(p,mf::Manager)->reset_token=bridge.next_reset_token++;if(shared)bridge.shared_manager=p;}
            if(shared){++bridge.manager_locks;hold(p);}put(arg(0),object(p,mf::Manager)->reset_token);put(arg(1),p,bits);
        }else if(name=="MFUnlockDXGIDeviceManager"){
            require(bridge.manager_locks!=0,0xc00d36b2U);if(!--bridge.manager_locks){const auto p=bridge.shared_manager;bridge.shared_manager=0;release(p);}
        }else if(name=="MFCreateDXGISurfaceBuffer"){
            out(arg(4));require(guid(arg(0))==d3d11_guid::IID_ID3D11Texture2D,no_interface);auto surface=media_graphics_surface(arg(1),d(2));require(surface.has_value(),invalid);const auto p=make(mf::Buffer);auto item=object(p,mf::Buffer);item->surface=std::move(surface);item->bottom_up=d(3)!=0;item->parent=arg(1);item->identifier=d(2);item->maximum=item->length=item->surface->size;hold(arg(1));put(arg(4),p,bits);
        }else {
            auto item=entry(arg(0)).object;
            if(name=="QueryInterface")query(arg(0),guid(arg(1)),arg(2));
            else if(name=="AddRef"){hold(arg(0));result=item->refs;}
            else if(name=="Release"){result=item->refs-1;release(arg(0));}
            #include "mf_bytestream_methods.inc"
            #include "mf_sink_methods.inc"
            #include "mf_pipeline_methods.inc"
            else if(name.starts_with("A_")){
                require(mf::attributes(item->kind),no_interface);
                auto& lock=runtime_.critical_sections[item->pointers.at(item->kind)];
                if(lock.owner && lock.owner!=threads_.current){auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Critical;thread.wait.handles={item->pointers.at(item->kind)};schedule(state,memory);return std::nullopt;}
                const auto method=name.substr(2);
                if(method=="LockStore"){lock.owner=threads_.current;require(lock.recursion<UINT32_MAX);++lock.recursion;}
                else if(method=="UnlockStore"){require(lock.owner==threads_.current && lock.recursion,0x8000ffffU);if(!--lock.recursion)lock.owner=0;}
                else if(method=="GetCount")put(arg(1),item->attributes.size());
                else if(method=="DeleteAllItems"){for(const auto& [key,value]:item->attributes){(void)key;if(value.type==13)release(value.scalar);}item->attributes.clear();}
                else if(method=="DeleteItem"){const auto found=item->attributes.find(guid(arg(1)));if(found!=item->attributes.end()){if(found->second.type==13)release(found->second.scalar);item->attributes.erase(found);}}
                else if(method=="GetItemByIndex"){require(d(1)<item->attributes.size(),invalid_index);check(arg(2),16);const auto at=std::next(item->attributes.begin(),d(1));memory.write(GuestAddress{arg(2)},at->first);write_value(arg(3),at->second);}
                else if(method=="CopyAllItems"){auto destination=entry(arg(1)).object;require(mf::attributes(destination->kind),no_interface);const auto copy=item->attributes;for(const auto& [key,value]:copy)set(*destination,key,value);}
                else if(method=="Compare"){
                    auto theirs=entry(arg(1)).object;require(mf::attributes(theirs->kind),no_interface);require(d(2)<=4);const auto subset=[](const auto& a,const auto& b){return std::all_of(a.begin(),a.end(),[&](const auto& value){const auto found=b.find(value.first);return found!=b.end() && found->second==value.second;});};bool equal=false;
                    switch(d(2)){case 0:equal=subset(item->attributes,theirs->attributes);break;case 1:equal=subset(theirs->attributes,item->attributes);break;case 2:equal=item->attributes==theirs->attributes;break;case 3:equal=item->attributes.size()<theirs->attributes.size()?subset(item->attributes,theirs->attributes):subset(theirs->attributes,item->attributes);break;case 4:equal=std::all_of(item->attributes.begin(),item->attributes.end(),[&](const auto& value){const auto found=theirs->attributes.find(value.first);return found==theirs->attributes.end() || found->second==value.second;});break;}put(arg(3),equal);
                }else {
                    const auto key=guid(arg(1));
                    if(method.starts_with("Set")){
                        Value v;if(method=="SetItem")v=read_value(arg(2));else if(method=="SetUINT32"){v.type=19;v.scalar=d(2);}else if(method=="SetUINT64"){v.type=21;v.scalar=arg(2);}else if(method=="SetDouble"){v.type=5;if(x86_)v.scalar=arg(2);else std::memcpy(&v.scalar,state.xmm[2].data(),8);}else if(method=="SetGUID"){v.type=72;v.id=guid(arg(2));}else if(method=="SetString"){v.type=31;v.text=read_utf16(memory,GuestAddress{arg(2)});}else if(method=="SetBlob"){v.type=0x1011;v.blob=data(arg(2),d(3));}else if(method=="SetUnknown"){require(arg(2)!=0);v.type=13;v.scalar=arg(2);}else throw Failure{invalid};set(*item,key,std::move(v));
                    }else if(method=="CompareItem"){put(arg(3),get(*item,key)==read_value(arg(2)));}
                    else if(method=="GetItem"){const auto& v=get(*item,key);if(arg(2))write_value(arg(2),v);}
                    else if(method=="GetItemType")put(arg(2),get(*item,key).type);
                    else if(method=="GetUINT32")put(arg(2),get(*item,key,19).scalar);
                    else if(method=="GetUINT64" || method=="GetDouble")put(arg(2),get(*item,key,method=="GetUINT64"?21:5).scalar,64);
                    else if(method=="GetGUID"){check(arg(2),16);memory.write(GuestAddress{arg(2)},get(*item,key,72).id);}
                    else if(method=="GetStringLength")put(arg(2),get(*item,key,31).text.size());
                    else if(method=="GetBlobSize")put(arg(2),get(*item,key,0x1011).blob.size());
                    else if(method=="GetString" || method=="GetBlob"){
                        const bool string=method=="GetString";const auto& v=get(*item,key,string?31:0x1011);const auto bytes=string?utf16_bytes(v.text):v.blob;const auto size=string?v.text.size():v.blob.size();if(arg(4))put(arg(4),size);require(d(3)>=(string?size+1:size),small);if(!bytes.empty()){check(arg(2),bytes.size());memory.write(GuestAddress{arg(2)},bytes);}
                    }else if(method=="GetAllocatedString" || method=="GetAllocatedBlob"){
                        const bool string=method=="GetAllocatedString";out(arg(2));const auto& v=get(*item,key,string?31:0x1011);if(arg(3))put(arg(3),string?v.text.size():v.blob.size());else require(string,pointer_error);const auto bytes=string?utf16_bytes(v.text):v.blob;const auto p=allocate(bytes.size());memory.write(GuestAddress{p},bytes);put(arg(2),p,bits);
                    }else if(method=="GetUnknown"){
                        out(arg(3));const auto pointer=get(*item,key,13).scalar;const auto iid=guid(arg(2));if(bridge.objects.contains(pointer))query(pointer,iid,arg(3));else {start_callbacks(state,memory,{{foreign_method(pointer,0),{pointer,arg(2),arg(3)}}},Continuation::ReturnCallback);return std::nullopt;}
                    }else throw Failure{invalid};
                }
            }else if(name.starts_with("B_")){
                require(item->kind==mf::Buffer,no_interface);
                if(name=="B_Lock"){check(arg(1),unit);if(arg(2))check(arg(2),4);if(arg(3))check(arg(3),4);require(item->locks<UINT32_MAX && !item->locks_2d,0xc00d36b2U);map_buffer(*item,true,false);++item->locks;put(arg(1),item->data,bits);if(arg(2))put(arg(2),item->maximum);if(arg(3))put(arg(3),item->length);}
                else if(name=="B_Unlock"){require(item->locks);if(!--item->locks)flush_buffer(*item,true);}
                else if(name=="B_GetCurrentLength")put(arg(1),item->length);
                else if(name=="B_GetMaxLength")put(arg(1),item->maximum);
                else if(name=="B_SetCurrentLength"){require(d(1)<=item->maximum);item->length=d(1);}
            }else if(name.starts_with("S_")){
                require(item->kind==mf::Sample,no_interface);
                if(name=="S_GetSampleFlags")put(arg(1),item->flags);else if(name=="S_SetSampleFlags")item->flags=d(1);
                else if(name=="S_SetSampleTime")item->time=arg(1);else if(name=="S_SetSampleDuration")item->duration=arg(1);
                else if(name=="S_GetSampleTime"){require(item->time.has_value(),0xc00d36c8U);put(arg(1),*item->time,64);}else if(name=="S_GetSampleDuration"){require(item->duration.has_value(),0xc00d36c9U);put(arg(1),*item->duration,64);}
                else if(name=="S_GetBufferCount")put(arg(1),item->children.size());
                else if(name=="S_AddBuffer"){(void)object(arg(1),mf::Buffer);require(item->children.size()<65536);hold(arg(1));item->children.push_back(arg(1));}
                else if(name=="S_GetBufferByIndex"){out(arg(2));require(d(1)<item->children.size(),invalid_index);hold(item->children[d(1)]);put(arg(2),item->children[d(1)],bits);}
                else if(name=="S_RemoveBufferByIndex"){require(d(1)<item->children.size(),invalid_index);const auto p=item->children[d(1)];item->children.erase(item->children.begin()+d(1));release(p);}
                else if(name=="S_RemoveAllBuffers"){auto children=std::move(item->children);item->children.clear();for(const auto p:children)release(p);}
                else if(name=="S_GetTotalLength"){std::uint64_t size=0;for(const auto p:item->children)size+=object(p,mf::Buffer)->length;require(size<=UINT32_MAX);put(arg(1),size);}
                else if(name=="S_CopyToBuffer"){auto buffer=object(arg(1),mf::Buffer);const auto bytes=sample_bytes(*item);require(bytes.size()<=buffer->maximum,small);map_buffer(*buffer,true,false);memory.write(GuestAddress{buffer->data},bytes);buffer->length=static_cast<std::uint32_t>(bytes.size());flush_buffer(*buffer,true);}
                else if(name=="S_ConvertToContiguousBuffer"){
                    out(arg(1));require(!item->children.empty(),0x8000ffffU);if(item->children.size()==1){hold(item->children.front());put(arg(1),item->children.front(),bits);}else {const auto bytes=sample_bytes(*item);const auto p=memory_buffer(static_cast<std::uint32_t>(bytes.size()),0);auto buffer=object(p,mf::Buffer);memory.write(GuestAddress{buffer->data},bytes);buffer->length=static_cast<std::uint32_t>(bytes.size());const auto old=std::move(item->children);item->children={p};for(const auto child:old)release(child);hold(p);put(arg(1),p,bits);}
                }
            }else if(name.starts_with("T_")){
                require(item->kind==mf::MediaType,no_interface);
                if(name=="T_GetMajorType"){check(arg(1),16);memory.write(GuestAddress{arg(1)},get(*item,mf::MF_MT_MAJOR_TYPE,72).id);}
                else if(name=="T_IsCompressedFormat"){const auto found=item->attributes.find(mf::MF_MT_COMPRESSED);put(arg(1),found!=item->attributes.end() && found->second.scalar!=0);}
                else if(name=="T_IsEqual"){auto theirs=object(arg(1),mf::MediaType);const auto equal=[&](const Guid& key){const auto a=item->attributes.find(key),b=theirs->attributes.find(key);return a!=item->attributes.end() && b!=theirs->attributes.end() && a->second==b->second;};std::uint32_t flags=0;if(equal(mf::MF_MT_MAJOR_TYPE))flags|=1;if(equal(mf::MF_MT_SUBTYPE))flags|=2;auto a=item->attributes,b=theirs->attributes;a.erase(mf::MF_MT_MAJOR_TYPE);a.erase(mf::MF_MT_SUBTYPE);b.erase(mf::MF_MT_MAJOR_TYPE);b.erase(mf::MF_MT_SUBTYPE);if(a==b)flags|=4;if(item->attributes==theirs->attributes)flags|=8;put(arg(2),flags);result=flags==15?0:1;}
                else throw Failure{invalid};
            }else if(name.starts_with("R_")){
                require(item->kind==mf::Async,no_interface);
                if(name=="R_GetStatus")result=item->status;else if(name=="R_SetStatus")item->status=d(1);else if(name=="R_GetStateNoAddRef")result=item->state;
                else {out(arg(1));const auto p=name=="R_GetState"?item->state:item->result_object;require(p!=0,pointer_error);hold(p);put(arg(1),p,bits);}
            }else if(name.starts_with("E_")){
                require(item->kind==mf::Event,no_interface);if(name=="E_GetType")put(arg(1),item->identifier);else if(name=="E_GetStatus")put(arg(1),item->status);else if(name=="E_GetExtendedType"){check(arg(1),16);memory.write(GuestAddress{arg(1)},item->extended);}else if(name=="E_GetValue")write_value(arg(1),item->value);
            }else if(name.starts_with("Q_")){
                require(item->kind==mf::Queue || item->kind==mf::Transform || item->kind==mf::Source || item->kind==mf::MediaStream,no_interface);require(!item->closed,shutdown);
                if(name=="Q_GetEvent"){
                    out(arg(2));require(d(1)<=1);require(!item->callback,0xc00d36daU);
                    if(item->events.empty()){
                        require(d(1)==0,no_events);auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::MediaFoundation;thread.wait.handles={arg(0)};schedule(state,memory);return std::nullopt;
                    }
                    const auto p=item->events.front();item->events.pop_front();put(arg(2),p,bits);
                }
                else if(name=="Q_BeginGetEvent"){require(arg(1)!=0);require(!item->callback,0xc00d36daU);hold(arg(1));hold(arg(2));item->callback=arg(1);item->state=arg(2);deliver(arg(0),item);}
                else if(name=="Q_EndGetEvent"){out(arg(2));auto async=object(arg(1),mf::Async);require(bridge.objects.contains(async->queue_owner) && entry(async->queue_owner).object==item && async->result_object);const auto p=async->result_object;async->result_object=0;put(arg(2),p,bits);}
                else if(name=="Q_QueueEvent"){(void)object(arg(1),mf::Event);hold(arg(1));item->events.push_back(arg(1));deliver(arg(0),item);}
                else if(name=="Q_QueueEventParamVar" || name=="Q_QueueEventParamUnk"){Value value;if(name=="Q_QueueEventParamVar")value=arg(4)?read_value(arg(4)):Value{};else {value.type=13;value.scalar=arg(4);}item->events.push_back(event(d(1),guid(arg(2)),d(3),std::move(value)));deliver(arg(0),item);}
                else if(name=="Q_Shutdown"){item->closed=true;for(const auto p:item->events)release(p);item->events.clear();release(item->state);release(item->callback);item->state=item->callback=0;}
            }else if(name.starts_with("G_")){
                require(item->kind==mf::Manager,no_interface);
                if(name=="G_ResetDevice"){
                    require(d(2)==item->reset_token);require(media_graphics_device(arg(1)),no_interface);hold(arg(1));release(item->parent);item->parent=arg(1);++item->generation;item->lock_handle=item->lock_owner=0;
                }else if(name=="G_OpenDeviceHandle"){
                    out(arg(1));require(item->parent!=0,0x80041000U);require(item->device_handles.size()<65536,0x8007000eU);const auto handle=bridge.next_device_handle++;item->device_handles.emplace(handle,item->generation);put(arg(1),handle,bits);
                }else {
                    const auto handle=arg(1);const auto found=item->device_handles.find(handle);require(found!=item->device_handles.end(),0x80070006U);
                    if(name=="G_CloseDeviceHandle"){if(item->lock_handle==handle)item->lock_handle=item->lock_owner=0;item->device_handles.erase(found);}
                    else {require(item->parent!=0,0x80041000U);require(found->second==item->generation,0x80041001U);
                        if(name=="G_UnlockDevice"){require(item->lock_handle==handle,invalid);item->lock_handle=item->lock_owner=0;}
                        else if(name=="G_TestDevice"){}
                        else {
                            out(arg(3));const bool locking=name=="G_LockDevice";
                            if(locking && item->lock_handle){require(d(4)!=0,0x80041002U);auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::MediaFoundation;thread.wait.handles={arg(0)};schedule(state,memory);return std::nullopt;}
                            if(locking){item->lock_handle=handle;item->lock_owner=threads_.current;}
                            const auto token=bridge.next_query++;bridge.queries.emplace(token,MediaFoundationState::Query{item,handle,item->generation,arg(3),threads_.current,locking});
                            try{start_callbacks(state,memory,{{foreign_method(item->parent,0),{item->parent,arg(2),arg(3)}}},Continuation::MediaFoundationQuery,token);}catch(...){discard_media_foundation_query(token,memory,state);throw;}return std::nullopt;
                        }
                    }
                }
            }else if(name.starts_with("X_")){
                require(item->surface.has_value(),no_interface);
                if(name=="X_GetSubresourceIndex")put(arg(1),item->identifier);
                else if(name=="X_GetResource"){out(arg(2));start_callbacks(state,memory,{{foreign_method(item->parent,0),{item->parent,arg(1),arg(2)}}},Continuation::ReturnCallback);return std::nullopt;}
                else if(name=="X_SetUnknown"){const auto key=guid(arg(1));if(!arg(2)){const auto found=item->attributes.find(key);if(found!=item->attributes.end()){release(found->second.scalar);item->attributes.erase(found);}}else {require(!item->attributes.contains(key),0x80071392U);Value value;value.type=13;value.scalar=arg(2);set(*item,key,value);}}
                else if(name=="X_GetUnknown"){out(arg(3));const auto found=item->attributes.find(guid(arg(1)));require(found!=item->attributes.end(),0xc00d36d5U);const auto p=found->second.scalar;if(bridge.objects.contains(p))query(p,guid(arg(2)),arg(3));else {start_callbacks(state,memory,{{foreign_method(p,0),{p,arg(2),arg(3)}}},Continuation::ReturnCallback);return std::nullopt;}}
            }else if(name.starts_with("Y_")){
                require(item->surface.has_value(),no_interface);
                if(name=="Y_Lock2D" || name=="Y_Lock2DSize"){
                    const bool sized=name=="Y_Lock2DSize";const auto flags=sized?d(1):3U;require(flags>=1 && flags<=3);const auto scan=arg(sized?2:1),pitch=arg(sized?3:2);check(scan,unit);check(pitch,4);if(sized){check(arg(4),unit);check(arg(5),4);}require(!item->locks && !item->locks_2d,0xc00d36b2U);map_buffer(*item,false,flags==2);item->locks_2d=1;item->lock_flags=flags;put(scan,item->data,bits);put(pitch,item->surface->row_pitch);if(sized){put(arg(4),item->data,bits);put(arg(5),item->maximum);}
                }else if(name=="Y_Unlock2D"){require(item->locks_2d!=0,0xc00d36b2U);item->locks_2d=0;if(item->lock_flags&2)flush_buffer(*item,false);}
                else if(name=="Y_GetScanline0AndPitch"){require(item->locks_2d!=0,0xc00d36b2U);put(arg(1),item->data,bits);put(arg(2),item->surface->row_pitch);}
                else if(name=="Y_IsContiguousFormat")put(arg(1),1);
                else if(name=="Y_GetContiguousLength")put(arg(1),item->maximum);
                else if(name=="Y_ContiguousCopyTo"){require(d(2)>=item->maximum,small);check(arg(1),item->maximum);map_buffer(*item,true,false);memory.write(GuestAddress{arg(1)},data(item->data,item->maximum));}
                else if(name=="Y_ContiguousCopyFrom"){require(d(2)>=item->maximum,small);const auto bytes=data(arg(1),item->maximum);map_buffer(*item,true,true);memory.write(GuestAddress{item->data},bytes);flush_buffer(*item,true);}
                else if(name=="Y_Copy2DTo"){auto target=object(arg(1),mf::Buffer);require(target->surface.has_value() && target->maximum==item->maximum && target->surface->row_pitch==item->surface->row_pitch && target->surface->height==item->surface->height);map_buffer(*item,false,false);const auto bytes=data(item->data,item->maximum);map_buffer(*target,false,true);memory.write(GuestAddress{target->data},bytes);flush_buffer(*target,false);}
            }else if(name.starts_with("D_") || name.starts_with("H_")){
                require(item->kind==mf::Stream,no_interface);
                if(name=="D_GetStreamIdentifier")put(arg(1),item->identifier);else if(name=="D_GetMediaTypeHandler"){out(arg(1));const auto p=create(item,mf::TypeHandler);hold(p);put(arg(1),p,bits);}
                else if(name=="H_GetMediaTypeCount")put(arg(1),item->children.size());else if(name=="H_GetMediaTypeByIndex"){out(arg(2));require(d(1)<item->children.size(),0xc00d36b9U);hold(item->children[d(1)]);put(arg(2),item->children[d(1)],bits);}
                else if(name=="H_GetCurrentMediaType"){out(arg(1));require(item->current_type!=0,0xc00d6d60U);hold(item->current_type);put(arg(1),item->current_type,bits);}
                else if(name=="H_GetMajorType"){check(arg(1),16);memory.write(GuestAddress{arg(1)},get(*object(item->current_type,mf::MediaType),mf::MF_MT_MAJOR_TYPE,72).id);}
                else if(name=="H_SetCurrentMediaType" || name=="H_IsMediaTypeSupported"){
                    auto type=object(arg(1),mf::MediaType);const auto supported=std::any_of(item->children.begin(),item->children.end(),[&](const auto p){return object(p,mf::MediaType)->attributes==type->attributes;});if(name=="H_IsMediaTypeSupported" && arg(2))out(arg(2));require(supported,0xc00d36b4U);if(name=="H_SetCurrentMediaType"){hold(arg(1));release(item->current_type);item->current_type=arg(1);}
                }
            }else if(name.starts_with("P_")){
                require(item->kind==mf::Presentation,no_interface);
                if(name=="P_GetStreamDescriptorCount")put(arg(1),item->children.size());else if(name=="P_GetStreamDescriptorByIndex"){out(arg(3));check(arg(2),4);require(d(1)<item->children.size(),invalid_index);hold(item->children[d(1)]);put(arg(2),item->selected[d(1)]);put(arg(3),item->children[d(1)],bits);}
                else if(name=="P_SelectStream" || name=="P_DeselectStream"){require(d(1)<item->children.size(),invalid_index);item->selected[d(1)]=name=="P_SelectStream";}
                else if(name=="P_Clone"){out(arg(1));const auto p=make(mf::Presentation);auto clone=object(p,mf::Presentation);clone->children=item->children;clone->selected=item->selected;for(const auto child:clone->children)hold(child);for(const auto& [key,value]:item->attributes)set(*clone,key,value);put(arg(1),p,bits);}
            }else throw Failure{no_interface};
        }
    }catch(const Failure& error){result=error.code;}catch(const MemoryFault&){result=pointer_error;}catch(const std::bad_alloc&){result=0x8007000eU;}catch(const std::invalid_argument&){result=invalid;}catch(const std::logic_error&){result=0xc00d36b2U;}catch(const std::runtime_error&){result=0x80004005U;}
    if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;}
    arg.return_to(state,result,name=="MFGetSystemTime");return std::nullopt;
}
}
