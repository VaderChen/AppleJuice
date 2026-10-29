#include "x64arm/windows_services.hpp"
#include "x64arm/midi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
struct WinmmMidiOutput {
    std::shared_ptr<midi::Playback> playback;
    std::unique_ptr<midi::Output> output;
    std::uint64_t callback{},target{},instance{};
    std::uint32_t callback_type{},volume{UINT32_MAX};
    std::uint8_t running_status{};
    struct Header {std::uint64_t data{};std::uint32_t capacity{};bool queued{};};
    std::map<std::uint64_t,Header> headers;
};
namespace {
std::optional<GuestCallback> midi_notification(const WinmmMidiOutput& output,std::uint64_t handle,unsigned message,std::uint64_t header=0){
    if(!output.callback)return {};
    if(output.callback_type==0x30000)return GuestCallback{output.target,{handle,message,output.instance,header,0}};
    if(output.callback_type==0x10000 || output.callback_type==0x20000)return GuestCallback{output.target,{output.callback,message,handle,header}};
    if(output.callback_type==0x50000 && message==0x3c9)return GuestCallback{output.target,{output.callback}};
    return {};
}
void midi_volume(WinmmMidiOutput& output,std::uint32_t value){
    std::lock_guard lock(output.playback->mutex);output.volume=value;
    output.playback->left_gain=static_cast<float>(value&65535U)/65535;
    output.playback->right_gain=static_cast<float>(value>>16)/65535;
}
}
bool WindowsServices::midi_output_waiting()const{
    for(const auto& [handle,output]:midi_outputs_){(void)handle;std::lock_guard lock(output->playback->mutex);
        if(const auto& stream=output->playback->stream;stream && !stream->buffers.empty())return true;}
    return false;
}
void WindowsServices::poll_midi_output(GuestMemory& memory){
    const unsigned flags_offset=x86_?16:24,position_offset=x86_?28:44;
    for(auto& [handle,output]:midi_outputs_){
        std::lock_guard lock(output->playback->mutex);const auto& stream=output->playback->stream;if(!stream)continue;
        if(output->playback->failed){stream->running=false;for(auto& buffer:stream->buffers)buffer->done=true;}
        while(!stream->buffers.empty() && wave_callbacks_.size()<4096){
            auto& buffer=*stream->buffers.front();
            while(buffer.reported<buffer.next && wave_callbacks_.size()<4096){
                const auto& event=buffer.events[buffer.reported++];
                if((event.word&0x40000000U) && output->callback_type!=0x50000){
                    if(auto callback=midi_notification(*output,handle,0x3ca,buffer.token)){
                        callback->writebacks.push_back({buffer.token+position_offset,UINT32_MAX,event.offset});wave_callbacks_.push_back(std::move(*callback));
                    }
                }
            }
            if(!buffer.done || buffer.reported<buffer.next || wave_callbacks_.size()>=4096)break;
            if(auto callback=midi_notification(*output,handle,0x3c9,buffer.token)){
                callback->writebacks.push_back({buffer.token+flags_offset,4U,1U});wave_callbacks_.push_back(std::move(*callback));
            }else {
                const auto flags=memory.load(GuestAddress{buffer.token+flags_offset},32);
                memory.store(GuestAddress{buffer.token+flags_offset},32,(flags&~4U)|1U);
            }
            output->headers.at(buffer.token).queued=false;stream->buffers.pop_front();
        }
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_winmm_midi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8;
    std::uint64_t result=0;std::vector<GuestCallback> callbacks;
    const auto notify=[&](const WinmmMidiOutput& output,std::uint64_t handle,unsigned message,std::uint64_t header=0){
        if(auto callback=midi_notification(output,handle,message,header))callbacks.push_back(std::move(*callback));
    };
    try {
        if(symbol=="midiOutGetNumDevs")result=midi::output_available()?1:0;
        else if(symbol.starts_with("midiOutGetDevCaps")){
            if(arg(0)!=0 && arg(0)!=UINT32_MAX && arg(0)!=UINT64_MAX && !midi_outputs_.contains(arg(0)))result=2;
            else if(!midi::output_available())result=2;
            else if(!arg(1) && d(2))result=11;
            else {
                const bool wide=symbol.ends_with('W');std::vector<std::uint8_t> caps(wide?84:52);
                const auto put=[&](unsigned at,std::uint32_t value,unsigned count){for(unsigned i=0;i<count;++i)caps[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                put(4,0x100,4);const auto name=wide?utf16_bytes(u"macOS DLS Synthesizer"):std::vector<std::uint8_t>{'m','a','c','O','S',' ','D','L','S',' ','S','y','n','t','h','e','s','i','z','e','r',0};
                std::copy(name.begin(),name.end(),caps.begin()+8);const unsigned offset=wide?72:40;
                put(offset,7,2);put(offset+6,0xffff,2);put(offset+8,0xb,4); // 軟體合成器、獨立左右音量及串流。
                if(d(2))memory.write(GuestAddress{arg(1)},std::span(caps).first(std::min<std::size_t>(caps.size(),d(2))));
            }
        }else if(symbol=="midiOutGetVolume" || symbol=="midiOutSetVolume"){
            const bool device=arg(0)==0 || arg(0)==UINT32_MAX || arg(0)==UINT64_MAX;
            const auto found=midi_outputs_.find(arg(0));
            if(!device && found==midi_outputs_.end())result=5;
            else if(symbol=="midiOutGetVolume"){
                if(!arg(1))result=11;else memory.store(GuestAddress{arg(1)},32,device?midi_device_volume_:found->second->volume);
            }else if(device){midi_device_volume_=d(1);for(auto& [handle,output]:midi_outputs_){(void)handle;midi_volume(*output,d(1));}}
            else midi_volume(*found->second,d(1));
        }else if(symbol=="midiOutOpen" || symbol=="midiStreamOpen"){
            const bool stream=symbol=="midiStreamOpen";
            const unsigned callback_arg=stream?3:2,instance_arg=stream?4:3,flags_arg=stream?5:4;
            if(!arg(0))result=11;
            else {
                memory.store(GuestAddress{arg(0)},bits,0);const auto kind=d(flags_arg)&0x70000;
                const auto device=stream?(arg(1)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32)):UINT32_MAX):d(1);
                if(stream && (d(2)!=1 || !arg(1)))result=11;
                else if(device!=0 && device!=UINT32_MAX)result=2;
                else if((d(flags_arg)&~0x70000U) || (kind!=0 && kind!=0x10000 && kind!=0x20000 && kind!=0x30000 && kind!=0x50000))result=stream?11:10;
                else if(kind && !arg(callback_arg))result=11;
                else if(kind==0x10000 && !gui_.windows.contains(arg(callback_arg)))result=11;
                else if(kind==0x20000 && (!thread_from_identifier(static_cast<std::uint32_t>(arg(callback_arg))) || threads_.items.at(thread_from_identifier(static_cast<std::uint32_t>(arg(callback_arg)))).status==ThreadStatus::Exited))result=11;
                else if(kind==0x50000 && (!runtime_.wait_objects.contains(arg(callback_arg)) || runtime_.wait_objects.at(arg(callback_arg)).kind!=WaitObject::Kind::Event || !(runtime_.wait_objects.at(arg(callback_arg)).access&2U)))result=11;
                else if(midi_outputs_.size()>=64)result=4;
                else {
                    if(kind==0x30000)memory.check(GuestAddress{arg(callback_arg)},1,Access::Execute);
                    if(stream)memory.check(GuestAddress{arg(1)},4,Access::Write);
                    if(!runtime_.network.external_io)throw HostCallFault("原生 MIDI 輸出不支援 deterministic compare");
                    auto output=std::make_shared<WinmmMidiOutput>();output->playback=std::make_shared<midi::Playback>();output->playback->reset=false;
                    if(stream)output->playback->stream=std::make_shared<midi::Stream>();
                    midi_volume(*output,midi_device_volume_);
                    try {output->output=midi::open_output(output->playback);}catch(const std::runtime_error&){result=3;}
                    if(output->output){
                        output->callback_type=kind;output->callback=arg(callback_arg);output->instance=arg(instance_arg);
                        output->target=kind==0x10000 || kind==0x20000?resolve("user32.dll",kind==0x10000?"PostMessageW":"PostThreadMessageW").value:kind==0x50000?resolve("kernel32.dll","SetEvent").value:output->callback;
                        if(stream)memory.store(GuestAddress{arg(1)},32,0);
                        const auto handle=runtime_.allocate_wait_handle();midi_outputs_.emplace(handle,output);memory.store(GuestAddress{arg(0)},bits,handle);notify(*output,handle,0x3c7);
                    }
                }
            }
        }else {
            const auto found=midi_outputs_.find(arg(0));
            if(found==midi_outputs_.end() || (symbol.starts_with("midiStream") && !found->second->playback->stream))result=5;
            else {
                const auto output=found->second;
                const auto stream=output->playback->stream;
                if(symbol=="midiOutGetID"){
                    if(!arg(1))result=11;else memory.store(GuestAddress{arg(1)},32,0);
                }else if(symbol=="midiOutClose" || symbol=="midiStreamClose"){
                    for(const auto& [address,header]:output->headers)
                        if(header.queued || (memory.load(GuestAddress{address+(x86_?16:24)},32)&4U)){result=65;break;}
                    if(!result){
                        if(stream){if(auto callback=midi_notification(*output,arg(0),0x3c8))wave_callbacks_.push_back(std::move(*callback));}
                        else notify(*output,arg(0),0x3c8);
                        midi_outputs_.erase(found);
                    }
                }else if(symbol=="midiStreamPause" || symbol=="midiStreamRestart"){
                    std::lock_guard lock(output->playback->mutex);
                    if(output->playback->failed)result=3;
                    else {stream->running=symbol=="midiStreamRestart";if(stream->running)stream->stopped=false;}
                }else if(symbol=="midiStreamProperty"){
                    const auto property=d(2),action=property&0xc0000000U,kind=property&0x3fffffffU;
                    if(!arg(1) || (action!=0x40000000 && action!=0x80000000) || (kind!=1 && kind!=2) || memory.load(GuestAddress{arg(1)},32)!=8)result=11;
                    else {
                        std::lock_guard lock(output->playback->mutex);
                        if(action==0x40000000)memory.store(GuestAddress{arg(1)+4},32,kind==1?stream->division:stream->tempo);
                        else {
                            const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4},32));
                            if(kind==1){if(!stream->stopped || !midi::Stream::valid_division(value))result=11;else stream->division=value&0xffffU;}
                            else if(!value)result=11;else stream->tempo=value;
                        }
                    }
                }else if(symbol=="midiStreamPosition"){
                    if(!arg(1) || d(2)<12)result=11;
                    else {
                        memory.check(GuestAddress{arg(1)},12,Access::Write);
                        auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
                        std::lock_guard lock(output->playback->mutex);std::uint64_t value;
                        if(type==32)value=static_cast<std::uint64_t>(stream->ticks);
                        else if(type==4)value=stream->bytes;
                        else {type=1;value=stream->frames/48;}
                        memory.store(GuestAddress{arg(1)},32,type);memory.store(GuestAddress{arg(1)+4},32,value);memory.store(GuestAddress{arg(1)+8},32,0);
                    }
                }else if(symbol=="midiOutShortMsg"){
                    auto status=static_cast<std::uint8_t>(d(1)),first=static_cast<std::uint8_t>(d(1)>>8),second=static_cast<std::uint8_t>(d(1)>>16);
                    if(status<0x80){second=first;first=status;status=output->running_status;}
                    unsigned count=0;
                    if(status>=0x80 && status<0xf0)count=(status&0xe0)==0xc0?1:2;
                    else if(status==0xf1 || status==0xf3)count=1;else if(status==0xf2)count=2;
                    if(status<0x80 || status==0xf0 || status==0xf4 || status==0xf5 || status==0xf7 || status==0xf9 || status==0xfd || (count && first>=128) || (count==2 && second>=128))result=11;
                    else if(!output->output->short_message(status,count?first:0,count==2?second:0))result=8;
                    else {
                        if(status<0xf0)output->running_status=status;else if(status<0xf8)output->running_status=0;
                        if(stream){std::lock_guard lock(output->playback->mutex);
                            if((status&0xf0)==0x90)stream->notes[status&15][first]=second!=0;
                            else if((status&0xf0)==0x80)stream->notes[status&15][first]=false;
                            else if((status&0xf0)==0xb0 && (first==120 || first==123))stream->notes[status&15].fill(false);
                        }
                    }
                }else if(symbol=="midiOutReset" || symbol=="midiStreamStop"){
                    std::vector<std::pair<unsigned,unsigned>> notes;
                    if(stream){
                        std::lock_guard lock(output->playback->mutex);stream->running=false;stream->stopped=true;stream->armed=false;stream->queued_status=0;
                        for(auto& buffer:stream->buffers)buffer->done=true;
                        for(unsigned c=0;c<16;++c)for(unsigned n=0;n<128;++n)if(stream->notes[c][n]){notes.emplace_back(c,n);stream->notes[c][n]=false;}
                        if(symbol=="midiOutReset")stream->ticks=stream->frames=stream->bytes=0;
                        stream->event_tick=stream->ticks;
                    }
                    if(symbol=="midiOutReset"){
                        for(unsigned channel=0;channel<16;++channel)for(unsigned control:{64U,123U,120U})
                            if(!output->output->short_message(static_cast<std::uint8_t>(0xb0+channel),static_cast<std::uint8_t>(control),0))result=8;
                    }else for(const auto& [channel,note]:notes)
                        if(!output->output->short_message(static_cast<std::uint8_t>(0x80+channel),static_cast<std::uint8_t>(note),0))result=8;
                    output->running_status=0;
                    if(stream)poll_midi_output(memory);
                }else {
                    const auto header=arg(1);const auto flags_offset=x86_?16U:24U;
                    if(!header || d(2)<(x86_?64U:112U))result=11;
                    else {
                        // WinMM 結構使用 pack(1)：Win64 MIDIHDR 為 112 bytes。
                        memory.check(GuestAddress{header},x86_?64:112,Access::Read);memory.check(GuestAddress{header+flags_offset},4,Access::Write);
                        const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{header+flags_offset},32));
                        const auto prepared=output->headers.find(header);
                        if(symbol=="midiOutUnprepareHeader"){
                            if((flags&4U) || (prepared!=output->headers.end() && prepared->second.queued))result=65;else if(prepared!=output->headers.end()){output->headers.erase(header);memory.store(GuestAddress{header+flags_offset},32,flags&~2U);}
                        }else if(symbol=="midiOutPrepareHeader"){
                            if(prepared==output->headers.end()){
                                const auto data=memory.load(GuestAddress{header},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit},32));
                                if(!data || !size || size>65536 || (flags&~11U))result=11;
                                else if(output->headers.size()>=1024)result=7;
                                else {
                                    for(const auto& [handle,other]:midi_outputs_)if(handle!=arg(0) && other->headers.contains(header))result=4;
                                    if(!result){memory.check(GuestAddress{data},size,Access::Read);output->headers.emplace(header,WinmmMidiOutput::Header{data,size});memory.store(GuestAddress{header+flags_offset},32,(flags&~1U)|2U);}
                                }
                            }
                        }else if(symbol=="midiStreamOut"){
                            if(prepared==output->headers.end() || !(flags&2U))result=64;
                            else if((flags&4U) || prepared->second.queued)result=65;
                            else {
                                const auto data=memory.load(GuestAddress{header},bits);
                                const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit},32));
                                const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit+4},32));
                                if(data!=prepared->second.data || capacity>prepared->second.capacity || size>capacity || !size || size>=65536)result=11;
                                else {
                                    memory.check(GuestAddress{header+(x86_?28:44)},4,Access::Write);
                                    std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{data},bytes);
                                    auto buffer=std::make_shared<midi::Stream::Buffer>();buffer->token=header;
                                    std::lock_guard lock(output->playback->mutex);auto status=stream->queued_status;
                                    buffer->events=midi::Stream::parse(bytes,status);
                                    if(output->playback->failed)result=3;
                                    else if(stream->buffers.size()>=1024)result=7;
                                    else {
                                        stream->buffers.push_back(std::move(buffer));stream->queued_status=status;prepared->second.queued=true;
                                        memory.store(GuestAddress{header+flags_offset},32,(flags&~1U)|12U);
                                    }
                                }
                            }
                        }else if(symbol=="midiOutLongMsg"){
                            if(prepared==output->headers.end() || !(flags&2U))result=64;
                            else if(flags&4U)result=65;
                            else if(flags&8U)result=8;
                            else {
                                const auto data=memory.load(GuestAddress{header},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit},32));
                                if(data!=prepared->second.data || size>prepared->second.capacity || size<2)result=11;
                                else {
                                    std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{data},bytes);
                                    if(bytes.front()!=0xf0 || bytes.back()!=0xf7 || std::any_of(bytes.begin()+1,bytes.end()-1,[](auto byte){return byte>=128;}))result=11;
                                    else if(!output->output->system_exclusive(bytes))result=8;
                                    else {output->running_status=0;memory.store(GuestAddress{header+flags_offset},32,(flags&~4U)|1U);notify(*output,arg(0),0x3c9,header);}
                                }
                            }
                        }else throw HostCallFault("未知的 WinMM MIDI 服務");
                    }
                }
            }
        }
    }catch(const MemoryFault&){result=11;}catch(const std::invalid_argument&){result=11;}catch(const std::bad_alloc&){result=7;}
    if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;}
    arg.return_to(state,result);return std::nullopt;
}
}
