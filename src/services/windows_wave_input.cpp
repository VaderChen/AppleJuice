#include "x64arm/windows_services.hpp"
#include "x64arm/audio.hpp"
#include "x64arm/unicode.hpp"
#include "pcm_wave_format.hpp"
#include "wave_device_message.hpp"
#include <algorithm>

namespace x64arm {
struct WinmmWaveInput {
    std::shared_ptr<audio::Buffer> buffer;
    std::unique_ptr<audio::Input> input;
    std::uint64_t callback{},target{},instance{},read_frames{};
    std::uint32_t callback_type{};
    bool started{};
    struct Header {std::uint64_t data{};std::uint32_t capacity{},size{},used{};bool queued{};};
    std::map<std::uint64_t,Header> headers;
    std::deque<std::uint64_t> queue;
    void notify(std::deque<GuestCallback>& callbacks,std::uint64_t handle,unsigned message,std::uint64_t header=0) const {
        if(callback_type==0x30000)callbacks.push_back({target,{handle,message,instance,header,0}});
        else if(callback_type==0x10000 || callback_type==0x20000)callbacks.push_back({target,{callback,message,handle,header}});
        else if(callback_type==0x50000 && message==0x3c0)callbacks.push_back({target,{callback}});
    }
    void complete(GuestMemory& memory,unsigned unit,std::deque<GuestCallback>& callbacks,std::uint64_t handle){
        const auto address=queue.front();auto& header=headers.at(address);const auto offset=unit==4?16U:24U;
        const auto flags=memory.load(GuestAddress{address+offset},32);
        memory.check(GuestAddress{address+unit+4},4,Access::Write);memory.check(GuestAddress{address+offset},4,Access::Write);
        memory.store(GuestAddress{address+unit+4},32,header.used);memory.store(GuestAddress{address+offset},32,(flags&~0x10U)|1U);
        header.queued=false;queue.pop_front();notify(callbacks,handle,0x3c0,address);
    }
    // 呼叫端持有 buffer.mutex；原生 callback 只寫宿主 PCM ring，不存取客體指標。
    void drain(GuestMemory& memory,unsigned unit,std::deque<GuestCallback>& callbacks,std::uint64_t handle){
        const auto frame_bytes=buffer->frame_bytes();const auto ring_frames=buffer->pcm.size()/frame_bytes;
        const auto total=buffer->captured_frames;
        if(total-read_frames>ring_frames)read_frames=total-ring_frames;
        while(read_frames<total && !queue.empty()){
            auto& header=headers.at(queue.front());const auto at=static_cast<std::size_t>(read_frames%ring_frames)*frame_bytes;
            const auto bytes=std::min<std::uint64_t>({header.size-header.used,(total-read_frames)*frame_bytes,buffer->pcm.size()-at});
            memory.write(GuestAddress{header.data+header.used},std::span(buffer->pcm).subspan(at,static_cast<std::size_t>(bytes)));
            header.used+=static_cast<std::uint32_t>(bytes);read_frames+=bytes/frame_bytes;
            if(header.used==header.size)complete(memory,unit,callbacks,handle);
        }
        // Windows 在應用程式沒有提供緩衝區時丟棄輸入；後來排入的 buffer 不接收舊資料。
        if(queue.empty())read_frames=total;
    }
};
void WindowsServices::set_audio_input_backend(std::shared_ptr<audio::InputBackend> backend){
    if(!wave_inputs_.empty())throw std::logic_error("錄音裝置開啟後不能更換輸入後端");
    audio_input_backend_=std::move(backend);
}
bool WindowsServices::wave_input_waiting() const {
    for(const auto& [handle,input]:wave_inputs_){(void)handle;if(input->started && !input->queue.empty())return true;}return false;
}
void WindowsServices::poll_wave_input(GuestMemory& memory){
    for(auto& [handle,input]:wave_inputs_){
        std::lock_guard lock(input->buffer->mutex);
        input->drain(memory,x86_?4U:8U,wave_callbacks_,handle);
        if(input->buffer->capture_failed){input->started=false;while(!input->queue.empty())input->complete(memory,x86_?4U:8U,wave_callbacks_,handle);}
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_wave_input(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8,header_size=x86_?32:48,flags_offset=x86_?16:24;
    const auto available=[&](){return audio_input_backend_?audio_input_backend_->available():audio::input_available();};
    std::uint32_t result=0;
    try {
        poll_wave_input(memory);poll_wave_output(memory);
        if(name=="waveInMessage")result=wave::device_message(memory,arg(0),d(1),arg(2),arg(3),true,available(),wave_inputs_.contains(arg(0)),x86_);
        else if(name=="waveInGetNumDevs")result=available()?1:0;
        else if(name.starts_with("waveInGetDevCaps")){
            if(arg(0)!=0 && arg(0)!=UINT32_MAX && arg(0)!=UINT64_MAX && !wave_inputs_.contains(arg(0)))result=2;
            else if(!available())result=2;
            else if(!arg(1) && d(2))result=11;
            else {
                const bool wide=name.ends_with('W');std::vector<std::uint8_t> caps(wide?80:48);
                const auto put=[&](unsigned at,std::uint32_t value,unsigned count){for(unsigned i=0;i<count;++i)caps[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
                const auto text=wide?utf16_bytes(u"macOS Core Audio PCM input"):std::vector<std::uint8_t>{'m','a','c','O','S',' ','C','o','r','e',' ','A','u','d','i','o',' ','P','C','M',' ','i','n','p','u','t',0};
                put(4,0x100,4);std::copy(text.begin(),text.end(),caps.begin()+8);const unsigned at=wide?72:40;
                put(at,0xfffff,4);put(at+4,2,2);
                if(d(2))memory.write(GuestAddress{arg(1)},std::span(caps).first(std::min<std::size_t>(caps.size(),d(2))));
            }
        }else if(name=="waveInOpen"){
            const auto flags=d(5),kind=flags&0x70000;const bool query=(flags&1)!=0;
            if(!arg(2) || (!query && !arg(0)))result=11;
            else {
                if(arg(0)){memory.check(GuestAddress{arg(0)},unit,Access::Write);memory.store(GuestAddress{arg(0)},bits,0);}
                const auto format=read_pcm_wave_format(memory,arg(2),true,true,8000,192000);
                if(d(1)!=0 && d(1)!=UINT32_MAX && !wave_inputs_.contains(arg(1)))result=2;
                else if((flags&~0x7000fU) || (kind!=0 && kind!=0x10000 && kind!=0x20000 && kind!=0x30000 && kind!=0x50000))result=10;
                else if(!format)result=32;
                else if(!available())result=3;
                else if(query)result=0;
                else if(kind && !arg(3))result=11;
                else if(kind==0x10000 && !gui_.windows.contains(arg(3)))result=11;
                else if(kind==0x20000 && (!thread_from_identifier(static_cast<std::uint32_t>(arg(3))) || threads_.items.at(thread_from_identifier(static_cast<std::uint32_t>(arg(3)))).status==ThreadStatus::Exited))result=11;
                else if(kind==0x50000 && (!runtime_.wait_objects.contains(arg(3)) || runtime_.wait_objects.at(arg(3)).kind!=WaitObject::Kind::Event || !(runtime_.wait_objects.at(arg(3)).access&2U)))result=11;
                else if(wave_inputs_.size()>=64)result=4;
                else {
                    if(kind==0x30000)memory.check(GuestAddress{arg(3)},1,Access::Execute);
                    if(!runtime_.network.external_io)throw HostCallFault("原生 PCM 輸入不支援 deterministic compare");
                    auto input=std::make_shared<WinmmWaveInput>();input->buffer=std::make_shared<audio::Buffer>();auto& buffer=*input->buffer;
                    buffer.channels=format->channels;buffer.bits=format->bits;buffer.floating=format->floating;buffer.rate=buffer.frequency=format->rate;
                    buffer.pcm.resize(static_cast<std::size_t>(std::max(format->rate,4096U))*format->align);buffer.looping=true;
                    try {input->input=audio_input_backend_?audio_input_backend_->open(input->buffer):audio::open_input(input->buffer);}catch(const std::runtime_error&){result=3;}
                    if(!input->input)result=3;
                    else {
                        input->callback_type=kind;input->callback=arg(3);input->instance=arg(4);
                        input->target=kind==0x30000?arg(3):kind==0x10000?resolve("user32.dll","PostMessageW").value:kind==0x20000?resolve("user32.dll","PostThreadMessageW").value:resolve("kernel32.dll","SetEvent").value;
                        const auto handle=runtime_.allocate_wait_handle();wave_inputs_.emplace(handle,input);memory.store(GuestAddress{arg(0)},bits,handle);input->notify(wave_callbacks_,handle,0x3be);
                    }
                }
            }
        }else {
            const auto found=wave_inputs_.find(arg(0));
            if(found==wave_inputs_.end())result=5;
            else {
                // input 的生命週期涵蓋 lock；CoreAudio 停止／釋放必須在解除 buffer.mutex 後。
                const auto input=found->second;auto& buffer=*input->buffer;std::lock_guard lock(buffer.mutex);
                input->drain(memory,unit,wave_callbacks_,arg(0));
                if(name=="waveInClose"){
                    if(!input->queue.empty())result=33;
                    else {input->started=false;buffer.playing=false;input->notify(wave_callbacks_,arg(0),0x3bf);wave_inputs_.erase(found);}
                }else if(name=="waveInGetID"){if(!arg(1))result=11;else memory.store(GuestAddress{arg(1)},32,0);}
                else if(name=="waveInStart"){
                    if(buffer.capture_failed)result=3;
                    else {input->started=true;buffer.playing=true;}
                }else if(name=="waveInStop" || name=="waveInReset"){
                    buffer.playing=false;
                    if(name=="waveInReset"){
                        while(!input->queue.empty())input->complete(memory,unit,wave_callbacks_,arg(0));
                        buffer.captured_frames=0;buffer.cursor=0;buffer.capture_fraction=0;input->read_frames=0;
                    }else if(input->started && !input->queue.empty())input->complete(memory,unit,wave_callbacks_,arg(0));
                    input->started=false;
                }else if(name=="waveInGetPosition"){
                    if(!arg(1) || d(2)<12)result=11;
                    else {memory.check(GuestAddress{arg(1)},12,Access::Write);auto type=memory.load(GuestAddress{arg(1)},32);if(type!=1 && type!=2 && type!=4)type=4;
                        const auto frames=buffer.captured_frames;memory.store(GuestAddress{arg(1)},32,type);
                        memory.store(GuestAddress{arg(1)+4},32,type==1?frames*1000/buffer.rate:type==2?frames:frames*buffer.frame_bytes());}
                }else {
                    const auto header=arg(1);
                    if(!header || d(2)<header_size)result=11;
                    else {
                        memory.check(GuestAddress{header},header_size,Access::Read);memory.check(GuestAddress{header+flags_offset},4,Access::Write);memory.check(GuestAddress{header+unit+4},4,Access::Write);
                        const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{header+flags_offset},32));const auto prepared=input->headers.find(header);
                        if(name=="waveInUnprepareHeader"){
                            if(prepared!=input->headers.end()){if(prepared->second.queued)result=33;else {input->headers.erase(prepared);memory.store(GuestAddress{header+flags_offset},32,flags&~2U);}}
                        }else if(name=="waveInPrepareHeader"){
                            if(prepared==input->headers.end()){
                                const auto data=memory.load(GuestAddress{header},bits);
                                const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit},32));
                                if(!data || !capacity || capacity>output_limit || (flags&~1U))result=11;
                                else if(input->headers.size()>=1024)result=7;
                                else {memory.check(GuestAddress{data},capacity,Access::Write);input->headers.emplace(header,WinmmWaveInput::Header{data,capacity,0,0,false});memory.store(GuestAddress{header+flags_offset},32,(flags&~1U)|2U);}
                            }
                        }else if(name=="waveInAddBuffer"){
                            if(prepared==input->headers.end() || !(flags&2U))result=34;
                            else if(prepared->second.queued)result=33;
                            else if(buffer.capture_failed)result=3;
                            else if(wave_callbacks_.size()>=4096)result=7;
                            else {
                                const auto data=memory.load(GuestAddress{header},bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header+unit},32));
                                if(data!=prepared->second.data || !size || size>prepared->second.capacity || size%buffer.frame_bytes())result=11;
                                else {memory.check(GuestAddress{data},size,Access::Write);input->queue.push_back(header);auto& h=prepared->second;h.size=size;h.used=0;h.queued=true;
                                    memory.store(GuestAddress{header+unit+4},32,0);memory.store(GuestAddress{header+flags_offset},32,(flags&~1U)|0x10U);}
                            }
                        }else throw HostCallFault("未知的 WaveIn 服務");
                    }
                }
            }
        }
    }catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}
    arg.return_to(state,result);return std::nullopt;
}
}
