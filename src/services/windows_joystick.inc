// WinMM 查詢與 DirectInput／Raw Input 共用同一份 HID 取樣；不另外消耗後端事件。
std::uint32_t result=0;
try {
    if(!bridge.backend)result=name=="joyGetNumDevs"?0U:6U;
    else if(name=="joyGetNumDevs")result=static_cast<std::uint32_t>(bridge.legacy_joystick_slots.size());
    else {
        poll_dinput();const auto devices=bridge.backend->devices();
        for(const auto& device:devices){
            if(device.kind!=input::Kind::Joystick && device.kind!=input::Kind::Gamepad)continue;
            if(std::find(bridge.legacy_joystick_slots.begin(),bridge.legacy_joystick_slots.end(),device.instance)!=bridge.legacy_joystick_slots.end())continue;
            const auto free=std::find(bridge.legacy_joystick_slots.begin(),bridge.legacy_joystick_slots.end(),std::nullopt);
            if(free!=bridge.legacy_joystick_slots.end())*free=device.instance;
        }
        const bool caps=name.starts_with("joyGetDevCaps"),wide=name.ends_with('W'),extended=name=="joyGetPosEx";
        const auto id=caps?arg(0):static_cast<std::uint32_t>(arg(0)),pointer=arg(1);
        const bool driver=caps && (id==(x86_?UINT32_MAX:UINT64_MAX));
        const input::Device* device=nullptr;
        if(id<bridge.legacy_joystick_slots.size() && bridge.legacy_joystick_slots[static_cast<std::size_t>(id)]){
            const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& item){return item.instance==*bridge.legacy_joystick_slots[static_cast<std::size_t>(id)];});
            if(found!=devices.end() && found->attached && bridge.backend->available(found->instance))device=&*found;
        }
        const auto base_size=wide?728U:404U,capacity=static_cast<std::uint32_t>(caps?arg(2):0);
        if(!pointer)result=11;
        else if(caps && capacity!=base_size && capacity!=base_size+48)result=165;
        else if(extended && memory.load(GuestAddress{pointer},32)<52)result=165;
        else if(!driver && id>=bridge.legacy_joystick_slots.size())result=caps?6U:2U;
        else if(!driver && !device)result=caps?6U:167U;
        else {
            std::array<std::optional<std::size_t>,6> axes;std::vector<std::size_t> buttons;std::optional<std::size_t> pov;
            if(device){
                // 依 HID Usage 對應 X、Y、Z／Wheel、Rz、Slider、Rx，已選物件不重複。
                constexpr std::array<std::array<std::uint16_t,5>,6> usage{{{{0x30,0x34,0,0,0}},{{0x31,0x33,0,0,0}},{{0x32,0x38,0x36,0x37,0}},{{0x35,0x36,0x37,0x34,0x33}},{{0x36,0x37,0x34,0x33,0}},{{0x33,0,0,0,0}}}};
                const auto used=[&](std::size_t object){return std::any_of(axes.begin(),axes.end(),[&](const auto& selected){return selected && *selected==object;});};
                for(unsigned axis=0;axis<axes.size();++axis){
                    for(const auto wanted:usage[axis]){if(!wanted)break;
                        for(std::size_t i=0;i<device->objects.size();++i){const auto& object=device->objects[i];
                            if(object.kind==input::ObjectKind::Axis && !object.relative && object.usage_page==1 && object.usage==wanted && !used(i)){axes[axis]=i;break;}}
                        if(axes[axis])break;
                    }
                    if(!axes[axis])for(std::size_t i=0;i<device->objects.size();++i){const auto& object=device->objects[i];
                        if(object.kind==input::ObjectKind::Axis && !object.relative && !object.usage_page && object.axis==axis && !used(i)){axes[axis]=i;break;}}
                }
                for(std::size_t i=0;i<device->objects.size();++i){const auto& object=device->objects[i];
                    if(object.kind==input::ObjectKind::Pov && !pov)pov=i;
                    else if(object.kind==input::ObjectKind::Button && buttons.size()<32)buttons.push_back(i);
                }
                std::stable_sort(buttons.begin(),buttons.end(),[&](auto a,auto b){return device->objects[a].instance<device->objects[b].instance;});
            }
            if(caps){
                memory.check(GuestAddress{pointer},capacity,Access::Write);std::vector<std::uint8_t> bytes(capacity);
                const auto store=[&](unsigned at,std::uint32_t value,unsigned length=4){for(unsigned i=0;i<length;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
                const auto string=[&](unsigned at,unsigned count,std::string_view text){
                    if(wide){const auto value=utf8_to_utf16_lossy(text);auto length=std::min<std::size_t>(count-1,value.size());if(length && value[length-1]>=0xd800 && value[length-1]<=0xdbff)--length;
                        for(std::size_t i=0;i<length;++i)store(at+static_cast<unsigned>(i)*2,value[i],2);}
                    else {auto length=std::min<std::size_t>(count-1,text.size());while(length<text.size() && length && (static_cast<unsigned char>(text[length])&0xc0)==0x80)--length;
                        std::copy_n(text.begin(),length,bytes.data()+at);}
                };
                const auto at=wide?68U:36U;string(at+76,32,"MacWin Joystick");
                if(device){
                    store(0,device->vendor,2);store(2,device->product_id,2);string(4,32,device->name);
                    for(unsigned axis=0;axis<6;++axis){const auto offset=axis<3?axis*8:36+(axis-3)*8;store(at+offset+4,axes[axis]?65535U:0U);}
                    store(at+24,static_cast<std::uint32_t>(buttons.size()));store(at+28,10);store(at+32,1000);
                    std::uint32_t features=pov?0x50U:0U;for(unsigned axis=2;axis<6;++axis)if(axes[axis])features|=1U<<(axis-2);
                    store(at+60,features);store(at+64,6);store(at+68,static_cast<std::uint32_t>(std::count_if(axes.begin(),axes.end(),[](const auto& a){return a.has_value();})));store(at+72,32);
                    if(capacity==base_size+48){std::copy(device->product.begin(),device->product.end(),bytes.data()+base_size+16);std::copy(device->instance.begin(),device->instance.end(),bytes.data()+base_size+32);}
                }
                memory.write(GuestAddress{pointer},bytes);
            }else {
                const auto flags=extended?static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+4},32)):0x87U;
                if(flags&~0x0fff0fffU)result=165;
                else {
                    memory.check(GuestAddress{pointer},extended?52:16,Access::Write);
                    std::uint32_t raw_axes=0;for(unsigned axis=0;axis<6;++axis){const auto flag=axis<2?0x100000U<<axis:0x1000000U<<(axis-2);if(flags&flag)raw_axes|=1U<<axis;}
                    for(unsigned count=2;count<=6;++count){const auto flag=count<=4?0x20000U<<(count-2):0x400000U<<(count-5);if(flags&flag)raw_axes|=(1U<<count)-1;}
                    if(flags&0x10000U)raw_axes|=0x3fU;
                    const auto wanted_axes=(flags&0x3fU)|raw_axes;
                    std::array<std::uint32_t,11> values{};
                    const auto current=[&](std::size_t i,std::int64_t fallback=0){return i<device->values.size()?device->values[i]:fallback;};
                    for(unsigned axis=0;axis<6;++axis)if((wanted_axes&(1U<<axis)) && axes[axis]){
                        const auto object=*axes[axis];const auto& description=device->objects[object];const auto value=current(object);
                        if((flags&0x100U) || (raw_axes&(1U<<axis)))values[axis]=static_cast<std::uint32_t>(value);
                        else if(description.maximum>description.minimum){const auto span=static_cast<std::uint64_t>(static_cast<std::int64_t>(description.maximum)-description.minimum);
                            const auto position=static_cast<std::uint64_t>(std::clamp<std::int64_t>(value,description.minimum,description.maximum)-description.minimum);
                            values[axis]=static_cast<std::uint32_t>((position*65535U+span/2)/span);
                            // HID 沒有額外校正表時，以描述符的邏輯範圍與中心校正。
                            if((flags&0x800U) && values[axis]>=31129 && values[axis]<=34406)values[axis]=32768;
                        }
                    }
                    if(flags&0x80U){for(unsigned i=0;i<buttons.size();++i)if(current(buttons[i]))values[6]|=1U<<i;values[7]=static_cast<std::uint32_t>(std::popcount(values[6]));}
                    if(flags&0x240U){const auto value=pov?current(*pov,-1):-1;values[8]=value<0 || value>=36000?65535U:static_cast<std::uint32_t>(value);
                        if(values[8]!=65535 && !(flags&0x200U))values[8]=((values[8]+4500)/9000%4)*9000;}
                    if(extended)for(unsigned i=0;i<values.size();++i)memory.store(GuestAddress{pointer+8+i*4},32,values[i]);
                    else {for(unsigned i=0;i<3;++i)memory.store(GuestAddress{pointer+i*4},32,values[i]);memory.store(GuestAddress{pointer+12},32,values[6]&0xfU);}
                }
            }
        }
    }
}catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}catch(const std::invalid_argument&){result=11;}
arg.return_to(state,result);return std::nullopt;
