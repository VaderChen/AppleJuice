#include "x64arm/windows_services.hpp"
#include "x64arm/midi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
struct WinmmMidiOutput {
    std::shared_ptr<midi::Playback> playback;
    std::unique_ptr<midi::Output> output;
    std::uint64_t callback{},instance{};
    std::uint32_t callback_type{};
    std::uint8_t running_status{};
    struct Header {std::uint64_t data{};std::uint32_t capacity{};};
    std::map<std::uint64_t,Header> headers;
};
std::optional<std::uint32_t> WindowsServices::invoke_winmm_midi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8;
    std::uint64_t result=0;std::vector<GuestCallback> callbacks;
    const auto notify=[&](const WinmmMidiOutput& output,std::uint64_t handle,unsigned message,std::uint64_t header=0){
        if(!output.callback)return;
        if(output.callback_type==0x30000)callbacks.push_back({output.callback,{handle,message,output.instance,header,0}});
        else if(output.callback_type==0x10000 || output.callback_type==0x20000)
            callbacks.push_back({resolve("user32.dll",output.callback_type==0x10000?"PostMessageW":"PostThreadMessageW").value,{output.callback,message,handle,header}});
        else if(output.callback_type==0x50000 && message==0x3c9)callbacks.push_back({resolve("kernel32.dll","SetEvent").value,{output.callback}});
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
                put(offset,7,2);put(offset+6,0xffff,2); // 軟體合成器：16 channels；未宣告固定 polyphony 或 volume API。
                if(d(2))memory.write(GuestAddress{arg(1)},std::span(caps).first(std::min<std::size_t>(caps.size(),d(2))));
            }
        }else if(symbol=="midiOutOpen"){
            if(!arg(0))result=11;
            else {
                memory.store(GuestAddress{arg(0)},bits,0);const auto kind=d(4)&0x70000;
                if(d(1)!=0 && d(1)!=UINT32_MAX)result=2;
                else if((d(4)&~0x70000U) || (kind!=0 && kind!=0x10000 && kind!=0x20000 && kind!=0x30000 && kind!=0x50000))result=10;
                else if(kind && !arg(2))result=11;
                else if(kind==0x10000 && !gui_.windows.contains(arg(2)))result=11;
                else if(kind==0x20000 && (!thread_from_identifier(static_cast<std::uint32_t>(arg(2))) || threads_.items.at(thread_from_identifier(static_cast<std::uint32_t>(arg(2)))).status==ThreadStatus::Exited))result=11;
                else if(kind==0x50000 && (!runtime_.wait_objects.contains(arg(2)) || runtime_.wait_objects.at(arg(2)).kind!=WaitObject::Kind::Event || !(runtime_.wait_objects.at(arg(2)).access&2U)))result=11;
                else if(midi_outputs_.size()>=64)result=4;
                else {
                    if(kind==0x30000)memory.check(GuestAddress{arg(2)},1,Access::Execute);
                    if(!runtime_.network.external_io)throw HostCallFault("原生 MIDI 輸出不支援 deterministic compare");
                    auto output=std::make_shared<WinmmMidiOutput>();output->playback=std::make_shared<midi::Playback>();output->playback->reset=false;
                    try {output->output=midi::open_output(output->playback);}catch(const std::runtime_error&){result=3;}
                    if(output->output){
                        output->callback_type=kind;output->callback=arg(2);output->instance=arg(3);
                        const auto handle=runtime_.allocate_wait_handle();midi_outputs_.emplace(handle,output);memory.store(GuestAddress{arg(0)},bits,handle);notify(*output,handle,0x3c7);
                    }
                }
            }
        }else {
            const auto found=midi_outputs_.find(arg(0));
            if(found==midi_outputs_.end())result=5;
            else {
                const auto output=found->second;
                if(symbol=="midiOutGetID"){
                    if(!arg(1))result=11;else memory.store(GuestAddress{arg(1)},32,0);
                }else if(symbol=="midiOutClose"){
                    // 此後端同步消耗 SysEx 副本；沒有仍被原生回呼持有的客體 buffer。
                    notify(*output,arg(0),0x3c8);midi_outputs_.erase(found);
                }else if(symbol=="midiOutShortMsg"){
                    auto status=static_cast<std::uint8_t>(d(1)),first=static_cast<std::uint8_t>(d(1)>>8),second=static_cast<std::uint8_t>(d(1)>>16);
                    if(status<0x80){second=first;first=status;status=output->running_status;}
                    unsigned count=0;
                    if(status>=0x80 && status<0xf0)count=(status&0xe0)==0xc0?1:2;
                    else if(status==0xf1 || status==0xf3)count=1;else if(status==0xf2)count=2;
                    if(status<0x80 || status==0xf0 || status==0xf4 || status==0xf5 || status==0xf7 || status==0xf9 || status==0xfd || (count && first>=128) || (count==2 && second>=128))result=11;
                    else if(!output->output->short_message(status,count?first:0,count==2?second:0))result=8;
                    else if(status<0xf0)output->running_status=status;else if(status<0xf8)output->running_status=0;
                }else if(symbol=="midiOutReset"){
                    for(unsigned channel=0;channel<16;++channel)for(unsigned control:{64U,123U,120U})
                        if(!output->output->short_message(static_cast<std::uint8_t>(0xb0+channel),static_cast<std::uint8_t>(control),0))result=8;
                    output->running_status=0;
                }else {
                    const auto header=arg(1);const auto flags_offset=x86_?16U:24U;
                    if(!header || d(2)<(x86_?64U:112U))result=11;
                    else {
                        // WinMM 結構使用 pack(1)：Win64 MIDIHDR 為 112 bytes。
                        memory.check(GuestAddress{header},x86_?64:112,Access::Read);memory.check(GuestAddress{header+flags_offset},4,Access::Write);
                        const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{header+flags_offset},32));
                        const auto prepared=output->headers.find(header);
                        if(symbol=="midiOutUnprepareHeader"){
                            if(flags&4U)result=65;else if(prepared!=output->headers.end()){output->headers.erase(header);memory.store(GuestAddress{header+flags_offset},32,flags&~2U);}
                        }else if(symbol=="midiOutPrepareHeader"){
                            if(prepared==output->headers.end()){
                                const auto data=memory.load(GuestAddress{header},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit},32));
                                if(!data || !size || size>65536 || (flags&~3U))result=11;
                                else if(output->headers.size()>=1024)result=7;
                                else {
                                    for(const auto& [handle,other]:midi_outputs_)if(handle!=arg(0) && other->headers.contains(header))result=4;
                                    if(!result){memory.check(GuestAddress{data},size,Access::Read);output->headers.emplace(header,WinmmMidiOutput::Header{data,size});memory.store(GuestAddress{header+flags_offset},32,(flags&~1U)|2U);}
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
    }catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}
    if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;}
    arg.return_to(state,result);return std::nullopt;
}
}
