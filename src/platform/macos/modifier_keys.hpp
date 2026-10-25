#pragma once
#import <AppKit/AppKit.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <cstdint>

namespace x64arm {
struct NativeModifierKey {
    unsigned code;
    std::uint16_t key,scan;
    NSEventModifierFlags aggregate,device,family;
};
inline const NativeModifierKey* native_modifier_key(unsigned code) {
    static constexpr NativeModifierKey keys[]{
        {56,16,0x2a,NSEventModifierFlagShift,NX_DEVICELSHIFTKEYMASK,NX_DEVICELSHIFTKEYMASK|NX_DEVICERSHIFTKEYMASK},
        {60,16,0x36,NSEventModifierFlagShift,NX_DEVICERSHIFTKEYMASK,NX_DEVICELSHIFTKEYMASK|NX_DEVICERSHIFTKEYMASK},
        {59,17,0x1d,NSEventModifierFlagControl,NX_DEVICELCTLKEYMASK,NX_DEVICELCTLKEYMASK|NX_DEVICERCTLKEYMASK},
        {62,17,0x11d,NSEventModifierFlagControl,NX_DEVICERCTLKEYMASK,NX_DEVICELCTLKEYMASK|NX_DEVICERCTLKEYMASK},
        {58,18,0x38,NSEventModifierFlagOption,NX_DEVICELALTKEYMASK,NX_DEVICELALTKEYMASK|NX_DEVICERALTKEYMASK},
        {61,18,0x138,NSEventModifierFlagOption,NX_DEVICERALTKEYMASK,NX_DEVICELALTKEYMASK|NX_DEVICERALTKEYMASK}};
    for(const auto& key:keys)if(key.code==code)return &key;
    return nullptr;
}
inline bool native_modifier_pressed(const NativeModifierKey& key,NSEventModifierFlags flags,bool previous) {
    if(!(flags&key.aggregate))return false;
    // 真實事件使用左右裝置位元；遠端僅附 aggregate 時，以發生變化的 keyCode 配對。
    if(flags&key.family)return (flags&key.device)!=0;
    return !previous;
}
}
