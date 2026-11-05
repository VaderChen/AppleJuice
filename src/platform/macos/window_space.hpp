#pragma once
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>

namespace x64arm {
// 宿主視窗政策由所有客體程序共用；環境變數可覆寫或以空字串停用。
// UUID 每次由當前拓樸解析，不能保存會在登入後變動的 Space 數字 ID。
inline NSString* native_window_space_uuid() {
    static NSString* selected = []() -> NSString* {
        if (const char* value = std::getenv("X64ARM_GUI_SPACE_UUID"))
            return *value ? [NSString stringWithUTF8String:value] : nil;
        id value = [NSUserDefaults.standardUserDefaults persistentDomainForName:@"org.macwin.x64arm"][@"AppleJuiceWindowSpaceUUID"];
        return [value isKindOfClass:NSString.class] && [value length] ? value : nil;
    }();
    return selected;
}

struct NativeWindowSpaces {
    using Main = int(*)();
    using Displays = CFArrayRef(*)(int);
    using Spaces = CFArrayRef(*)(int, int, CFArrayRef);
    using Move = void(*)(int, CFArrayRef, std::uint64_t);
    void* library{};
    int connection{};
    Displays displays{};
    Spaces spaces{};
    Move move{};
    NativeWindowSpaces() {
        library = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
        if (!library) throw std::runtime_error("無法載入指定桌面所需的視窗介面");
        const auto main = reinterpret_cast<Main>(dlsym(library, "SLSMainConnectionID"));
        displays = reinterpret_cast<Displays>(dlsym(library, "SLSCopyManagedDisplaySpaces"));
        spaces = reinterpret_cast<Spaces>(dlsym(library, "SLSCopySpacesForWindows"));
        move = reinterpret_cast<Move>(dlsym(library, "SLSMoveWindowsToManagedSpace"));
        if (!main || !displays || !spaces || !move)
            throw std::runtime_error("目前 macOS 無法將客體視窗放到指定桌面");
        connection = main();
    }
    struct Target { std::uint64_t id{}; NSScreen* screen{}; bool active{}; };
    Target target() const {
        NSString* uuid = native_window_space_uuid();
        NSArray* topology = CFBridgingRelease(displays(connection));
        for (NSDictionary* display in topology) {
            for (NSDictionary* space in display[@"Spaces"]) {
                if (![space[@"uuid"] isEqual:uuid] || [space[@"type"] intValue] != 0) continue;
                const auto id = [space[@"id64"] unsignedLongLongValue];
                NSScreen* selected = nil;
                for (NSScreen* screen in NSScreen.screens) {
                    CFUUIDRef displayUUID = CGDisplayCreateUUIDFromDisplayID([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
                    NSString* name = displayUUID ? CFBridgingRelease(CFUUIDCreateString(nullptr, displayUUID)) : nil;
                    if (displayUUID) CFRelease(displayUUID);
                    if ([name isEqual:display[@"Display Identifier"]]) { selected = screen; break; }
                }
                if (!selected || !id) throw std::runtime_error("指定桌面的螢幕已離線，已停止建立視窗");
                return {id, selected, [display[@"Current Space"][@"id64"] unsignedLongLongValue] == id};
            }
        }
        throw std::runtime_error("指定的 macOS 桌面已不存在，已停止建立視窗");
    }
};
inline NativeWindowSpaces& native_window_spaces() { static NativeWindowSpaces api; return api; }
inline NSScreen* native_window_space_screen() {
    // 座標換算是繪圖熱路徑，不能逐次查詢 WindowServer 的全部桌面拓樸。
    static NSScreen* selected = native_window_space_uuid() ? native_window_spaces().target().screen : nil;
    return selected;
}
inline bool native_window_space_active() {
    return !native_window_space_uuid() || native_window_spaces().target().active;
}
inline void native_place_window_in_space(NSWindow* window) {
    if (!window || !native_window_space_uuid()) return;
    auto& api = native_window_spaces();
    const auto target = api.target();
    NSArray* windows = @[@(window.windowNumber)];
    const auto membership = [&]() -> NSArray* {
        return CFBridgingRelease(api.spaces(api.connection, 7, (__bridge CFArrayRef)windows));
    };
    if ([membership() isEqual:@[@(target.id)]]) return;
    // 未 order 的 NSWindow 已有 WindowServer 物件，可在第一張畫面出現前完成歸屬。
    api.move(api.connection, (__bridge CFArrayRef)windows, target.id);
    if (![membership() isEqual:@[@(target.id)]]) {
        [window orderOut:nil];
        throw std::runtime_error("macOS 未接受指定桌面的視窗歸屬，已保持視窗隱藏");
    }
}
}
