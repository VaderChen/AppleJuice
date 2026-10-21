#include "x64arm/windows_services.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/windows_keyboard.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
void put(std::span<std::uint8_t> bytes,unsigned at,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
std::vector<std::uint8_t> packet_bytes(const RawInputPacket& packet,bool wide){
    const unsigned header=wide?24:16;std::vector<std::uint8_t> data(header+packet.payload.size());put(data,0,packet.type);put(data,4,data.size());put(data,8,packet.device,wide?8:4);put(data,wide?16:12,packet.code,wide?8:4);std::copy(packet.payload.begin(),packet.payload.end(),data.begin()+header);return data;
}
}
std::span<const WindowsServiceDescriptor> raw_input_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","RegisterRawInputDevices",3},{"user32.dll","GetRegisteredRawInputDevices",3},
        {"user32.dll","GetRawInputDeviceList",3},{"user32.dll","GetRawInputDeviceInfoA",4},
        {"user32.dll","GetRawInputDeviceInfoW",4},{"user32.dll","GetRawInputData",5},
        {"user32.dll","GetRawInputBuffer",3},{"user32.dll","DefRawInputProc",3}};return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_raw_input(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4:8,header=x86_?16:24;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto d=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;result=name=="RegisterRawInputDevices"?0U:name=="DefRawInputProc"?UINT64_MAX:UINT32_MAX;};
    const auto count_at=[&](std::uint64_t address){if(!address || address%4)throw std::invalid_argument("Raw Input 計數指標");memory.check(GuestAddress{address},4,Access::Write);return static_cast<std::uint32_t>(memory.load(GuestAddress{address},32));};
    try{
        if(name=="DefRawInputProc"){if(d(2)!=header)fail(87);}
        else {
            prepare_input_backend();if(!input_backend())throw HostCallFault("Raw Input 不允許使用比較模式的外部裝置");
            if(!raw_input_)raw_input_=std::make_shared<RawInputState>();poll_dinput();auto& bridge=*raw_input_;
            if(name=="RegisterRawInputDevices"){
                const unsigned stride=x86_?12:16;
                if(d(2)!=stride || !arg(0) || arg(0)%4 || !d(1) || d(1)>4096)fail(87);
                else {
                    memory.check(GuestAddress{arg(0)},std::size_t{d(1)}*stride,Access::Read);auto registrations=bridge.registrations;bool valid=true;
                    for(unsigned i=0;i<d(1);++i){const auto p=arg(0)+std::uint64_t{i}*stride;RawInputRegistration item;
                        item.page=static_cast<std::uint16_t>(memory.load(GuestAddress{p},16));item.usage=static_cast<std::uint16_t>(memory.load(GuestAddress{p+2},16));item.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));item.window=memory.load(GuestAddress{p+8},bits);
                        const bool keyboard=item.page==1 && item.usage==6,mouse=item.page==1 && item.usage==2,nolegacy=(item.flags&0x30U)==0x30U;
                        if(!item.page || (item.flags&~0x3731U) || ((item.flags&1U) && item.window) || ((item.flags&0x100U) && !item.window) || ((item.flags&0x1000U) && !item.window) || ((item.flags&0x1100U)==0x1100U)
                            || (nolegacy && !keyboard && !mouse) || ((item.flags&0x20U) && !nolegacy && item.usage) || (!item.usage && !(item.flags&0x21U))
                            || ((item.flags&0x200U) && (!keyboard && (!mouse || !nolegacy))) || ((item.flags&0x400U) && (!keyboard || !nolegacy))){fail(87);valid=false;break;}
                        if(item.window && !gui_.windows.contains(item.window)){fail(1400);valid=false;break;}
                        const auto key=std::make_pair(item.page,item.usage);if(item.flags&1U)registrations.erase(key);else registrations[key]=item;
                    }
                    if(valid){bridge.registrations=std::move(registrations);
                        if(native_gui_){const auto mouse=bridge.registrations.find({1,2});native_gui_->raw_input_policy(!raw_legacy_enabled(true),!raw_legacy_enabled(false),mouse!=bridge.registrations.end() && (mouse->second.flags&0x200U));}result=1;}
                }
            }else if(name=="GetRawInputDeviceList" || name=="GetRegisteredRawInputDevices"){
                const bool registered=name=="GetRegisteredRawInputDevices";const unsigned stride=registered?(x86_?12U:16U):unit*2;
                if(d(2)!=stride || (arg(0) && arg(0)%4))fail(87);
                else {
                    const auto capacity=count_at(arg(1));unsigned required=0;
                    if(registered)required=static_cast<unsigned>(bridge.registrations.size());else for(const auto& [handle,device]:bridge.devices){(void)handle;if(device.device.attached)++required;}
                    if(!arg(0)){memory.store(GuestAddress{arg(1)},32,required);if(registered)fail(122);else result=0;}
                    else if(capacity<required){memory.store(GuestAddress{arg(1)},32,required);fail(122);}
                    else {std::vector<std::uint8_t> output(std::size_t{required}*stride);unsigned i=0;
                        if(registered)for(const auto& [key,item]:bridge.registrations){(void)key;const auto p=i++*stride;put(output,p,item.page,2);put(output,p+2,item.usage,2);put(output,p+4,item.flags);put(output,p+8,item.window,unit);}
                        else for(const auto& [handle,item]:bridge.devices)if(item.device.attached){const auto p=i++*stride;put(output,p,handle,unit);put(output,p+unit,item.type);}
                        memory.write(GuestAddress{arg(0)},output);result=required;
                    }
                }
            }else if(name.starts_with("GetRawInputDeviceInfo")){
                const auto capacity=count_at(arg(3));const auto device=bridge.devices.find(arg(0));const auto command=d(1);const bool wide=name.ends_with('W');
                if(device==bridge.devices.end() || !device->second.device.attached)fail(6);
                else if(arg(2) && arg(2)%4)fail(87);
                else {
                    const auto& entry=device->second;std::vector<std::uint8_t> output;unsigned required{};
                    if(command==0x20000007U){output=wide?utf16_bytes(entry.name):std::vector<std::uint8_t>{};if(!wide){const auto text=utf16_to_utf8(entry.name);output.assign(text.begin(),text.end());output.push_back(0);}required=static_cast<unsigned>(output.size()/(wide?2U:1U));}
                    else if(command==0x20000005U){output=entry.preparsed;required=static_cast<unsigned>(output.size());if(output.empty())fail(50);}
                    else if(command==0x2000000bU){output.resize(32);required=32;put(output,0,32);put(output,4,entry.type);
                        if(entry.type==0){put(output,12,static_cast<unsigned>(std::count_if(entry.device.objects.begin(),entry.device.objects.end(),[](const auto& object){return object.kind==input::ObjectKind::Button;})));put(output,20,1);}
                        else if(entry.type==1){const auto& caps=windows_keyboard_capabilities;put(output,8,caps.type);put(output,12,caps.subtype);put(output,16,1);put(output,20,caps.function_keys);put(output,24,3);unsigned keys=0;for(unsigned i=1;i<256;++i){const auto scan=i==0xc5?0xe11dU:(i&127U)|((i&128U)?0xe000U:0);if(windows_map_virtual_key(scan,3))++keys;}put(output,28,keys);}
                        else {put(output,8,entry.device.vendor);put(output,12,entry.device.product_id);put(output,16,entry.device.version);put(output,20,entry.device.usage_page,2);put(output,22,entry.device.usage,2);}
                        if(arg(2) && capacity>=32 && memory.load(GuestAddress{arg(2)},32)!=32)fail(87);
                    }else fail(87);
                    if(result!=UINT32_MAX){if(!arg(2)){memory.store(GuestAddress{arg(3)},32,required);result=0;}
                        else if(capacity<required){memory.store(GuestAddress{arg(3)},32,required);fail(122);}
                        else {memory.write(GuestAddress{arg(2)},output);result=required;}}
                }
            }else if(name=="GetRawInputData"){
                if(d(4)!=header || (d(1)!=0x10000003U && d(1)!=0x10000005U) || (arg(2) && arg(2)%4))fail(87);
                else {const auto capacity=count_at(arg(3));const auto packet=bridge.packets.find(arg(0));
                    if(packet==bridge.packets.end() || packet->second.thread!=threads_.current)fail(6);
                    else {auto output=packet_bytes(packet->second,!x86_);if(d(1)==0x10000005U)output.resize(header);const auto required=static_cast<unsigned>(output.size());
                        if(!arg(2)){memory.store(GuestAddress{arg(3)},32,required);result=0;}
                        else if(capacity<required){memory.store(GuestAddress{arg(3)},32,required);fail(122);}
                        else {memory.write(GuestAddress{arg(2)},output);result=required;}}
                }
            }else if(name=="GetRawInputBuffer"){
                if(d(2)!=header || (arg(0) && arg(0)%(x86_?8U:4U)))fail(87);
                else {
                    const auto capacity=count_at(arg(1));std::vector<std::uint64_t> packets;std::vector<std::uint8_t> output;unsigned first=0;
                    for(const auto& message:gui_.queue){if(message.message!=0xff || message.thread!=threads_.current)continue;const auto packet=bridge.packets.find(message.lparam);if(packet==bridge.packets.end())continue;
                        // WOW64 buffered RAWINPUT 是 64-bit 結構；GetRawInputData 仍是客體結構。
                        const auto data=packet_bytes(packet->second,true);if(!first)first=static_cast<unsigned>(data.size());if(!arg(0))break;
                        const auto offset=(output.size()+7U)&~std::size_t{7};if(offset+data.size()>capacity)break;output.resize(offset);output.insert(output.end(),data.begin(),data.end());packets.push_back(message.lparam);
                    }
                    if(!arg(0)){memory.store(GuestAddress{arg(1)},32,first);result=0;}
                    else if(first && packets.empty()){memory.store(GuestAddress{arg(1)},32,first);fail(122);}
                    else if(packets.empty()){memory.store(GuestAddress{arg(1)},32,0);result=0;}
                    else {
                        memory.write(GuestAddress{arg(0)},output);
                        for(const auto handle:packets){const auto message=std::find_if(gui_.queue.begin(),gui_.queue.end(),[&](const auto& item){return item.message==0xff && item.lparam==handle;});if(message!=gui_.queue.end()){
                                auto& thread=threads_.items.at(threads_.current);thread.message_time=message->queued_time;thread.message_position=static_cast<std::uint16_t>(message->queued_cursor[0])|(std::uint32_t{static_cast<std::uint16_t>(message->queued_cursor[1])}<<16);if(message->input_extra)thread.message_extra=*message->input_extra;
                                gui_.queue.erase(message);}bridge.packets.erase(handle);}
                        result=packets.size();if(output.size()==capacity)fail(122);
                    }
                }
            }
        }
    }catch(const MemoryFault&){fail(998);}catch(const std::invalid_argument&){fail(87);}catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
}
