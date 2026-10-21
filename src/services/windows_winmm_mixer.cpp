#include "x64arm/windows_services.hpp"
#include "x64arm/audio_mixer.hpp"
#include "x64arm/midi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
namespace x64arm {
struct WinmmMixerState {
    struct Handle {std::uint64_t token{},window{};unsigned id{};std::optional<audio::MixerEndpoint> observed;};
    std::map<std::uint64_t,Handle> handles;
    std::chrono::steady_clock::time_point polled{};
};
namespace {
struct MixerError {std::uint32_t value;};
constexpr std::uint32_t volume_type=0x50030001, mute_type=0x20010002;
unsigned line_id(unsigned device,bool source){return 0x1000U+device*16U+source;}
unsigned control_id(unsigned device,bool source,bool mute){return 1U+device*32U+source*4U+mute;}
std::vector<std::uint8_t> text_bytes(std::u16string_view text,bool wide,unsigned capacity){
    auto value=std::u16string(text.substr(0,capacity-1));if(!value.empty() && value.back()>=0xd800 && value.back()<=0xdbff)value.pop_back();
    if(wide)return utf16_bytes(value);
    auto encoded=utf16_to_utf8(value);
    while(encoded.size()>=capacity){value.pop_back();if(!value.empty() && value.back()>=0xd800 && value.back()<=0xdbff)value.pop_back();encoded=utf16_to_utf8(value);}
    std::vector<std::uint8_t> result(encoded.begin(),encoded.end());result.push_back(0);return result;
}
void put(std::vector<std::uint8_t>& bytes,unsigned at,std::uint64_t value,unsigned count=4){for(unsigned n=0;n<count;++n)bytes.at(at+n)=static_cast<std::uint8_t>(value>>(n*8));}
void text(std::vector<std::uint8_t>& bytes,unsigned at,std::u16string_view value,bool wide,unsigned capacity){const auto data=text_bytes(value,wide,capacity);std::copy(data.begin(),data.end(),bytes.begin()+at);}
}
void WindowsServices::set_audio_mixer_backend(std::shared_ptr<audio::MixerBackend> backend){
    if(winmm_mixer_ && !winmm_mixer_->handles.empty())throw std::logic_error("Mixer 已開啟，不能更換裝置後端");audio_mixer_backend_=std::move(backend);
}
void WindowsServices::poll_winmm_mixer(){
    if(!winmm_mixer_ || !audio_mixer_backend_ || winmm_mixer_->handles.empty())return;
    auto& bridge=*winmm_mixer_;const auto now=std::chrono::steady_clock::now();if(now-bridge.polled<std::chrono::milliseconds(50))return;bridge.polled=now;
    for(auto& [handle,item]:bridge.handles){
        if(item.window && wave_callbacks_.size()+4>4096)continue;
        const auto current=audio_mixer_backend_->query(item.token);const auto old=item.observed;
        const auto notify=[&](unsigned message,unsigned id){if(item.window && wave_callbacks_.size()<4096)wave_callbacks_.push_back({resolve("user32.dll","PostMessageW").value,{item.window,message,handle,id}});};
        if(bool(current)!=bool(old)){for(bool source:{false,true})notify(0x3d0,line_id(item.id,source));}
        else if(current && old){
            if(current->volume!=old->volume)for(bool source:{false,true})notify(0x3d1,control_id(item.id,source,false));
            if(current->muted!=old->muted)for(bool source:{false,true})notify(0x3d1,control_id(item.id,source,true));
        }
        item.observed=current;
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_winmm_mixer(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};std::uint32_t result=0;
    try {
        if(!audio_mixer_backend_)audio_mixer_backend_=audio::make_mixer_backend();if(!winmm_mixer_)winmm_mixer_=std::make_shared<WinmmMixerState>();auto& bridge=*winmm_mixer_;
        poll_winmm_mixer();const auto endpoints=audio_mixer_backend_->endpoints();if(endpoints.size()>256)throw MixerError{7};
        struct Selection {audio::MixerEndpoint endpoint;unsigned id;};
        const auto select=[&](std::uint64_t object,std::uint32_t flags)->Selection {
            const auto kind=(flags>>28)&7U;const bool handle=flags&0x80000000U;
            if(kind==0){
                const auto found=bridge.handles.find(object);
                if(found!=bridge.handles.end()){const auto endpoint=audio_mixer_backend_->query(found->second.token);if(!endpoint)throw MixerError{6};return {*endpoint,found->second.id};}
                if(handle)throw MixerError{5};if(object>=endpoints.size())throw MixerError{2};return {endpoints[object],static_cast<unsigned>(object)};
            }
            if(kind>5)throw MixerError{10};
            if(kind==1 || kind==2){
                if(handle){if(!(kind==1?wave_outputs_.contains(object):wave_inputs_.contains(object)))throw MixerError{5};}
                else if(object!=0 && object!=UINT32_MAX && object!=UINT64_MAX)throw MixerError{2};
            }else if(kind==3){
                if(handle){if(!midi_outputs_.contains(object))throw MixerError{5};}
                else if((object!=0 && object!=UINT32_MAX && object!=UINT64_MAX) || !midi::output_available())throw MixerError{2};
            }else if(kind==4){
                if(handle){if(!midi_inputs_.contains(object))throw MixerError{5};}
                else if(object>=midi::input_devices().size())throw MixerError{2};throw MixerError{6};
            }else throw MixerError{2};
            for(unsigned i=0;i<endpoints.size();++i)if(endpoints[i].capture==(kind==2))return {endpoints[i],i};throw MixerError{6};
        };
        const auto validate_flags=[&](std::uint32_t flags,unsigned allowed){if(flags&~(0xf0000000U|allowed))throw MixerError{10};};
        if(name=="mixerGetNumDevs")result=static_cast<std::uint32_t>(endpoints.size());
        else if(name=="mixerClose"){if(!bridge.handles.erase(arg(0)))throw MixerError{5};}
        else if(name=="mixerOpen"){
            const auto flags=d(4);validate_flags(flags,0x10000);if(!arg(0) || arg(3) || ((flags&0x10000) && (!arg(2) || !gui_.windows.contains(arg(2)))) || (!(flags&0x10000) && arg(2)))throw MixerError{11};
            memory.check(GuestAddress{arg(0)},unit,Access::Write);memory.store(GuestAddress{arg(0)},bits,0);const auto selected=select(d(1),flags);
            if(bridge.handles.size()>=64)throw MixerError{4};const auto handle=runtime_.allocate_wait_handle();bridge.handles.emplace(handle,WinmmMixerState::Handle{selected.endpoint.token,flags&0x10000?arg(2):0,selected.id,selected.endpoint});memory.store(GuestAddress{arg(0)},bits,handle);
        }else if(name=="mixerGetID"){
            validate_flags(d(2),0);if(!arg(1))throw MixerError{11};memory.store(GuestAddress{arg(1)},32,UINT32_MAX);const auto selected=select(arg(0),d(2));memory.store(GuestAddress{arg(1)},32,selected.id);
        }else if(name.starts_with("mixerGetDevCaps")){
            if(!arg(1) && d(2))throw MixerError{11};const auto selected=select(arg(0),0);std::vector<std::uint8_t> caps(wide?80U:48U);put(caps,4,0x100);text(caps,8,selected.endpoint.name,wide,32);put(caps,wide?76:44,1);
            if(d(2))memory.write(GuestAddress{arg(1)},std::span(caps).first(std::min<std::size_t>(d(2),caps.size())));
        }else if(name=="mixerMessage"){
            if(bridge.handles.contains(arg(0)))throw MixerError{8};const auto selected=select(arg(0),0);(void)selected;
            if(d(1)==0x80d){if(!arg(2) || arg(3))throw MixerError{11};memory.store(GuestAddress{arg(2)},32,0);}
            else if(d(1)==0x80c){if(!arg(2) || arg(3)<2)throw MixerError{11};memory.store(GuestAddress{arg(2)},16,0);}else throw MixerError{8};
        }else {
            const auto flags=d(2);validate_flags(flags,15);const auto mode=flags&15U;const auto selected=select(arg(0),flags);const auto& endpoint=selected.endpoint;
            const auto read=[&](unsigned at){return static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+at},32));};
            if(!arg(1))throw MixerError{11};
            if(name.starts_with("mixerGetLineInfo")){
                const unsigned scalar=20+unit,strings=scalar+16,target=strings+(wide?160U:80U),size=target+16+(wide?64U:32U);
                if(read(0)!=size)throw MixerError{11};memory.check(GuestAddress{arg(1)},size,Access::Read);bool source=false;
                if(mode==0){if(read(4))throw MixerError{1024};}
                else if(mode==1){if(read(4) || read(8))throw MixerError{1024};source=true;}
                else if(mode==2){if(read(12)==line_id(selected.id,true))source=true;else if(read(12)!=line_id(selected.id,false))throw MixerError{1024};}
                else if(mode==3){const auto type=read(scalar);if(type==(endpoint.capture?0x1003U:0x1008U))source=true;else if(type!=(endpoint.capture?7U:4U))throw MixerError{1024};}
                else if(mode==4){
                    if(read(target)!=(endpoint.capture?2U:1U) || memory.load(GuestAddress{arg(1)+target+8},32) || read(target+12)!=0x100)throw MixerError{1024};
                    const auto label=wide?read_utf16(memory,GuestAddress{arg(1)+target+16},32):utf8_to_utf16(read_ansi(memory,arg(1)+target+16,32));
                    if(label!=(endpoint.capture?u"macOS Core Audio PCM input":u"macOS Core Audio PCM"))throw MixerError{1024};
                }else throw MixerError{10};
                std::vector<std::uint8_t> line(size);put(line,0,size);put(line,12,line_id(selected.id,source));put(line,16,1U|(source?0x80000000U:0));put(line,scalar,endpoint.capture?(source?0x1003U:7U):(source?0x1008U:4U));put(line,scalar+4,endpoint.channels);put(line,scalar+8,source?0U:1U);put(line,scalar+12,unsigned(endpoint.volume.has_value())+unsigned(endpoint.muted.has_value()));
                text(line,strings,endpoint.capture?(source?u"Microphone":u"Recording"):(source?u"Wave PCM":u"Speakers"),wide,16);text(line,strings+(wide?32U:16U),endpoint.name,wide,64);put(line,target,endpoint.capture?2U:1U);put(line,target+12,0x100);text(line,target+16,endpoint.capture?u"macOS Core Audio PCM input":u"macOS Core Audio PCM",wide,32);memory.write(GuestAddress{arg(1)},line);
            }else if(name.starts_with("mixerGetLineControls")){
                if(read(0)!=20+unit)throw MixerError{11};const auto id=read(4);const bool source=id==line_id(selected.id,true);if(!source && id!=line_id(selected.id,false))throw MixerError{1024};if(mode>2)throw MixerError{10};
                std::vector<bool> controls;for(bool mute:{false,true})if(mute?endpoint.muted.has_value():endpoint.volume.has_value())if(!mode || (mode==1 && read(8)==control_id(selected.id,source,mute)) || (mode==2 && read(8)==(mute?mute_type:volume_type)))controls.push_back(mute);
                if(mode && controls.empty())throw MixerError{1025};if(read(12)!=controls.size() || read(16)!=(wide?228U:148U))throw MixerError{11};const auto output=memory.load(GuestAddress{arg(1)+20},bits);if(!output && !controls.empty())throw MixerError{11};memory.check(GuestAddress{output},controls.size()*read(16),Access::Write);
                for(unsigned i=0;i<controls.size();++i){const bool mute=controls[i];const unsigned size=wide?228U:148U,bounds=wide?180U:100U;std::vector<std::uint8_t> control(size);put(control,0,size);put(control,4,control_id(selected.id,source,mute));put(control,8,mute?mute_type:volume_type);put(control,12,(mute || endpoint.uniform_volume?1U:0U)|((mute?endpoint.mute_writable:endpoint.volume_writable)?0U:0x80000000U));text(control,20,mute?u"Mute":u"Volume",wide,16);text(control,20+(wide?32U:16U),mute?u"CoreAudio mute":u"CoreAudio volume",wide,64);put(control,bounds+4,mute?1U:65535U);put(control,bounds+24,mute?2U:65536U);memory.write(GuestAddress{output+std::uint64_t{i}*size},control);}
            }else if(name.starts_with("mixerGetControlDetails") || name=="mixerSetControlDetails"){
                const bool setting=name=="mixerSetControlDetails";if(mode)throw MixerError{10};if(read(0)!=16+unit*2 || memory.load(GuestAddress{arg(1)+12},bits) || read(12+unit)!=4)throw MixerError{11};
                const auto id=read(4);bool source=false,mute=false,valid=false;
                for(bool s:{false,true})for(bool m:{false,true})if(id==control_id(selected.id,s,m) && (m?endpoint.muted.has_value():endpoint.volume.has_value())){source=s;mute=m;valid=true;}
                (void)source;if(!valid)throw MixerError{1025};const auto count=read(8);if(count!=1 && (mute || endpoint.uniform_volume || count!=endpoint.channels))throw MixerError{11};const auto data=memory.load(GuestAddress{arg(1)+16+unit},bits);if(!data)throw MixerError{11};memory.check(GuestAddress{data},count*4,setting?Access::Read:Access::Write);
                if(setting){
                    if(!runtime_.network.external_io)throw HostCallFault("mixerSetControlDetails is disabled during deterministic compare");
                    if(mute){if(!endpoint.mute_writable)throw MixerError{8};const auto value=memory.load(GuestAddress{data},32);if(value>1)throw MixerError{1026};if(!audio_mixer_backend_->set_mute(endpoint.token,value!=0))throw MixerError{1};}
                    else {if(!endpoint.volume_writable)throw MixerError{8};std::array<float,2> values{};for(unsigned c=0;c<2;++c){const auto value=memory.load(GuestAddress{data+(count==1?0:c*4)},32);if(value>65535)throw MixerError{1026};values[c]=static_cast<float>(value)/65535.F;}if(!audio_mixer_backend_->set_volume(endpoint.token,values))throw MixerError{1};}
                    bridge.polled={};poll_winmm_mixer();
                }else for(unsigned c=0;c<count;++c){const auto value=mute?static_cast<unsigned>(*endpoint.muted):static_cast<unsigned>(std::lround(std::clamp(count==1?std::max((*endpoint.volume)[0],(*endpoint.volume)[1]):(*endpoint.volume)[c],0.F,1.F)*65535.F));memory.store(GuestAddress{data+c*4},32,value);}
            }else throw MixerError{8};
        }
    }catch(const MixerError& error){result=error.value;}catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}
    arg.return_to(state,result);return std::nullopt;
}
}
