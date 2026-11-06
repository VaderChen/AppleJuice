#include "x64arm/windows_services.hpp"
#include "x64arm/audio_graph.hpp"
#include "x64arm/audio_adpcm.hpp"
#include "x64arm/unicode.hpp"
#include "xaudio_interfaces.hpp"
#include "audio_endpoints.hpp"
#include "pcm_wave_format.hpp"
#include "guest_xaudio.hpp"
#include "guest_xaudio_code.hpp"
#include "xaudio_layout.h"
#include "x64arm/pe_loader.hpp"
#include "x64arm/shared_wake.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <set>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace x64arm {
void xaudio_lowering::initialize(GuestMemory& memory,std::map<std::uint64_t,RuntimeImage>& images,bool x86,std::uint64_t gateway){
    const auto bytes=x86?std::span<const std::uint8_t>{image_x86}:std::span<const std::uint8_t>{image_x64};
    const auto base=gateway+image_offset;auto parsed=parse_pe_image(bytes,GuestAddress{base},true,memory.limit());
    if(!parsed.imports.empty() || !parsed.delay_imports.empty() || parsed.metadata.image_size>image_capacity)throw std::logic_error("XAudio2 常式有外部相依或超出映像範圍");
    for(const auto& [address,image]:images)if(base<address+image.size && address<base+parsed.metadata.image_size)throw std::logic_error("XAudio2 常式映像重疊");
    auto image=parsed.runtime;image.path=u"[guest-xaudio]";image.export_names.clear();image.export_ordinals.clear();
    for(const auto& [name,ordinal]:parsed.runtime.export_names)image.export_names.emplace(name,parsed.runtime.export_ordinals.at(ordinal));
    for(auto& mapping:parsed.mappings)mapping.permissions=(mapping.permissions&permission(Access::Execute))?read_execute:permission(Access::Read);
    map_pe_image(memory,parsed);images.emplace(base,std::move(image));
}
namespace {
using Graph=audio::VoiceGraph;
constexpr std::uint32_t invalid=0x88960001U,no_interface=0x80004002U,pointer_error=0x80004003U;
struct Method {std::string_view name;unsigned args;std::uint32_t narrow{};};
constexpr Method identity[]={{"QueryInterface",3},{"AddRef",1},{"Release",1}};
constexpr Method enumeration[]={{"GetDeviceCount",2},{"GetDeviceDetails",3,2},{"Initialize",3,6}};
constexpr Method engine_methods[]={{"RegisterForCallbacks",2},{"UnregisterForCallbacks",2},{"CreateSourceVoice",8,24},{"CreateSubmixVoice",8,60},{"CreateMasteringVoice27",7,60},{"StartEngine",1},{"StopEngine",1},{"CommitChanges",2,2},{"GetPerformanceData",2},{"SetDebugConfiguration",3}};
constexpr Method voice_methods[]={{"GetVoiceDetails",2},{"SetOutputVoices",2},{"SetEffectChain",2},{"EnableEffect",3,6},{"DisableEffect",3,6},{"GetEffectState",3,2},{"SetEffectParameters",5,26},{"GetEffectParameters",4,10},{"SetFilterParameters",3,4},{"GetFilterParameters",2},{"SetOutputFilterParameters",4,8},{"GetOutputFilterParameters",3},{"SetVolume",3,6},{"GetVolume",2},{"SetChannelVolumes",4,10},{"GetChannelVolumes",3,2},{"SetOutputMatrix",6,44},{"GetOutputMatrix",5,12},{"DestroyVoice",1}};
constexpr Method source_methods[]={{"Start",3,6},{"Stop",3,6},{"SubmitSourceBuffer",3},{"FlushSourceBuffers",1},{"Discontinuity",1},{"ExitLoop",2,2},{"GetState27",2},{"SetFrequencyRatio",3,6},{"GetFrequencyRatio",2},{"SetSourceSampleRate",2,2}};
constexpr Method factory_methods[]={{"CreateInstance",4},{"LockServer",2,2}};
struct Engine {
    std::shared_ptr<Graph> graph=std::make_shared<Graph>();
    std::shared_ptr<audio::Buffer> pcm=std::make_shared<audio::Buffer>();
    std::unique_ptr<audio::Output> output;
    std::vector<audio_endpoint::Endpoint> devices;
    std::set<std::uint64_t> callbacks;
    std::vector<float> quantum;
    std::size_t produced{};unsigned rate{48000},fraction{},version{7};
    bool initialized{},active{true},busy{},audible_reported{};
    std::vector<std::pair<std::uint64_t,Graph::Voice>> pass_voices;
    std::uint32_t ticket{};
    std::uint64_t handle{},master{},rendered{},cpu_ns{},last_cpu{},last_wall{};
    // X64ARM_TRACE_AUDIO：每 2 秒輸出一次區間缺糧，附 unix 毫秒，供量測區間對照載入與遊玩。
    std::uint64_t stats_wall{},stats_starved{},stats_runs{},stats_quanta{};
    std::array<std::uint32_t,6> debug{};
};
struct Object {enum Kind {Factory,EngineObject,Voice} kind{};std::shared_ptr<Engine> engine;Graph::Voice voice{};std::uint64_t callback{};std::uint32_t references{1};std::optional<audio::AdpcmFormat> adpcm{};};
void require(bool valid){if(!valid)throw std::invalid_argument("XAudio2 呼叫參數無效");}
std::uint64_t now(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
std::uint64_t thread_cpu(){timespec value{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value))throw HostCallFault("XAudio2 無法讀取音訊處理 CPU 時鐘");return static_cast<std::uint64_t>(value.tv_sec)*1000000000ULL+static_cast<std::uint64_t>(value.tv_nsec);}
bool trace_audio(){static const bool enabled=std::getenv("X64ARM_TRACE_AUDIO")!=nullptr;return enabled;}
bool void_method(std::string_view name){
    for(auto method:{"GetVoiceDetails","GetVolume","GetFrequencyRatio","GetFilterParameters","GetChannelVolumes","GetEffectState","GetEffectParameters","GetOutputFilterParameters","GetOutputMatrix","GetState27","GetState28","DestroyVoice","GetPerformanceData","SetDebugConfiguration","StopEngine","UnregisterForCallbacks"})if(name==method)return true;
    return false;
}
}
struct XAudioState {
    std::map<std::uint64_t,Object> objects;
    std::map<unsigned,std::uint64_t> tables;
    std::uint32_t locks{};
    std::uint32_t next_ticket{1};
};
std::span<const WindowsServiceDescriptor> xaudio_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> result{{"xaudio2_7.dll","DllGetClassObject",3},{"xaudio2_7.dll","DllCanUnloadNow",0},{"xaudio2_8.dll","XAudio2Create",3,0,0,6},{"xaudio2_9.dll","XAudio2Create",3,0,0,6}};
        for(auto methods:{std::span<const Method>(identity),std::span<const Method>(enumeration),std::span<const Method>(engine_methods),std::span<const Method>(voice_methods),std::span<const Method>(source_methods),std::span<const Method>(factory_methods)})for(const auto& m:methods)result.push_back({"internal-xaudio",m.name,m.args,0,0,m.narrow});
        result.push_back({"internal-xaudio","CreateMasteringVoice28",8,0,0,156});result.push_back({"internal-xaudio","GetState28",3,0,0,4});result.push_back({"internal-xaudio","GetChannelMask",2});
        for(auto name:{"Work","Render","Complete"})result.push_back({"internal-xaudio",name,2,0,0,2});return result;
    }();return table;
}
bool WindowsServices::xaudio_waiting()const {
    if(!xaudio_)return false;for(const auto& [p,o]:xaudio_->objects)if(o.kind==Object::EngineObject && o.engine->active && o.engine->output)return true;return false;
}
void WindowsServices::poll_xaudio(){
    if(!xaudio_)return;
    for(auto& [p,o]:xaudio_->objects)if(o.kind==Object::EngineObject){auto& e=*o.engine;if(!e.active || !e.output || e.busy)continue;
        std::lock_guard lock(e.pcm->mutex);auto& b=*e.pcm;
        if(trace_audio() && !e.audible_reported && (b.rendered_left_peak>0 || b.rendered_right_peak>0)){
            e.audible_reported=true;const auto time=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::fprintf(stderr,"xaudio_first_audible unix_us=%lld pid=%d stage=host_render frames=%llu peak_left=%g peak_right=%g\n",static_cast<long long>(time),static_cast<int>(::getpid()),static_cast<unsigned long long>(b.rendered_frames),double(b.rendered_left_peak),double(b.rendered_right_peak));
        }
        const auto consumed=std::min(b.pcm.size(),static_cast<std::size_t>(b.cursor)*b.frame_bytes());
        if(consumed){b.pcm.erase(b.pcm.begin(),b.pcm.begin()+static_cast<std::ptrdiff_t>(consumed));b.cursor-=static_cast<double>(consumed/b.frame_bytes());}
        if(b.pcm.size()/b.frame_bytes()>=e.rate*3/100)continue;
        if(!xaudio_->next_ticket)throw HostCallFault("XAudio2 工作序號耗盡");
        e.ticket=xaudio_->next_ticket++;wave_callbacks_.push_back({resolve("internal-xaudio","Work").value,{p,e.ticket}});e.busy=true;
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_xaudio(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("XAudio2 不能在 compare 模式使用");
    if(!xaudio_)xaudio_=std::make_shared<XAudioState>();auto& bridge=*xaudio_;
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto get=[&](std::uint64_t p,unsigned offset=0,std::uint8_t width=32){return memory.load(GuestAddress{p+offset},width);};
    const auto put=[&](std::uint64_t p,std::uint64_t v,std::uint8_t width=32){memory.store(GuestAddress{p},width,v);};
    const auto check=[&](std::uint64_t p,std::size_t count){memory.check(GuestAddress{p},count,Access::Write);};
    const auto floats=[&](std::uint64_t p,unsigned count){require(p && count<=4096);std::vector<float> v(count);memory.read(GuestAddress{p},{reinterpret_cast<std::uint8_t*>(v.data()),v.size()*4});return v;};
    const auto fp=[&](unsigned i){auto word=d(i);if(!x86_ && i<4)std::memcpy(&word,state.xmm[i].data(),4);return std::bit_cast<float>(word);};
    const auto object=[&](std::uint64_t p)->Object&{const auto i=bridge.objects.find(p);require(i!=bridge.objects.end());return i->second;};
    const auto method=[&](std::uint64_t p,unsigned slot){require(p!=0);return get(get(p,0,bits),slot*unit,bits);};
    const auto publish=[&](std::uint64_t p,const Object& o){if(o.kind!=Object::Voice)return;const auto info=o.engine->graph->state(o.voice);
        put(p+XA_VERSION,o.engine->version);put(p+XA_FLAGS,info.creation_flags);put(p+XA_ACTIVE,info.flags);put(p+XA_CHANNELS,info.format.channels);put(p+XA_RATE,info.format.rate);
        put(p+XA_VOLUME,std::bit_cast<std::uint32_t>(info.volume));put(p+XA_RATIO,std::bit_cast<std::uint32_t>(info.ratio));put(p+XA_FILTER,info.filter.type);put(p+XA_FILTER+4,std::bit_cast<std::uint32_t>(info.filter.frequency));put(p+XA_FILTER+8,std::bit_cast<std::uint32_t>(info.filter.one_over_q));put(p+XA_MASK,info.format.channels==1?4:3);
        memory.write(GuestAddress{p+XA_VOLUMES},{reinterpret_cast<const std::uint8_t*>(info.channel_volumes.data()),info.channel_volumes.size()*4});
        put(p+XA_CONTEXT,info.context,64);put(p+XA_QUEUED,info.queued);put(p+XA_SAMPLES,info.samples,64);
    };
    const auto create=[&](Object o){
        const auto version=o.engine?o.engine->version:7;const auto kind=o.kind==Object::Factory?0U:o.kind==Object::EngineObject?1U:2U+static_cast<unsigned>(o.engine->graph->state(o.voice).kind);const auto key=version*8+kind;
        if(!bridge.tables.contains(key)){
            std::vector<Method> methods;
            if(kind<2)methods.insert(methods.end(),std::begin(identity),std::end(identity));
            if(kind==0)methods.insert(methods.end(),std::begin(factory_methods),std::end(factory_methods));
            else if(kind==1){if(version==7)methods.insert(methods.end(),std::begin(enumeration),std::end(enumeration));for(auto m:engine_methods){if(m.name=="CreateMasteringVoice27" && version>=8)m={"CreateMasteringVoice28",8,156};methods.push_back(m);}}
            else {methods.insert(methods.end(),std::begin(voice_methods),std::end(voice_methods));if(kind==2)for(auto m:source_methods){if(m.name=="GetState27" && version>=8)m={"GetState28",3,4};methods.push_back(m);}if(kind==4 && version>=8)methods.push_back({"GetChannelMask",2});}
            const auto table=runtime_allocate(memory,{},methods.size()*unit);
            const auto image=runtime_.builtin_images.find(callback_return_.value+xaudio_lowering::image_offset);
            for(unsigned i=0;i<methods.size();++i){auto target=resolve("internal-xaudio",methods[i].name).value;
                if(image!=runtime_.builtin_images.end())if(const auto routine=image->second.export_names.find(std::string(methods[i].name));routine!=image->second.export_names.end())target=image->first+routine->second;
                put(table+i*unit,target,bits);
            }bridge.tables.emplace(key,table);
        }
        const auto pointer=allocate_heap(memory,XA_OBJECT_BYTES);if(!pointer)throw std::bad_alloc{};memory.write(GuestAddress{pointer},std::array<std::uint8_t,XA_OBJECT_BYTES>{});put(pointer,bridge.tables.at(key),bits);publish(pointer,o);bridge.objects.emplace(pointer,std::move(o));return pointer;
    };
    const auto new_engine=[&](unsigned version,bool initialized){auto e=std::make_shared<Engine>();e->version=version;e->initialized=initialized;e->devices=audio_endpoint::render();e->last_wall=now();const auto p=create({Object::EngineObject,e});e->handle=p;return p;};
    const auto iid=[&](std::uint64_t p){xaudio::Guid g{};memory.read(GuestAddress{p},g);return g;};
    const auto sends=[&](const std::shared_ptr<Engine>& engine,std::uint64_t p){std::vector<Graph::Send> result;
        if(!p){require(engine->master!=0);result.push_back({object(engine->master).voice,{},{}});return result;}
        const auto count=get(p),items=get(p,4,bits);require(count<=64 && (!count || items));
        for(unsigned i=0;i<count;++i){const auto at=items+i*(4+unit),flags=get(at),target=get(at,4,bits);require(!(flags&~128U));auto& o=object(target);require(o.kind==Object::Voice && o.engine==engine);result.push_back({o.voice,{},bool(flags&128U)});}return result;
    };
    const auto effect_chain=[&](std::uint64_t p){if(p && get(p))throw HostCallFault("尚未實作 XAudio2 的客體 XAPO 效果鏈");};
    std::uint64_t result=0;
    try {
        if(name=="DllCanUnloadNow")result=bridge.objects.empty() && !bridge.locks?0:1;
        else if(name=="DllGetClassObject" || name=="CoGetClassObject"){
            const bool com=name=="CoGetClassObject";const auto out=arg(com?4:2);check(out,unit);put(out,0,bits);
            if(!xaudio::is_class(memory,arg(0)) || (com && !(d(1)&1)))result=0x80040111U;
            else if(iid(arg(com?3:1))!=xaudio::factory && iid(arg(com?3:1))!=xaudio::unknown)result=no_interface;else put(out,create({Object::Factory,{}}),bits);
        }else if(name=="CoCreateInstance" || name=="XAudio2Create"){
            const bool com=name=="CoCreateInstance";const auto out=arg(com?4:0);check(out,unit);put(out,0,bits);
            if(com && arg(1))result=0x80040110U;else if(com && !(d(2)&1))result=0x80040154U;
            else if(com && !com_apartment(threads_.current))result=0x800401f0U;
            else if(com && iid(arg(3))!=xaudio::iid27 && iid(arg(3))!=xaudio::unknown)result=no_interface;
            // XAudio2Create 的旗標：DEBUG_ENGINE、STOP_ENGINE_WHEN_IDLE、1024_QUANTUM、NO_VIRTUAL_AUDIO_CLIENT。處理器 0 是 Windows 10 標頭的 XAUDIO2_USE_DEFAULT_PROCESSOR，內建混音由宿主排程，不受此值影響。
            else {if(!com)require(!(d(1)&~UINT32_C(0x1a001)));put(out,new_engine(com?7:service.family=="xaudio2_8.dll"?8:9,!com),bits);}
        }else {
            if(name=="Work" || name=="Render" || name=="Complete"){
                const auto found=bridge.objects.find(arg(0));
                if(found==bridge.objects.end() || found->second.kind!=Object::EngineObject || found->second.engine->ticket!=d(1)){arg.return_to(state,0);return std::nullopt;}
            }
            auto& o=object(arg(0));auto e=o.engine;
            if(name=="QueryInterface"){
                check(arg(2),unit);put(arg(2),0,bits);const auto query=iid(arg(1));require(o.kind!=Object::Voice);
                const auto accepted=o.kind==Object::Factory?xaudio::factory:e->version==7?xaudio::iid27:e->version==8?xaudio::iid28:xaudio::iid29;
                if(query!=xaudio::unknown && query!=accepted)result=no_interface;else {++o.references;put(arg(2),arg(0),bits);}
            }else if(name=="AddRef")result=++o.references;
            else if(name=="Release"){
                require(o.kind!=Object::Voice);result=--o.references;if(!result){
                    if(e){e->output.reset();if(trace_audio()){std::lock_guard lock(e->pcm->mutex);const auto& pcm=*e->pcm;
                        std::fprintf(stderr,"xaudio_output pid=%d version=%u rendered_frames=%llu starved_frames=%llu starved_runs=%llu peak_left=%g peak_right=%g\n",static_cast<int>(::getpid()),e->version,static_cast<unsigned long long>(pcm.rendered_frames),static_cast<unsigned long long>(pcm.starved_frames),static_cast<unsigned long long>(pcm.starved_runs),double(pcm.rendered_left_peak),double(pcm.rendered_right_peak));}
                        std::vector<std::uint64_t> voices;for(const auto& [p,item]:bridge.objects)if(item.engine==e && item.kind==Object::Voice)voices.push_back(p);for(auto p:voices){bridge.objects.erase(p);(void)free_heap(memory,p,state);}}
                    bridge.objects.erase(arg(0));(void)free_heap(memory,arg(0),state);
                }
            }else if(o.kind==Object::Factory){
                if(name=="LockServer"){if(d(1))++bridge.locks;else if(bridge.locks)--bridge.locks;}
                else if(name=="CreateInstance"){check(arg(3),unit);put(arg(3),0,bits);if(arg(1))result=0x80040110U;else if(iid(arg(2))!=xaudio::iid27 && iid(arg(2))!=xaudio::unknown)result=no_interface;else put(arg(3),new_engine(7,false),bits);}
                else throw HostCallFault("XAudio2 工廠方法不合法");
            }else if(name=="Work" || name=="Render" || name=="Complete"){
                require(o.kind==Object::EngineObject);std::vector<GuestCallback> callbacks;
                const auto call=[&](std::uint64_t target,unsigned slot,std::vector<std::uint64_t> values){callbacks.push_back({method(target,slot),std::move(values)});};
                if(!e->active || !e->output){e->busy=false;arg.return_to(state,0);return std::nullopt;}
                if(name=="Work"){
                    e->fraction+=e->rate;const auto frames=e->fraction/100;e->fraction%=100;e->quantum.assign(frames*2,0);e->produced=0;e->pass_voices.clear();
                    for(const auto cb:e->callbacks)call(cb,0,{cb});
                    for(const auto& [p,voice]:bridge.objects)if(voice.engine==e && voice.kind==Object::Voice && voice.callback){const auto info=e->graph->state(voice.voice);if(info.kind==Graph::Kind::Source && info.playing){
                        auto bytes=e->graph->required_bytes(voice.voice,frames,e->rate);
                        if(voice.adpcm){const auto& f=*voice.adpcm;const std::uint64_t decoded_block=f.samples_per_block*f.channels*2;bytes=static_cast<std::uint32_t>(std::min<std::uint64_t>(UINT32_MAX,((bytes+decoded_block-1)/decoded_block)*f.block_bytes));}
                        e->pass_voices.emplace_back(p,voice.voice);call(voice.callback,0,{voice.callback,bytes});
                    }}
                    callbacks.push_back({resolve("internal-xaudio","Render").value,{arg(0),e->ticket}});
                }else if(name=="Render"){
                    const auto began=thread_cpu();e->produced+=e->graph->render_until_event(std::span(e->quantum).subspan(e->produced*2),e->rate);e->cpu_ns+=thread_cpu()-began;
                    for(const auto& event:e->graph->take_events()){
                        if(event.kind==Graph::EventKind::CriticalError){e->active=false;e->busy=false;{std::lock_guard lock(e->pcm->mutex);e->pcm->playing=false;}for(const auto cb:e->callbacks)call(cb,2,{cb,0x88960001U});continue;}
                        for(const auto& [p,voice]:bridge.objects)if(voice.engine==e && voice.kind==Object::Voice && voice.voice==event.voice && voice.callback){
                            const unsigned slot=event.kind==Graph::EventKind::BufferStart?3:event.kind==Graph::EventKind::BufferEnd?4:event.kind==Graph::EventKind::LoopEnd?5:2;
                            if(slot==2)call(voice.callback,slot,{voice.callback});else call(voice.callback,slot,{voice.callback,event.context});break;
                        }
                    }
                    if(e->active && e->produced*2<e->quantum.size())callbacks.push_back({resolve("internal-xaudio","Render").value,{arg(0),e->ticket}});
                    else if(e->active){
                        for(const auto& [p,id]:e->pass_voices){const auto found=bridge.objects.find(p);if(found!=bridge.objects.end() && found->second.kind==Object::Voice && found->second.engine==e && found->second.voice==id)call(found->second.callback,1,{found->second.callback});}
                        for(const auto cb:e->callbacks)call(cb,1,{cb});callbacks.push_back({resolve("internal-xaudio","Complete").value,{arg(0),e->ticket}});
                    }
                }else {
                    std::uint64_t starved=0,runs=0,queued=0;bool report=false;
                    {std::lock_guard lock(e->pcm->mutex);const auto* bytes=reinterpret_cast<const std::uint8_t*>(e->quantum.data());e->pcm->pcm.insert(e->pcm->pcm.end(),bytes,bytes+e->quantum.size()*4);e->rendered+=e->produced;e->busy=false;
                     if(trace_audio()){++e->stats_quanta;const auto t=now();if(!e->stats_wall){e->stats_wall=t;e->stats_starved=e->pcm->starved_frames;e->stats_runs=e->pcm->starved_runs;}
                        else if(t-e->stats_wall>=2000000000ULL){report=true;starved=e->pcm->starved_frames-e->stats_starved;runs=e->pcm->starved_runs-e->stats_runs;queued=e->pcm->pcm.size()/8;
                            e->stats_wall=t;e->stats_starved=e->pcm->starved_frames;e->stats_runs=e->pcm->starved_runs;}}}
                    if(report){std::fprintf(stderr,"xaudio_stats pid=%d starved_frames=%llu starved_runs=%llu queued_frames=%llu quanta=%llu unix_ms=%lld\n",static_cast<int>(::getpid()),
                        static_cast<unsigned long long>(starved),static_cast<unsigned long long>(runs),static_cast<unsigned long long>(queued),static_cast<unsigned long long>(e->stats_quanta),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));e->stats_quanta=0;}
                }
                if(!callbacks.empty()){
                    // 回呼內的 resident getter 必須看見同一個 BufferStart／End 邊界。
                    for(const auto& [p,voice]:bridge.objects)if(voice.engine==e)publish(p,voice);
                    start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
                }
            }else if(o.kind==Object::EngineObject){
                if(name=="GetDeviceCount")put(arg(1),e->devices.size());
                else if(name=="GetDeviceDetails"){
                    require(d(1)<e->devices.size());check(arg(2),1068);std::vector<std::uint8_t> bytes(1068);memory.write(GuestAddress{arg(2)},bytes);const auto& device=e->devices[d(1)];
                    memory.write(GuestAddress{arg(2)},utf16_bytes(device.id.substr(0,255)));memory.write(GuestAddress{arg(2)+512},utf16_bytes(device.device.name.substr(0,255)));put(arg(2)+1024,device.device.default_output?15:0);
                    const auto p=arg(2)+1028;put(p,0xfffe,16);put(p+2,2,16);put(p+4,48000);put(p+8,384000);put(p+12,8,16);put(p+14,32,16);put(p+16,22,16);put(p+18,32,16);put(p+20,3);memory.write(GuestAddress{p+24},std::array<std::uint8_t,16>{3,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71});
                }else if(name=="Initialize"){require(!e->initialized && !(d(1)&~1U) && d(2));e->initialized=true;}
                else {require(e->initialized);
                    if(name=="RegisterForCallbacks"){for(unsigned i=0;i<3;++i)(void)method(arg(1),i);e->callbacks.insert(arg(1));}
                    else if(name=="UnregisterForCallbacks")e->callbacks.erase(arg(1));
                    else if(name=="StartEngine" || name=="StopEngine"){e->active=name=="StartEngine";if(!e->active){e->ticket=0;e->busy=false;}e->graph->start(e->active);std::lock_guard lock(e->pcm->mutex);e->pcm->playing=e->active;}
                    else if(name=="CommitChanges"){require(d(1)!=UINT32_MAX);e->graph->commit(d(1));}
                    else if(name=="SetDebugConfiguration"){require(!arg(2));if(arg(1)){std::array<std::uint32_t,6> config{};memory.read(GuestAddress{arg(1)},{reinterpret_cast<std::uint8_t*>(config.data()),24});e->debug=config;}else e->debug={};}
                    else if(name=="GetPerformanceData"){
                        check(arg(1),64);memory.write(GuestAddress{arg(1)},std::array<std::uint8_t,64>{});const auto time=now();put(arg(1),e->cpu_ns-e->last_cpu,64);put(arg(1)+8,time-e->last_wall,64);e->last_wall=time;e->last_cpu=e->cpu_ns;
                        unsigned active=0,total=0,submix=0;for(const auto& [p,v]:bridge.objects)if(v.kind==Object::Voice && v.engine==e){const auto vs=e->graph->state(v.voice);if(vs.kind==Graph::Kind::Source){++total;if(vs.playing)++active;}else if(vs.kind==Graph::Kind::Submix)++submix;}
                        std::lock_guard lock(e->pcm->mutex);put(arg(1)+24,e->pcm->pcm.size());put(arg(1)+28,e->pcm->pcm.size()/8);put(arg(1)+32,e->pcm->starved_runs);put(arg(1)+36,active);put(arg(1)+40,total);put(arg(1)+44,submix);
                    }else if(name=="CreateSourceVoice" || name=="CreateSubmixVoice" || name.starts_with("CreateMasteringVoice")){
                        check(arg(1),unit);put(arg(1),0,bits);Graph::Format format;Graph::Kind kind;unsigned flags=0,stage=0;float ratio=2;std::uint64_t callback=0,send=0;std::string device;std::optional<audio::AdpcmFormat> adpcm;
                        if(name=="CreateSourceVoice"){
                            const auto max_rate=e->version==7?200000U:384000U;
                            if(get(arg(2),0,16)==2){
                                std::array<std::uint8_t,50> encoded{};require(get(arg(2),16,16)==32);memory.read(GuestAddress{arg(2)},encoded);adpcm=audio::parse_adpcm_format(encoded);
                                require(adpcm && adpcm->rate>=1000 && adpcm->rate<=max_rate && adpcm->samples_per_block>=32 && adpcm->samples_per_block<=512 && std::ranges::equal(adpcm->coefficients,audio::adpcm_coefficients));
                                format={adpcm->channels,adpcm->rate,16,false};
                            }else {const auto parsed=read_pcm_wave_format(memory,arg(2),true,true,1000,max_rate,{64,32,true,true});require(bool(parsed));format={parsed->channels,parsed->rate,parsed->bits,parsed->floating,parsed->channel_mask};}
                            kind=Graph::Kind::Source;flags=d(3);ratio=fp(4);callback=arg(5);send=arg(6);effect_chain(arg(7));if(callback)for(unsigned i=0;i<7;++i)(void)method(callback,i);
                        }else if(name=="CreateSubmixVoice"){format={d(2),d(3),32,true};kind=Graph::Kind::Submix;flags=d(4);stage=d(5);send=arg(6);effect_chain(arg(7));}
                        else {require(!e->master && !d(4));format={d(2)?d(2):2U,d(3)?d(3):48000U,32,true};kind=Graph::Kind::Master;effect_chain(arg(6));
                            if(e->version==7){require(d(5)<e->devices.size());device=e->devices[d(5)].device.uid;}
                            else {if(arg(5)){const auto id=read_utf16(memory,GuestAddress{arg(5)});const auto i=std::find_if(e->devices.begin(),e->devices.end(),[&](const auto& v){return v.id==id;});require(i!=e->devices.end());device=i->device.uid;}require(d(7)<=11);}
                        }
                        require(format.rate<=(e->version==7?200000U:384000U));
                        if(kind==Graph::Kind::Master)require(format.rate%100==0);
                        const auto id=e->graph->create(kind,format,flags,stage,ratio);std::uint64_t p=0;
                        try {
                            if(kind!=Graph::Kind::Master)e->graph->set_sends(id,sends(e,send));
                            p=create({Object::Voice,e,id,callback});
                            bridge.objects.at(p).adpcm=std::move(adpcm);
                            if(kind==Graph::Kind::Master){(void)mapping_session();e->pcm->progress_notify=[wake=mapping_wake()]()noexcept{if(wake)wake->notify();};e->pcm->rate=e->pcm->frequency=e->rate=format.rate;e->pcm->channels=2;e->pcm->bits=32;e->pcm->floating=e->pcm->streaming=true;e->pcm->playing=e->active;e->pcm->pcm.reserve(e->rate*8/10);e->output=audio::open_output(e->pcm,device);require(bool(e->output));e->master=p;}
                            put(arg(1),p,bits);
                        }catch(...){if(p){bridge.objects.erase(p);(void)free_heap(memory,p,state);}e->graph->destroy(id);throw;}
                    }else throw HostCallFault("尚未實作 XAudio2 引擎方法："+std::string(name));
                }
            }else {
                auto info=e->graph->state(o.voice);Graph::Change c;c.voice=o.voice;
                const auto dest=[&](std::uint64_t p){if(!p)p=e->master;auto& target=object(p);require(target.kind==Object::Voice && target.engine==e);return target.voice;};
                const auto filter=[&](std::uint64_t p){return Graph::Filter{static_cast<unsigned>(get(p)),std::bit_cast<float>(static_cast<std::uint32_t>(get(p,4))),std::bit_cast<float>(static_cast<std::uint32_t>(get(p,8)))};};
                const auto store_filter=[&](std::uint64_t p,Graph::Filter f){check(p,12);put(p,f.type);put(p+4,std::bit_cast<std::uint32_t>(f.frequency));put(p+8,std::bit_cast<std::uint32_t>(f.one_over_q));};
                if(name=="GetVoiceDetails"){check(arg(1),e->version==7?12:16);put(arg(1),info.creation_flags);if(e->version>=8)put(arg(1)+4,info.flags);put(arg(1)+(e->version==7?4:8),info.format.channels);put(arg(1)+(e->version==7?8:12),info.format.rate);}
                else if(name=="GetChannelMask")put(arg(1),info.format.channels==1?4:3);
                else if(name=="SetOutputVoices")e->graph->set_sends(o.voice,sends(e,arg(1)));
                else if(name=="SetEffectChain"){effect_chain(arg(1));e->graph->set_effects(o.voice,{});}
                else if(name=="EnableEffect" || name=="DisableEffect" || name=="GetEffectState" || name=="SetEffectParameters" || name=="GetEffectParameters")result=invalid; // 空效果鏈沒有合法索引；非空鏈另有明確缺口。
                else if(name=="GetVolume")put(arg(1),std::bit_cast<std::uint32_t>(info.volume));
                else if(name=="GetFrequencyRatio")put(arg(1),std::bit_cast<std::uint32_t>(info.ratio));
                else if(name=="SetVolume" || name=="SetFrequencyRatio"){c.operation=name=="SetVolume"?Graph::Operation::Volume:Graph::Operation::Frequency;c.value=fp(1);require(d(2)!=UINT32_MAX);e->graph->change(std::move(c),d(2));}
                else if(name=="SetChannelVolumes"){require(d(1)==info.format.channels);c.operation=Graph::Operation::ChannelVolumes;c.values=floats(arg(2),d(1));e->graph->change(std::move(c),d(3));}
                else if(name=="GetChannelVolumes"){require(d(1)==info.format.channels);check(arg(2),d(1)*4);memory.write(GuestAddress{arg(2)},{reinterpret_cast<const std::uint8_t*>(info.channel_volumes.data()),d(1)*4U});}
                else if(name=="SetOutputMatrix" || name=="GetOutputMatrix"){
                    c.destination=dest(arg(1));const auto target=e->graph->state(c.destination);require(d(2)==info.format.channels && d(3)==target.format.channels);
                    if(name=="SetOutputMatrix"){c.operation=Graph::Operation::Matrix;c.values=floats(arg(4),d(2)*d(3));e->graph->change(std::move(c),d(5));}
                    else {const auto v=e->graph->matrix(o.voice,c.destination);check(arg(4),v.size()*4);memory.write(GuestAddress{arg(4)},{reinterpret_cast<const std::uint8_t*>(v.data()),v.size()*4});}
                }else if(name=="SetFilterParameters" || name=="SetOutputFilterParameters"){const bool output=name=="SetOutputFilterParameters";c.operation=output?Graph::Operation::OutputFilter:Graph::Operation::Filter;if(output)c.destination=dest(arg(1));c.filter=filter(arg(output?2:1));e->graph->change(std::move(c),d(output?3:2));}
                else if(name=="GetFilterParameters"){require(info.flags&8U);store_filter(arg(1),info.filter);}
                else if(name=="GetOutputFilterParameters")store_filter(arg(2),e->graph->output_filter(o.voice,dest(arg(1))));
                else if(name=="Start" || name=="Stop"){require(!(d(1)&~(name=="Stop"?32U:0U)));c.operation=name=="Start"?Graph::Operation::Start:Graph::Operation::Stop;c.tails=d(1)!=0;e->graph->change(std::move(c),d(2));}
                else if(name=="SubmitSourceBuffer"){
                    require(!arg(2));const auto p=arg(1);Graph::Packet packet;const auto flags=get(p),size=get(p,4),data=get(p,8,bits);require(!(flags&~64U) && size && size<=0x80000000ULL && data);packet.end_stream=(flags&64U)!=0;const unsigned fields=8+unit;
                    packet.play_begin=static_cast<unsigned>(get(p,fields));packet.play_length=static_cast<unsigned>(get(p,fields+4));packet.loop_begin=static_cast<unsigned>(get(p,fields+8));packet.loop_length=static_cast<unsigned>(get(p,fields+12));packet.loop_count=static_cast<unsigned>(get(p,fields+16));packet.context=get(p,fields+20,bits);
                    if(o.adpcm)require(packet.play_length%o.adpcm->samples_per_block==0 && packet.loop_length%o.adpcm->samples_per_block==0);
                    memory.check(GuestAddress{data},size,Access::Read);packet.pcm.resize(static_cast<std::size_t>(size));memory.read(GuestAddress{data},packet.pcm);
                    if(o.adpcm)packet.pcm=audio::decode_adpcm(packet.pcm,*o.adpcm);
                    e->graph->submit(o.voice,std::move(packet));
                }else if(name=="FlushSourceBuffers")e->graph->flush(o.voice);
                else if(name=="ExitLoop"){c.operation=Graph::Operation::ExitLoop;e->graph->change(std::move(c),d(1));}
                else if(name=="Discontinuity")e->graph->discontinuity(o.voice);
                else if(name=="GetState27" || name=="GetState28"){if(name=="GetState28")require(!(d(2)&~256U));check(arg(1),unit+12);put(arg(1),info.context,bits);put(arg(1)+unit,info.queued);put(arg(1)+unit+4,info.samples,64);}
                else if(name=="SetSourceSampleRate"){require(d(1)<=(e->version==7?200000U:384000U));e->graph->set_sample_rate(o.voice,d(1));}
                else if(name=="DestroyVoice"){e->graph->destroy(o.voice);if(arg(0)==e->master){e->output.reset();e->master=0;}bridge.objects.erase(arg(0));(void)free_heap(memory,arg(0),state);}
                else throw HostCallFault("尚未實作 XAudio2 音源方法："+std::string(name));
            }
        }
    }catch(const MemoryFault&){if(void_method(name))throw;result=pointer_error;}catch(const std::invalid_argument&){result=invalid;}catch(const std::bad_alloc&){result=0x8007000eU;}
    if(trace_audio() && ((result&0x80000000U) || name=="CoCreateInstance" || name=="CreateInstance" || name=="XAudio2Create" || name=="Initialize" || name.starts_with("CreateMasteringVoice")))
        std::fprintf(stderr,"xaudio_call pid=%d method=%.*s result=0x%08x\n",static_cast<int>(::getpid()),static_cast<int>(name.size()),name.data(),static_cast<unsigned>(result));
    for(const auto& [p,o]:bridge.objects)publish(p,o);
    poll_xaudio();arg.return_to(state,result);return std::nullopt;
}
}
