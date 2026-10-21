const auto xinput_result=[&]() -> std::uint32_t {
    // DirectInput／RawInput 沿用相同 backend 與事件輪詢；XInput 僅使用明確的 gamepad profile。
    const auto index=static_cast<std::uint32_t>(arg(0));
    const auto output=arg(name=="XInputGetCapabilities"?2:1);
    if(index>=bridge.gamepad_slots.size() || !output ||
       (name=="XInputGetCapabilities" && arg(1)>1))return 87U;
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
        word(0,pad.buttons&pad.supported_buttons&0xf3ffU);
        for(unsigned i=0;i<2;++i)bytes[2+i]=pad.supported_triggers[i]?static_cast<std::uint8_t>(std::lround(normalized(pad.triggers[i],0)*255)):0;
        for(unsigned i=0;i<4;++i){const auto value=pad.supported_sticks[i]?normalized(pad.sticks[i],-1):0;word(4+i*2,static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value*(value<0?32768:32767)))));}
        return bytes;
    };
    // 在任一 XInput 查詢時更新全部 slot，確保能力查詢不會漏掉已觀察到的狀態變化。
    for(auto& slot:bridge.gamepad_slots)if(slot.device){
        const auto pad=std::find_if(pads.begin(),pads.end(),[&](const auto& item){return item.device==*slot.device;});
        const auto bytes=encode(*pad);if(!slot.sampled || bytes!=slot.state){++slot.packet;slot.state=bytes;slot.sampled=true;}
    }
    auto& slot=bridge.gamepad_slots[index];if(!slot.device)return 1167U;
    const auto pad=std::find_if(pads.begin(),pads.end(),[&](const auto& item){return item.device==*slot.device;});
    if(name=="XInputSetState"){
        memory.check(GuestAddress{output},4,Access::Read);
        return bridge.backend->rumble(*slot.device,static_cast<std::uint16_t>(memory.load(GuestAddress{output},16)),static_cast<std::uint16_t>(memory.load(GuestAddress{output+2},16)));
    }
    if(name=="XInputGetState"){
        memory.check(GuestAddress{output},16,Access::Write);
        memory.store(GuestAddress{output},32,slot.packet);memory.write(GuestAddress{output+4},slot.state);return 0U;
    }
    if(name=="XInputGetCapabilities"){
        memory.check(GuestAddress{output},20,Access::Write);
        memory.write(GuestAddress{output},std::array<std::uint8_t,20>{});
        memory.store(GuestAddress{output},8,1);memory.store(GuestAddress{output+1},8,1);
        memory.store(GuestAddress{output+2},16,(pad->rumble[0] || pad->rumble[1])?1:0);
        memory.store(GuestAddress{output+4},16,pad->supported_buttons&0xf3ffU);
        for(unsigned i=0;i<2;++i)memory.store(GuestAddress{output+6+i},8,pad->supported_triggers[i]?255:0);
        for(unsigned i=0;i<4;++i)memory.store(GuestAddress{output+8+i*2},16,pad->supported_sticks[i]?65535:0);
        for(unsigned i=0;i<2;++i)memory.store(GuestAddress{output+16+i*2},16,pad->rumble[i]?65535:0);
        return 0U;
    }
    return 87U;
};
arg.return_to(state,xinput_result());return std::nullopt;
