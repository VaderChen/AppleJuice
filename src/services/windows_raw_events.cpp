#include "x64arm/windows_services.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
void put(std::span<std::uint8_t> data,unsigned at,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)data[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
const RawInputRegistration* registration(const RawInputState& state,std::uint16_t page,std::uint16_t usage){
    if(const auto found=state.registrations.find({page,usage});found!=state.registrations.end()){
        if((found->second.flags&0x30U)==0x10U)return nullptr;return &found->second;}
    if(const auto found=state.registrations.find({page,0});found!=state.registrations.end() && (found->second.flags&0x20U))return &found->second;
    return nullptr;
}
unsigned usb_key(unsigned scan){
    constexpr std::array<unsigned,89> codes{0,41,30,31,32,33,34,35,36,37,38,39,45,46,42,43,
        20,26,8,21,23,28,24,12,18,19,47,48,40,224,4,22,7,9,10,11,13,14,15,51,52,53,225,49,
        29,27,6,25,5,17,16,54,55,56,229,85,226,44,57,58,59,60,61,62,63,64,65,66,67,83,71,
        95,96,97,86,92,93,94,87,89,90,91,98,99,0,0,100,68,69};
    if(scan<codes.size())return codes[scan];
    switch(scan){case 0x9c:return 88;case 0x9d:return 228;case 0xb5:return 84;case 0xb7:return 70;case 0xb8:return 230;case 0xc5:return 72;case 0xc7:return 74;case 0xc8:return 82;case 0xc9:return 75;case 0xcb:return 80;case 0xcd:return 79;case 0xcf:return 77;case 0xd0:return 81;case 0xd1:return 78;case 0xd2:return 73;case 0xd3:return 76;case 0xdb:return 227;case 0xdc:return 231;case 0xdd:return 101;default:return 0;}
}
}
void WindowsServices::refresh_raw_input(const std::vector<input::Device>& devices){
    if(!raw_input_)return;auto& state=*raw_input_;std::vector<std::pair<std::uint64_t,bool>> changes;
    for(auto& [handle,item]:state.devices){const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.instance==item.device.instance && d.attached;});
        if(item.device.attached && found==devices.end()){item.device.attached=false;changes.push_back({handle,false});}}
    for(const auto& device:devices){
        if(!device.attached)continue;const auto id=state.identities.find(device.instance);
        if(id!=state.identities.end()){auto& current=state.devices.at(id->second);if(!current.device.attached)changes.push_back({id->second,true});current.device=device;continue;}
        if(state.devices.size()>=4096)continue;RawInputDevice entry;entry.device=device;entry.type=device.kind==input::Kind::Mouse?0U:device.kind==input::Kind::Keyboard?1U:2U;
        std::string name="\\\\?\\HID#MACWIN_";constexpr char hex[]="0123456789ABCDEF";for(auto byte:device.instance){name+=hex[byte>>4];name+=hex[byte&15U];}entry.name=utf8_to_utf16(name);
        if(!device.hid_descriptor.empty())try{entry.preparsed=hid::preparsed(device.hid_descriptor,{device.usage_page,device.usage});}catch(const hid::Error&){}
        const auto handle=runtime_.allocate_wait_handle();state.identities.emplace(device.instance,handle);state.devices.emplace(handle,std::move(entry));changes.push_back({handle,true});
    }
    for(auto& [handle,file]:files_){(void)handle;if(!file.hid)continue;
        const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.instance==file.hid->device.instance && d.attached;});if(found==devices.end())file.hid->disconnected=true;}
    for(const auto [handle,attached]:changes){const auto& device=state.devices.at(handle).device;const auto reg=registration(state,device.usage_page,device.usage);if(!reg || !(reg->flags&0x2000U))continue;
        const auto window=reg->window?reg->window:gui_.focus;if(!window || !gui_.windows.contains(window) || gui_.queue.size()>=10000)continue;
        GuiMessage message{window,attached?1U:2U,handle,0xfe};message.queue_priority=1;enqueue_gui_message(std::move(message));}
}
void WindowsServices::queue_raw_input(std::uint64_t device,std::uint32_t type,std::vector<std::uint8_t> payload,std::uint32_t time,std::uint64_t extra){
    if(!raw_input_ || gui_.queue.size()>=10000 || raw_input_->packets.size()>=10000)return;
    std::uint16_t page=1,usage=type==0?2U:6U;
    if(device){const auto found=raw_input_->devices.find(device);if(found==raw_input_->devices.end() || !found->second.device.attached)return;page=found->second.device.usage_page;usage=found->second.device.usage;}
    const auto reg=registration(*raw_input_,page,usage);if(!reg)return;
    const auto target=reg->window?reg->window:gui_.focus;const auto window=gui_.windows.find(target);
    if(window==gui_.windows.end() || window->second.desktop_object!=gui_.input_desktop)return;
    const bool foreground=(!native_gui_ || native_gui_->input_active()) && gui_.active && gui_.windows.contains(gui_.active) && gui_.windows.at(gui_.active).desktop_object==gui_.input_desktop;
    if(!foreground && !(reg->flags&(0x100U|0x1000U)))return;
    const auto handle=runtime_.allocate_wait_handle();raw_input_->packets.emplace(handle,RawInputPacket{device,target,window->second.thread,type,foreground?0U:1U,std::move(payload)});
    GuiMessage message{target,foreground?0U:1U,handle,0xff};message.thread=window->second.thread;message.queue_priority=1;message.injected=true;message.queued_time=time;message.input_extra=extra;enqueue_gui_message(std::move(message));
}
bool WindowsServices::raw_legacy_enabled(bool keyboard)const{
    if(!raw_input_)return true;const auto reg=registration(*raw_input_,1,keyboard?6U:2U);return !reg || (reg->flags&0x30U)!=0x30U;
}
bool WindowsServices::raw_hotkeys_enabled()const{
    if(!raw_input_)return true;const auto reg=registration(*raw_input_,1,6);return !reg || !(reg->flags&0x200U);
}
void WindowsServices::raw_input_keyboard(const GuiMessage& message,std::uint32_t time,std::uint64_t extra,std::uint64_t device){
    if(!raw_input_)return;const auto vk=static_cast<unsigned>(message.wparam);const bool up=message.message==0x101 || message.message==0x105;
    std::vector<std::uint8_t> data(16);const auto scan=vk==0xe7?0U:static_cast<unsigned>((message.lparam>>16)&255U);
    const auto full=vk==0x13?0xe11dU:scan|((message.lparam&0x1000000U)?0xe000U:0);
    put(data,0,full&127U,2);put(data,2,(up?1U:0U)|((full&0xff00U)==0xe000U?2U:(full&0xff00U)==0xe100U?4U:0U),2);
    put(data,6,vk>=0xa0 && vk<=0xa5?(vk<0xa2?16U:vk<0xa4?17U:18U):vk,2);put(data,8,message.message);put(data,12,extra);
    queue_raw_input(device,1,std::move(data),time,extra);
    if(!up && vk>=0xa6 && vk<=0xb7){const auto reg=registration(*raw_input_,1,6);if(reg && (reg->flags&0x400U)){
        const auto window=reg->window?reg->window:gui_.focus;if(gui_.windows.contains(window))enqueue_gui_message({window,window,static_cast<std::uint64_t>(vk-0xa6+1)<<16,0x319});}}
}
void WindowsServices::raw_input_mouse(std::uint32_t flags,std::int32_t x,std::int32_t y,std::uint16_t buttons,std::uint16_t wheel,std::uint32_t time,std::uint64_t extra,std::uint64_t device){
    if(!raw_input_)return;std::vector<std::uint8_t> data(24);put(data,0,flags,2);put(data,4,buttons,2);put(data,6,(buttons&0xc00U)?wheel:0,2);
    const auto raw_buttons=(gui_.input_keyboard[1]&0x80U?1U:0U)|(gui_.input_keyboard[2]&0x80U?2U:0U)|(gui_.input_keyboard[4]&0x80U?4U:0U)|(gui_.input_keyboard[5]&0x80U?8U:0U)|(gui_.input_keyboard[6]&0x80U?16U:0U)|(raw_input_->buttons&0xe0U);put(data,8,raw_buttons);
    put(data,12,std::bit_cast<std::uint32_t>(x));put(data,16,std::bit_cast<std::uint32_t>(y));put(data,20,extra);queue_raw_input(device,0,std::move(data),time,extra);
}
void WindowsServices::raw_input_event(const input::Event& event){
    if(!raw_input_)return;const auto id=raw_input_->identities.find(event.device);if(id==raw_input_->identities.end())return;const auto& d=raw_input_->devices.at(id->second).device;
    const auto time=static_cast<std::uint32_t>(runtime_.clock_ticks/10000);
    if(d.kind==input::Kind::Keyboard && event.object<256){
        const auto scan=event.object==0xc5?0xe11dU:(event.object&127U)|((event.object&128U)?0xe000U:0);auto vk=windows_map_virtual_key(scan,3);if(!vk)vk=255;
        const bool sys=((gui_.input_keyboard[18]&0x80U) || vk==0xa4 || vk==0xa5 || vk==0x79) && !(gui_.input_keyboard[17]&0x80U);
        GuiMessage message{0,vk,1|(std::uint64_t{scan&255U}<<16)|((scan&0xff00U)==0xe000U?0x1000000U:0),event.value?(sys?0x104U:0x100U):(sys?0x105U:0x101U)};
        raw_input_keyboard(message,time,event.extra,id->second);raw_input_->keyboard[event.object]=event.value?1:0;
        std::vector<std::uint8_t> bytes(8);unsigned count=0;
        for(unsigned i=0;i<256;++i)if(raw_input_->keyboard[i]){const auto usage=usb_key(i);if(usage>=224 && usage<=231)bytes[0]|=static_cast<std::uint8_t>(1U<<(usage-224));else if(usage && usage<=101){if(count<6)bytes[2+count]=static_cast<std::uint8_t>(usage);++count;}}
        if(count>6)std::fill(bytes.begin()+2,bytes.end(),1);raw_input_report({event.device,0,std::move(bytes)});
    }else if(d.kind==input::Kind::Mouse){
        std::int32_t x=0,y=0,wheel=0;std::uint16_t buttons=0;
        if(event.object==0)x=static_cast<std::int32_t>(event.value);else if(event.object==1)y=static_cast<std::int32_t>(event.value);
        else if(event.object==2 || event.object==11){wheel=static_cast<std::int32_t>(event.value);buttons=event.object==2?0x400U:0x800U;}
        else if(event.object>=3 && event.object<11){const auto index=event.object-3;if(event.value)raw_input_->buttons|=static_cast<std::uint8_t>(1U<<index);else raw_input_->buttons&=static_cast<std::uint8_t>(~(1U<<index));if(index<5)buttons=static_cast<std::uint16_t>(1U<<(index*2+(event.value?0U:1U)));}
        else return;
        raw_input_mouse(0,x,y,buttons,static_cast<std::uint16_t>(wheel),time,event.extra,id->second);
        std::vector<std::uint8_t> bytes(13);bytes[0]=raw_input_->buttons;put(bytes,1,std::bit_cast<std::uint32_t>(x));put(bytes,5,std::bit_cast<std::uint32_t>(y));put(bytes,9,std::bit_cast<std::uint32_t>(event.object==2?wheel:0));raw_input_report({event.device,0,std::move(bytes)});
    }
}
void WindowsServices::raw_input_report(const input::Report& report){
    if(!raw_input_ || report.id>255)return;const auto id=raw_input_->identities.find(report.device);if(id==raw_input_->identities.end())return;const auto& entry=raw_input_->devices.at(id->second);
    if(entry.preparsed.empty() || !entry.device.attached)return;
    const auto layout=hid::decode(entry.preparsed);std::vector<std::uint8_t> data;
    if(layout.numbered && (report.bytes.empty() || report.bytes.front()!=report.id))return;
    if(!layout.numbered)data.push_back(0);data.insert(data.end(),report.bytes.begin(),report.bytes.end());
    if(data.size()>layout.bytes(0) || !layout.report_bytes[0][data.front()])return;data.resize(layout.bytes(0));
    for(auto& [handle,file]:files_){(void)handle;if(file.hid && !file.hid->disconnected && file.hid->device.instance==report.device){if(file.hid->reports.size()>=32)file.hid->reports.pop_front();file.hid->reports.push_back(data);}}
    if(entry.type==2){std::vector<std::uint8_t> payload(8+data.size());put(payload,0,data.size());put(payload,4,1);std::copy(data.begin(),data.end(),payload.begin()+8);queue_raw_input(id->second,2,std::move(payload),static_cast<std::uint32_t>(runtime_.clock_ticks/10000),0);}
}
void WindowsServices::release_raw_input(std::uint64_t packet,std::uint64_t window,std::uint64_t thread){
    release_pointer_input(window,thread);
    if(!raw_input_)return;std::set<std::uint64_t> removed;
    std::erase_if(raw_input_->packets,[&](const auto& item){const bool remove=(packet && item.first==packet && item.second.thread==threads_.current) || (window && item.second.window==window) || (thread && item.second.thread==thread);if(remove)removed.insert(item.first);return remove;});
    if(window)std::erase_if(raw_input_->registrations,[&](const auto& item){return item.second.window==window;});
    if(window && native_gui_){const auto mouse=raw_input_->registrations.find({1,2});native_gui_->raw_input_policy(!raw_legacy_enabled(true),!raw_legacy_enabled(false),mouse!=raw_input_->registrations.end() && (mouse->second.flags&0x200U));}
    std::erase_if(gui_.queue,[&](const auto& message){return message.message==0xff && removed.contains(message.lparam);});
}
}
