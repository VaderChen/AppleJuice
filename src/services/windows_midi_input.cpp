#include "x64arm/windows_services.hpp"
#include "x64arm/midi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
struct WinmmMidiInput {
    std::unique_ptr<midi::Input> input;std::uint32_t device{},endpoint{},kind{};
    std::uint64_t callback{},target{},instance{};bool started{},io_status{},sysex{};
    std::uint8_t running{},status{},count{},needed{};std::uint32_t message{},stamp{};
    struct Header {std::uint64_t data{};std::uint32_t capacity{},size{},used{},stamp{};bool queued{};};
    std::map<std::uint64_t,Header> headers;std::deque<std::uint64_t> queue;
    void notify(std::deque<GuestCallback>& callbacks,std::uint64_t handle,unsigned msg,std::uint64_t param=0,std::uint32_t time=0)const{
        if(kind==0x30000)callbacks.push_back({target,{handle,msg,instance,param,time}});
        else if(kind==0x10000 || kind==0x20000)callbacks.push_back({target,{callback,msg,handle,param}});
        else if(kind==0x50000 && (msg==0x3c4 || msg==0x3c6))callbacks.push_back({target,{callback}});
    }
    void complete(GuestMemory& memory,unsigned unit,std::deque<GuestCallback>& callbacks,std::uint64_t handle,bool error=false){
        if(queue.empty())return;const auto address=queue.front();auto& header=headers.at(address);
        const auto offset=unit==4?16U:24U;const auto flags=memory.load(GuestAddress{address+offset},32);
        memory.store(GuestAddress{address+unit+4},32,header.used);memory.store(GuestAddress{address+offset},32,(flags&~4U)|1U);
        header.queued=false;queue.pop_front();notify(callbacks,handle,error?0x3c6:0x3c4,address,header.stamp);
    }
    void consume(GuestMemory& memory,unsigned unit,std::deque<GuestCallback>& callbacks,std::uint64_t handle,const midi::InputPacket& packet){
        if(packet.overflow){if(sysex)complete(memory,unit,callbacks,handle,true);notify(callbacks,handle,0x3c5,0,packet.milliseconds);running=status=count=needed=0;sysex=false;return;}
        for(const auto byte:packet.bytes){
            // 即時訊息不打斷 SysEx、running status 或尚未收完的短訊息。
            if(byte>=0xf8){notify(callbacks,handle,byte==0xf9 || byte==0xfd?0x3c5:0x3c3,byte,packet.milliseconds);continue;}
            if(sysex){
                if(byte>=0x80 && byte!=0xf7){complete(memory,unit,callbacks,handle,true);sysex=false;}
                else {
                    if(!queue.empty()){auto& h=headers.at(queue.front());if(!h.used)h.stamp=packet.milliseconds;memory.store(GuestAddress{h.data+h.used++},8,byte);if(h.used==h.size || byte==0xf7)complete(memory,unit,callbacks,handle);}
                    if(byte==0xf7)sysex=false;continue;
                }
            }
            if(byte&0x80){
                if(status && count<needed)notify(callbacks,handle,0x3c5,message,stamp);
                status=count=needed=0;running=byte<0xf0?byte:0;
                if(byte==0xf0){sysex=true;if(!queue.empty()){auto& h=headers.at(queue.front());h.stamp=packet.milliseconds;memory.store(GuestAddress{h.data+h.used++},8,byte);if(h.used==h.size)complete(memory,unit,callbacks,handle);}continue;}
                if(byte==0xf4 || byte==0xf5 || byte==0xf7){notify(callbacks,handle,0x3c5,byte,packet.milliseconds);continue;}
                if(byte==0xf6){notify(callbacks,handle,0x3c3,byte,packet.milliseconds);continue;}
                status=byte;needed=byte<0xf0?((byte&0xe0)==0xc0?1:2):(byte==0xf2?2:1);message=byte;stamp=packet.milliseconds;continue;
            }
            if(!status){if(!running){notify(callbacks,handle,0x3c5,byte,packet.milliseconds);continue;}status=running;needed=(status&0xe0)==0xc0?1:2;message=status;count=0;stamp=packet.milliseconds;}
            message|=static_cast<std::uint32_t>(byte)<<(8*(++count));
            if(count==needed){notify(callbacks,handle,io_status && callbacks.size()>128?0x3cc:0x3c3,message,stamp);status=count=needed=0;}
        }
    }
};
bool WindowsServices::midi_input_waiting()const{for(const auto& [handle,input]:midi_inputs_){(void)handle;if(input->started)return true;}return false;}
void WindowsServices::poll_midi_input(GuestMemory& memory){
    if(wave_callbacks_.size()>512)return;
    for(auto& [handle,input]:midi_inputs_){if(wave_callbacks_.size()>512)break;if(input->started)for(const auto& packet:input->input->drain())input->consume(memory,x86_?4:8,wave_callbacks_,handle,packet);}
}
std::optional<std::uint32_t> WindowsServices::invoke_midi_input(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8,header_size=x86_?64:112,flags_offset=x86_?16:24;std::uint64_t result=0;
    try {
        if(name=="midiInGetNumDevs")result=midi::input_devices().size();
        else if(name.starts_with("midiInGetDevCaps")){
            const auto devices=midi::input_devices();auto index=arg(0);const auto opened=midi_inputs_.find(index);
            if(opened!=midi_inputs_.end()){index=devices.size();for(std::size_t i=0;i<devices.size();++i)if(devices[i].endpoint==opened->second->endpoint){index=i;break;}}
            if(index>=devices.size())result=2;
            else if(!arg(1) && d(2))result=11;
            else {
                const bool wide=name.ends_with('W');std::vector<std::uint8_t> caps(wide?76:44);caps[5]=1;
                auto text=utf8_to_utf16_lossy(devices[index].name);if(text.size()>31){text.resize(31);if(text.back()>=0xd800 && text.back()<=0xdbff)text.pop_back();}
                if(wide){const auto bytes=utf16_bytes(text);std::copy(bytes.begin(),bytes.end(),caps.begin()+8);}
                else {auto bytes=utf16_to_utf8(text);while(bytes.size()>31){text.pop_back();if(!text.empty() && text.back()>=0xd800 && text.back()<=0xdbff)text.pop_back();bytes=utf16_to_utf8(text);}std::copy(bytes.begin(),bytes.end(),caps.begin()+8);}
                if(d(2))memory.write(GuestAddress{arg(1)},std::span(caps).first(std::min<std::size_t>(caps.size(),d(2))));
            }
        }else if(name=="midiInOpen"){
            if(!arg(0))result=11;
            else {
                memory.store(GuestAddress{arg(0)},bits,0);const auto kind=d(4)&0x70000;const auto devices=midi::input_devices();
                if(d(1)>=devices.size())result=2;
                else if((d(4)&~0x70020U) || (kind!=0 && kind!=0x10000 && kind!=0x20000 && kind!=0x30000 && kind!=0x50000))result=10;
                else if(kind && !arg(2))result=11;
                else if(kind==0x10000 && !gui_.windows.contains(arg(2)))result=11;
                else if(kind==0x20000 && (!thread_from_identifier(static_cast<std::uint32_t>(arg(2))) || threads_.items.at(thread_from_identifier(static_cast<std::uint32_t>(arg(2)))).status==ThreadStatus::Exited))result=11;
                else if(kind==0x50000 && (!runtime_.wait_objects.contains(arg(2)) || runtime_.wait_objects.at(arg(2)).kind!=WaitObject::Kind::Event || !(runtime_.wait_objects.at(arg(2)).access&2U)))result=11;
                else if(midi_inputs_.size()>=64)result=4;
                else {
                    if(kind==0x30000)memory.check(GuestAddress{arg(2)},1,Access::Execute);
                    if(!runtime_.network.external_io)throw HostCallFault("原生 MIDI 輸入不支援 deterministic compare");
                    auto input=std::make_shared<WinmmMidiInput>();input->device=d(1);input->endpoint=devices[d(1)].endpoint;
                    try{input->input=midi::open_input(input->endpoint);}catch(const std::runtime_error&){result=3;}
                    if(input->input){
                        input->kind=kind;input->callback=arg(2);input->instance=arg(3);input->io_status=(d(4)&0x20) && (kind==0x10000 || kind==0x30000);
                        input->target=kind==0x30000?arg(2):kind==0x50000?resolve("kernel32.dll","SetEvent").value:kind?resolve("user32.dll",kind==0x10000?"PostMessageW":"PostThreadMessageW").value:0;
                        const auto handle=runtime_.allocate_wait_handle();midi_inputs_.emplace(handle,input);memory.store(GuestAddress{arg(0)},bits,handle);input->notify(wave_callbacks_,handle,0x3c1);
                    }
                }
            }
        }else {
            const auto found=midi_inputs_.find(arg(0));
            if(found==midi_inputs_.end())result=5;
            else {
                auto& input=*found->second;
                if(name=="midiInGetID"){if(!arg(1))result=11;else memory.store(GuestAddress{arg(1)},32,input.device);}
                else if(name=="midiInClose"){
                    if(!input.queue.empty())result=65;
                    else {input.input->stop();input.notify(wave_callbacks_,arg(0),0x3c2);midi_inputs_.erase(found);}
                }else if(name=="midiInStart"){
                    if(!input.started){input.input->start();input.started=true;}
                }else if(name=="midiInStop" || name=="midiInReset"){
                    input.input->stop();
                    for(;;){const auto packets=input.input->drain();if(packets.empty())break;for(const auto& packet:packets)input.consume(memory,unit,wave_callbacks_,arg(0),packet);}
                    if(input.started && !input.queue.empty() && input.headers.at(input.queue.front()).used)input.complete(memory,unit,wave_callbacks_,arg(0));
                    input.started=false;
                    if(name=="midiInReset"){while(!input.queue.empty())input.complete(memory,unit,wave_callbacks_,arg(0));input.running=input.status=input.count=input.needed=0;input.sysex=false;}
                }else {
                    const auto address=arg(1);
                    if(!address || d(2)<header_size)result=11;
                    else {
                        memory.check(GuestAddress{address},header_size,Access::Read);memory.check(GuestAddress{address+flags_offset},4,Access::Write);memory.check(GuestAddress{address+unit+4},4,Access::Write);
                        const auto flags=memory.load(GuestAddress{address+flags_offset},32);const auto prepared=input.headers.find(address);
                        if(name=="midiInUnprepareHeader"){
                            if(prepared!=input.headers.end() && prepared->second.queued)result=65;
                            else {input.headers.erase(address);memory.store(GuestAddress{address+flags_offset},32,flags&~2U);}
                        }else if(name=="midiInPrepareHeader"){
                            if(prepared==input.headers.end()){
                                const auto data=memory.load(GuestAddress{address},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{address+unit},32));
                                if(!data || !size || size>65536 || (flags&~3U))result=11;
                                else if(input.headers.size()>=1024)result=7;
                                else {
                                    for(const auto& [handle,other]:midi_inputs_)if(handle!=arg(0) && other->headers.contains(address))result=4;
                                    if(!result){memory.check(GuestAddress{data},size,Access::Write);input.headers.emplace(address,WinmmMidiInput::Header{data,size});memory.store(GuestAddress{address+flags_offset},32,(flags&~1U)|2U);}
                                }
                            }
                        }else if(name=="midiInAddBuffer"){
                            if(prepared==input.headers.end() || !(flags&2U))result=64;
                            else if(prepared->second.queued)result=65;
                            else {
                                auto& header=prepared->second;const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{address+unit},32));
                                if(memory.load(GuestAddress{address},bits)!=header.data || !size || size>header.capacity)result=11;
                                else {memory.check(GuestAddress{header.data},size,Access::Write);input.queue.push_back(address);header.size=size;header.used=0;header.stamp=0;header.queued=true;memory.store(GuestAddress{address+unit+4},32,0);memory.store(GuestAddress{address+flags_offset},32,(flags&~1U)|6U);}
                            }
                        }else throw HostCallFault("未知的 WinMM MIDI 輸入服務");
                    }
                }
            }
        }
    }catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}
    arg.return_to(state,result);return std::nullopt;
}
}
