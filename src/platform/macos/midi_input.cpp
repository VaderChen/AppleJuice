#include "x64arm/midi.hpp"
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <deque>
#include <stdexcept>

namespace x64arm::midi {
namespace {
void check(OSStatus status){if(status!=noErr)throw std::runtime_error("CoreMIDI input unavailable: "+std::to_string(status));}
class Port final:public Input {
    MIDIClientRef client_{};MIDIPortRef port_{};MIDIEndpointRef endpoint_{};
    std::mutex mutex_;std::deque<InputPacket> packets_;std::size_t bytes_{};
    bool active_{},overflow_{};std::uint64_t started_{};mach_timebase_info_data_t timebase_{};
    static void receive(const MIDIPacketList* list,void* context,void*) noexcept {
        auto& self=*static_cast<Port*>(context);std::lock_guard lock(self.mutex_);
        if(!self.active_)return;
        try {
            const auto* packet=&list->packet[0];
            for(UInt32 i=0;i<list->numPackets;++i,packet=MIDIPacketNext(packet)){
                if(!packet->length)continue;
                if(self.overflow_ || self.packets_.size()>=4096 || self.bytes_+packet->length>1048576){self.overflow_=true;break;}
                const auto stamp=packet->timeStamp?packet->timeStamp:mach_absolute_time();
                const auto ticks=stamp>self.started_?stamp-self.started_:0;
                const auto ms=static_cast<std::uint64_t>((static_cast<__uint128_t>(ticks)*self.timebase_.numer)/self.timebase_.denom/1000000);
                self.packets_.push_back({static_cast<std::uint32_t>(ms),{packet->data,packet->data+packet->length},false});self.bytes_+=packet->length;
            }
        }catch(...){self.overflow_=true;}
    }
    void close(){if(port_){MIDIPortDisconnectSource(port_,endpoint_);MIDIPortDispose(port_);port_=0;}if(client_){MIDIClientDispose(client_);client_=0;}}
public:
    explicit Port(std::uint32_t endpoint):endpoint_(endpoint){
        mach_timebase_info(&timebase_);
        try {
            check(MIDIClientCreate(CFSTR("Win64Sandbox MIDI input"),nullptr,nullptr,&client_));
            // WinMM 使用 MIDI 1.0 byte stream；CoreMIDI 在這個入口執行來源協定轉換。
            check(MIDIInputPortCreate(client_,CFSTR("WinMM input"),receive,this,&port_));
            check(MIDIPortConnectSource(port_,endpoint_,nullptr));
        }catch(...){close();throw;}
    }
    ~Port()override{stop();close();}
    void start()override{std::lock_guard lock(mutex_);if(!active_){packets_.clear();bytes_=0;overflow_=false;started_=mach_absolute_time();active_=true;}}
    void stop()override{std::lock_guard lock(mutex_);active_=false;}
    std::vector<InputPacket> drain()override{
        std::lock_guard lock(mutex_);std::vector<InputPacket> result;
        std::size_t remaining=1024;
        while(!packets_.empty() && remaining){
            auto& packet=packets_.front();const auto count=std::min(remaining,packet.bytes.size());
            if(count==packet.bytes.size()){result.push_back(std::move(packet));packets_.pop_front();}
            else {result.push_back({packet.milliseconds,{packet.bytes.begin(),packet.bytes.begin()+static_cast<std::ptrdiff_t>(count)},false});packet.bytes.erase(packet.bytes.begin(),packet.bytes.begin()+static_cast<std::ptrdiff_t>(count));}
            bytes_-=count;remaining-=count;
        }
        if(packets_.empty() && overflow_){result.push_back({0,{},true});overflow_=false;}
        return result;
    }
};
}
std::vector<InputDevice> input_devices(){
    // 建立 client 才能向 MIDI server 取得目前來源；不虛構輸入裝置。
    MIDIClientRef client{};if(MIDIClientCreate(CFSTR("Win64Sandbox MIDI discovery"),nullptr,nullptr,&client)!=noErr)return {};
    std::vector<InputDevice> result;
    try {
        for(ItemCount i=0;i<MIDIGetNumberOfSources();++i){
            const auto endpoint=MIDIGetSource(i);if(!endpoint)continue;
            SInt32 offline=0;MIDIObjectGetIntegerProperty(endpoint,kMIDIPropertyOffline,&offline);if(offline)continue;
            CFStringRef name=nullptr;MIDIObjectGetStringProperty(endpoint,kMIDIPropertyDisplayName,&name);
            std::string text="MIDI input";
            if(name){const auto length=CFStringGetMaximumSizeForEncoding(CFStringGetLength(name),kCFStringEncodingUTF8)+1;std::vector<char> buffer(static_cast<std::size_t>(length));if(CFStringGetCString(name,buffer.data(),length,kCFStringEncodingUTF8))text=buffer.data();CFRelease(name);}
            result.push_back({endpoint,std::move(text)});
        }
    }catch(...){MIDIClientDispose(client);throw;}
    MIDIClientDispose(client);return result;
}
std::unique_ptr<Input> open_input(std::uint32_t endpoint){return std::make_unique<Port>(endpoint);}
}
