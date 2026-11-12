#pragma once
#include "x64arm/native_application.hpp"
#include "x64arm/startup_profile.hpp"
#include "window_space.hpp"
#import <AppKit/AppKit.h>
#include <stdexcept>
#include <cstdlib>
#include <string_view>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace x64arm {
// X64ARM_PRESENT_UPSCALE=2：整個客體畫面以 2 倍顯示。客體看到的是實體螢幕一半大小的虛擬顯示器（除非另以
// X64ARM_DISPLAY_SIZE 指定），頂層視窗的原生大小與位置是客體座標的 2 倍，內容視圖的座標系維持客體單位，
// 所以滑鼠與子視窗座標不必另外換算；3D 畫面由 MetalFX 空間放大輸出到 2 倍的顯示表面，其餘內容由系統放大。
inline unsigned native_present_upscale(){
    static const unsigned value=[]{const char* text=std::getenv("X64ARM_PRESENT_UPSCALE");return text && std::string_view(text)=="2"?2U:1U;}();
    return value;
}
// 所有 AppKit presenter 共用同一背景螢幕，子程序仍沿用 Launcher 的固定 ID。
inline bool native_background_display_enabled(){
    if(const char* value=std::getenv("X64ARM_GUI_BACKGROUND_DISPLAY"))return std::string_view(value)=="1";
    return native_window_space_uuid()!=nil;
}
inline NSScreen* native_background_display(){
    // 指定桌面的螢幕與背景啟動角色獨立：正常 Launcher 也明確選定主螢幕。
    if(native_window_space_uuid())return native_window_space_screen();
    if(!native_background_display_enabled())return nil;
    static NSScreen* selected=[]()->NSScreen*{
        NSArray<NSScreen*>* screens=NSScreen.screens;NSScreen* active=NSScreen.mainScreen;
        // 同一次 Launcher 執行固定使用它已選定的螢幕，所有子程序沿用。
        // 父子啟動時間不同，重新判斷前景視窗會把 WebHelper 放回使用者正在操作的螢幕。
        if(const char* requested=std::getenv("X64ARM_GUI_BACKGROUND_DISPLAY_ID")){
            std::uint32_t display{};const auto end=requested+std::strlen(requested);const auto parsed=std::from_chars(requested,end,display);
            if(parsed.ec!=std::errc{} || parsed.ptr!=end || !display)throw std::runtime_error("背景螢幕 ID 無效，已停止 GUI 啟動");
            for(NSScreen* screen in screens)if([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]==display){
                std::fprintf(stderr,"gui_background_display=%u inherited=1 no_activate=1\n",display);return screen;
            }
            throw std::runtime_error("指定的背景螢幕已離線，已停止 GUI 啟動");
        }
        const auto pid=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        NSArray* windows=CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly|kCGWindowListExcludeDesktopElements,kCGNullWindowID));
        for(NSDictionary* window in windows){
            if([window[(NSString*)kCGWindowOwnerPID] intValue]!=pid || [window[(NSString*)kCGWindowLayer] intValue]!=0)continue;
            CGRect bounds{};if(!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)window[(NSString*)kCGWindowBounds],&bounds))continue;
            CGFloat largest=0;for(NSScreen* screen in screens){const auto display=[screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];const auto overlap=CGRectIntersection(bounds,CGDisplayBounds(display));const auto area=CGRectIsNull(overlap)?0:overlap.size.width*overlap.size.height;if(area>largest){largest=area;active=screen;}}
            if(largest>0)break;
        }
        NSScreen* result=nil;CGFloat largest=0;for(NSScreen* screen in screens){if(screen==active)continue;const auto frame=screen.visibleFrame;const auto area=frame.size.width*frame.size.height;if(area>largest){largest=area;result=screen;}}
        if(!result)throw std::runtime_error("找不到非焦點螢幕，已停止背景 GUI 啟動");
        std::fprintf(stderr,"gui_background_display=%u active_display=%u no_activate=1\n",[result.deviceDescription[@"NSScreenNumber"] unsignedIntValue],[active.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);return result;
    }();return selected;
}

inline void prepare_native_application() {
    if(!NSThread.isMainThread)throw std::runtime_error("AppKit 應用程式必須在主執行緒初始化");
    // 客體視窗由客體程式自行建立，引擎不使用 AppKit 視窗還原。引擎曾異常結束後，AppKit 會在
    // 第一次取事件時以 NSAlert runModal 詢問是否重開視窗；背景角色看不到該對話框，排程器會被
    // 模態迴圈無限期卡住。須在 NSApplication 處理開啟事件前登錄。
    {startup_profile::Scope measurement("gui_init","register_defaults");
    [NSUserDefaults.standardUserDefaults registerDefaults:@{@"ApplePersistenceIgnoreState":@YES,@"NSQuitAlwaysKeepsWindows":@NO}];}
    {startup_profile::Scope measurement("gui_init","shared_application");[NSApplication sharedApplication];}
    startup_profile::Scope policy_measurement("gui_init","activation_policy");
    const auto policy=native_application_role()==NativeApplicationRole::Child || native_background_display_enabled()
        ?NSApplicationActivationPolicyAccessory:NSApplicationActivationPolicyRegular;
    // 重複設定既有角色可能回傳 NO；以設定後的角色判斷，避免第二個 presenter 誤判失敗。
    if(![NSApp setActivationPolicy:policy] && NSApp.activationPolicy!=policy)
        throw std::runtime_error("無法設定原生應用程式角色");
}
}
