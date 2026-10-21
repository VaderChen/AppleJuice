#include "x64arm/windows_services.hpp"
#include "x64arm/windows_pointer.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
// 與現有虛擬桌面及 96 DPI 相同；未知感測器物理單位映射至此座標系。
constexpr std::array<std::int32_t,4> display{0,0,1440,900};
constexpr std::array<std::int32_t,4> virtual_himetric{0,0,38100,23812};
void put(std::span<std::uint8_t> bytes,unsigned offset,std::uint64_t value,unsigned size=4){
    for(unsigned i=0;i<size;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));
}
std::uint32_t type(const input::PointerDevice& d){return d.type<=2?3U:d.type==3?2U:5U;}
std::int32_t mapped(std::int32_t value,std::int32_t lo,std::int32_t hi,std::int32_t dest_lo,std::int32_t dest_hi){
    const auto span=static_cast<std::int64_t>(hi)-lo,range=static_cast<std::int64_t>(dest_hi)-dest_lo;
    const auto relative=static_cast<std::int64_t>(std::clamp(value,lo,hi))-lo;
    return static_cast<std::int32_t>(dest_lo+static_cast<std::int64_t>(static_cast<std::uint64_t>(relative)*static_cast<std::uint64_t>(range)/static_cast<std::uint64_t>(span)));
}
std::vector<std::uint8_t> device_bytes(const PointerDeviceState& d,std::uint64_t handle,bool small){
    std::vector<std::uint8_t> bytes(small?1064:1080);const unsigned unit=small?4:8;
    put(bytes,0,1);put(bytes,small?4:8,handle,unit);put(bytes,small?8:16,d.device.type);
    put(bytes,small?12:24,1,unit);put(bytes,small?16:32,d.starting_id);put(bytes,small?20:36,d.device.max_contacts,2);
    auto text=utf8_to_utf16_lossy(d.device.name);if(text.size()>519){text.resize(519);if(text.back()>=0xd800 && text.back()<=0xdbff)text.pop_back();}
    for(unsigned i=0;i<text.size();++i)put(bytes,(small?22:38)+i*2,text[i],2);return bytes;
}
std::vector<std::uint8_t> info_bytes(const PointerPacket& packet,std::uint32_t pointer_type,bool small){
    std::vector<std::uint8_t> bytes(small?88:96);const unsigned unit=small?4:8,points=small?24:32;
    put(bytes,0,pointer_type);put(bytes,4,packet.id);put(bytes,8,packet.frame);put(bytes,12,packet.flags);
    put(bytes,16,packet.device,unit);put(bytes,16+unit,packet.window,unit);
    for(unsigned i=0;i<2;++i){put(bytes,points+i*4,std::bit_cast<std::uint32_t>(packet.pixel[i]));put(bytes,points+8+i*4,std::bit_cast<std::uint32_t>(packet.himetric[i]));
        put(bytes,points+16+i*4,std::bit_cast<std::uint32_t>(packet.pixel[i]));put(bytes,points+24+i*4,std::bit_cast<std::uint32_t>(packet.himetric[i]));}
    put(bytes,points+32,packet.time);put(bytes,points+36,1);put(bytes,points+44,packet.keys);put(bytes,points+48,packet.performance,8);put(bytes,points+56,packet.button_change);
    return bytes;
}
}
std::span<const WindowsServiceDescriptor> pointer_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","RegisterPointerDeviceNotifications",2,0,0,2},
        {"user32.dll","GetPointerDevices",2},{"user32.dll","GetPointerDevice",2},
        {"user32.dll","GetPointerDeviceRects",3},{"user32.dll","GetPointerType",2,0,0,1},
        {"user32.dll","GetPointerInfo",2,0,0,1},{"user32.dll","GetPointerPenInfo",2,0,0,1},
        {"user32.dll","GetPointerTouchInfo",2,0,0,1}};return table;
}
void WindowsServices::poll_pointer_input(){
    if(!pointer_input_ || !input_backend())return;auto& state=*pointer_input_;const auto backend=input_backend();
    const auto devices=backend->pointer_devices();
    const auto notify=[&](std::uint32_t message,std::uint64_t handle,std::uint64_t value){
        for(const auto& [window,ranges]:state.registrations){const auto found=gui_.windows.find(window);
            if(found==gui_.windows.end() || found->second.desktop_object!=gui_.input_desktop || (message!=0x238 && !ranges) || gui_.queue.size()>=10000)continue;
            GuiMessage event{window,value,handle,message};event.thread=found->second.thread;event.queue_priority=1;enqueue_gui_message(std::move(event));}
    };
    for(auto& [handle,entry]:state.devices)if(entry.attached && !entry.injected && std::none_of(devices.begin(),devices.end(),[&](const auto& d){return d.device==entry.device.device;})){
        entry.attached=false;if(entry.in_range)notify(0x23a,handle,0);entry.in_range=false;notify(0x238,handle,2);
        std::erase_if(state.current,[&](const auto& item){return item.second.device==handle;});
    }
    for(const auto& device:devices){
        if(device.himetric_rectangle && ((*device.himetric_rectangle)[0]>=(*device.himetric_rectangle)[2] || (*device.himetric_rectangle)[1]>=(*device.himetric_rectangle)[3]))continue;
        if(device.type<1 || device.type>4 || !device.max_contacts || device.max_contacts>256 || device.logical_rectangle[0]>=device.logical_rectangle[2] || device.logical_rectangle[1]>=device.logical_rectangle[3])continue;
        auto identity=state.identities.find(device.device);
        if(identity==state.identities.end()){
            if(state.devices.size()>=256 || state.next_id+device.max_contacts>2562)continue;
            const auto handle=runtime_.allocate_wait_handle();state.identities.emplace(device.device,handle);
            state.devices.emplace(handle,PointerDeviceState{device,true,false,state.next_id});state.next_id+=device.max_contacts;notify(0x238,handle,1);
        }else {auto& entry=state.devices.at(identity->second);const bool arrival=!entry.attached;entry.attached=true;entry.device=device;if(arrival)notify(0x238,identity->second,1);}
    }
    for(const auto& sample:backend->pointer_samples())deliver_pointer_sample(sample);
}
void WindowsServices::deliver_pointer_sample(const input::PointerSample& sample){
    // 原生指標裝置與 InjectTouchInput 共用：更新接觸狀態並送出 WM_POINTER* 訊息。
    auto& state=*pointer_input_;
    const auto notify=[&](std::uint32_t message,std::uint64_t handle,std::uint64_t value){
        for(const auto& [window,ranges]:state.registrations){const auto found=gui_.windows.find(window);
            if(found==gui_.windows.end() || found->second.desktop_object!=gui_.input_desktop || (message!=0x238 && !ranges) || gui_.queue.size()>=10000)continue;
            GuiMessage event{window,value,handle,message};event.thread=found->second.thread;event.queue_priority=1;enqueue_gui_message(std::move(event));}
    };
        const auto found=state.identities.find(sample.device);if(found==state.identities.end())return;
    auto& entry=state.devices.at(found->second);if(!entry.attached || sample.contact>=entry.device.max_contacts)return;
    const auto id=entry.starting_id+sample.contact;const auto old=state.current.find(id);
    const bool had_contact=old!=state.current.end() && old->second.sample.in_contact;
    const bool had_range=old!=state.current.end() && old->second.sample.in_range;
    PointerPacket packet;packet.sample=sample;packet.device=found->second;packet.id=id;packet.frame=++state.frame;
    const auto& rect=entry.device.logical_rectangle;const auto physical=entry.device.himetric_rectangle.value_or(virtual_himetric);
    for(unsigned i=0;i<2;++i){packet.pixel[i]=mapped(i?sample.y:sample.x,rect[i],rect[i+2],display[i],display[i+2]-1);
        packet.himetric[i]=mapped(i?sample.y:sample.x,rect[i],rect[i+2],physical[i],physical[i+2]);}
    packet.window=gui_.capture?gui_.capture:gui_window_from_point(gui_,packet.pixel[0],packet.pixel[1],gui_.input_desktop);
    if(old!=state.current.end() && had_contact)packet.window=old->second.window; // 接觸期間隱含擷取。
    const auto target=gui_.windows.find(packet.window);if(target!=gui_.windows.end())packet.thread=target->second.thread;
    packet.time=static_cast<std::uint32_t>(runtime_.clock_ticks/10000);packet.performance=runtime_.clock_ticks;
    packet.keys=(gui_.input_keyboard[16]&0x80U?4U:0U)|(gui_.input_keyboard[17]&0x80U?8U:0U);
    packet.flags=(sample.in_range?2U:0U)|(sample.in_contact?4U|0x10U:0U)|(sample.barrel?0x20U:0U)|0x4000U;
    const bool primary=old!=state.current.end()?(old->second.flags&0x2000U)!=0:std::none_of(state.current.begin(),state.current.end(),[](const auto& item){return item.second.sample.in_range;});
    if(primary)packet.flags|=0x2000U;
    packet.flags|=sample.in_contact&&!had_contact?0x10000U:!sample.in_contact&&had_contact?0x40000U:0x20000U;
    packet.button_change=sample.in_contact&&!had_contact?1U:!sample.in_contact&&had_contact?2U:0U;
    if(!had_range && sample.in_range)packet.flags|=1U;
    state.current[id]=packet;
    const bool range=std::any_of(state.current.begin(),state.current.end(),[&](const auto& item){return item.second.device==packet.device && item.second.sample.in_range;});
    if(range!=entry.in_range){entry.in_range=range;notify(range?0x239:0x23a,packet.device,0);}
    const auto queue=[&](std::uint32_t message){
        if(target==gui_.windows.end() || target->second.desktop_object!=gui_.input_desktop || gui_.queue.size()>=10000 || state.packets.size()>=10000)return;
        const auto token=state.next_packet++;state.packets.emplace(token,packet);
        GuiMessage event{packet.window,id|(std::uint64_t{packet.flags&0xffffU}<<16),static_cast<std::uint16_t>(packet.pixel[0])|(std::uint64_t{static_cast<std::uint16_t>(packet.pixel[1])}<<16),message};
        event.thread=packet.thread;event.queue_priority=1;event.injected=true;event.queued_time=packet.time;event.pointer_packet=token;enqueue_gui_message(std::move(event));
    };
    if(!had_range && sample.in_range)queue(0x249);
    if(sample.in_range || had_contact)queue(sample.in_contact&&!had_contact?0x246:!sample.in_contact&&had_contact?0x247:0x245);
    if(had_range && !sample.in_range)queue(0x24a);
    if(!sample.in_range && !sample.in_contact)state.current.erase(id);
}
void WindowsServices::remember_pointer_message(const GuiMessage& message){
    if(!pointer_input_)return;auto& state=*pointer_input_;const auto old=state.delivered.find(threads_.current);
    if(old!=state.delivered.end() && old->second!=message.pointer_packet){
        const auto token=old->second;
        if(std::none_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& item){return item.pointer_packet==token;}))state.packets.erase(token);
        state.delivered.erase(old);
    }
    if(message.pointer_packet && state.packets.contains(message.pointer_packet))state.delivered[threads_.current]=message.pointer_packet;
}
void WindowsServices::release_pointer_input(std::uint64_t window,std::uint64_t thread){
    if(!pointer_input_ || (!window && !thread))return;auto& state=*pointer_input_;
    if(window)state.registrations.erase(window);
    std::erase_if(state.registrations,[&](const auto& item){const auto found=gui_.windows.find(item.first);return found==gui_.windows.end() || (thread && found->second.thread==thread);});
    std::erase_if(state.packets,[&](const auto& item){return (window && item.second.window==window) || (thread && item.second.thread==thread);});
    std::erase_if(state.current,[&](const auto& item){return (window && item.second.window==window) || (thread && item.second.thread==thread);});
    std::erase_if(state.delivered,[&](const auto& item){return (thread && item.first==thread) || !state.packets.contains(item.second);});
    std::erase_if(gui_.queue,[&](const auto& message){return message.pointer_packet && !state.packets.contains(message.pointer_packet);});
}
std::optional<std::uint32_t> WindowsServices::invoke_pointer_input(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    try {
        if(!pointer_input_)pointer_input_=std::make_shared<PointerInputState>();auto& bridge=*pointer_input_;
        if(name=="RegisterPointerDeviceNotifications"){
            if(!gui_.windows.contains(arg(0)))fail(1400);
            else {prepare_input_backend();if(!input_backend())fail(50);else {poll_dinput();bridge.registrations[arg(0)]=arg(1)!=0;result=1;}}
        }else if(name=="GetPointerDevices" || name=="GetPointerDevice" || name=="GetPointerDeviceRects"){
            prepare_input_backend();if(!input_backend())fail(50);
            else {poll_dinput();
                if(name=="GetPointerDevices"){
                    if(!arg(0))fail(87);else {
                        memory.check(GuestAddress{arg(0)},4,Access::Write);const auto capacity=arg(1)?memory.load(GuestAddress{arg(0)},32):0;
                        const auto count=static_cast<std::uint32_t>(std::count_if(bridge.devices.begin(),bridge.devices.end(),[](const auto& item){return item.second.attached;}));
                        if(arg(1) && capacity<count){memory.store(GuestAddress{arg(0)},32,count);fail(122);}
                        else {std::vector<std::uint8_t> bytes;if(arg(1))for(const auto& [handle,device]:bridge.devices)if(device.attached){const auto value=device_bytes(device,handle,x86_);bytes.insert(bytes.end(),value.begin(),value.end());}
                            if(!bytes.empty())memory.write(GuestAddress{arg(1)},bytes);memory.store(GuestAddress{arg(0)},32,count);result=1;}
                    }
                }else {
                    const auto found=bridge.devices.find(arg(0));if(found==bridge.devices.end() || !found->second.attached)fail(6);
                    else if(!arg(1) || (name=="GetPointerDeviceRects" && !arg(2)))fail(87);
                    else if(name=="GetPointerDevice"){memory.write(GuestAddress{arg(1)},device_bytes(found->second,arg(0),x86_));result=1;}
                    else {memory.check(GuestAddress{arg(1)},16,Access::Write);memory.check(GuestAddress{arg(2)},16,Access::Write);
                        const auto physical=found->second.device.himetric_rectangle.value_or(virtual_himetric);
                        for(unsigned i=0;i<4;++i){memory.store(GuestAddress{arg(1)+i*4},32,std::bit_cast<std::uint32_t>(physical[i]));memory.store(GuestAddress{arg(2)+i*4},32,std::bit_cast<std::uint32_t>(display[i]));}result=1;}
                }
            }
        }else {
            const auto id=static_cast<std::uint32_t>(arg(0));const auto delivered=bridge.delivered.find(threads_.current);
            const auto packet=delivered==bridge.delivered.end()?bridge.packets.end():bridge.packets.find(delivered->second);
            if(!arg(1) || !id)fail(87);
            else if(name=="GetPointerType"){
                const auto active=bridge.current.find(id);const auto* info=active!=bridge.current.end()?&active->second:packet!=bridge.packets.end() && packet->second.id==id?&packet->second:nullptr;
                if(!info)fail(87);else {memory.store(GuestAddress{arg(1)},32,type(bridge.devices.at(info->device).device));result=1;}
            }else if(packet==bridge.packets.end() || packet->second.id!=id){
                const auto active=bridge.current.find(id);fail(active!=bridge.current.end() && active->second.thread!=threads_.current?5U:232U);
            }else if(packet->second.thread!=threads_.current)fail(5);
            else {const auto& value=packet->second;const auto pointer_type=type(bridge.devices.at(value.device).device);
                if((name=="GetPointerPenInfo" && pointer_type!=3) || (name=="GetPointerTouchInfo" && pointer_type!=2))fail(87);
                else {auto bytes=info_bytes(value,pointer_type,x86_);const auto offset=static_cast<unsigned>(bytes.size());const auto& sample=value.sample;
                    if(name=="GetPointerPenInfo"){bytes.resize(offset+24);put(bytes,offset,(sample.barrel?1U:0U)|(sample.inverted?2U:0U)|(sample.eraser?4U:0U));put(bytes,offset+4,sample.mask&15U);
                        put(bytes,offset+8,sample.pressure);put(bytes,offset+12,sample.rotation);put(bytes,offset+16,std::bit_cast<std::uint32_t>(sample.tilt_x));put(bytes,offset+20,std::bit_cast<std::uint32_t>(sample.tilt_y));}
                    else if(name=="GetPointerTouchInfo"){bytes.resize(offset+48);put(bytes,offset+4,sample.mask&7U);const auto& rect=bridge.devices.at(value.device).device.logical_rectangle;
                        for(unsigned i=0;i<4;++i){const auto axis=i&1U;const auto contact=mapped(sample.contact_rectangle[i],rect[axis],rect[axis+2],display[axis],display[axis+2]-1);put(bytes,offset+8+i*4,std::bit_cast<std::uint32_t>(contact));put(bytes,offset+24+i*4,std::bit_cast<std::uint32_t>(contact));}
                        put(bytes,offset+40,sample.rotation);put(bytes,offset+44,sample.pressure);}
                    memory.write(GuestAddress{arg(1)},bytes);result=1;
                }
            }
        }
    }catch(const MemoryFault&){fail(998);}catch(const std::invalid_argument&){fail(87);}catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return {};
}
}
