// WinMM 查詢與 DirectInput／Raw Input 共用同一份 HID 取樣；不另外消耗後端事件。
std::uint32_t result=0;
try {
    if(name=="joySetThreshold" || name=="joyGetThreshold" || name=="joySetCapture" || name=="joyReleaseCapture" || name=="joyConfigChanged"){
        const auto id=static_cast<std::uint32_t>(arg(0));
        if(name=="joyConfigChanged"){
            // 重新讀取設定：已拔除裝置的槽位釋放，下次查詢依目前裝置清單重新指派。
            if(arg(0))result=11;
            else if(bridge.backend){poll_dinput();const auto devices=bridge.backend->devices();
                for(auto& slot:bridge.legacy_joystick_slots)if(slot && std::none_of(devices.begin(),devices.end(),[&](const auto& d){return d.instance==*slot && d.attached;}))slot.reset();}
        }else if(!bridge.backend)result=6;
        else if(name=="joySetThreshold" || name=="joyGetThreshold"){
            if(id>=gui_.joystick_thresholds.size())result=165;
            else if(name=="joySetThreshold")gui_.joystick_thresholds[id]=static_cast<std::uint32_t>(arg(1));
            else if(!arg(1))result=11;
            else memory.store(GuestAddress{arg(1)},32,gui_.joystick_thresholds[id]);
        }else if(name=="joyReleaseCapture"){
            if(id>=gui_.joystick_captures.size())result=165;else gui_.joystick_captures[id].reset();
        }else{
            const auto window=arg(0);const auto joystick_id=static_cast<std::uint32_t>(arg(1));const auto period=static_cast<std::uint32_t>(arg(2));
            const auto target=gui_.windows.find(window);
            if(!window || target==gui_.windows.end() || target->second.destroying || joystick_id>=gui_.joystick_captures.size())result=165;
            else if(auto& slot=gui_.joystick_captures[joystick_id];slot && slot->window!=window && gui_.windows.contains(slot->window))result=166;
            else{
                LegacyJoystick joystick;legacy_joystick(joystick_id,joystick);
                if(!joystick.device)result=167;
                else{
                    // 期限依 joyGetDevCaps 的 wPeriodMin／wPeriodMax（10～1000 ms）夾住；以目前位置為門檻比較的起點。
                    const auto values=legacy_joystick_values(joystick,0x87);
                    GuiJoystickCapture capture;capture.window=window;capture.interval=std::uint64_t{std::clamp(period,10U,1000U)}*10000;
                    capture.deadline=runtime_.clock_ticks+capture.interval;capture.changed=arg(3)!=0;
                    capture.x=values[0];capture.y=values[1];capture.z=values[2];capture.buttons=values[6]&0xfU;slot=capture;
                }
            }
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(!bridge.backend)result=name=="joyGetNumDevs"?0U:6U;
    else if(name=="joyGetNumDevs")result=static_cast<std::uint32_t>(bridge.legacy_joystick_slots.size());
    else {
        const bool caps=name.starts_with("joyGetDevCaps"),wide=name.ends_with('W'),extended=name=="joyGetPosEx";
        const auto id=caps?arg(0):static_cast<std::uint32_t>(arg(0)),pointer=arg(1);
        const bool driver=caps && (id==(x86_?UINT32_MAX:UINT64_MAX));
        LegacyJoystick joystick;legacy_joystick(driver?UINT64_MAX:id,joystick);const auto* device=joystick.device;
        const auto& axes=joystick.axes;const auto& buttons=joystick.buttons;const auto& pov=joystick.pov;
        const auto base_size=wide?728U:404U,capacity=static_cast<std::uint32_t>(caps?arg(2):0);
        if(!pointer)result=11;
        else if(caps && capacity!=base_size && capacity!=base_size+48)result=165;
        else if(extended && memory.load(GuestAddress{pointer},32)<52)result=165;
        else if(!driver && id>=bridge.legacy_joystick_slots.size())result=caps?6U:2U;
        else if(!driver && !device)result=caps?6U:167U;
        else {
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
                    const auto values=legacy_joystick_values(joystick,flags);
                    if(extended)for(unsigned i=0;i<values.size();++i)memory.store(GuestAddress{pointer+8+i*4},32,values[i]);
                    else {for(unsigned i=0;i<3;++i)memory.store(GuestAddress{pointer+i*4},32,values[i]);memory.store(GuestAddress{pointer+12},32,values[6]&0xfU);}
                }
            }
        }
    }
}catch(const MemoryFault&){result=11;}catch(const std::bad_alloc&){result=7;}catch(const std::invalid_argument&){result=11;}
arg.return_to(state,result);return std::nullopt;
