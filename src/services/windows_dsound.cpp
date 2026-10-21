#include "x64arm/windows_services.hpp"
#include <cstdio>
#include <cstdlib>
#include "x64arm/audio.hpp"
#include "x64arm/unicode.hpp"
#include "dsound_interfaces.hpp"
#include "audio_endpoints.hpp"
#include "pcm_wave_format.hpp"
#include "audio_spatial.hpp"
#include "wave_device_message.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <bit>
#include <set>
#include <functional>
#include <sstream>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,unsupported=0x80004001U,no_interface=0x80004002U,
    control_unavailable=0x8878001eU,invalid_call=0x88780032U,no_driver=0x88780078U;
using Guid=std::array<std::uint8_t,16>;
constexpr Guid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid sound{0x83,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0,0x20,0xaf,0x0b,0xe5,0x60};
constexpr Guid sound8{0x93,0x7e,0x0a,0xc5,0x95,0xf3,0x34,0x48,0x9e,0xf6,0x7f,0xa9,0x9d,0xe5,0x09,0x66};
constexpr Guid buffer{0x85,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0,0x20,0xaf,0x0b,0xe5,0x60};
constexpr Guid buffer8{0x49,0xa4,0x25,0x68,0x24,0x75,0x82,0x4d,0x92,0x0f,0x50,0xe3,0x6a,0xb3,0xab,0x1e};
using namespace audio::spatial;
struct Listener {Vector position,velocity,front{0,0,1},top{0,1,0};float distance{1},rolloff{1},doppler{1};};
struct Source {Vector position,velocity,cone{0,0,1};std::uint32_t inside{360},outside{360},mode{};std::int32_t cone_volume{};float minimum{1},maximum{1e9F};};
struct Device {std::uint32_t level{1},speakers{4};std::int32_t volume{},pan{};Listener listener,pending;bool deferred{};std::string host;}; // host：Core Audio UID，空字串跟隨系統預設輸出
struct Samples {std::uint64_t data{};};
struct Object {
    bool device{},version8{},primary{},initialized{true},capture{},factory{},duplex{};
    Guid cls{};std::vector<std::uint64_t> links;
    std::uint32_t references{1},flags{},level{1};
    std::int32_t volume{},pan{};
    std::uint64_t data{},lock_first{},lock_second{};
    std::uint32_t lock_bytes1{},lock_bytes2{};
    bool locked{};
    std::uint64_t canonical{};
    std::map<unsigned,std::uint64_t> interfaces;
    std::shared_ptr<Device> context=std::make_shared<Device>();
    std::shared_ptr<Samples> samples=std::make_shared<Samples>();
    Source source,pending;bool deferred{};
    std::shared_ptr<audio::Buffer> pcm=std::make_shared<audio::Buffer>();
    std::shared_ptr<audio::Effect> effect;
    struct EffectEntry {std::shared_ptr<audio::Effect> dsp;std::uint64_t object{};bool allocated{},hardware{};};
    std::vector<EffectEntry> effects;
    std::unique_ptr<audio::Output> output;
    std::unique_ptr<audio::Input> input;
};
struct Method {std::string_view name;unsigned args;std::uint32_t scalars{};};
constexpr Method device_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"CreateSoundBuffer",4},{"DeviceCaps",2},{"DuplicateSoundBuffer",3},{"SetCooperativeLevel",3,4},{"Compact",1},{"GetSpeakerConfig",2},{"SetSpeakerConfig",2,2},{"DeviceInitialize",2},{"VerifyCertification",2}};
constexpr Method buffer_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"BufferCaps",2},{"GetCurrentPosition",3},{"GetFormat",4,4},{"GetVolume",2},{"GetPan",2},{"GetFrequency",2},{"GetStatus",2},{"BufferInitialize",4},{"Lock",8,134},{"Play",4,14},{"SetCurrentPosition",2,2},{"SetFormat",2},{"SetVolume",2,2},{"SetPan",2,2},{"SetFrequency",2,2},{"Stop",1},{"Unlock",5,20},{"Restore",1},{"SetFX",4,2},{"AcquireResources",4,6},{"GetObjectInPath",5,4}};
constexpr Method notify_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"N_SetNotificationPositions",3,2}};
constexpr Method listener_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"L3_GetAllParameters",2},{"L3_GetDistanceFactor",2},{"L3_GetDopplerFactor",2},{"L3_GetOrientation",3},{"L3_GetPosition",2},{"L3_GetRolloffFactor",2},{"L3_GetVelocity",2},{"L3_SetAllParameters",3,4},{"L3_SetDistanceFactor",3,6},{"L3_SetDopplerFactor",3,6},{"L3_SetOrientation",8,254},{"L3_SetPosition",5,30},{"L3_SetRolloffFactor",3,6},{"L3_SetVelocity",5,30},{"L3_CommitDeferredSettings",1}};
constexpr Method source_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"B3_GetAllParameters",2},{"B3_GetConeAngles",3},{"B3_GetConeOrientation",2},{"B3_GetConeOutsideVolume",2},{"B3_GetMaxDistance",2},{"B3_GetMinDistance",2},{"B3_GetMode",2},{"B3_GetPosition",2},{"B3_GetVelocity",2},{"B3_SetAllParameters",3,4},{"B3_SetConeAngles",4,14},{"B3_SetConeOrientation",5,30},{"B3_SetConeOutsideVolume",3,6},{"B3_SetMaxDistance",3,6},{"B3_SetMinDistance",3,6},{"B3_SetMode",3,6},{"B3_SetPosition",5,30},{"B3_SetVelocity",5,30}};
constexpr Method capture_device_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"C_CreateCaptureBuffer",4},{"C_DeviceCaps",2},{"C_DeviceInitialize",2}};
constexpr Method capture_buffer_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"BufferCaps",2},{"GetCurrentPosition",3},{"GetFormat",4,4},{"GetStatus",2},{"C_BufferInitialize",3},{"Lock",8,134},{"C_Start",2,2},{"Stop",1},{"Unlock",5,20},{"GetObjectInPath",5,4},{"C_GetFXStatus",3,2}};
constexpr Method factory_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"F_CreateInstance",4},{"F_LockServer",2,2}};
constexpr Method duplex_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"F_Initialize",9,64}};
constexpr Method effect_methods[]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"FX_SetAllParameters",2},{"FX_GetAllParameters",2},{"FX_SetPreset",2,2},{"FX_GetPreset",2},{"FX_SetQuality",2,2},{"FX_GetQuality",2}};
bool capture_guid(const Guid& g){return g==Guid{} || g==dsound::default_capture || g==dsound::default_voice_capture || g==dsound::capture_device;}
bool playback_guid(const Guid& g){return g==Guid{} || g==dsound::default_playback || g==dsound::default_voice_playback || g==dsound::playback_device;}
// DirectSound 播放裝置 GUID 即端點的 PKEY_AudioEndpoint_GUID；預設別名跟隨系統預設輸出（空字串）。
std::optional<std::string> playback_host(const Guid& g){
    if(playback_guid(g))return std::string{};
    if(auto endpoint=audio_endpoint::render_by_guid(g))return std::move(endpoint->device.uid);
    return std::nullopt;
}
float gain(std::int32_t db){return db<=-10000?0.0F:std::pow(10.0F,static_cast<float>(db)/2000.0F);}
void update(Object& o){
    auto& pcm=*o.pcm;const auto& d=*o.context;
    pcm.left=gain(o.volume)*gain(-std::max(o.pan,0))*gain(o.primary?0:d.volume)*gain(-std::max(o.primary?0:d.pan,0));
    pcm.right=gain(o.volume)*gain(std::min(o.pan,0))*gain(o.primary?0:d.volume)*gain(std::min(o.primary?0:d.pan,0));
    pcm.spatial_left=pcm.spatial_right=pcm.doppler=1;pcm.spatial_distance=0;
    if(!(o.flags&0x10) || o.primary || o.source.mode==2)return;
    const auto& s=o.source;const auto& l=d.listener;
    const auto v=s.mode==1?s.position:sub(s.position,l.position);const auto distance=length(v);const auto direction=normalized(v);
    pcm.spatial_distance=distance*l.distance;
    auto attenuation=s.minimum/(s.minimum+l.rolloff*(std::clamp(distance,s.minimum,s.maximum)-s.minimum));
    if((o.flags&0x20000) && distance>=s.maximum)attenuation=0;
    if(s.outside<360 && distance>0 && length(s.cone)>0){
        const auto angle=std::acos(std::clamp(-dot(normalized(s.cone),direction),-1.0F,1.0F))*360.0F/3.14159265358979323846F;
        const auto inside=static_cast<float>(s.inside),outside=static_cast<float>(s.outside);
        const auto fraction=outside>inside?std::clamp((angle-inside)/(outside-inside),0.0F,1.0F):(angle>inside?1.0F:0.0F);
        attenuation*=std::pow(10.0F,static_cast<float>(s.cone_volume)*fraction/2000.0F);
    }
    const auto right=s.mode==1?Vector{1,0,0}:normalized(cross(l.top,l.front));const auto pan=std::clamp(dot(direction,right),-1.0F,1.0F);
    const auto gains=stereo(pan,attenuation);pcm.spatial_left=gains.first;pcm.spatial_right=gains.second;
    const auto listener_speed=s.mode==1?0.0F:dot(l.velocity,direction)*l.distance*l.doppler;
    const auto source_speed=dot(s.velocity,direction)*l.distance*l.doppler;
    pcm.doppler=audio::spatial::doppler(343.3F,listener_speed,source_speed,300.0F,0.125F,8.0F);
}
}
struct DirectSoundState {
    std::map<std::uint64_t,std::shared_ptr<Object>> objects;
    std::map<unsigned,std::uint64_t> tables;
    struct Enumeration { std::uint64_t guid{},primary{},description{},endpoint{},empty{}; };
    std::map<std::pair<unsigned,std::string>,std::vector<Enumeration>> enumerations;
    std::uint32_t locks{};
};
std::span<const WindowsServiceDescriptor> dsound_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> result{{"dsound.dll","DirectSoundCreate",3},{"dsound.dll","DirectSoundCreate8",3},{"dsound.dll","DirectSoundEnumerateA",2},{"dsound.dll","DirectSoundEnumerateW",2},{"dsound.dll","GetDeviceID",2}};
        for(const auto& method:device_methods)result.push_back({"internal-dsound",method.name,static_cast<std::uint8_t>(method.args),0,0,method.scalars});
        for(std::size_t i=3;i<std::size(buffer_methods);++i)result.push_back({"internal-dsound",buffer_methods[i].name,static_cast<std::uint8_t>(buffer_methods[i].args),0,0,buffer_methods[i].scalars});
        for(const auto table:{std::span<const Method>(notify_methods),std::span<const Method>(listener_methods),std::span<const Method>(source_methods)})for(std::size_t i=3;i<table.size();++i)result.push_back({"internal-dsound",table[i].name,table[i].args,0,0,table[i].scalars});
        for(auto symbol:{"DirectSoundCaptureCreate","DirectSoundCaptureCreate8"})result.push_back({"dsound.dll",symbol,3});
        for(auto symbol:{"DirectSoundCaptureEnumerateA","DirectSoundCaptureEnumerateW"})result.push_back({"dsound.dll",symbol,2});
        for(const auto table:{std::span<const Method>(capture_device_methods),std::span<const Method>(capture_buffer_methods)})for(std::size_t i=3;i<table.size();++i)if(table[i].name.starts_with("C_"))result.push_back({"internal-dsound",table[i].name,table[i].args,0,0,table[i].scalars});
        result.push_back({"dsound.dll","DirectSoundFullDuplexCreate",10,0,0,0x20});result.push_back({"dsound.dll","DllGetClassObject",3});result.push_back({"dsound.dll","DllCanUnloadNow",0});
        for(auto table:{std::span<const Method>(factory_methods),std::span<const Method>(duplex_methods)})for(std::size_t i=3;i<table.size();++i)result.push_back({"internal-dsound",table[i].name,table[i].args,0,0,table[i].scalars});
        for(std::size_t i=3;i<std::size(effect_methods);++i)result.push_back({"internal-dsound",effect_methods[i].name,effect_methods[i].args,0,0,effect_methods[i].scalars});
        return result;
    }();return table;
}
void WindowsServices::poll_dsound(){
    if(!dsound_)return;
    for(const auto& [p,o]:dsound_->objects){if(p!=o->canonical || o->effect)continue;std::lock_guard lock(o->pcm->mutex);
        for(auto event:o->pcm->pending_notifications){const auto it=runtime_.wait_objects.find(event);if(it!=runtime_.wait_objects.end() && it->second.kind==WaitObject::Kind::Event)it->second.set_signal(true);}
        o->pcm->pending_notifications.clear();
    }
}
bool WindowsServices::dsound_waiting()const{
    if(!dsound_)return false;for(const auto& [p,o]:dsound_->objects){if(p!=o->canonical || o->effect)continue;std::lock_guard lock(o->pcm->mutex);if(o->pcm->playing && !o->pcm->notifications.empty())return true;}return false;
}
std::string WindowsServices::dsound_diagnostics() const {
    if(!dsound_)return {};std::ostringstream out;std::set<const Object*> seen;
    for(const auto& [handle,object]:dsound_->objects){if(!seen.insert(object.get()).second || object->device || object->factory || object->duplex || object->effect || object->capture)continue;
        auto& pcm=*object->pcm;std::lock_guard lock(pcm.mutex);if(!pcm.rendered_frames)continue;
        out<<"dsound_output handle="<<handle<<" channels="<<pcm.channels<<" rate="<<pcm.rate<<" bits="<<pcm.bits<<" rendered_frames="<<pcm.rendered_frames<<" peak_left="<<pcm.rendered_left_peak<<" peak_right="<<pcm.rendered_right_peak<<" playing="<<pcm.playing<<'\n';
    }return out.str();
}
std::optional<std::uint32_t> WindowsServices::invoke_dsound(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("DirectSound 不能在 compare 模式使用");
    if(!dsound_)dsound_=std::make_shared<DirectSoundState>();
    auto& bridge=*dsound_;const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    poll_dsound();
    const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    const auto read=[&](std::uint64_t p,unsigned offset,std::uint8_t width=32){return memory.load(GuestAddress{p+offset},width);};
    const auto write=[&](std::uint64_t p,std::uint64_t value,std::uint8_t width=32){if(!p)throw std::invalid_argument("null output");memory.store(GuestAddress{p},width,value);};
    const auto check_out=[&](std::uint64_t p,unsigned size){if(!p)throw std::invalid_argument("null output");memory.check(GuestAddress{p},size,Access::Write);};
    const auto create=[&](const std::shared_ptr<Object>& object,unsigned interface=UINT32_MAX){
        const unsigned key=interface==UINT32_MAX?(object->effect?20U+static_cast<unsigned>(object->effect->type()):object->factory?11U:object->duplex?12U:(object->capture?(object->device?7U:9U):object->device?2U:0U)+(object->version8?1U:0U)):interface;
        if(object->interfaces.contains(key))return object->interfaces.at(key);
        if(bridge.objects.size()>=256)throw std::bad_alloc{};
        if(!bridge.tables.contains(key)){
            const auto methods=object->effect?std::span<const Method>(effect_methods).first(object->effect->type()==audio::Effect::Type::I3DL2Reverb?9U:5U):key==11?std::span<const Method>(factory_methods):key==12?std::span<const Method>(duplex_methods):key==4?std::span<const Method>(notify_methods):key==5?std::span<const Method>(listener_methods):key==6?std::span<const Method>(source_methods):object->capture?(object->device?std::span<const Method>(capture_device_methods):std::span<const Method>(capture_buffer_methods)):object->device?std::span<const Method>(device_methods):std::span<const Method>(buffer_methods);
            const auto count=key==4?methods.size():object->capture?(!object->device && !object->version8?12U:methods.size()):key>=4 || object->version8?methods.size():(object->device?11U:21U);
            const auto table=runtime_allocate(memory,{},count*unit);
            for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{table+i*unit},bits,resolve("internal-dsound",methods[i].name).value);
            bridge.tables.emplace(key,table);
        }
        const auto pointer=allocate_heap(memory,unit);if(!pointer)throw std::bad_alloc{};
        try {memory.store(GuestAddress{pointer},bits,bridge.tables.at(key));bridge.objects.emplace(pointer,object);object->interfaces.emplace(key,pointer);if(!object->canonical)object->canonical=pointer;}
        catch(...){(void)free_heap(memory,pointer,state);throw;}
        return pointer;
    };
    const auto format=[&](std::uint64_t p,audio::Buffer& pcm){
        if(!p)throw std::invalid_argument("null format");
        const auto f=read_pcm_wave_format(memory,p);if(!f)throw std::invalid_argument("unsupported PCM format");
        pcm.channels=f->channels;pcm.bits=f->bits;pcm.rate=pcm.frequency=f->rate;pcm.floating=f->floating;
    };
    std::function<void(std::uint64_t)> release;
    release=[&](std::uint64_t pointer){auto o=bridge.objects.at(pointer);if(--o->references)return;o->output.reset();o->input.reset();
        if(std::getenv("X64ARM_TRACE_AUDIO")){auto& pcm=*o->pcm;std::lock_guard lock(pcm.mutex);if(pcm.rendered_frames)std::fprintf(stderr,"[dsound-output] handle=%llu channels=%u rate=%u bits=%u frames=%llu peak_left=%g peak_right=%g\n",static_cast<unsigned long long>(pointer),pcm.channels,pcm.rate,pcm.bits,static_cast<unsigned long long>(pcm.rendered_frames),static_cast<double>(pcm.rendered_left_peak),static_cast<double>(pcm.rendered_right_peak));}
        for(auto child:o->links)release(child);for(const auto& effect:o->effects)if(effect.object)release(effect.object);if(o->samples->data && o->samples.use_count()==1)(void)free_heap(memory,o->samples->data,state);for(auto [key,p]:o->interfaces){(void)key;bridge.objects.erase(p);(void)free_heap(memory,p,state);}};
    const auto instantiate=[&](Guid cls,Guid iid,bool factory){
        auto o=std::make_shared<Object>();o->cls=cls;o->factory=factory;o->duplex=!factory && cls==dsound::duplex_clsid;o->capture=cls==dsound::capture_clsid || cls==dsound::capture_clsid8;o->device=!factory && !o->duplex;o->version8=cls==dsound::clsid8 || cls==dsound::capture_clsid8;o->initialized=false;
        if(iid!=unknown && (factory?iid!=dsound::factory_iid:o->duplex?iid!=dsound::duplex_iid:o->capture?iid!=dsound::capture_iid:iid!=sound && !(o->version8 && iid==sound8)))throw std::invalid_argument("DirectSound IID 無效");return create(o);
    };
    const auto allocate_effects=[&](Object& object){
        std::vector<std::shared_ptr<audio::Effect>> processing;processing.reserve(object.effects.size());bool failed=false;
        for(auto& effect:object.effects){effect.allocated=!effect.hardware;if(effect.allocated)processing.push_back(effect.dsp);else failed=true;}
        object.pcm->effects=std::move(processing);object.flags|=8U;return failed;
    };
    const auto duplex=[&](const std::shared_ptr<Object>& root,unsigned first)->std::uint32_t{
        const unsigned out=first?first+6:7;check_out(arg(out),unit);check_out(arg(out+1),unit);write(arg(out),0,bits);write(arg(out+1),0,bits);if(root->initialized)return 0x88780082U;
        if(!arg(first+4) || arg(first+5)<1 || arg(first+5)>4)return invalid;Guid cap{},render{};if(arg(first))memory.read(GuestAddress{arg(first)},cap);if(arg(first+1))memory.read(GuestAddress{arg(first+1)},render);const auto render_host=playback_host(render);if(!capture_guid(cap) || !render_host)return no_driver;root->context->host=*render_host;
        std::vector<std::uint64_t> made;
        try {for(unsigned i=0;i<2;++i){auto child=std::make_shared<Object>();child->capture=i==0;child->version8=true;child->context=root->context;const auto p=arg(first+2+i);const auto size=read(p,0),flags=read(p,4),count=read(p,8),fmt=read(p,16,bits);if(size!=(x86_?20U:24U) && size!=(child->capture?(x86_?28U:40U):(x86_?36U:40U)))throw std::invalid_argument("雙工描述大小無效");if(flags&~(child->capture?UINT64_C(0):UINT64_C(0xfc3fa)) || count<4 || count>16*1024*1024 || read(p,12))throw std::invalid_argument("雙工緩衝區描述無效");if(flags&4){for(auto created:made)release(created);return control_unavailable;}format(fmt,*child->pcm);if(count%child->pcm->frame_bytes() || ((flags&0x10) && (child->pcm->channels!=1 || (flags&0x40))))throw std::invalid_argument("雙工 PCM 格式無效");if((flags&0x200) && count*1000<static_cast<std::uint64_t>(child->pcm->rate)*child->pcm->frame_bytes()*150){for(auto created:made)release(created);return 0x887800b4U;}child->flags=static_cast<std::uint32_t>(flags)|(child->capture || (flags&0x40000)?0U:8U);child->pcm->pcm.resize(static_cast<std::size_t>(count),child->pcm->bits==8?128:0);child->data=allocate_heap(memory,count);if(!child->data)throw std::bad_alloc{};child->samples->data=child->data;try{memory.write(GuestAddress{child->data},child->pcm->pcm);made.push_back(create(child));}catch(...){(void)free_heap(memory,child->data,state);throw;}}
        }catch(...){for(auto p:made)release(p);throw;}
        root->context->level=static_cast<std::uint32_t>(arg(first+5));root->links=made;root->initialized=true;for(unsigned i=0;i<2;++i){++bridge.objects.at(made[i])->references;write(arg(out+i),made[i],bits);}return 0;
    };
    std::uint64_t result=0;
    try {
        if(name=="DllCanUnloadNow")result=bridge.objects.empty() && !bridge.locks?0:1;
        else if(name=="DllGetClassObject" || name=="CoGetClassObject"){
            const bool com=name=="CoGetClassObject";const auto out=arg(com?4:2);check_out(out,unit);write(out,0,bits);Guid cls{},iid{};memory.read(GuestAddress{arg(0)},cls);memory.read(GuestAddress{arg(com?3:1)},iid);
            if(!dsound::is_class(memory,arg(0)) || (com && !(arg(1)&1)))result=0x80040111U;else if(iid!=unknown && iid!=dsound::factory_iid)result=no_interface;else write(out,instantiate(cls,iid,true),bits);
        }else if(name=="DirectSoundFullDuplexCreate"){
            check_out(arg(6),unit);write(arg(6),0,bits);check_out(arg(7),unit);write(arg(7),0,bits);check_out(arg(8),unit);write(arg(8),0,bits);
            if(arg(9))result=0x80040110U;else {auto root=std::make_shared<Object>();root->duplex=true;root->initialized=false;const auto p=create(root);try {result=duplex(root,0);if(result)release(p);else write(arg(6),p,bits);}catch(...){release(p);throw;}}
        }else if(name=="GetDeviceID"){
            check_out(arg(1),16);Guid guid{};if(arg(0))memory.read(GuestAddress{arg(0)},guid);
            // 預設播放別名回傳目前系統預設輸出端點的 GUID。
            if(playback_guid(guid)){const auto list=audio::output_available()?audio_endpoint::render():std::vector<audio_endpoint::Endpoint>{};guid=!list.empty() && list.front().device.default_output?list.front().guid:dsound::playback_device;}
            else if(capture_guid(guid))guid=dsound::capture_device;memory.write(GuestAddress{arg(1)},guid);
        }else if(name=="CoCreateInstance"){
            check_out(arg(4),unit);write(arg(4),0,bits);Guid cls{},iid{};memory.read(GuestAddress{arg(0)},cls);memory.read(GuestAddress{arg(3)},iid);
            if(arg(1))result=0x80040110U;else if(!(arg(2)&1))result=0x80040154U;
            else if(!gui_.com_initializations.contains(threads_.current) || gui_.com_initializations.at(threads_.current)<16)result=0x800401f0U;
            else {try{write(arg(4),instantiate(cls,iid,false),bits);}catch(const std::invalid_argument&){result=no_interface;}}
        }else if(name.starts_with("DirectSound") && name.find("Enumerate")!=name.npos){
            if(!arg(0))result=invalid;
            else {
                const bool wide=name.ends_with("W"),capture=name.starts_with("DirectSoundCapture");
                // 播放：Primary Sound Driver 之後列出每個 Core Audio 輸出裝置（GUID＝端點 GUID、描述＝裝置名稱、模組＝端點 ID）；
                // 錄音仍為系統預設輸入。字串區塊依裝置組合快取，裝置變更時重建。
                std::vector<audio_endpoint::Endpoint> devices;
                if(!capture && audio::output_available())devices=audio_endpoint::render();
                std::string signature;for(const auto& item:devices){signature+=item.device.uid;signature.push_back('\n');for(const auto c:item.device.name){signature.push_back(static_cast<char>(c));signature.push_back(static_cast<char>(c>>8));}signature.push_back('\0');}
                const auto key=std::pair{(capture?2U:0U)+(wide?1U:0U),signature};
                auto cached=bridge.enumerations.find(key);
                if(cached==bridge.enumerations.end()){
                    if(bridge.enumerations.size()>=16)bridge.enumerations.erase(bridge.enumerations.begin());
                    std::vector<std::uint8_t> bytes;
                    const auto append=[&](std::u16string_view text){
                        const auto offset=bytes.size();
                        for(const auto c:text){if(wide){bytes.push_back(static_cast<std::uint8_t>(c));bytes.push_back(static_cast<std::uint8_t>(c>>8));}}
                        if(!wide){const auto narrow=utf16_to_utf8(std::u16string(text));bytes.insert(bytes.end(),narrow.begin(),narrow.end());}
                        bytes.push_back(0);if(wide)bytes.push_back(0);return offset;
                    };
                    struct Pending {std::size_t guid,description,endpoint;};std::vector<Pending> pending;
                    const auto primary=append(capture?u"Primary Sound Capture Driver":u"Primary Sound Driver"),empty=append(u"");
                    const auto add=[&](const Guid& guid,std::u16string_view description,std::u16string_view endpoint){
                        const auto text=append(description),module=append(endpoint);const auto at=bytes.size();bytes.insert(bytes.end(),guid.begin(),guid.end());pending.push_back({at,text,module});};
                    if(capture)add(dsound::capture_device,u"macOS Default Audio Input",wave::input_id);
                    else for(const auto& item:devices)add(item.guid,item.device.name.empty()?std::u16string_view(u"Core Audio"):std::u16string_view(item.device.name),item.id);
                    const auto base=runtime_allocate(memory,{},bytes.size());memory.write(GuestAddress{base},bytes);
                    std::vector<DirectSoundState::Enumeration> entries;entries.push_back({0,base+primary,0,base+empty,0});
                    for(const auto& item:pending)entries.push_back({base+item.guid,0,base+item.description,base+item.endpoint,0});
                    cached=bridge.enumerations.emplace(key,std::move(entries)).first;
                }
                std::vector<GuestCallback> calls;
                for(const auto& entry:cached->second)calls.push_back({arg(0),{entry.guid,entry.guid?entry.description:entry.primary,entry.endpoint,arg(1)}});
                start_callbacks(state,memory,std::move(calls),Continuation::ReturnUntilFalse,0);return std::nullopt;
            }
        }else if(name=="DirectSoundCreate" || name=="DirectSoundCreate8" || name=="DirectSoundCaptureCreate" || name=="DirectSoundCaptureCreate8"){
            check_out(arg(1),unit);write(arg(1),0,bits);
            Guid guid{};if(arg(0))memory.read(GuestAddress{arg(0)},guid);
            if(arg(2))result=0x80040110U;
            else if(name.starts_with("DirectSoundCapture")?!capture_guid(guid):!playback_host(guid))result=no_driver;
            else {auto object=std::make_shared<Object>();object->device=true;object->capture=name.starts_with("DirectSoundCapture");object->version8=name.ends_with("8");if(!object->capture)object->context->host=*playback_host(guid);write(arg(1),create(object),bits);}
        }else{
            const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end())throw std::invalid_argument("invalid DirectSound object");
            auto object=found->second;auto& o=*object;auto& pcm=*o.pcm;
            if(name=="QueryInterface"){
                check_out(arg(2),unit);write(arg(2),0,bits);Guid iid{};memory.read(GuestAddress{arg(1)},iid);
                std::uint64_t p=0;
                if(iid==unknown || (o.effect?iid==dsound::effect_iids[static_cast<unsigned>(o.effect->type())]:o.factory?iid==dsound::factory_iid:o.duplex?iid==dsound::duplex_iid:o.capture?(o.device?iid==dsound::capture_iid:iid==dsound::capture_buffer_iid || (o.version8 && iid==dsound::capture_buffer8_iid)):(o.device && (iid==sound || (o.version8 && iid==sound8))) || (!o.device && (iid==buffer || (o.version8 && !o.primary && iid==buffer8)))))p=o.canonical;
                else if(!o.device && !o.primary && (o.capture || (o.flags&0x100)) && iid==dsound::notify)p=create(object,4);
                else if(!o.device && o.primary && (o.flags&0x10) && iid==dsound::listener)p=create(object,5);
                else if(!o.device && !o.primary && (o.flags&0x10) && iid==dsound::source)p=create(object,6);
                if(p){++o.references;write(arg(2),p,bits);}else result=no_interface;
            }else if(name=="AddRef")result=++o.references;
            else if(name=="Release"){
                result=--o.references;if(!result){++o.references;release(arg(0));}
            }else if(o.effect){
                std::lock_guard lock(pcm.mutex);
                if(name=="FX_GetAllParameters"){const auto words=o.effect->parameters();check_out(arg(1),static_cast<unsigned>(words.size()*4));for(std::size_t i=0;i<words.size();++i)write(arg(1)+i*4,words[i]);}
                else if(name=="FX_SetAllParameters"){if(!arg(1))result=invalid;else {std::vector<std::uint32_t> words(o.effect->parameters().size());for(std::size_t i=0;i<words.size();++i)words[i]=static_cast<std::uint32_t>(read(arg(1),static_cast<unsigned>(i*4)));if(!o.effect->set_parameters(words))result=invalid;}}
                else if(name=="FX_SetPreset"){if(!o.effect->set_preset(static_cast<unsigned>(arg(1))))result=invalid;}
                else if(name=="FX_GetPreset"){check_out(arg(1),4);if(o.effect->preset()==UINT32_MAX)result=invalid_call;else write(arg(1),o.effect->preset());}
                else if(name=="FX_SetQuality"){if(!o.effect->set_quality(static_cast<unsigned>(arg(1))))result=invalid;}
                else if(name=="FX_GetQuality")write(arg(1),o.effect->quality());
                else result=no_interface;
            }else if(o.factory){
                if(name=="F_LockServer"){if(arg(1)){if(bridge.locks==UINT32_MAX)result=invalid;else ++bridge.locks;}else if(bridge.locks)--bridge.locks;else result=invalid;}
                else if(name=="F_CreateInstance"){check_out(arg(3),unit);write(arg(3),0,bits);if(arg(1))result=0x80040110U;else {Guid iid{};memory.read(GuestAddress{arg(2)},iid);try{write(arg(3),instantiate(o.cls,iid,false),bits);}catch(const std::invalid_argument&){result=no_interface;}}}
                else result=invalid_call;
            }else if(o.duplex){if(name=="F_Initialize")result=duplex(object,1);else result=invalid_call;
            }else if(o.device && !o.initialized && name!="DeviceInitialize" && name!="C_DeviceInitialize")result=0x887800aaU;
            else if(name.starts_with("L3_") || name.starts_with("B3_")){
                const bool listener=name.starts_with("L3_");const auto method=name.substr(3);auto& d=*o.context;
                const auto number=[&](unsigned i){std::uint32_t v=static_cast<std::uint32_t>(arg(i));if(!x86_ && i<4){v=0;for(unsigned j=0;j<4;++j)v|=std::uint32_t{state.xmm[i][j]}<<(j*8);}const auto f=std::bit_cast<float>(v);if(!std::isfinite(f))throw std::invalid_argument("3D 浮點數無效");return f;};
                const auto load_float=[&](std::uint64_t p){const auto f=std::bit_cast<float>(static_cast<std::uint32_t>(read(p,0)));if(!std::isfinite(f))throw std::invalid_argument("3D 浮點數無效");return f;};
                const auto load_vector=[&](std::uint64_t p){return Vector{load_float(p),load_float(p+4),load_float(p+8)};};
                const auto put_float=[&](std::uint64_t p,float f){write(p,std::bit_cast<std::uint32_t>(f));};
                const auto put_vector=[&](std::uint64_t p,Vector v){check_out(p,12);put_float(p,v.x);put_float(p+4,v.y);put_float(p+8,v.z);};
                const auto refresh=[&]{for(const auto& [p,child]:bridge.objects)if(p==child->canonical && child->context==o.context){std::lock_guard lock(child->pcm->mutex);update(*child);}};
                if(method=="CommitDeferredSettings"){
                    if(d.deferred){d.listener=d.pending;d.deferred=false;}
                    for(const auto& [p,child]:bridge.objects)if(p==child->canonical && child->context==o.context && child->deferred){child->source=child->pending;child->deferred=false;}refresh();
                }else if(method.starts_with("Get")){
                    const auto& l=d.listener;const auto& s=o.source;
                    if(method=="GetAllParameters"){
                        const auto p=arg(1);constexpr unsigned size=64;if(read(p,0)!=size)result=invalid;
                        else {check_out(p,size);put_vector(p+4,listener?l.position:s.position);put_vector(p+16,listener?l.velocity:s.velocity);
                            if(listener){put_vector(p+28,l.front);put_vector(p+40,l.top);put_float(p+52,l.distance);put_float(p+56,l.rolloff);put_float(p+60,l.doppler);}
                            else {write(p+28,s.inside);write(p+32,s.outside);put_vector(p+36,s.cone);write(p+48,static_cast<std::uint32_t>(s.cone_volume));put_float(p+52,s.minimum);put_float(p+56,s.maximum);write(p+60,s.mode);}
                        }
                    }else if(method=="GetPosition")put_vector(arg(1),listener?l.position:s.position);
                    else if(method=="GetVelocity")put_vector(arg(1),listener?l.velocity:s.velocity);
                    else if(method=="GetOrientation"){put_vector(arg(1),l.front);put_vector(arg(2),l.top);}
                    else if(method=="GetConeOrientation")put_vector(arg(1),s.cone);
                    else if(method=="GetConeAngles"){write(arg(1),s.inside);write(arg(2),s.outside);}
                    else if(method=="GetConeOutsideVolume")write(arg(1),static_cast<std::uint32_t>(s.cone_volume));
                    else if(method=="GetMode")write(arg(1),s.mode);
                    else put_float(arg(1),method=="GetDistanceFactor"?l.distance:method=="GetRolloffFactor"?l.rolloff:method=="GetDopplerFactor"?l.doppler:method=="GetMinDistance"?s.minimum:s.maximum);
                }else {
                    const auto apply=arg(service.arguments-1);if(apply>1)throw std::invalid_argument("3D 套用模式無效");
                    auto l=apply && d.deferred?d.pending:d.listener;auto s=apply && o.deferred?o.pending:o.source;
                    if(method=="SetAllParameters"){
                        const auto p=arg(1);if(read(p,0)!=64)throw std::invalid_argument("3D 結構大小無效");
                        if(listener){l.position=load_vector(p+4);l.velocity=load_vector(p+16);l.front=load_vector(p+28);l.top=load_vector(p+40);l.distance=load_float(p+52);l.rolloff=load_float(p+56);l.doppler=load_float(p+60);}
                        else {s.position=load_vector(p+4);s.velocity=load_vector(p+16);s.inside=static_cast<std::uint32_t>(read(p,28));s.outside=static_cast<std::uint32_t>(read(p,32));s.cone=load_vector(p+36);s.cone_volume=static_cast<std::int32_t>(read(p,48));s.minimum=load_float(p+52);s.maximum=load_float(p+56);s.mode=static_cast<std::uint32_t>(read(p,60));}
                    }else if(method=="SetPosition" || method=="SetVelocity" || method=="SetConeOrientation"){
                        const Vector v{number(1),number(2),number(3)};
                        if(method=="SetPosition"){if(listener)l.position=v;else s.position=v;}else if(method=="SetVelocity"){if(listener)l.velocity=v;else s.velocity=v;}else s.cone=v;
                    }else if(method=="SetOrientation"){l.front={number(1),number(2),number(3)};l.top={number(4),number(5),number(6)};}
                    else if(method=="SetConeAngles"){s.inside=static_cast<std::uint32_t>(arg(1));s.outside=static_cast<std::uint32_t>(arg(2));}
                    else if(method=="SetConeOutsideVolume")s.cone_volume=static_cast<std::int32_t>(arg(1));
                    else if(method=="SetMode")s.mode=static_cast<std::uint32_t>(arg(1));
                    else if(method=="SetMinDistance")s.minimum=number(1);else if(method=="SetMaxDistance")s.maximum=number(1);
                    else if(method=="SetDistanceFactor")l.distance=number(1);else if(method=="SetRolloffFactor")l.rolloff=number(1);else if(method=="SetDopplerFactor")l.doppler=number(1);
                    if(listener){if(l.distance<=0 || l.rolloff<0 || l.rolloff>10 || l.doppler<0 || l.doppler>10 || length(cross(l.front,l.top))<1e-6F)throw std::invalid_argument("3D listener 參數無效");if(apply){d.pending=l;d.deferred=true;}else d.listener=l;}
                    else {if(s.minimum<=0 || s.maximum<=0 || s.minimum>s.maximum || s.inside>360 || s.outside>360 || s.inside>s.outside || s.cone_volume>0 || s.cone_volume< -10000 || s.mode>2)throw std::invalid_argument("3D source 參數無效");if(apply){o.pending=s;o.deferred=true;}else o.source=s;}
                    if(!apply)refresh();
                }
            }else if(o.device && o.capture){
                if(name=="C_DeviceInitialize"){
                    if(o.initialized)result=0x88780082U;else {Guid guid{};if(arg(1))memory.read(GuestAddress{arg(1)},guid);if(!capture_guid(guid))result=no_driver;else o.initialized=true;}
                }else if(name=="C_DeviceCaps"){
                    if(read(arg(1),0)!=16)result=invalid;else {check_out(arg(1),16);write(arg(1)+4,0);write(arg(1)+8,0x00000fff);write(arg(1)+12,2);}
                }else if(name=="C_CreateCaptureBuffer"){
                    check_out(arg(2),unit);write(arg(2),0,bits);const auto p=arg(1);if(!p)throw std::invalid_argument("錄音描述為空");const auto size=read(p,0),flags=read(p,4),count=read(p,8),fmt=read(p,16,bits);
                    if(size!=(x86_?20U:24U) && size!=(x86_?28U:40U))result=invalid;else if(arg(3))result=0x80040110U;else if(flags || (size==(x86_?28U:40U) && read(p,x86_?20:24)))result=control_unavailable;
                    else if(count<4 || count>16*1024*1024)result=invalid;
                    else {auto child=std::make_shared<Object>();child->capture=true;child->version8=o.version8;child->context=o.context;format(fmt,*child->pcm);if(count%child->pcm->frame_bytes())throw std::invalid_argument("錄音大小未對齊");child->pcm->pcm.resize(static_cast<std::size_t>(count),child->pcm->bits==8?128:0);child->data=allocate_heap(memory,count);if(!child->data)throw std::bad_alloc{};child->samples->data=child->data;
                        try{memory.write(GuestAddress{child->data},child->pcm->pcm);write(arg(2),create(child),bits);}catch(...){(void)free_heap(memory,child->data,state);throw;}}
                }else result=invalid_call;
            }else if(o.device){
                if(name=="CreateSoundBuffer"){
                    check_out(arg(2),unit);write(arg(2),0,bits);
                    const auto p=arg(1);if(!p)throw std::invalid_argument("null descriptor");
                    const auto size=read(p,0),flags=read(p,4),bytes=read(p,8),fmt=read(p,16,bits);
                    if(std::getenv("X64ARM_TRACE_AUDIO"))std::fprintf(stderr,"[dsound-create] size=%llu flags=%llx bytes=%llu reserved=%llu format=%llx primary=%u\n",static_cast<unsigned long long>(size),static_cast<unsigned long long>(flags),static_cast<unsigned long long>(bytes),static_cast<unsigned long long>(read(p,12)),static_cast<unsigned long long>(fmt),static_cast<unsigned>((flags&1)!=0));
                    const bool primary=(flags&1)!=0;
                    if(size!=(x86_?36U:40U) && size!=(x86_?20U:24U))result=invalid;
                    else if(arg(3))result=0x80040110U;
                    else if(read(p,12) || (flags&12)==12)result=invalid;
                    else if(flags&~std::uint64_t{0x000fc3fb} || (flags&4))result=control_unavailable;
                    else if((flags&0x200) && !o.version8)result=control_unavailable;
                    else if((flags&0x10) && (flags&0x40))result=invalid;
                    else if(primary && (bytes || fmt || (flags&~std::uint64_t{0x900d1})))result=invalid;
                    else if(!primary && (bytes<4 || bytes>16*1024*1024))result=invalid;
                    else {
                        auto child=std::make_shared<Object>();child->version8=o.version8;child->primary=primary;child->flags=static_cast<std::uint32_t>(flags)|((flags&0x40000)?0U:8U);child->context=o.context;
                        if(!primary){
                            format(fmt,*child->pcm);if(bytes%child->pcm->frame_bytes())throw std::invalid_argument("unaligned PCM buffer");
                            if((flags&0x10) && child->pcm->channels!=1)throw std::invalid_argument("3D 緩衝區必須為單聲道");
                            if((flags&0x200) && bytes*1000<static_cast<std::uint64_t>(child->pcm->rate)*child->pcm->frame_bytes()*150){result=0x887800b4U;arg.return_to(state,result);return std::nullopt;}
                            child->pcm->pcm.resize(static_cast<std::size_t>(bytes),child->pcm->bits==8?128:0);
                            child->data=allocate_heap(memory,bytes);if(!child->data)throw std::bad_alloc{};
                        }else {
                            child->pcm->pcm.resize(child->pcm->rate*child->pcm->frame_bytes());child->data=allocate_heap(memory,child->pcm->pcm.size());if(!child->data)throw std::bad_alloc{};
                        }
                        child->samples->data=child->data;update(*child);
                        try {if(child->data)memory.write(GuestAddress{child->data},child->pcm->pcm);write(arg(2),create(child),bits);}
                        catch(...){if(child->data)(void)free_heap(memory,child->data,state);throw;}
                    }
                }else if(name=="DuplicateSoundBuffer"){
                    check_out(arg(2),unit);write(arg(2),0,bits);const auto original=bridge.objects.find(arg(1));
                    if(original==bridge.objects.end() || original->second->device || original->second->capture || original->second->effect || original->second->primary || (original->second->flags&0x200) || original->second->context!=o.context)result=invalid_call;
                    else {
                        auto old=original->second;auto child=std::make_shared<Object>();std::lock_guard lock(old->pcm->mutex);
                        child->version8=old->version8;child->flags=old->flags;child->context=o.context;child->samples=old->samples;child->data=old->data;child->volume=old->volume;child->pan=old->pan;child->source=old->source;
                        child->pcm->channels=old->pcm->channels;child->pcm->bits=old->pcm->bits;child->pcm->rate=old->pcm->rate;child->pcm->frequency=old->pcm->frequency;child->pcm->pcm=old->pcm->pcm;update(*child);write(arg(2),create(child),bits);
                    }
                }else if(name=="SetCooperativeLevel"){
                    if(!arg(1) || arg(2)<1 || arg(2)>4)result=invalid;else o.context->level=static_cast<std::uint32_t>(arg(2));
                }else if(name=="GetSpeakerConfig")write(arg(1),o.context->speakers);
                else if(name=="SetSpeakerConfig"){
                    const auto config=static_cast<std::uint32_t>(arg(1)),kind=config&0xffU,geometry=(config>>16)&0xffU;
                    if(config&0xff00ff00U || kind<1 || kind>8 || (kind==4 && geometry>180))result=invalid;else o.context->speakers=config;
                }else if(name=="Compact"){if(o.context->level==1)result=0x88780046U;else for(const auto& [p,child]:bridge.objects)if(p==child->canonical && child->context==o.context){bool stopped;{std::lock_guard lock(child->pcm->mutex);stopped=!child->pcm->playing;}if(stopped)child->output.reset();}}
                else if(name=="DeviceInitialize"){
                    if(o.initialized)result=0x88780082U;else {Guid guid{};if(arg(1))memory.read(GuestAddress{arg(1)},guid);const auto host=playback_host(guid);if(!host)result=no_driver;else {o.context->host=*host;o.initialized=true;}}
                }
                else if(name=="VerifyCertification")write(arg(1),1); // DS_UNCERTIFIED.
                else if(name=="DeviceCaps"){
                    if(read(arg(1),0)!=96)result=invalid;
                    else {check_out(arg(1),96);for(unsigned i=4;i<96;i+=4)write(arg(1)+i,0);write(arg(1)+4,0x00000f0f);write(arg(1)+8,100);write(arg(1)+12,200000);write(arg(1)+16,1);}
                }else result=unsupported;
            }else if(name=="C_Start"){
                if(arg(1)&~UINT64_C(1))result=invalid;else {if(!o.input){try{o.input=audio::open_input(o.pcm);}catch(const std::runtime_error&){result=no_driver;}}if(!result){std::lock_guard lock(pcm.mutex);pcm.looping=arg(1)!=0;pcm.playing=true;pcm.capture_failed=false;}}
            }else if(name=="Play"){
                if(o.primary && !(arg(3)&1))result=invalid;
                else if(arg(1) || (arg(2) && !(o.flags&0x40000)) || (arg(3)&~std::uint64_t{0x3f}) || ((arg(3)&6) && !(o.flags&0x40000)) || (arg(3)&6)==6)result=invalid;
                else if(arg(3)&2)result=control_unavailable;
                else if(std::any_of(o.effects.begin(),o.effects.end(),[](const auto& effect){return effect.hardware;}))result=control_unavailable;
                else {
                    {std::lock_guard lock(pcm.mutex);(void)allocate_effects(o);}
                    if(!o.output){try{o.output=audio::open_output(o.pcm,o.context->host);}catch(const std::runtime_error&){result=no_driver;}}
                    if(!result){std::lock_guard lock(pcm.mutex);if(!pcm.playing)for(auto& effect:pcm.effects)effect->reset();pcm.looping=(arg(3)&1)!=0;pcm.playing=true;}
                }
            }else {
                std::lock_guard lock(pcm.mutex);
                if(name=="BufferCaps"){
                    if(read(arg(1),0)!=(o.capture?16U:20U))result=invalid;
                    else {check_out(arg(1),o.capture?16:20);write(arg(1)+4,o.flags);write(arg(1)+8,pcm.pcm.size());write(arg(1)+12,0);if(!o.capture)write(arg(1)+16,0);}
                }else if(name=="GetStatus"){
                    if(o.capture && pcm.capture_failed)result=0x80004005U;
                    else {
                        const std::uint32_t status=pcm.playing?(pcm.looping?(o.capture?3U:5U):1U):0U;
                        write(arg(1),status);
                        static const bool diagnose=std::getenv("X64ARM_AUDIO_STATUS_DIAGNOSTIC")!=nullptr;
                        if(diagnose)std::fprintf(stderr,"[dsound-status] buffer=%llu status=%u rendered_frames=%llu cursor=%g\n",
                            static_cast<unsigned long long>(arg(0)),static_cast<unsigned>(status),
                            static_cast<unsigned long long>(pcm.rendered_frames),pcm.cursor);
                    }
                }
                else if(name=="GetCurrentPosition"){
                    if(o.capture && pcm.capture_failed)throw std::runtime_error("Core Audio 錄音失敗");
                    const auto pos=static_cast<std::uint64_t>(pcm.cursor)*pcm.frame_bytes();
                    if(arg(1))write(arg(1),pos);
                    if(arg(2) && o.capture)write(arg(2),pos);else if(arg(2))write(arg(2),pcm.pcm.empty()?0:(pos+(pcm.playing?std::min<std::size_t>(pcm.pcm.size()/pcm.frame_bytes(),pcm.frequency/50)*pcm.frame_bytes():0))%pcm.pcm.size());
                }else if(name=="Stop"){pcm.stop();if(o.flags&0x40000){pcm.effects.clear();o.flags&=~8U;for(auto& effect:o.effects)effect.allocated=false;}}
                else if(name=="Restore"){} // Host-owned buffers are never lost.
                else if(name=="SetCurrentPosition"){
                    if(o.primary || arg(1)>=pcm.pcm.size())result=invalid;else pcm.cursor=static_cast<double>(arg(1)/pcm.frame_bytes());
                }else if(name=="GetFormat"){
                    if(!arg(1)){if(!arg(3))result=invalid;else write(arg(3),18);}
                    else if(arg(2)<18)result=invalid;
                    else {check_out(arg(1),18);write(arg(1),1,16);write(arg(1)+2,pcm.channels,16);write(arg(1)+4,pcm.rate);write(arg(1)+8,pcm.rate*pcm.frame_bytes());write(arg(1)+12,pcm.frame_bytes(),16);write(arg(1)+14,pcm.bits,16);write(arg(1)+16,0,16);if(arg(3))write(arg(3),18);}
                }else if(name=="SetFormat"){
                    if(!o.primary)result=invalid_call;else if(o.context->level==1)result=0x88780046U;else if(o.locked)result=invalid_call;
                    else {audio::Buffer candidate;format(arg(1),candidate);const auto count=candidate.rate*candidate.frame_bytes();const auto data=allocate_heap(memory,count);if(!data)throw std::bad_alloc{};
                        std::vector<std::uint8_t> silence(count,candidate.bits==8?128:0);memory.write(GuestAddress{data},silence);(void)free_heap(memory,o.data,state);o.data=data;o.samples->data=data;
                        pcm.pcm=std::move(silence);pcm.channels=candidate.channels;pcm.bits=candidate.bits;pcm.rate=pcm.frequency=candidate.rate;pcm.cursor=0;
                    }
                }else if(name=="GetVolume" || name=="GetPan" || name=="GetFrequency" || name=="SetVolume" || name=="SetPan" || name=="SetFrequency"){
                    const bool volume=name.ends_with("Volume"),pan=name.ends_with("Pan");const auto flag=volume?0x80U:pan?0x40U:0x20U;
                    if(!(o.flags&flag) || (pan && (o.flags&0x10)))result=control_unavailable;
                    else if(name.starts_with("Get"))write(arg(1),volume?static_cast<std::uint32_t>(o.volume):pan?static_cast<std::uint32_t>(o.pan):pcm.frequency);
                    else {
                        const auto value=static_cast<std::int32_t>(arg(1));
                        if(volume){if(value>0 || value< -10000)result=invalid;else o.volume=value;}
                        else if(pan){if(value< -10000 || value>10000)result=invalid;else o.pan=value;}
                        else {if(value && (value<100 || value>200000))result=invalid;else pcm.frequency=value?static_cast<unsigned>(value):pcm.rate;}
                        if(o.primary){o.context->volume=o.volume;o.context->pan=o.pan;for(const auto& [p,child]:bridge.objects)if(p==child->canonical && child.get()!=&o && child->context==o.context){std::lock_guard child_lock(child->pcm->mutex);update(*child);}}
                        update(o);
                    }
                }else if(name=="Lock"){
                    auto offset=arg(1),count=arg(2);const auto flags=arg(7);
                    if((o.primary && o.context->level!=4) || o.locked)result=invalid_call;
                    else if(flags&~std::uint64_t{3})result=invalid;
                    else {
                        if(flags&1)offset=(static_cast<std::uint64_t>(pcm.cursor)*pcm.frame_bytes()+(pcm.playing?std::min<std::size_t>(pcm.pcm.size()/pcm.frame_bytes(),pcm.frequency/50)*pcm.frame_bytes():0))%pcm.pcm.size();
                        if(flags&2)count=pcm.pcm.size();
                        if(offset>=pcm.pcm.size() || !count || count>pcm.pcm.size() || offset%pcm.frame_bytes() || count%pcm.frame_bytes())result=invalid;
                        else {
                            if(o.capture)memory.write(GuestAddress{o.data},pcm.pcm);check_out(arg(3),unit);check_out(arg(4),4);if(arg(5))check_out(arg(5),unit);if(arg(6))check_out(arg(6),4);
                            auto first=std::min<std::uint64_t>(count,pcm.pcm.size()-offset),second=count-first;
                            if(second && (!arg(5) || !arg(6)))second=0;
                            write(arg(3),o.data+offset,bits);write(arg(4),first);if(arg(5))write(arg(5),second?o.data:0,bits);if(arg(6))write(arg(6),second);
                            o.locked=true;o.lock_first=o.data+offset;o.lock_second=second?o.data:0;o.lock_bytes1=static_cast<std::uint32_t>(first);o.lock_bytes2=static_cast<std::uint32_t>(second);
                        }
                    }
                }else if(name=="Unlock"){
                    if(!o.locked || arg(1)!=o.lock_first || arg(2)!=o.lock_bytes1 || arg(3)!=o.lock_second || arg(4)!=o.lock_bytes2)result=invalid;
                    else {
                        // Validate both guest ranges before publishing data to the render callback.
                        memory.check(GuestAddress{o.lock_first},o.lock_bytes1,Access::Read);if(o.lock_bytes2)memory.check(GuestAddress{o.lock_second},o.lock_bytes2,Access::Read);
                        if(!o.capture)memory.read(GuestAddress{o.lock_first},std::span(pcm.pcm).subspan(static_cast<std::size_t>(o.lock_first-o.data),o.lock_bytes1));
                        if(!o.capture && o.lock_bytes2)memory.read(GuestAddress{o.lock_second},std::span(pcm.pcm).first(o.lock_bytes2));o.locked=false;
                        for(const auto& [p,child]:bridge.objects)if(p==child->canonical && child.get()!=&o && child->samples==o.samples){std::lock_guard child_lock(child->pcm->mutex);child->pcm->pcm=pcm.pcm;}
                    }
                }else if(name=="N_SetNotificationPositions"){
                    if(pcm.playing)result=invalid_call;else if(arg(1)>100000 || (arg(1) && !arg(2)))result=invalid;
                    else {std::vector<audio::Buffer::Notification> positions;positions.reserve(static_cast<std::size_t>(arg(1)));const auto stride=x86_?8U:16U;
                        for(unsigned i=0;i<arg(1);++i){const auto p=arg(2)+i*stride;const auto offset=static_cast<std::uint32_t>(read(p,0));const auto event=read(p,x86_?4:8,bits);const auto it=runtime_.wait_objects.find(event);
                            if((offset!=UINT32_MAX && offset>=pcm.pcm.size()) || it==runtime_.wait_objects.end() || it->second.kind!=WaitObject::Kind::Event)throw std::invalid_argument("通知位置或事件無效");positions.push_back({offset,event});}
                        pcm.notifications=std::move(positions);pcm.pending_notifications.clear();
                    }
                }else if(name=="SetFX"){
                    if(!(o.flags&0x200))result=control_unavailable;
                    else if(pcm.playing || o.locked)result=invalid_call;
                    else if(arg(1)>64 || (!arg(1) && (arg(2) || arg(3))) || (arg(1) && !arg(2)))result=invalid;
                    else if(!gui_.com_initializations.contains(threads_.current) || gui_.com_initializations.at(threads_.current)<16)result=0x800401f0U;
                    else {const auto count=static_cast<unsigned>(arg(1)),stride=x86_?32U:40U;if(arg(3))check_out(arg(3),count*4);std::vector<Object::EffectEntry> effects;std::vector<std::shared_ptr<audio::Effect>> processing;std::vector<std::uint32_t> codes;effects.reserve(count);processing.reserve(count);codes.reserve(count);bool unknown_effect=false,unallocated=false;
                        for(unsigned i=0;i<count;++i){const auto p=arg(2)+i*stride;const auto size=read(p,0),flags=read(p,4);Guid cls{};memory.read(GuestAddress{p+8},cls);if(size!=stride || flags&~UINT64_C(3) || flags==3 || read(p,24,bits) || read(p,x86_?28:32,bits))throw std::invalid_argument("DSEFFECTDESC 無效");const auto it=std::find(dsound::effect_classes.begin(),dsound::effect_classes.end(),cls);
                            if(it==dsound::effect_classes.end()){unknown_effect=true;codes.push_back(5);continue;}codes.push_back(0);const auto type=static_cast<audio::Effect::Type>(it-dsound::effect_classes.begin());auto effect=std::make_shared<audio::Effect>(type);const bool hardware=(flags&1)!=0,allocated=!hardware && !(o.flags&0x40000);effects.push_back({effect,0,allocated,hardware});if(allocated)processing.push_back(effect);else if(hardware && !(o.flags&0x40000))unallocated=true;}
                        if(unknown_effect)result=no_interface;
                        else {for(const auto& old:o.effects)if(old.object)release(old.object);o.effects=std::move(effects);pcm.effects=std::move(processing);for(unsigned i=0;i<count;++i)codes[i]=o.effects[i].allocated?2U:(o.flags&0x40000)?3U:4U;result=unallocated?0x08780014U:0;}
                        if(arg(3))for(unsigned i=0;i<count;++i)write(arg(3)+i*4,codes[i]);}
                }else if(name=="AcquireResources"){
                    if(arg(1)&~std::uint64_t{0x3e} || (arg(1)&6)==6 || (arg(3)?arg(2)!=o.effects.size():arg(2)!=0))result=invalid;else if(arg(1)&2)result=control_unavailable;
                    else {if(arg(3))check_out(arg(3),static_cast<unsigned>(o.effects.size()*4));if(allocate_effects(o))result=0x08780014U;if(arg(3))for(std::size_t i=0;i<o.effects.size();++i)write(arg(3)+i*4,o.effects[i].allocated?2U:4U);}
                }else if(name=="GetObjectInPath"){
                    check_out(arg(4),unit);write(arg(4),0,bits);Guid cls{},iid{};memory.read(GuestAddress{arg(1)},cls);memory.read(GuestAddress{arg(3)},iid);auto index=arg(2);result=0x88781161U;
                    for(auto& entry:o.effects){const auto type=static_cast<unsigned>(entry.dsp->type());if(cls!=dsound::all_objects && cls!=dsound::effect_classes[type])continue;if(index){--index;continue;}if(iid!=unknown && iid!=dsound::effect_iids[type]){result=no_interface;break;}if(!entry.object){auto effect=std::make_shared<Object>();effect->effect=entry.dsp;effect->pcm=o.pcm;entry.object=create(effect);}++bridge.objects.at(entry.object)->references;write(arg(4),entry.object,bits);result=0;break;}
                }else if(name=="C_GetFXStatus"){if(arg(1))result=invalid;}else if(name=="BufferInitialize" || name=="C_BufferInitialize")result=0x88780082U;
                else result=unsupported;
            }
        }
    }catch(const std::bad_alloc&){result=0x8007000eU;}
     catch(const std::invalid_argument&){result=invalid;}
    arg.return_to(state,result);return std::nullopt;
}
}
