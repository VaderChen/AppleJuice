#include "x64arm/windows_services.hpp"
#include "x64arm/midi.hpp"
#include "x64arm/unicode.hpp"
#include "directmusic_interfaces.hpp"
#include <functional>
#include <map>
#include <set>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <bit>
#include <deque>

namespace x64arm {
namespace {
using namespace dmusic;
constexpr std::uint32_t invalid=0x80070057U,unsupported=0x80004001U,no_interface=0x80004002U,not_initialized=0x800401f0U;
struct MusicObject {
    char kind{};std::uint32_t references{1},repeats{};
    std::uint64_t segment{},active_state{};
    std::uint64_t canonical{},default_path{},performance{};
    std::uint64_t core{},clock_object{},raw_buffer{};
    std::uint32_t buffer_capacity{},buffer_used{},buffer_read{},port_voices{64};
    std::int64_t buffer_start{},last_clock{INT64_MIN},latency{};
    bool buffer_start_set{},directsound_set{},port_active{};
    Guid buffer_format{MIDI_BufferFormat};
    struct Advice {std::int64_t due{},period{};std::uint64_t handle{};};
    std::map<std::uint64_t,Advice> advice;
    std::uint64_t next_advice{1};
    std::map<char,std::uint64_t> interfaces;
    std::vector<std::uint64_t> runs;
    std::uint32_t start_point{},loop_start{},loop_end{},resolution{},prepare{1000},bumper{50},channels{16},path_type{8};
    std::int32_t volume{};double master_tempo{1},tempo{120};bool auto_download{},cache_enabled{true};
    std::chrono::steady_clock::time_point epoch{std::chrono::steady_clock::now()};
    std::int64_t adjustment{};
    struct ClockPoint {std::int64_t reference;double music,bpm;};
    std::vector<ClockPoint> clock{{0,0,120}};
    std::shared_ptr<midi::Playback::Voice> voice;
    std::weak_ptr<midi::Playback> voice_playback;
    std::int32_t music_start{};
    std::int64_t reference_start{};
    std::uint64_t observed_loops{};bool observed_start{},observed_end{};
    struct Cached {std::u16string filename,name;Guid id{};std::uint64_t pointer{};};
    std::vector<Cached> cache;
    std::u16string filename,title,category;Guid id{},cls{};
    struct Allocation {std::uint32_t size;std::uint64_t user{};};
    std::map<std::uint64_t,Allocation> messages;
    std::set<Guid> notification_types;
    std::uint64_t notification_event{};
    struct Notification {Guid type;std::uint32_t option;std::int64_t reference;std::int32_t music;std::uint64_t state;};
    std::deque<Notification> notifications;
    std::u16string directory;
    std::shared_ptr<const midi::Sequence> sequence;
    std::set<std::uint64_t> downloads;
    std::shared_ptr<midi::Playback> playback;
    std::unique_ptr<midi::Output> output;
};
std::span<const WindowsServiceDescriptor> methods(char kind){switch(kind){case 'L':return L_methods;case 'P':return P_methods;case 'S':return S_methods;case 'A':return A_methods;case 'O':return O_methods;case 'F':return F_methods;case 'C':return C_methods;case 'B':return B_methods;case 'R':return R_methods;case 'K':return K_methods;default:return T_methods;}}
bool accepts(char kind,const Guid& iid){
    if(iid==IUnknown)return true;
    if(kind=='F')return iid==IClassFactory;
    if(kind=='C')return iid==IID_IDirectMusic || iid==IID_IDirectMusic2 || iid==IID_IDirectMusic8;
    if(kind=='B')return iid==IID_IDirectMusicBuffer;
    if(kind=='R')return iid==IID_IDirectMusicPort;
    if(kind=='K')return iid==IID_IReferenceClock;
    if(kind=='L')return iid==IID_IDirectMusicLoader || iid==IID_IDirectMusicLoader8;
    if(kind=='P')return iid==IID_IDirectMusicPerformance || iid==IID_IDirectMusicPerformance2 || iid==IID_IDirectMusicPerformance8;
    if(kind=='S')return iid==IID_IDirectMusicSegment || iid==IID_IDirectMusicSegment2 || iid==IID_IDirectMusicSegment8 || iid==IID_IDirectMusicObject;
    if(kind=='A')return iid==IID_IDirectMusicAudioPath;
    return iid==IID_IDirectMusicSegmentState || iid==IID_IDirectMusicSegmentState8;
}
std::int64_t now(const MusicObject& o){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-o.epoch).count()/100+o.adjustment;}
double reference_music(const MusicObject& o,std::int64_t reference){const auto* point=&o.clock.front();for(const auto& t:o.clock){if(t.reference>reference)break;point=&t;}return point->music+static_cast<double>(reference-point->reference)*768*point->bpm/600000000.0;}
std::int64_t music_to_reference(const MusicObject& o,std::int32_t music){const auto* point=&o.clock.front();for(const auto& t:o.clock){if(t.music>music)break;point=&t;}return point->reference+static_cast<std::int64_t>((static_cast<double>(music)-point->music)*600000000.0/(768*point->bpm));}
std::int32_t reference_to_music(const MusicObject& o,std::int64_t reference){const auto music=reference_music(o,reference);if(music<INT32_MIN || music>INT32_MAX)throw std::invalid_argument("MUSIC_TIME 溢位");return static_cast<std::int32_t>(music);}
void clock_change(MusicObject& o,std::int64_t reference,double bpm){const auto music=reference_music(o,reference);std::erase_if(o.clock,[&](const auto& t){return t.reference>=reference;});o.clock.push_back({reference,music,bpm});}
[[maybe_unused]] std::int64_t add_time(std::int64_t a,std::int64_t b){if((b>0 && a>INT64_MAX-b) || (b<0 && a<INT64_MIN-b))throw std::invalid_argument("REFERENCE_TIME 溢位");return a+b;}
[[maybe_unused]] char class_kind(const Guid& cls){return cls==CLSID_DirectMusicLoader?'L':cls==CLSID_DirectMusicPerformance?'P':cls==CLSID_DirectMusic?'C':'S';}
}
struct DirectMusicState {std::map<std::uint64_t,std::shared_ptr<MusicObject>> objects;std::map<char,std::uint64_t> tables;std::uint32_t locks{};};
std::span<const WindowsServiceDescriptor> directmusic_service_descriptors(){
    static const auto table=[] {std::vector<WindowsServiceDescriptor> result;for(const auto kind:{'L','P','S','T','A','O','F','C','B','R','K'})for(const auto& method:methods(kind))result.push_back(method);for(auto dll:{"dmusic.dll","dmime.dll","dmloader.dll"}){result.push_back({dll,"DllGetClassObject",3});result.push_back({dll,"DllCanUnloadNow",0});}return result;}();return table;
}
void WindowsServices::poll_directmusic(){
    if(!dmusic_)return;
    for(const auto& [p,clock]:dmusic_->objects)if(p==clock->canonical && clock->kind=='K'){
        const auto reference=now(*clock)+clock->latency;
        for(auto it=clock->advice.begin();it!=clock->advice.end();){auto& advice=it->second;if(reference<advice.due){++it;continue;}
            const auto event=runtime_.wait_objects.find(advice.handle);
            if(advice.period){
                const auto ticks=static_cast<std::uint64_t>((reference-advice.due)/advice.period)+1;
                if(event!=runtime_.wait_objects.end() && event->second.kind==WaitObject::Kind::Semaphore){auto& semaphore=runtime_.wait_state(event->second);semaphore.count+=static_cast<std::uint32_t>(std::min<std::uint64_t>(ticks,semaphore.maximum-semaphore.count));}
                if(ticks>static_cast<std::uint64_t>(INT64_MAX-advice.due)/static_cast<std::uint64_t>(advice.period)){it=clock->advice.erase(it);continue;}
                advice.due+=static_cast<std::int64_t>(ticks)*advice.period;++it;
            }else {if(event!=runtime_.wait_objects.end() && event->second.kind==WaitObject::Kind::Event)event->second.set_signal(true);it=clock->advice.erase(it);}
        }
    }
    for(const auto& [p,perf]:dmusic_->objects){if(p!=perf->canonical || perf->kind!='P' || !perf->playback)continue;
        for(auto address:perf->runs){const auto it=dmusic_->objects.find(address);if(it==dmusic_->objects.end())continue;auto& run=*it->second;auto playback=run.voice_playback.lock();if(!playback)continue;std::lock_guard lock(playback->mutex);
            const auto queue=[&](Guid type,std::uint32_t option){if(!perf->notification_types.contains(type) || perf->notifications.size()>=4096)return;++run.references;perf->notifications.push_back({type,option,now(*perf),reference_to_music(*perf,now(*perf)),address});if(perf->notification_event){const auto event=runtime_.wait_objects.find(perf->notification_event);if(event!=runtime_.wait_objects.end() && event->second.kind==WaitObject::Kind::Event)event->second.set_signal(true);}};
            if(run.voice->started && !run.observed_start){queue(GUID_NOTIFICATION_SEGMENT,0);queue(GUID_NOTIFICATION_PERFORMANCE,0);run.observed_start=true;}
            while(run.observed_loops<run.voice->loops){queue(GUID_NOTIFICATION_SEGMENT,3);++run.observed_loops;}
            if(run.voice->finished && !run.observed_end){queue(GUID_NOTIFICATION_SEGMENT,run.voice->aborted?4:1);queue(GUID_NOTIFICATION_PERFORMANCE,1);run.observed_end=true;}
        }
    }
}
bool WindowsServices::directmusic_waiting()const{
    if(dmusic_)for(const auto& [p,o]:dmusic_->objects)if(p==o->canonical && o->kind=='K' && !o->advice.empty())return true;
    if(!dmusic_)return false;for(const auto& [p,o]:dmusic_->objects)if(p==o->canonical && o->kind=='P' && o->notification_event && !o->notification_types.empty())for(auto run:o->runs){const auto it=dmusic_->objects.find(run);if(it==dmusic_->objects.end())continue;if(auto playback=it->second->voice_playback.lock()){std::lock_guard lock(playback->mutex);if(!it->second->voice->finished)return true;}}return false;
}
std::optional<std::uint32_t> WindowsServices::invoke_directmusic(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("DirectMusic 不能在 compare 模式使用");
    if(!dmusic_)dmusic_=std::make_shared<DirectMusicState>();auto& bridge=*dmusic_;poll_directmusic();
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;
    const bool activation=service.symbol=="CoCreateInstance" || service.symbol=="CoGetClassObject" || service.symbol=="DllGetClassObject";const auto name=activation?service.symbol:service.symbol.substr(2);
    const auto check=[&](std::uint64_t p,unsigned size){if(!p)throw std::invalid_argument("null output");memory.check(GuestAddress{p},size,Access::Write);};
    const auto put=[&](std::uint64_t p,std::uint64_t value,std::uint8_t width){check(p,width/8);memory.store(GuestAddress{p},width,value);};
    const auto object=[&](std::uint64_t p,char kind=0){const auto it=bridge.objects.find(p);if(it==bridge.objects.end() || (kind && it->second->kind!=kind && !(kind=='O' && it->second->kind=='S')))throw std::invalid_argument("invalid DirectMusic object");return it->second;};
    const auto hold=[&](std::uint64_t p){auto o=object(p);if(o->references==UINT32_MAX)throw std::invalid_argument("reference overflow");return ++o->references;};
    std::function<void(std::uint64_t)> release;
    release=[&](std::uint64_t p){auto o=object(p);if(--o->references)return;
        if(o->kind=='P')for(auto address:o->runs){const auto run=object(address);if(auto pb=run->voice_playback.lock()){std::lock_guard lock(pb->mutex);run->voice->playing=false;run->voice->aborted=true;}}
        o->output.reset();if(o->raw_buffer)(void)free_heap(memory,o->raw_buffer,state);for(const auto& [kind,address]:o->interfaces){(void)kind;bridge.objects.erase(address);(void)free_heap(memory,address,state);}for(auto run:o->runs)release(run);if(o->segment)release(o->segment);if(o->default_path)release(o->default_path);if(o->clock_object)release(o->clock_object);if(o->kind=='P' && o->core)release(o->core);for(auto& item:o->cache)if(item.pointer)release(item.pointer);for(auto [message,allocation]:o->messages){if(allocation.user)release(allocation.user);(void)free_heap(memory,message,state);}for(const auto& notification:o->notifications)if(notification.state)release(notification.state);if(o->kind=='P' || o->kind=='C')for(auto& [address,other]:bridge.objects){(void)address;other->downloads.erase(o->canonical);if(other->performance==o->canonical)other->performance=0;if(other->core==o->canonical)other->core=0;}};
    const auto create=[&](std::shared_ptr<MusicObject> o,char interface=0){
        const auto kind=interface?interface:o->kind;if(o->interfaces.contains(kind))return o->interfaces.at(kind);
        if(bridge.objects.size()>=256)throw std::bad_alloc{};
        if(!bridge.tables.contains(kind)){
            const auto descriptors=methods(kind);const auto table=runtime_allocate(memory,{},descriptors.size()*unit);
            for(std::size_t i=0;i<descriptors.size();++i)memory.store(GuestAddress{table+i*unit},bits,resolve("internal-dmusic",descriptors[i].symbol).value);
            bridge.tables.emplace(kind,table);
        }
        const auto p=allocate_heap(memory,unit);if(!p)throw std::bad_alloc{};
        try {memory.store(GuestAddress{p},bits,bridge.tables.at(kind));bridge.objects.emplace(p,o);o->interfaces.emplace(kind,p);if(!o->canonical)o->canonical=p;}catch(...){(void)free_heap(memory,p,state);throw;}return p;
    };
    const auto failed=[](MusicObject& o){std::lock_guard lock(o.playback->mutex);return o.playback->failed;};
    const auto activate_port=[](MusicObject& port,bool active){
        if(active){if(!port.output)port.output=midi::open_output(port.playback);std::lock_guard lock(port.playback->mutex);port.playback->active=true;port.port_active=true;}
        else {port.output.reset();std::lock_guard lock(port.playback->mutex);port.playback->active=false;port.playback->messages.clear();for(auto& channel:port.playback->direct_notes)channel.fill({});port.playback->active_notes=0;port.playback->reset=true;port.port_active=false;}
    };
    const auto port_caps=[&](std::uint64_t p){check(p,308);if(memory.load(GuestAddress{p},32)!=308)throw std::invalid_argument("DMUS_PORTCAPS 大小無效");std::array<std::uint8_t,308> bytes{};memory.write(GuestAddress{p},bytes);put(p,308,32);put(p+4,0x104,32);memory.write(GuestAddress{p+8},CLSID_DirectMusicSynth);put(p+24,1,32);put(p+28,1,32);put(p+36,1,32);put(p+40,64,32);put(p+44,2,32);memory.write(GuestAddress{p+52},utf16_bytes(u"macOS DLS MIDI 合成器"));};
    const auto make_clock=[&](MusicObject& source,std::int64_t latency){
        if(!source.clock_object){auto clock=std::make_shared<MusicObject>();clock->kind='K';clock->epoch=source.epoch;clock->adjustment=source.adjustment;clock->latency=latency;source.clock_object=create(clock);}return source.clock_object;
    };
    struct BufferEvent {std::int64_t reference{};std::uint32_t group{},length{},flags{},next{};std::uint64_t data{};};
    const auto buffer_event=[&](const MusicObject& buffer,std::uint32_t offset){
        if(offset>buffer.buffer_used || buffer.buffer_used-offset<20)throw std::invalid_argument("MIDI buffer 事件標頭不完整");
        const auto p=buffer.raw_buffer+offset;BufferEvent event;event.length=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));event.group=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
        event.reference=add_time(buffer.buffer_start,static_cast<std::int64_t>(memory.load(GuestAddress{p+8},64)));event.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{p+16},32));event.data=p+20;
        const auto size=(20ULL+event.length+7)&~UINT64_C(7);if(!event.length || event.flags>1 || size>buffer.buffer_used-offset || (event.flags && event.length>3))throw std::invalid_argument("MIDI buffer 事件內容不完整");event.next=offset+static_cast<std::uint32_t>(size);return event;
    };
    const auto stop_run=[&](MusicObject& run){
        if(auto playback=run.voice_playback.lock()){std::lock_guard lock(playback->mutex);run.voice->playing=false;run.voice->aborted=true;}
        if(!run.observed_end && run.performance && bridge.objects.contains(run.performance)){
            auto perf=object(run.performance);run.observed_end=true;
            if(perf->notification_types.contains(GUID_NOTIFICATION_SEGMENT) && perf->notifications.size()<4096){hold(run.canonical);const auto reference=now(*perf);perf->notifications.push_back({GUID_NOTIFICATION_SEGMENT,4,reference,reference_to_music(*perf,reference),run.canonical});
                const auto event=runtime_.wait_objects.find(perf->notification_event);if(event!=runtime_.wait_objects.end() && event->second.kind==WaitObject::Kind::Event)event->second.set_signal(true);}
        }
    };
    const auto stop=[&](MusicObject& o){if(o.playback){std::lock_guard lock(o.playback->mutex);o.playback->playing=false;o.playback->reset=true;}for(auto run:o.runs){stop_run(*object(run));release(run);}o.runs.clear();o.active_state=0;};
    const auto make_audio_path=[&](MusicObject& perf,std::uint32_t type,std::uint32_t channels,bool active){
        if((type!=7 && type!=8) || !channels || channels>16)throw std::invalid_argument("音訊路徑種類或 PChannel 數量無效");
        auto a=std::make_shared<MusicObject>();a->kind='A';a->path_type=type;a->channels=channels;a->performance=perf.canonical;a->playback=std::make_shared<midi::Playback>();a->playback->active=active;a->playback->mono=type==7;a->output=midi::open_output(a->playback);return create(a);
    };
    const auto descriptor=[&](std::uint64_t p){if(memory.load(GuestAddress{p},32)!=(x86_?848U:856U))throw std::invalid_argument("DMUS_OBJECTDESC 大小無效");memory.check(GuestAddress{p},x86_?848:856,Access::Read);return static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));};
    const auto describe=[&](const MusicObject& o,std::uint64_t p){check(p,x86_?848:856);if(memory.load(GuestAddress{p},32)!=(x86_?848U:856U))throw std::invalid_argument("DMUS_OBJECTDESC 大小無效");std::vector<std::uint8_t> bytes(x86_?848:856);memory.write(GuestAddress{p},bytes);put(p,x86_?848:856,32);put(p+4,2U|0x200U|(o.category.empty()?0:8U)|(o.filename.empty()?0:0x30U)|(o.title.empty()?0:4U)|(o.id==Guid{}?0:1U),32);memory.write(GuestAddress{p+8},o.id);memory.write(GuestAddress{p+24},CLSID_DirectMusicSegment);memory.write(GuestAddress{p+56},utf16_bytes(o.title.substr(0,63)));memory.write(GuestAddress{p+184},utf16_bytes(o.category.substr(0,63)));memory.write(GuestAddress{p+312},utf16_bytes(o.filename.substr(0,259)));};
    const auto allocate_message=[&](MusicObject& perf,std::uint32_t size){if(size<(x86_?56U:72U) || size>16*1024*1024 || perf.messages.size()>=4096)throw std::invalid_argument("PMsg 大小或數量無效");const auto p=allocate_heap(memory,size);if(!p)throw std::bad_alloc{};std::vector<std::uint8_t> bytes(size);memory.write(GuestAddress{p},bytes);put(p,size,32);perf.messages.emplace(p,MusicObject::Allocation{size,0});return p;};
    const auto free_message=[&](MusicObject& perf,std::uint64_t p){const auto found=perf.messages.find(p);if(found==perf.messages.end())throw std::invalid_argument("PMsg 所有權無效");const auto user=found->second.user?found->second.user:memory.load(GuestAddress{p+(x86_?52:64)},bits);if(user)release(user);(void)free_heap(memory,p,state);perf.messages.erase(found);};
    const auto parameter=[&](const midi::Sequence& sequence,const Guid& guid,std::uint32_t music,std::uint64_t next,std::uint64_t out){
        if(music>sequence.music_length)return invalid;
        std::uint32_t following=0;
        if(guid==GUID_TempoParam){const midi::Tempo* point=&sequence.tempos.front();for(const auto& t:sequence.tempos){if(t.music>music){following=t.music-music;break;}point=&t;}check(out,16);put(out,static_cast<std::uint32_t>(point->music-music),32);put(out+8,std::bit_cast<std::uint64_t>(point->bpm),64);}
        else if(guid==GUID_TimeSignature){const midi::TimeSignature* point=&sequence.signatures.front();for(const auto& t:sequence.signatures){if(t.music>music){following=t.music-music;break;}point=&t;}check(out,8);put(out,static_cast<std::uint32_t>(point->music-music),32);put(out+4,point->beats,8);put(out+5,point->beat,8);put(out+6,point->grids,16);}
        else return unsupported;
        if(next)put(next,following,32);return 0U;
    };
    const auto set_parameter=[&](MusicObject& segment,const Guid& guid,std::uint32_t music,std::uint64_t data){
        if(!segment.sequence || music>segment.sequence->music_length)return invalid;
        auto seq=std::make_shared<midi::Sequence>(*segment.sequence);
        if(guid==GUID_TimeSignature){const auto beats=memory.load(GuestAddress{data+4},8),beat=memory.load(GuestAddress{data+5},8),grids=memory.load(GuestAddress{data+6},16);if(!beats || !beat || (beat&(beat-1)) || !grids || grids>3072/beat)return invalid;std::erase_if(seq->signatures,[&](const auto& t){return t.music==music;});seq->signatures.push_back({music,static_cast<std::uint8_t>(beats),static_cast<std::uint8_t>(beat),static_cast<std::uint16_t>(grids)});std::sort(seq->signatures.begin(),seq->signatures.end(),[](const auto& a,const auto& b){return a.music<b.music;});}
        else if(guid==GUID_TempoParam){const auto bpm=std::bit_cast<double>(memory.load(GuestAddress{data+8},64));if(!std::isfinite(bpm) || bpm<1 || bpm>1000)return invalid;std::erase_if(seq->tempos,[&](const auto& t){return t.music==music;});seq->tempos.push_back({music,0,bpm});std::sort(seq->tempos.begin(),seq->tempos.end(),[](const auto& a,const auto& b){return a.music<b.music;});for(std::size_t i=1;i<seq->tempos.size();++i){const auto& previous=seq->tempos[i-1];seq->tempos[i].frame=previous.frame+static_cast<std::uint64_t>(static_cast<double>(seq->tempos[i].music-previous.music)*48000*60/(768*previous.bpm));}for(auto& e:seq->events)e.frame=seq->frame_at(segment.sequence->music_at(e.frame));seq->frames=seq->frame_at(seq->music_length);if(seq->frames>48000*600ULL)return invalid;}
        else return unsupported;
        segment.sequence=std::move(seq);return 0U;
    };
    const auto path_key=[](std::u16string path){for(auto& c:path){if(c==u'/')c=u'\\';else if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);}return path;};
    const auto loader_class=[](const Guid& guid){return guid==CLSID_DirectMusicSegment || guid==GUID_DirectMusicAllTypes;};
    const auto path_object=[&](MusicObject& source,std::uint64_t stage,const Guid& iid,std::uint64_t out){check(out,unit);put(out,0,bits);const auto p=stage==0x2000?(source.kind=='A'?source.canonical:source.default_path):stage==0x3000?source.performance:0;if(!p)return 0x88781161U;auto target=object(p);if(!accepts(target->kind,iid))return no_interface;hold(p);put(out,p,bits);return 0U;};
    const auto load=[&](MusicObject& loader,const Guid& cls,const Guid& iid,std::u16string path,std::uint64_t out)->std::uint32_t{
        check(out,unit);put(out,0,bits);if(cls!=CLSID_DirectMusicSegment)return 0x80040154U;if(!accepts('S',iid))return no_interface;
        const bool relative=path.find(u':')==path.npos && !path.starts_with(u"\\") && !path.starts_with(u"/");
        auto requested=relative?runtime_.current_directory+u"\\"+path:path;
        if(loader.cache_enabled)for(const auto& c:loader.cache)if(c.pointer && (path_key(c.filename)==path_key(path) || path_key(c.filename)==path_key(requested) || (relative && !loader.directory.empty() && path_key(c.filename)==path_key(loader.directory+u"\\"+path)))){auto existing=object(c.pointer);hold(c.pointer);put(out,iid==IID_IDirectMusicObject?create(existing,'O'):c.pointer,bits);return 0;}
        auto file=filesystem_.open_reader(utf16_to_utf8(requested),runtime_.application_source,utf16_to_utf8(runtime_.application_directory));
        if((file.error==2 || file.error==3) && !loader.directory.empty() && relative){requested=loader.directory+u"\\"+path;file=filesystem_.open_reader(utf16_to_utf8(requested),runtime_.application_source,utf16_to_utf8(runtime_.application_directory));}
        if(file.error)return 0x80070000U|(file.error&0xffffU);
        if(file.metadata.size>16*1024*1024)return invalid;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.metadata.size));const auto read=file.file->read(bytes,0);if(read.error || read.bytes!=bytes.size())return 0x80004005U;
        auto o=std::make_shared<MusicObject>();o->kind='S';o->filename=requested;o->sequence=std::make_shared<midi::Sequence>(midi::parse(bytes));
        if(loader.cache_enabled)for(const auto& c:loader.cache)if((c.filename==path || c.filename==requested) && c.pointer){auto existing=object(c.pointer);hold(c.pointer);put(out,iid==IID_IDirectMusicObject?create(existing,'O'):c.pointer,bits);return 0;}
        const auto p=create(o);if(loader.cache_enabled){hold(p);const auto entry=std::find_if(loader.cache.begin(),loader.cache.end(),[&](const auto& c){return c.filename==path || c.filename==requested;});if(entry!=loader.cache.end()){entry->pointer=p;o->id=entry->id;o->title=entry->name;}else loader.cache.push_back({requested,{},Guid{},p});}put(out,iid==IID_IDirectMusicObject?create(o,'O'):p,bits);return 0;
    };
    std::vector<std::shared_ptr<MusicObject>> performances;for(const auto& [address,o]:bridge.objects)if(address==o->canonical && o->kind=='P')performances.push_back(o);
    for(auto& perf:performances)for(auto it=perf->runs.begin();it!=perf->runs.end();){const auto run=object(*it);bool finished=false;if(auto pb=run->voice_playback.lock()){std::lock_guard lock(pb->mutex);finished=run->voice->finished && run->observed_end;}if(!finished){++it;continue;}const auto address=*it;it=perf->runs.erase(it);if(perf->active_state==address)perf->active_state=0;release(address);}
    std::uint64_t result=0;
    try {
        if(service.symbol=="DllCanUnloadNow")result=bridge.objects.empty() && !bridge.locks?0:1;
        else if(activation){
            const bool factory=service.symbol!="CoCreateInstance",dll=service.symbol=="DllGetClassObject";const auto out=arg(dll?2:4);check(out,unit);put(out,0,bits);const auto cls=read_guid(memory,arg(0));const auto iid=read_guid(memory,arg(dll?1:3));
            const char kind=factory?'F':class_kind(cls);
            if(!is_class(memory,arg(0)))result=0x80040111U;
            else if(!dll && !factory && arg(1))result=0x80040110U;
            else if(!dll && !(arg(factory?1:2)&1))result=0x80040154U;
            else if(!accepts(kind,iid))result=no_interface;
            else if(!dll && (!gui_.com_initializations.contains(threads_.current) || gui_.com_initializations.at(threads_.current)<16))result=not_initialized;
            else {auto o=std::make_shared<MusicObject>();o->kind=kind;o->cls=cls;const auto p=create(o);put(out,iid==IID_IDirectMusicObject?create(o,'O'):p,bits);}
        }else{
            auto ptr=object(arg(0),service.symbol.front());auto& o=*ptr;
            // 即使 performance 尚未初始化，建立 segment state 的輸出也必須清空。
            if(o.kind=='P' && (name=="PlaySegment" || name=="PlaySegmentEx")){const auto out=arg(name=="PlaySegmentEx"?6:4);if(out){check(out,unit);put(out,0,bits);}}
            if(name=="QueryInterface"){check(arg(2),unit);put(arg(2),0,bits);const auto iid=read_guid(memory,arg(1));if(accepts(o.kind,iid)){const auto p=iid==IID_IDirectMusicObject?create(ptr,'O'):o.canonical;hold(arg(0));put(arg(2),p,bits);}else result=no_interface;}
            else if(name=="AddRef")result=hold(arg(0));
            else if(name=="Release"){result=o.references-1;release(arg(0));}
            else if(o.kind=='C'){
                if(name=="GetDefaultPort"){check(arg(1),16);memory.write(GuestAddress{arg(1)},CLSID_DirectMusicSynth);}
                else if(name=="EnumPort"){check(arg(2),308);if(arg(1))result=1;else port_caps(arg(2));}
                else if(name=="SetDirectSound"){if(arg(1))result=unsupported;else o.directsound_set=true;}
                else if(name=="CreateMusicBuffer"){
                    check(arg(2),unit);put(arg(2),0,bits);if(arg(3))result=0x80040110U;
                    else if(!arg(1) || memory.load(GuestAddress{arg(1)},32)!=28 || memory.load(GuestAddress{arg(1)+4},32))result=invalid;
                    else {const auto format=read_guid(memory,arg(1)+8);const auto capacity=memory.load(GuestAddress{arg(1)+24},32);if(!capacity || capacity>16*1024*1024 || (format!=Guid{} && format!=MIDI_BufferFormat))result=invalid;
                        else {auto buffer=std::make_shared<MusicObject>();buffer->kind='B';buffer->buffer_capacity=static_cast<std::uint32_t>((capacity+3)&~UINT64_C(3));buffer->raw_buffer=allocate_heap(memory,buffer->buffer_capacity);if(!buffer->raw_buffer)throw std::bad_alloc{};try {put(arg(2),create(buffer),bits);}catch(...){(void)free_heap(memory,buffer->raw_buffer,state);throw;}}}
                }else if(name=="CreatePort"){
                    check(arg(3),unit);put(arg(3),0,bits);if(!arg(1) || !arg(2))throw std::invalid_argument("null port descriptor");const auto guid=read_guid(memory,arg(1));const auto p=arg(2),size=memory.load(GuestAddress{p},32),valid=memory.load(GuestAddress{p+4},32);
                    if(arg(4))result=0x80040110U;else if(!o.directsound_set)result=0x88781139U;else if(guid!=Guid{} && guid!=CLSID_DirectMusicSynth)result=0x80040154U;
                    else if((size!=32 && size!=36) || (valid&~UINT64_C(0xef)))result=invalid;
                    else {const auto voices=valid&1?memory.load(GuestAddress{p+8},32):64,groups=valid&2?memory.load(GuestAddress{p+12},32):1,channels=valid&4?memory.load(GuestAddress{p+16},32):2,rate=valid&8?memory.load(GuestAddress{p+20},32):48000;
                        if(!voices || voices>64 || groups!=1 || (channels!=1 && channels!=2))result=invalid;
                        else if(rate!=48000)result=0x8878113cU;
                        else if((valid&0x20 && memory.load(GuestAddress{p+24},32)) || (valid&0x80 && (size<36 || memory.load(GuestAddress{p+32},32))))result=unsupported;
                        else {auto port=std::make_shared<MusicObject>();port->kind='R';port->core=o.canonical;port->epoch=o.epoch;port->port_voices=static_cast<std::uint32_t>(voices);port->channels=static_cast<std::uint32_t>(channels);port->playback=std::make_shared<midi::Playback>();port->playback->voice_limit=port->port_voices;port->playback->mono=channels==1;port->playback->active=false;put(arg(3),create(port),bits);}}
                }else if(name=="Activate"){for(auto& [p,port]:bridge.objects)if(p==port->canonical && port->kind=='R' && port->core==o.canonical)activate_port(*port,arg(1)!=0);o.port_active=arg(1)!=0;}
                else if(name=="EnumMasterClock"){
                    check(arg(2),4);const auto size=memory.load(GuestAddress{arg(2)},32);if(size!=280 && size!=284)result=invalid;else {check(arg(2),static_cast<unsigned>(size));if(arg(1))result=1;else {std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.write(GuestAddress{arg(2)},bytes);put(arg(2),size,32);memory.write(GuestAddress{arg(2)+8},CLSID_SystemClock);memory.write(GuestAddress{arg(2)+24},utf16_bytes(u"macOS 單調參考時鐘"));}}
                }else if(name=="GetMasterClock"){if(arg(1))check(arg(1),16);if(arg(2))check(arg(2),unit);if(arg(1))memory.write(GuestAddress{arg(1)},o.id==Guid{}?CLSID_SystemClock:Guid{});if(arg(2)){const auto clock=make_clock(o,0);hold(clock);put(arg(2),clock,bits);}}
                else if(name=="SetMasterClock"){const auto guid=read_guid(memory,arg(1));if(guid!=Guid{} && guid!=CLSID_SystemClock)result=invalid;else if(o.id!=Guid{}){if(o.clock_object)release(o.clock_object);o.clock_object=0;o.id={};}}
                else if(name=="SetExternalMasterClock"){if(arg(1)){auto clock=object(arg(1),'K');bool active=false;for(const auto& [p,port]:bridge.objects)if(p==port->canonical && port->kind=='R' && port->core==o.canonical && port->port_active)active=true;if(active)result=0x88781132U;else {hold(arg(1));if(o.clock_object)release(o.clock_object);o.clock_object=arg(1);o.epoch=clock->epoch;o.adjustment=clock->adjustment;o.id=IID_IReferenceClock;for(auto& [p,port]:bridge.objects)if(p==port->canonical && port->kind=='R' && port->core==o.canonical){port->epoch=o.epoch;port->adjustment=o.adjustment;if(port->clock_object){auto latency=object(port->clock_object,'K');latency->epoch=o.epoch;latency->adjustment=o.adjustment;}}}}else {if(o.clock_object)release(o.clock_object);o.clock_object=0;o.id={};}}
                else result=unsupported;
            }else if(o.kind=='B'){
                if(name=="Flush"){o.buffer_used=0;o.buffer_read=0;o.buffer_start_set=false;o.buffer_start=0;}
                else if(name=="GetRawBufferPtr")put(arg(1),o.raw_buffer,bits);
                else if(name=="GetMaxBytes" || name=="GetUsedBytes")put(arg(1),name=="GetMaxBytes"?o.buffer_capacity:o.buffer_used,32);
                else if(name=="GetBufferFormat"){check(arg(1),16);memory.write(GuestAddress{arg(1)},o.buffer_format);}
                else if(name=="SetUsedBytes"){if(arg(1)>o.buffer_capacity)result=0x88781136U;else {o.buffer_used=static_cast<std::uint32_t>(arg(1));o.buffer_read=std::min(o.buffer_read,o.buffer_used);}}
                else if(name=="SetStartTime"){o.buffer_start=static_cast<std::int64_t>(arg(1));o.buffer_start_set=true;}
                else if(name=="GetStartTime"){check(arg(1),8);if(!o.buffer_used)result=0x88781135U;else put(arg(1),static_cast<std::uint64_t>(o.buffer_start),64);}
                else if(name=="ResetReadPtr")o.buffer_read=0;
                else if(name=="TotalTime"){check(arg(1),8);std::int64_t first=INT64_MAX,last=INT64_MIN;for(std::uint32_t offset=0;offset<o.buffer_used;){const auto event=buffer_event(o,offset);first=std::min(first,event.reference);last=std::max(last,event.reference);offset=event.next;}put(arg(1),o.buffer_used?static_cast<std::uint64_t>(last)-static_cast<std::uint64_t>(first):0,64);}
                else if(name=="GetNextEvent"){
                    for(unsigned i=1;i<=4;++i)if(arg(i))check(arg(i),i==1?8:i==4?unit:4);
                    if(o.buffer_read>=o.buffer_used)result=1;else {const auto event=buffer_event(o,o.buffer_read);if(arg(1))put(arg(1),static_cast<std::uint64_t>(event.reference),64);if(arg(2))put(arg(2),event.group,32);if(arg(3))put(arg(3),event.length,32);if(arg(4))put(arg(4),event.data,bits);o.buffer_read=event.next;}
                }else if(name=="PackStructured" || name=="PackUnstructured"){
                    const bool structured=name=="PackStructured";const auto reference=static_cast<std::int64_t>(arg(1));const auto length=structured?3:arg(3);const auto size=(20ULL+length+7)&~UINT64_C(7);
                    if(!length || length>16*1024*1024 || (structured && (!(arg(3)&0x80) || (arg(3)&0xff000000U))))result=0x8878113fU;
                    else if(size>o.buffer_capacity-o.buffer_used)result=0x88781136U;
                    else {std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));if(structured){for(unsigned i=0;i<3;++i)bytes[20+i]=static_cast<std::uint8_t>(arg(3)>>(i*8));}else memory.read(GuestAddress{arg(4)},std::span(bytes).subspan(20,static_cast<std::size_t>(length)));
                        const auto start=o.buffer_used || o.buffer_start_set?o.buffer_start:reference;if((start<0 && reference>INT64_MAX+start) || (start>0 && reference<INT64_MIN+start))result=invalid;
                        else {const auto p=o.raw_buffer+o.buffer_used;memory.write(GuestAddress{p},bytes);put(p,length,32);put(p+4,arg(2),32);put(p+8,static_cast<std::uint64_t>(reference-start),64);put(p+16,structured?1:0,32);o.buffer_start=start;o.buffer_used+=static_cast<std::uint32_t>(size);}}
                }else result=unsupported;
            }else if(o.kind=='K'){
                if(name=="GetTime"){const auto reference=now(o)+o.latency;put(arg(1),static_cast<std::uint64_t>(reference),64);result=reference==o.last_clock?1:0;o.last_clock=reference;}
                else if(name=="AdviseTime" || name=="AdvisePeriodic"){
                    check(arg(4),unit);put(arg(4),0,bits);const bool periodic=name=="AdvisePeriodic";const auto first=static_cast<std::int64_t>(arg(1)),second=static_cast<std::int64_t>(arg(2));const auto due=periodic?first:add_time(first,second);const auto event=runtime_.wait_objects.find(arg(3));
                    if(due<0 || (periodic && second<=0) || event==runtime_.wait_objects.end() || event->second.kind!=(periodic?WaitObject::Kind::Semaphore:WaitObject::Kind::Event) || o.advice.size()>=4096 || o.next_advice>(x86_?UINT32_MAX:UINT64_MAX-1))result=invalid;
                    else {const auto cookie=o.next_advice++;o.advice.emplace(cookie,MusicObject::Advice{due,periodic?second:0,arg(3)});put(arg(4),cookie,bits);poll_directmusic();}
                }else if(name=="Unadvise")result=o.advice.erase(arg(1))?0:1;
                else result=unsupported;
            }else if(o.kind=='R'){
                if(name=="GetCaps")port_caps(arg(1));
                else if(name=="Activate")activate_port(o,arg(1)!=0);
                else if(name=="SetNumChannelGroups"){if(arg(1)!=1)result=invalid;}
                else if(name=="GetNumChannelGroups")put(arg(1),1,32);
                else if(name=="SetDirectSound"){if(o.port_active)result=0x88781132U;else if(arg(1) || arg(2))result=unsupported;}
                else if(name=="GetLatencyClock"){check(arg(1),unit);const auto clock=make_clock(o,200000);hold(clock);put(arg(1),clock,bits);}
                else if(name=="GetFormat"){
                    check(arg(2),4);const auto capacity=memory.load(GuestAddress{arg(2)},32);if(arg(3))check(arg(3),4);if(arg(1) && capacity>=18)check(arg(1),18);put(arg(2),18,32);if(arg(3))put(arg(3),o.channels*4*960,32);
                    if(arg(1)){if(capacity<18)result=0x8878113cU;else {std::array<std::uint8_t,18> bytes{};memory.write(GuestAddress{arg(1)},bytes);put(arg(1),3,16);put(arg(1)+2,o.channels,16);put(arg(1)+4,48000,32);put(arg(1)+8,o.channels*192000,32);put(arg(1)+12,o.channels*4,16);put(arg(1)+14,32,16);}}
                }else if(name=="SetChannelPriority" || name=="GetChannelPriority"){
                    if(arg(1)!=1 || arg(2)>=16)result=invalid;else {std::lock_guard lock(o.playback->mutex);if(name=="SetChannelPriority")o.playback->channel_priorities[static_cast<std::size_t>(arg(2))]=static_cast<std::uint32_t>(arg(3));else put(arg(3),o.playback->channel_priorities[static_cast<std::size_t>(arg(2))],32);}
                }else if(name=="Compact"){std::lock_guard lock(o.playback->mutex);std::erase_if(o.playback->voices,[](const auto& voice){return voice->finished;});o.playback->voices.shrink_to_fit();o.playback->messages.shrink_to_fit();}
                else if(name=="GetRunningStats"){
                    check(arg(1),4);const auto size=memory.load(GuestAddress{arg(1)},32);if(size!=32 && size!=36)result=invalid;else {check(arg(1),static_cast<unsigned>(size));std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.write(GuestAddress{arg(1)},bytes);std::lock_guard lock(o.playback->mutex);put(arg(1),size,32);put(arg(1)+4,0x19,32);put(arg(1)+8,o.playback->active_notes,32);put(arg(1)+20,o.playback->lost_notes,32);const auto peak=o.playback->rendered_peak>0?static_cast<std::int32_t>(std::clamp(2000.0F*std::log10(o.playback->rendered_peak),-20000.0F,2000.0F)):-20000;put(arg(1)+28,static_cast<std::uint32_t>(peak),32);}
                }else if(name=="PlayBuffer"){
                    auto buffer=object(arg(1),'B');if(!o.output || !o.port_active)result=0x8878113dU;else if(failed(o))result=0x80004005U;
                    else {std::vector<midi::Playback::Message> messages;std::lock_guard lock(o.playback->mutex);const auto reference=now(o);for(std::uint32_t offset=0;offset<buffer->buffer_used;){const auto event=buffer_event(*buffer,offset);if(event.group!=1)throw std::invalid_argument("MIDI channel group 無效");midi::Playback::Message message;const auto delta=event.reference>reference?static_cast<std::uint64_t>(event.reference)-static_cast<std::uint64_t>(reference):0;if(delta>6000000000ULL)throw std::invalid_argument("MIDI 時間超過十分鐘");message.frame=o.playback->clock_frames+delta*48000/10000000;
                        std::vector<std::uint8_t> bytes(event.length);memory.read(GuestAddress{event.data},bytes);if(event.flags){const auto status=bytes[0];const unsigned expected=status>=0xc0 && status<0xe0?2:3;if(status<0x80 || status>=0xf0 || bytes.size()<expected || bytes[1]>127 || (expected==3 && bytes[2]>127))throw std::invalid_argument("MIDI channel event 無效");message.events.push_back({0,status,bytes[1],expected==3?bytes[2]:std::uint8_t{0}});}else {if(bytes.size()<2 || bytes.front()!=0xf0 || bytes.back()!=0xf7)throw std::invalid_argument("MIDI SysEx event 無效");message.sysex=std::move(bytes);}messages.push_back(std::move(message));if(messages.size()+o.playback->messages.size()>4096)throw std::bad_alloc{};offset=event.next;}
                        o.playback->messages.reserve(o.playback->messages.size()+messages.size());for(auto& message:messages)o.playback->messages.push_back(std::move(message));}
                }else if(name=="Read" || name=="SetReadNotificationHandle")result=0x88781137U;
                else if(name=="DownloadInstrument"){check(arg(2),unit);put(arg(2),0,bits);result=unsupported;}
                else if(name=="DeviceIoControl"){if(arg(6))put(arg(6),0,32);result=0x80070032U;}
                else result=unsupported;
            }else if(o.kind=='F'){
                if(name=="LockServer"){if(arg(1)){if(bridge.locks==UINT32_MAX)result=invalid;else ++bridge.locks;}else if(bridge.locks)--bridge.locks;else result=invalid;}
                else if(name=="CreateInstance"){check(arg(3),unit);put(arg(3),0,bits);const char kind=class_kind(o.cls);const auto iid=read_guid(memory,arg(2));if(arg(1))result=0x80040110U;else if(!accepts(kind,iid))result=no_interface;else {auto child=std::make_shared<MusicObject>();child->kind=kind;const auto p=create(child);put(arg(3),iid==IID_IDirectMusicObject?create(child,'O'):p,bits);}}
                else result=invalid;
            }else if(service.symbol.front()=='O'){
                if(name=="GetDescriptor")describe(o,arg(1));
                else if(name=="SetDescriptor"){const auto flags=descriptor(arg(1));if(flags&1)o.id=read_guid(memory,arg(1)+8);if(flags&4)o.title=read_utf16(memory,GuestAddress{arg(1)+56},64);if(flags&8)o.category=read_utf16(memory,GuestAddress{arg(1)+184},64);if(flags&0x10)o.filename=read_utf16(memory,GuestAddress{arg(1)+312},260);}
                else result=unsupported;
            }else if(o.kind=='L'){
                if(name=="LoadObjectFromFile")result=load(o,read_guid(memory,arg(1)),read_guid(memory,arg(2)),read_utf16(memory,GuestAddress{arg(3)},32768),arg(4));
                else if(name=="SetSearchDirectory"){
                    if(!loader_class(read_guid(memory,arg(1))))result=unsupported;
                    else {auto path=read_utf16(memory,GuestAddress{arg(2)},32768);if(path.find(u':')==path.npos && !path.starts_with(u"\\") && !path.starts_with(u"/"))path=runtime_.current_directory+u"\\"+path;const auto info=filesystem_.information(utf16_to_utf8(path));if(info.error)result=0x80070000U|(info.error&0xffffU);else if(!(info.metadata.attributes&0x10))result=invalid;else {result=path_key(o.directory)==path_key(path)?1:0;o.directory=std::move(path);if(arg(3))std::erase_if(o.cache,[](const auto& c){return !c.pointer;});}}
                }else if(name=="GetObject"){
                    check(arg(3),unit);put(arg(3),0,bits);const auto desc=arg(1);const auto size=memory.load(GuestAddress{desc},32),flags=memory.load(GuestAddress{desc+4},32);
                    if(size!=(x86_?848U:856U) || !(flags&2) || (flags&~std::uint64_t{0x73f}))result=invalid;
                    else if(read_guid(memory,desc+24)!=CLSID_DirectMusicSegment)result=0x80040154U;
                    else if(!accepts('S',read_guid(memory,arg(2))))result=no_interface;
                    else if(flags&0x400){
                        const auto length=memory.load(GuestAddress{desc+832},64),data=memory.load(GuestAddress{desc+840},bits);if(!length || length>16*1024*1024 || !data)result=invalid;
                        else {std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));memory.read(GuestAddress{data},bytes);auto segment=std::make_shared<MusicObject>();segment->kind='S';segment->sequence=std::make_shared<midi::Sequence>(midi::parse(bytes));if(flags&1)segment->id=read_guid(memory,desc+8);if(flags&4)segment->title=read_utf16(memory,GuestAddress{desc+56},64);const auto p=create(segment);put(arg(3),read_guid(memory,arg(2))==IID_IDirectMusicObject?create(segment,'O'):p,bits);}
                    }else if(flags&0x10)result=load(o,read_guid(memory,desc+24),read_guid(memory,arg(2)),read_utf16(memory,GuestAddress{desc+312},260),arg(3));
                    else {
                        const auto id=flags&1?read_guid(memory,desc+8):Guid{};const auto title=flags&4?read_utf16(memory,GuestAddress{desc+56},64):std::u16string{};
                        const auto entry=std::find_if(o.cache.begin(),o.cache.end(),[&](const auto& c){return ((flags&1) && c.id==id) || ((flags&4) && c.name==title);});
                        if(entry==o.cache.end())result=0x88781133U;
                        else if(entry->pointer){auto segment=object(entry->pointer);hold(entry->pointer);put(arg(3),read_guid(memory,arg(2))==IID_IDirectMusicObject?create(segment,'O'):entry->pointer,bits);}
                        else result=load(o,CLSID_DirectMusicSegment,read_guid(memory,arg(2)),entry->filename,arg(3));
                    }
                }else if(name=="EnableCache"){if(!loader_class(read_guid(memory,arg(1))))result=1;else {const auto enabled=arg(2)!=0;result=o.cache_enabled==enabled?1:0;o.cache_enabled=enabled;if(!enabled)for(auto& c:o.cache)if(c.pointer){release(c.pointer);c.pointer=0;}}}
                else if(name=="SetObject"){
                    const auto flags=descriptor(arg(1));if(!(flags&2) || read_guid(memory,arg(1)+24)!=CLSID_DirectMusicSegment || !(flags&0x10))result=invalid;
                    else {MusicObject::Cached c;c.filename=read_utf16(memory,GuestAddress{arg(1)+312},260);if(flags&1)c.id=read_guid(memory,arg(1)+8);if(flags&4)c.name=read_utf16(memory,GuestAddress{arg(1)+56},64);o.cache.push_back(std::move(c));}
                }else if(name=="CacheObject"){
                    const auto segment=object(arg(1),'S');const auto p=segment->canonical;const auto entry=std::find_if(o.cache.begin(),o.cache.end(),[&](const auto& c){return c.pointer==p;});
                    if(entry!=o.cache.end())result=1;else {hold(p);o.cache.push_back({segment->filename,segment->title,segment->id,p});}
                }else if(name=="ReleaseObject" || name=="ReleaseObjectByUnknown"){
                    const auto segment=object(arg(1),'S');bool removed=false;for(auto& entry:o.cache)if(entry.pointer==segment->canonical){release(entry.pointer);entry.pointer=0;removed=true;}if(!removed)result=1;
                }else if(name=="ClearCache" || name=="CollectGarbage"){
                    if(name=="ClearCache" && !loader_class(read_guid(memory,arg(1))))result=unsupported;
                    else for(auto& entry:o.cache)if(entry.pointer && (name=="ClearCache" || object(entry.pointer)->references==1)){release(entry.pointer);entry.pointer=0;}
                }else if(name=="EnumObject"){
                    if(!loader_class(read_guid(memory,arg(1))))result=unsupported;
                    else if(arg(2)>=o.cache.size())result=1;
                    else {const auto& c=o.cache[static_cast<std::size_t>(arg(2))];if(c.pointer)describe(*object(c.pointer),arg(3));else {MusicObject temp;temp.filename=c.filename;temp.title=c.name;temp.id=c.id;describe(temp,arg(3));put(arg(3)+4,memory.load(GuestAddress{arg(3)+4},32)&~UINT64_C(0x200),32);}}
                }else if(name=="ScanDirectory"){
                    if(read_guid(memory,arg(1))!=CLSID_DirectMusicSegment)result=unsupported;
                    else {const auto extension=read_utf16(memory,GuestAddress{arg(2)},260);const auto list=filesystem_.list(utf16_to_utf8(o.directory.empty()?runtime_.current_directory:o.directory));if(list.error)result=0x80070000U|list.error;
                        else for(const auto& file:list.entries){const auto filename=utf8_to_utf16(file.name);if(file.metadata.attributes&0x10)continue;if(extension!=u"*" && !filename.ends_with(u"."+extension))continue;const auto full=(o.directory.empty()?runtime_.current_directory:o.directory)+u"\\"+filename;const auto entry=std::find_if(o.cache.begin(),o.cache.end(),[&](const auto& c){return c.filename==full;});if(entry==o.cache.end())o.cache.push_back({full,{},Guid{},0});}
                    }
                }
                else result=unsupported;
            }else if(o.kind=='S'){
                if(name=="GetLength")put(arg(1),o.sequence?o.sequence->music_length:0,32);
                else if(name=="GetRepeats")put(arg(1),o.repeats,32);
                else if(name=="SetRepeats")o.repeats=static_cast<std::uint32_t>(arg(1));
                else if(name=="GetStartPoint")put(arg(1),o.start_point,32);
                else if(name=="SetStartPoint"){if(!o.sequence || arg(1)>=o.sequence->music_length)result=invalid;else o.start_point=static_cast<std::uint32_t>(arg(1));}
                else if(name=="GetDefaultResolution")put(arg(1),o.resolution,32);
                else if(name=="SetDefaultResolution")o.resolution=static_cast<std::uint32_t>(arg(1));
                else if(name=="GetLoopPoints"){put(arg(1),o.loop_start,32);put(arg(2),o.loop_end,32);}
                else if(name=="SetLoopPoints"){if(!o.sequence || arg(1)>=arg(2) || arg(2)>o.sequence->music_length)result=invalid;else {o.loop_start=static_cast<std::uint32_t>(arg(1));o.loop_end=static_cast<std::uint32_t>(arg(2));}}
                else if(name=="SetLength"){
                    if(!o.sequence || !arg(1) || arg(1)>INT32_MAX || o.start_point>=arg(1) || o.loop_end>arg(1))result=invalid;
                    else {auto seq=std::make_shared<midi::Sequence>(*o.sequence);seq->music_length=static_cast<std::uint32_t>(arg(1));seq->frames=seq->frame_at(seq->music_length);if(seq->frames>48000*600ULL)result=invalid;else {std::erase_if(seq->events,[&](const auto& e){return e.frame>seq->frames;});std::erase_if(seq->tempos,[&](const auto& t){return t.music>seq->music_length;});std::erase_if(seq->signatures,[&](const auto& t){return t.music>seq->music_length;});o.sequence=std::move(seq);}}
                }else if(name=="Clone"){
                    check(arg(3),unit);put(arg(3),0,bits);const auto start=static_cast<std::uint32_t>(arg(1));const auto end=static_cast<std::uint32_t>(arg(2))==UINT32_MAX && o.sequence?o.sequence->music_length:static_cast<std::uint32_t>(arg(2));
                    if(!o.sequence || start>=end || end>o.sequence->music_length)result=invalid;
                    else {auto seq=std::make_shared<midi::Sequence>();const auto first=o.sequence->frame_at(start),last=o.sequence->frame_at(end);seq->music_length=end-start;seq->frames=last-first;seq->tempos.clear();const midi::Tempo* tempo=&o.sequence->tempos.front();for(const auto& t:o.sequence->tempos){if(t.music<=start)tempo=&t;else if(t.music<end)seq->tempos.push_back({t.music-start,t.frame-first,t.bpm});}seq->tempos.insert(seq->tempos.begin(),{0,0,tempo->bpm});seq->signatures.clear();const midi::TimeSignature* signature=&o.sequence->signatures.front();for(const auto& t:o.sequence->signatures){if(t.music<=start)signature=&t;else if(t.music<end)seq->signatures.push_back({t.music-start,t.beats,t.beat,t.grids});}seq->signatures.insert(seq->signatures.begin(),{0,signature->beats,signature->beat,signature->grids});seq->events=midi::chase(*o.sequence,first);for(const auto& e:o.sequence->events)if(e.frame>=first && e.frame<last)seq->events.push_back({e.frame-first,e.status,e.first,e.second,e.sysex});auto child=std::make_shared<MusicObject>();child->kind='S';child->sequence=seq;child->repeats=o.repeats;child->resolution=o.resolution;put(arg(3),create(child),bits);}
                }else if(name=="GetParam"){
                    if(!o.sequence || !arg(2) || (arg(3)!=0 && arg(3)!=UINT32_MAX))result=invalid;
                    else result=parameter(*o.sequence,read_guid(memory,arg(1)),static_cast<std::uint32_t>(arg(4)),arg(5),arg(6));
                }

                else if(name=="Download" || name=="Unload"){
                    const auto target=object(arg(1));const auto performance=target->kind=='A'?target->performance:target->canonical;if((target->kind!='P' && target->kind!='A') || !target->playback)result=not_initialized;else if(!o.sequence)result=invalid;else if(name=="Download")o.downloads.insert(performance);else o.downloads.erase(performance);
                }else if(name=="SetParam"){
                    const auto guid=read_guid(memory,arg(1));
                    if(!arg(2) || (arg(3)!=0 && arg(3)!=UINT32_MAX))result=unsupported;
                    else if(guid==GUID_StandardMIDIFile){if(!o.sequence)result=invalid;}
                    else if(guid==GUID_TempoParam || guid==GUID_TimeSignature)result=set_parameter(o,guid,static_cast<std::uint32_t>(arg(4)),arg(5));
                    else if((guid==GUID_Download || guid==GUID_Unload) && !arg(4)){const auto target=object(arg(5),'P');if(!target->playback)result=not_initialized;else if(guid==GUID_Download)o.downloads.insert(arg(5));else o.downloads.erase(arg(5));}
                    else result=unsupported;
                }else result=unsupported;
            }else if(o.kind=='T'){
                if(name=="GetSegment"){check(arg(1),unit);hold(o.segment);put(arg(1),o.segment,bits);}
                else if(name=="GetRepeats")put(arg(1),o.repeats,32);
                else if(name=="GetStartTime")put(arg(1),static_cast<std::uint32_t>(o.music_start),32);
                else if(name=="GetSeek" || name=="GetStartPoint")put(arg(1),o.start_point,32);
                else if(name=="GetObjectInPath"){if(arg(3) || arg(5))result=invalid;else result=path_object(o,arg(2),read_guid(memory,arg(6)),arg(7));}
                else result=unsupported;
            }else if(o.kind=='A'){
                if(name=="Activate"){std::lock_guard lock(o.playback->mutex);o.playback->active=arg(1)!=0;}
                else if(name=="SetVolume"){
                    const auto volume=static_cast<std::int32_t>(arg(1));if(volume>0 || volume< -10000 || arg(2)>600000)result=invalid;
                    else {o.volume=volume;const auto master=o.performance && bridge.objects.contains(o.performance)?object(o.performance)->volume:0;const auto target=volume+master<=-10000?0.0F:std::pow(10.0F,static_cast<float>(volume+master)/2000.0F);std::lock_guard lock(o.playback->mutex);o.playback->gain_target=target;o.playback->gain_remaining=arg(2)*48;if(!arg(2))o.playback->gain=target;}
                }else if(name=="ConvertPChannel"){if(arg(1)>=o.channels)result=invalid;else put(arg(2),arg(1),32);}
                else if(name=="GetObjectInPath"){if(arg(3) || arg(5))result=invalid;else result=path_object(o,arg(2),read_guid(memory,arg(6)),arg(7));}
                else result=unsupported;
            }else {
                if(name=="Init" || name=="InitAudio"){
                    if(o.playback)result=invalid;
                    else if(arg(2))result=unsupported;
                    else if(name=="InitAudio" && ((arg(4)!=7 && arg(4)!=8) || !arg(5) || arg(5)>16 || (arg(6)&~std::uint64_t{0x30}) || arg(7)))result=unsupported;
                    else {if(arg(1))check(arg(1),unit);const auto supplied=arg(1)?memory.load(GuestAddress{arg(1)},bits):0;std::shared_ptr<MusicObject> core;if(supplied){core=object(supplied,'C');hold(supplied);}else {core=std::make_shared<MusicObject>();core->kind='C';create(core);}core->directsound_set=true;
                        try {o.default_path=make_audio_path(o,name=="InitAudio"?static_cast<std::uint32_t>(arg(4)):8,name=="InitAudio"?static_cast<std::uint32_t>(arg(5)):16,true);}catch(...){release(core->canonical);throw;}
                        o.core=core->canonical;o.playback=object(o.default_path)->playback;o.epoch=core->epoch;o.adjustment=core->adjustment;o.clock={{0,0,120}};if(arg(1) && !supplied){hold(core->canonical);put(arg(1),core->canonical,bits);}}
                }else if(name=="CloseDown"){
                    stop(o);for(auto& [address,a]:bridge.objects)if(address==a->canonical && a->kind=='A' && a->performance==o.canonical){a->output.reset();{std::lock_guard lock(a->playback->mutex);a->playback->messages.clear();a->playback->active=false;}a->performance=0;}o.playback.reset();if(o.default_path){release(o.default_path);o.default_path=0;}
                    for(auto& [address,other]:bridge.objects){(void)address;other->downloads.erase(o.canonical);}
                    if(o.core){release(o.core);o.core=0;}
                }else if(name=="SetPrepareTime")o.prepare=static_cast<std::uint32_t>(arg(1));
                else if(name=="GetPrepareTime")put(arg(1),o.prepare,32);
                else if(name=="SetBumperLength")o.bumper=static_cast<std::uint32_t>(arg(1));
                else if(name=="GetBumperLength")put(arg(1),o.bumper,32);
                else if(name=="AllocPMsg"){check(arg(2),unit);put(arg(2),0,bits);put(arg(2),allocate_message(o,static_cast<std::uint32_t>(arg(1))),bits);}
                else if(name=="FreePMsg")free_message(o,arg(1));
                else if(name=="ClonePMsg"){
                    check(arg(2),unit);put(arg(2),0,bits);const auto found=o.messages.find(arg(1));if(found==o.messages.end())result=invalid;
                    else {const auto user=found->second.user?found->second.user:memory.load(GuestAddress{arg(1)+(x86_?52:64)},bits);if(user && object(user)->references==UINT32_MAX)throw std::invalid_argument("PMsg 使用者引用溢位");std::vector<std::uint8_t> bytes(found->second.size);memory.read(GuestAddress{arg(1)},bytes);const auto p=allocate_message(o,found->second.size);memory.write(GuestAddress{p},bytes);if(user){hold(user);o.messages.at(p).user=user;}put(arg(2),p,bits);}
                }else if(name=="GetGlobalParam" || name=="SetGlobalParam"){
                    const auto guid=read_guid(memory,arg(1));const bool set=name=="SetGlobalParam";
                    if(arg(3)!=4)result=invalid;
                    else if(guid==GUID_PerfAutoDownload){if(set)o.auto_download=memory.load(GuestAddress{arg(2)},32)!=0;else put(arg(2),o.auto_download,32);}
                    else if(guid==GUID_PerfMasterVolume){if(set){const auto volume=static_cast<std::int32_t>(memory.load(GuestAddress{arg(2)},32));if(volume< -10000 || volume>0)result=invalid;else {o.volume=volume;for(auto& [p,a]:bridge.objects)if(p==a->canonical && a->kind=='A' && a->performance==o.canonical){std::lock_guard lock(a->playback->mutex);a->playback->gain=a->playback->gain_target=o.volume+a->volume<=-10000?0.0F:std::pow(10.0F,static_cast<float>(o.volume+a->volume)/2000.0F);}}}else put(arg(2),static_cast<std::uint32_t>(o.volume),32);}
                    else if(guid==GUID_PerfMasterTempo){if(set){const auto value=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32)));if(!std::isfinite(value) || value<0.01F || value>100.0F)result=invalid;else {if(o.playback){const auto reference=now(o);const auto music=reference_music(o,reference);const MusicObject::ClockPoint* current=&o.clock.front();for(const auto& t:o.clock){if(t.reference>reference)break;current=&t;}const auto ratio=static_cast<double>(value)/o.master_tempo;const auto bpm=current->bpm;std::vector<MusicObject::ClockPoint> future;for(const auto& t:o.clock)if(t.reference>reference)future.push_back({reference+static_cast<std::int64_t>(static_cast<double>(t.reference-reference)/ratio),t.music,t.bpm*ratio});std::erase_if(o.clock,[&](const auto& t){return t.reference>=reference;});o.clock.push_back({reference,music,bpm*ratio});o.clock.insert(o.clock.end(),future.begin(),future.end());o.tempo=bpm/o.master_tempo;}o.master_tempo=value;for(auto& [p,a]:bridge.objects)if(p==a->canonical && a->kind=='A' && a->performance==o.canonical){std::lock_guard lock(a->playback->mutex);a->playback->tempo_scale=value;}}}else put(arg(2),std::bit_cast<std::uint32_t>(static_cast<float>(o.master_tempo)),32);}
                    else result=unsupported;
                }else if(name=="SetNotificationHandle"){
                    if(arg(1)){const auto found=runtime_.wait_objects.find(arg(1));if(found==runtime_.wait_objects.end() || found->second.kind!=WaitObject::Kind::Event)throw std::invalid_argument("通知事件無效");}o.notification_event=arg(1);
                }else if(name=="AddNotificationType" || name=="RemoveNotificationType"){
                    const auto type=read_guid(memory,arg(1));if(type!=GUID_NOTIFICATION_SEGMENT && type!=GUID_NOTIFICATION_PERFORMANCE)result=unsupported;else if(name=="AddNotificationType")o.notification_types.insert(type);else o.notification_types.erase(type);
                }else if(name=="GetNotificationPMsg"){
                    check(arg(1),unit);put(arg(1),0,bits);if(o.notifications.empty())result=1;
                    else {const auto n=o.notifications.front();const auto header=x86_?56U:72U;const auto p=allocate_message(o,x86_?88U:104U);put(p+8,static_cast<std::uint64_t>(n.reference),64);put(p+16,static_cast<std::uint32_t>(n.music),32);put(p+20,3,32);put(p+(x86_?40:48),3,32);put(p+(x86_?52:64),n.state,bits);o.messages.at(p).user=n.state;memory.write(GuestAddress{p+header},n.type);put(p+header+16,n.option,32);o.notifications.pop_front();put(arg(1),p,bits);}
                }else if(!o.playback)result=not_initialized;
                else if(name=="GetTime"){if(!arg(1) && !arg(2))result=invalid;else {const auto reference=now(o);if(arg(1))put(arg(1),static_cast<std::uint64_t>(reference),64);if(arg(2))put(arg(2),static_cast<std::uint32_t>(reference_to_music(o,reference)),32);}}
                else if(name=="MusicToReferenceTime")put(arg(2),static_cast<std::uint64_t>(music_to_reference(o,static_cast<std::int32_t>(arg(1)))),64);
                else if(name=="ReferenceToMusicTime")put(arg(2),static_cast<std::uint32_t>(reference_to_music(o,static_cast<std::int64_t>(arg(1)))),32);
                else if(name=="TimeToRhythm" || name=="RhythmToTime"){
                    const bool to_rhythm=name=="TimeToRhythm";const auto sig=arg(to_rhythm?2:5);const auto origin=static_cast<std::int32_t>(memory.load(GuestAddress{sig},32));const auto beats=memory.load(GuestAddress{sig+4},8),beat=memory.load(GuestAddress{sig+5},8),grids=memory.load(GuestAddress{sig+6},16);
                    if(!beats || !beat || (beat&(beat-1)) || !grids || 3072%beat || (3072/beat)%grids)result=invalid;
                    else {const auto beat_ticks=static_cast<std::int64_t>(3072/beat),grid_ticks=beat_ticks/static_cast<std::int64_t>(grids),measure_ticks=static_cast<std::int64_t>(beats)*beat_ticks;
                        if(to_rhythm){const auto delta=static_cast<std::int64_t>(static_cast<std::int32_t>(arg(1)))-origin;if(delta<0 || delta/measure_ticks>UINT16_MAX)result=invalid;else {put(arg(3),static_cast<std::uint64_t>(delta/measure_ticks),16);put(arg(4),static_cast<std::uint64_t>((delta%measure_ticks)/beat_ticks),8);put(arg(5),static_cast<std::uint64_t>((delta%beat_ticks)/grid_ticks),8);put(arg(6),static_cast<std::uint64_t>(delta%grid_ticks),16);}}
                        else {const auto value=static_cast<std::int64_t>(origin)+static_cast<std::uint16_t>(arg(1))*measure_ticks+static_cast<std::uint8_t>(arg(2))*beat_ticks+static_cast<std::uint8_t>(arg(3))*grid_ticks+static_cast<std::int16_t>(arg(4));if(arg(2)>=beats || arg(3)>=grids || value<INT32_MIN || value>INT32_MAX)result=invalid;else put(arg(6),static_cast<std::uint32_t>(value),32);}
                    }
                }else if(name=="GetParam" || name=="GetParamEx" || name=="SetParam"){
                    const bool ex=name=="GetParamEx",set=name=="SetParam";const auto index=ex?5U:4U;const auto time=static_cast<std::int32_t>(arg(index));result=0x88781161U;
                    if(!arg(ex?3:2) || (arg(ex?4:3)!=0 && arg(ex?4:3)!=UINT32_MAX) || (ex && arg(2)))result=invalid;
                    else for(auto it=o.runs.rbegin();it!=o.runs.rend();++it){const auto run=object(*it);if(run->voice->secondary || run->music_start>time)continue;const auto segment=object(run->segment);const auto local=static_cast<std::int64_t>(time)-run->music_start+run->start_point;if(local>=0 && local<=segment->sequence->music_length){result=set?set_parameter(*segment,read_guid(memory,arg(1)),static_cast<std::uint32_t>(local),arg(index+1)):parameter(*segment->sequence,read_guid(memory,arg(1)),static_cast<std::uint32_t>(local),arg(index+1),arg(index+2));if(set && !result && read_guid(memory,arg(1))==GUID_TempoParam){const auto tempo=std::bit_cast<double>(memory.load(GuestAddress{arg(index+1)+8},64));const auto reference=music_to_reference(o,time);clock_change(o,reference,tempo*o.master_tempo);o.tempo=tempo;for(auto& [address,a]:bridge.objects)if(address==a->canonical && a->kind=='A' && a->performance==o.canonical){midi::Playback::Message change;change.tempo=tempo;std::lock_guard lock(a->playback->mutex);change.frame=a->playback->clock_frames+static_cast<std::uint64_t>(std::max<std::int64_t>(0,reference-now(o)))*48000/10000000;a->playback->messages.push_back(std::move(change));}}break;}}
                }else if(name=="GetLatencyTime")put(arg(1),static_cast<std::uint64_t>(now(o)+200000),64);
                else if(name=="GetQueueTime")put(arg(1),static_cast<std::uint64_t>(now(o)+static_cast<std::int64_t>(o.bumper)*10000),64);
                else if(name=="AdjustTime"){const auto delta=static_cast<std::int64_t>(arg(1));if(delta< -100000000 || delta>100000000)result=invalid;else o.adjustment+=delta;}
                else if(name=="GetResolvedTime"){
                    const auto flags=arg(3);if(flags&~UINT64_C(0x602c00))result=unsupported;
                    else {auto reference=static_cast<std::int64_t>(arg(1));if(flags&0x400)reference=std::max(reference,now(o)+static_cast<std::int64_t>(o.prepare)*10000);if(flags&0x200000)reference=std::max(reference,now(o)+static_cast<std::int64_t>(o.bumper)*10000);if(flags&0x400000)reference=std::max(reference,now(o)+200000);const auto quantum=flags&0x2000?3072:flags&0x1000?768:flags&0x800?192:1;const auto music=reference_to_music(o,reference);if(quantum>1)reference=music_to_reference(o,static_cast<std::int32_t>((static_cast<std::int64_t>(music)+quantum-1)/quantum*quantum));put(arg(2),static_cast<std::uint64_t>(reference),64);}
                }else if(name=="CreateStandardAudioPath"){check(arg(4),unit);put(arg(4),0,bits);put(arg(4),make_audio_path(o,static_cast<std::uint32_t>(arg(1)),static_cast<std::uint32_t>(arg(2)),arg(3)!=0),bits);}
                else if(name=="GetDefaultAudioPath"){check(arg(1),unit);hold(o.default_path);put(arg(1),o.default_path,bits);}
                else if(name=="SetDefaultAudioPath"){const auto a=object(arg(1),'A');if(a->performance!=o.canonical)result=invalid;else {hold(arg(1));if(o.default_path)release(o.default_path);o.default_path=arg(1);o.playback=a->playback;}}
                else if(name=="AddPort"){if(arg(1))result=unsupported;}
                else if(name=="PlaySegment" || name=="PlaySegmentEx"){
                    const bool ex=name=="PlaySegmentEx";const auto out=arg(ex?6:4);if(out){check(out,unit);put(out,0,bits);}
                    const auto flags=arg(ex?4:2);if(flags&~UINT64_C(0x603dc0) || (ex && (arg(2) || arg(3) || arg(8))))result=unsupported;
                    else if(!(flags&0x40) && arg(ex?5:3)>INT32_MAX)result=invalid;
                    else if(failed(o))result=0x80004005U;
                    else {
                        const auto segment=object(arg(1),'S');const auto audio_path=ex && arg(7)?arg(7):o.default_path;const auto a=object(audio_path,'A');
                        if(a->performance!=o.canonical || !segment->sequence || (!o.auto_download && !segment->downloads.contains(o.canonical)))result=invalid;
                        else {
                            auto reference=arg(ex?5:3)?((flags&0x40)?static_cast<std::int64_t>(arg(ex?5:3)):music_to_reference(o,static_cast<std::int32_t>(arg(ex?5:3)))):now(o);
                            if(flags&0x400)reference=std::max(reference,now(o)+static_cast<std::int64_t>(o.prepare)*10000);if(flags&0x200000)reference=std::max(reference,now(o)+static_cast<std::int64_t>(o.bumper)*10000);if(flags&0x400000)reference=std::max(reference,now(o)+200000);
                            if(flags&0x100){for(auto address:o.runs){const auto old=object(address);if(auto pb=old->voice_playback.lock()){std::lock_guard lock(pb->mutex);const auto& v=*old->voice;if(v.secondary || !v.playing)continue;if(v.repeats==UINT32_MAX)throw std::invalid_argument("無限循環不能佇列至尾端");auto remaining=static_cast<double>(v.sequence->frames)-v.position+static_cast<double>(v.repeats)*static_cast<double>(v.loop_end-v.loop_start);if(pb->tempo_override>0){const auto ticks=static_cast<double>(v.sequence->music_length-v.sequence->music_at(static_cast<std::uint64_t>(v.position)))+static_cast<double>(v.repeats)*static_cast<double>(v.sequence->music_at(v.loop_end)-v.sequence->music_at(v.loop_start));remaining=ticks*48000*60/(768*pb->tempo_override);}const auto wait=v.start>pb->clock_frames?v.start-pb->clock_frames:0;reference=std::max(reference,now(o)+static_cast<std::int64_t>((static_cast<double>(wait)+remaining/pb->tempo_scale)*10000000/48000));}}}
                            const auto quantum=flags&0x2000?3072:flags&0x1000?768:flags&0x800?192:1;if(quantum>1){const auto music=reference_to_music(o,reference);reference=music_to_reference(o,static_cast<std::int32_t>((static_cast<std::int64_t>(music)+quantum-1)/quantum*quantum));}
                            auto run=std::make_shared<MusicObject>();run->kind='T';run->repeats=segment->repeats;run->start_point=segment->start_point;run->music_start=reference_to_music(o,reference);run->reference_start=reference;run->performance=o.canonical;run->voice=std::make_shared<midi::Playback::Voice>();run->voice_playback=a->playback;
                            auto& voice=*run->voice;voice.sequence=segment->sequence;voice.position=static_cast<double>(segment->sequence->frame_at(segment->start_point));voice.loop_start=segment->sequence->frame_at(segment->loop_start);voice.loop_end=segment->loop_end?segment->sequence->frame_at(segment->loop_end):segment->sequence->frames;voice.repeats=segment->repeats;voice.secondary=(flags&0x80)!=0;voice.next=static_cast<std::size_t>(std::lower_bound(segment->sequence->events.begin(),segment->sequence->events.end(),static_cast<std::uint64_t>(voice.position),[](const auto& e,std::uint64_t frame){return e.frame<frame;})-segment->sequence->events.begin());
                            voice.initial_chase=midi::chase(*segment->sequence,static_cast<std::uint64_t>(voice.position));voice.loop_chase=midi::chase(*segment->sequence,voice.loop_start);
                            const auto run_address=create(run);hold(segment->canonical);run->segment=segment->canonical;hold(audio_path);run->default_path=audio_path;voice.id=run_address;
                            {std::lock_guard lock(a->playback->mutex);voice.start=a->playback->clock_frames+static_cast<std::uint64_t>(std::max<std::int64_t>(0,reference-now(o)))*48000/10000000;a->playback->voices.push_back(run->voice);}
                            if(!voice.secondary && !(flags&0x100)){
                                for(auto it=o.runs.begin();it!=o.runs.end();){auto old=object(*it);if(!old->voice->secondary){if(reference<=now(o)){stop_run(*old);release(*it);it=o.runs.erase(it);}else {if(auto pb=old->voice_playback.lock()){std::lock_guard lock(pb->mutex);old->voice->stop=pb->clock_frames+static_cast<std::uint64_t>(reference-now(o))*48000/10000000;}++it;}}else ++it;}
                            }
                            if(!voice.secondary){const auto& seq=*segment->sequence;const auto seek=seq.frame_at(segment->start_point);const midi::Tempo* initial=&seq.tempos.front();for(const auto& t:seq.tempos)if(t.frame<=seek)initial=&t;o.tempo=initial->bpm;clock_change(o,reference,initial->bpm*o.master_tempo);for(const auto& t:seq.tempos)if(t.frame>seek){const auto ref=reference+static_cast<std::int64_t>(static_cast<double>(t.frame-seek)*10000000/(48000*o.master_tempo));o.clock.push_back({ref,static_cast<double>(run->music_start)+t.music-segment->start_point,t.bpm*o.master_tempo});}}
                            o.runs.push_back(run_address);o.active_state=run_address;if(out){hold(run_address);put(out,run_address,bits);}
                        }
                    }
                }else if(name=="IsPlaying"){
                    if(arg(1))object(arg(1),'S');if(arg(2))object(arg(2),'T');result=1;
                    for(auto address:o.runs){const auto run=object(address);if((!arg(1) || arg(1)==run->segment) && (!arg(2) || arg(2)==address)){if(auto pb=run->voice_playback.lock()){std::lock_guard lock(pb->mutex);if(pb->failed){result=0x80004005U;break;}if(run->voice->playing && !run->voice->finished){result=0;break;}}}}
                }else if(name=="GetSegmentState"){
                    check(arg(1),unit);put(arg(1),0,bits);result=0x88781161U;const auto time=static_cast<std::int32_t>(arg(2));
                    for(auto it=o.runs.rbegin();it!=o.runs.rend();++it){const auto run=object(*it);if(auto pb=run->voice_playback.lock()){std::lock_guard lock(pb->mutex);if(!run->voice->secondary && run->music_start<=time && run->voice->playing){hold(*it);put(arg(1),*it,bits);result=0;break;}}}
                }else if(name=="Stop" || name=="StopEx"){
                    const bool ex=name=="StopEx";const auto flags=arg(ex?3:4);if(flags&~UINT64_C(0x40))result=unsupported;
                    else {if(arg(1))object(arg(1));if(!ex && arg(2))object(arg(2),'T');const auto time=arg(ex?2:3);const auto reference=time?((flags&0x40)?static_cast<std::int64_t>(time):music_to_reference(o,static_cast<std::int32_t>(time))):now(o);
                        for(auto it=o.runs.begin();it!=o.runs.end();){const auto run=object(*it);const bool matches=ex?(!arg(1) || arg(1)==*it || arg(1)==run->segment || arg(1)==run->default_path):((!arg(1) || arg(1)==run->segment) && (!arg(2) || arg(2)==*it));if(!matches){++it;continue;}
                            if(reference<=now(o)){stop_run(*run);if(o.active_state==*it)o.active_state=0;release(*it);it=o.runs.erase(it);}else {if(auto pb=run->voice_playback.lock()){std::lock_guard lock(pb->mutex);run->voice->stop=pb->clock_frames+static_cast<std::uint64_t>(reference-now(o))*48000/10000000;}++it;}
                        }
                    }
                }else if(name=="SendPMsg"){
                    const auto p=arg(1);const auto found=o.messages.find(p);if(found==o.messages.end())result=invalid;
                    else {
                        const auto header=x86_?56U:72U;const auto size=found->second.size;const auto flags=memory.load(GuestAddress{p+20},32),channel=memory.load(GuestAddress{p+24},32),type=memory.load(GuestAddress{p+(x86_?40:48)},32);
                        if(memory.load(GuestAddress{p},32)!=size || channel>=16 || memory.load(GuestAddress{p+32},bits) || memory.load(GuestAddress{p+32+unit},bits))result=invalid;
                        else {
                            auto reference=flags&1?static_cast<std::int64_t>(memory.load(GuestAddress{p+8},64)):flags&2?music_to_reference(o,static_cast<std::int32_t>(memory.load(GuestAddress{p+16},32))):now(o);
                            if(flags&4)reference=now(o);midi::Playback::Message message;
                            const auto byte=[&](unsigned offset){if(header+offset>=size)throw std::invalid_argument("PMsg 內容截斷");return static_cast<std::uint8_t>(memory.load(GuestAddress{p+header+offset},8));};
                            if(type==0){const auto status=byte(0),a=byte(1),b=byte(2);if(status<0x80 || status>=0xf0 || a>127 || b>127)throw std::invalid_argument("MIDI PMsg 無效");message.events.push_back({0,static_cast<std::uint8_t>((status&0xf0)|channel),a,b});}
                            else if(type==1){const auto note=byte(19),velocity=byte(12),note_flags=byte(13);if(note>127 || velocity>127)throw std::invalid_argument("Note PMsg 無效");message.events.push_back({0,static_cast<std::uint8_t>((note_flags&1?0x90:0x80)|channel),note,velocity});if(note_flags&1){const auto duration=static_cast<std::int32_t>(memory.load(GuestAddress{p+header},32));if(duration<0)throw std::invalid_argument("Note 時長無效");midi::Playback::Message off;const auto music=reference_to_music(o,reference);const auto end=static_cast<std::int64_t>(music)+duration;if(end>INT32_MAX)throw std::invalid_argument("Note 時長溢位");off.frame=static_cast<std::uint64_t>(std::max<std::int64_t>(0,music_to_reference(o,static_cast<std::int32_t>(end))-reference))*48000/10000000;off.events.push_back({0,static_cast<std::uint8_t>(0x80|channel),note,0});std::lock_guard lock(o.playback->mutex);off.frame+=o.playback->clock_frames+static_cast<std::uint64_t>(std::max<std::int64_t>(0,reference-now(o)))*48000/10000000;o.playback->messages.push_back(std::move(off));}}
                            else if(type==2){if(size<header+4)throw std::invalid_argument("SysEx PMsg 截斷");const auto length=memory.load(GuestAddress{p+header},32);if(length<2 || length>size-header-4)throw std::invalid_argument("SysEx 長度無效");message.sysex.resize(static_cast<std::size_t>(length));memory.read(GuestAddress{p+header+4},message.sysex);if(message.sysex.front()!=0xf0 || message.sysex.back()!=0xf7)throw std::invalid_argument("SysEx 格式無效");}
                            else if(type==4){if(size<header+8)throw std::invalid_argument("Tempo PMsg 截斷");const auto tempo=std::bit_cast<double>(memory.load(GuestAddress{p+header},64));if(!std::isfinite(tempo) || tempo<1 || tempo>1000)throw std::invalid_argument("Tempo PMsg 無效");clock_change(o,reference,tempo*o.master_tempo);o.tempo=tempo;for(auto& [address,a]:bridge.objects)if(address==a->canonical && a->kind=='A' && a->performance==o.canonical){midi::Playback::Message change;change.tempo=tempo;std::lock_guard lock(a->playback->mutex);change.frame=a->playback->clock_frames+static_cast<std::uint64_t>(std::max<std::int64_t>(0,reference-now(o)))*48000/10000000;a->playback->messages.push_back(std::move(change));}}
                            else if(type==7){message.events={{0,static_cast<std::uint8_t>(0xb0|channel),0,byte(1)},{0,static_cast<std::uint8_t>(0xb0|channel),32,byte(2)},{0,static_cast<std::uint8_t>(0xc0|channel),byte(0),0}};}
                            else result=unsupported;
                            if(!result && type!=4){std::lock_guard lock(o.playback->mutex);message.frame=o.playback->clock_frames+static_cast<std::uint64_t>(std::max<std::int64_t>(0,reference-now(o)))*48000/10000000;o.playback->messages.push_back(std::move(message));}
                            if(!result)free_message(o,p);
                        }
                    }
                }else result=unsupported;
            }
        }
    }catch(const std::bad_alloc&){result=0x8007000eU;}
     catch(const std::invalid_argument&){result=invalid;}
     catch(const std::runtime_error& error){
        // Guest memory/ABI faults must retain the runtime's normal fault semantics.
        if(dynamic_cast<const MemoryFault*>(&error) || dynamic_cast<const HostCallFault*>(&error))throw;
        result=0x80004005U;
    }
    arg.return_to(state,result);return std::nullopt;
}
}
