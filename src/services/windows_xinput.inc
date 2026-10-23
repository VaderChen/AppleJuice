const auto xinput_result=[&]() -> std::uint32_t {
    // DirectInput／RawInput 沿用相同 backend 與事件輪詢；XInput 僅使用明確的 gamepad profile。
    // 入口家族：GetState／SetState／GetCapabilities／Enable／GetBatteryInformation／GetKeystroke／
    // GetAudioDeviceIds（1_4）／GetDSoundAudioDeviceGuids（1_3）／#100 GetStateEx／#108 GetCapabilitiesEx（1_4）。
    const bool capabilities_ex=name=="#108";
    const auto index=static_cast<std::uint32_t>(arg(capabilities_ex?1:0));
    const unsigned output_index=name=="XInputGetCapabilities"?2:name=="XInputGetBatteryInformation" || name=="XInputGetKeystroke" || name=="XInputGetDSoundAudioDeviceGuids"?2:capabilities_ex?3:1;
    const auto output=name=="XInputEnable" || name=="XInputGetAudioDeviceIds"?0:arg(output_index);
    if(name=="XInputEnable"){bridge.xinput_enabled=arg(0)!=0;return 0U;}
    const bool any_index=name=="XInputGetKeystroke" && index==0xffU;
    if((index>=bridge.gamepad_slots.size() && !any_index) || (!output && name!="XInputGetAudioDeviceIds") ||
       (name=="XInputGetCapabilities" && arg(1)>1) || (capabilities_ex && (arg(0)!=1 || arg(2)>1)) ||
       (name=="XInputGetBatteryInformation" && arg(1)>1))return 87U;
    poll_dinput();
    const auto pads=bridge.backend->gamepads();
    for(auto& slot:bridge.gamepad_slots)if(slot.device && std::none_of(pads.begin(),pads.end(),[&](const auto& pad){return pad.device==*slot.device;}))slot={};
    for(const auto& pad:pads){
        if(std::any_of(bridge.gamepad_slots.begin(),bridge.gamepad_slots.end(),[&](const auto& slot){return slot.device==pad.device;}))continue;
        const auto free=std::find_if(bridge.gamepad_slots.begin(),bridge.gamepad_slots.end(),[](const auto& slot){return !slot.device;});
        if(free==bridge.gamepad_slots.end())break;
        free->device=pad.device;
    }
    const auto normalized=[](float value,float minimum){return std::isfinite(value)?std::clamp(value,minimum,1.0F):0.0F;};
    const auto encode=[&](const input::Gamepad& pad){
        std::array<std::uint8_t,12> bytes{};
        const auto word=[&](unsigned at,std::uint16_t value){bytes[at]=static_cast<std::uint8_t>(value);bytes[at+1]=static_cast<std::uint8_t>(value>>8);};
        // 公開 GetState 遮罩掉 Guide（0x400）；#100 GetStateEx 於輸出時另行補回。
        word(0,pad.buttons&pad.supported_buttons&0xf7ffU);
        for(unsigned i=0;i<2;++i)bytes[2+i]=pad.supported_triggers[i]?static_cast<std::uint8_t>(std::lround(normalized(pad.triggers[i],0)*255)):0;
        for(unsigned i=0;i<4;++i){const auto value=pad.supported_sticks[i]?normalized(pad.sticks[i],-1):0;word(4+i*2,static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value*(value<0?32768:32767)))));}
        return bytes;
    };
    // 在任一 XInput 查詢時更新全部 slot，確保能力查詢不會漏掉已觀察到的狀態變化。
    for(auto& slot:bridge.gamepad_slots)if(slot.device){
        const auto pad=std::find_if(pads.begin(),pads.end(),[&](const auto& item){return item.device==*slot.device;});
        const auto bytes=encode(*pad);if(!slot.sampled || bytes!=slot.state){++slot.packet;slot.state=bytes;slot.sampled=true;}
    }
    const auto state_buttons=[](const auto& slot){return static_cast<std::uint16_t>(slot.state[0]|(slot.state[1]<<8));};
    if(name=="XInputGetKeystroke"){
        // 以按鍵邊緣產生 VK_PAD 鍵擊：每次回報一個變化，沒有變化回 ERROR_EMPTY。
        static constexpr std::array<std::pair<std::uint16_t,std::uint16_t>,15> keys{{{0x0001,0x5810},{0x0002,0x5811},{0x0004,0x5812},{0x0008,0x5813},{0x0010,0x5814},{0x0020,0x5815},{0x0040,0x5816},{0x0080,0x5817},{0x0100,0x5805},{0x0200,0x5806},{0x1000,0x5800},{0x2000,0x5801},{0x4000,0x5802},{0x8000,0x5803},{0x0400,0}}};
        memory.check(GuestAddress{output},8,Access::Write);
        for(std::uint32_t i=0;i<bridge.gamepad_slots.size();++i){
            if(!any_index && i!=index)continue;auto& slot=bridge.gamepad_slots[i];if(!slot.device){if(!any_index)return 1167U;continue;}
            const auto current=bridge.xinput_enabled?state_buttons(slot):std::uint16_t{};
            for(auto diff=static_cast<std::uint16_t>(current^slot.keystroke_buttons);diff;diff=static_cast<std::uint16_t>(current^slot.keystroke_buttons)){
                const auto bit=static_cast<std::uint16_t>(diff&static_cast<unsigned>(-static_cast<int>(diff)));slot.keystroke_buttons=static_cast<std::uint16_t>(slot.keystroke_buttons^bit);
                const auto key=std::find_if(keys.begin(),keys.end(),[&](const auto& item){return item.first==bit;});
                if(key==keys.end() || !key->second)continue;
                memory.write(GuestAddress{output},std::array<std::uint8_t,8>{});
                memory.store(GuestAddress{output},16,key->second);memory.store(GuestAddress{output+4},16,(current&bit)?1U:2U);memory.store(GuestAddress{output+6},8,i);
                return 0U;
            }
        }
        return 4306U;
    }
    auto& slot=bridge.gamepad_slots[index];if(!slot.device)return 1167U;
    const auto pad=std::find_if(pads.begin(),pads.end(),[&](const auto& item){return item.device==*slot.device;});
    if(name=="XInputSetState"){
        memory.check(GuestAddress{output},4,Access::Read);
        if(!bridge.xinput_enabled)return 0U;
        return bridge.backend->rumble(*slot.device,static_cast<std::uint16_t>(memory.load(GuestAddress{output},16)),static_cast<std::uint16_t>(memory.load(GuestAddress{output+2},16)));
    }
    if(name=="XInputGetState" || name=="#100"){
        memory.check(GuestAddress{output},16,Access::Write);
        memory.store(GuestAddress{output},32,slot.packet);
        if(!bridge.xinput_enabled){memory.write(GuestAddress{output+4},std::array<std::uint8_t,12>{});return 0U;}
        auto bytes=slot.state;if(name!="#100")bytes[1]&=static_cast<std::uint8_t>(~0x04U);
        memory.write(GuestAddress{output+4},bytes);return 0U;
    }
    if(name=="XInputGetCapabilities" || capabilities_ex){
        const auto size=capabilities_ex?32U:20U;
        memory.check(GuestAddress{output},size,Access::Write);
        memory.write(GuestAddress{output},std::vector<std::uint8_t>(size));
        memory.store(GuestAddress{output},8,1);memory.store(GuestAddress{output+1},8,1);
        memory.store(GuestAddress{output+2},16,(pad->rumble[0] || pad->rumble[1])?1:0);
        memory.store(GuestAddress{output+4},16,pad->supported_buttons&(capabilities_ex?0xf7ffU:0xf3ffU));
        for(unsigned i=0;i<2;++i)memory.store(GuestAddress{output+6+i},8,pad->supported_triggers[i]?255:0);
        for(unsigned i=0;i<4;++i)memory.store(GuestAddress{output+8+i*2},16,pad->supported_sticks[i]?65535:0);
        for(unsigned i=0;i<2;++i)memory.store(GuestAddress{output+16+i*2},16,pad->rumble[i]?65535:0);
        // XINPUT_CAPABILITIES_EX：VendorId／ProductId／VersionNumber 以後端未提供時為 0。
        return 0U;
    }
    if(name=="XInputGetBatteryInformation"){
        // 後端未回報電量：gamepad 視為有線（BATTERY_TYPE_WIRED、BATTERY_LEVEL_FULL），headset 視為未連接。
        memory.check(GuestAddress{output},2,Access::Write);
        const bool headset=arg(1)==1;memory.store(GuestAddress{output},8,headset?0U:1U);memory.store(GuestAddress{output+1},8,headset?0U:3U);return 0U;
    }
    if(name=="XInputGetAudioDeviceIds"){
        // 沒有 headset 音訊裝置：兩個 ID 為空字串、長度 0。
        for(unsigned i=0;i<2;++i){const auto buffer=arg(1+i*2),count=arg(2+i*2);
            if(count){memory.check(GuestAddress{count},4,Access::Write);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{count},32));
                if(buffer && capacity){memory.check(GuestAddress{buffer},2,Access::Write);memory.store(GuestAddress{buffer},16,0);}
                memory.store(GuestAddress{count},32,0);}
            else if(buffer)return 87U;}
        return 0U;
    }
    if(name=="XInputGetDSoundAudioDeviceGuids"){
        memory.check(GuestAddress{output},16,Access::Write);memory.write(GuestAddress{output},std::array<std::uint8_t,16>{});
        if(arg(2)){memory.check(GuestAddress{arg(2)},16,Access::Write);memory.write(GuestAddress{arg(2)},std::array<std::uint8_t,16>{});}
        return 0U;
    }
    return 87U;
};
arg.return_to(state,xinput_result());return std::nullopt;
