#include "x64arm/native_desktop_notifications.hpp"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <deque>
#include <set>

namespace x64arm { NSImage* native_gui_icon_image(const GuiIcon&); }
namespace {
NSString* text(const std::u16string& value){return [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(value.data()) length:value.size()];}
std::array<std::int32_t,2> point(){const auto p=NSEvent.mouseLocation;return {static_cast<std::int32_t>(p.x),static_cast<std::int32_t>(NSMaxY(NSScreen.screens.firstObject.frame)-p.y)};}
}
@interface X64ArmStatusEvents : NSView {
@public
    std::vector<x64arm::desktop::Event>* events;
    std::uint64_t token;
    std::uint32_t version;
    NSTrackingArea* tracking;
}
@end
@implementation X64ArmStatusEvents
- (BOOL)acceptsFirstResponder{return YES;}
- (void)send:(std::uint32_t)code{if(events)events->push_back({token,code,point()});}
- (void)sendKey:(std::uint32_t)code{
    const auto anchor=[self.window convertPointToScreen:[self convertPoint:NSMakePoint(0,NSMaxY(self.bounds)) toView:nil]];
    if(events)events->push_back({token,code,{static_cast<std::int32_t>(anchor.x),static_cast<std::int32_t>(NSMaxY(NSScreen.screens.firstObject.frame)-anchor.y)}});
}
- (void)updateTrackingAreas{
    if(tracking)[self removeTrackingArea:tracking];
    tracking=[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseEnteredAndExited|NSTrackingMouseMoved|NSTrackingActiveAlways|NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:tracking];[super updateTrackingAreas];
}
- (void)mouseEntered:(NSEvent*)event{(void)event;[self send:version==4?0x406:0x200];}
- (void)mouseExited:(NSEvent*)event{(void)event;if(version==4)[self send:0x407];}
- (void)mouseMoved:(NSEvent*)event{(void)event;[self send:0x200];}
- (void)mouseDown:(NSEvent*)event{if(version!=4)[self send:event.clickCount>1?0x203:0x201];}
- (void)mouseUp:(NSEvent*)event{(void)event;[self send:version==4?0x400:0x202];}
- (void)rightMouseDown:(NSEvent*)event{if(version!=4)[self send:event.clickCount>1?0x206:0x204];}
- (void)rightMouseUp:(NSEvent*)event{(void)event;[self send:version==4?0x7b:0x205];}
- (void)otherMouseDown:(NSEvent*)event{[self send:event.clickCount>1?0x209:0x207];}
- (void)otherMouseUp:(NSEvent*)event{(void)event;[self send:0x208];}
- (void)keyDown:(NSEvent*)event{
    const auto c=event.charactersIgnoringModifiers.length?[event.charactersIgnoringModifiers characterAtIndex:0]:0;
    if(c==13 || c==32){if(version>=3)[self sendKey:0x401];else{[self sendKey:0x204];[self sendKey:0x205];}}
    else if(c==NSF10FunctionKey && (event.modifierFlags&NSEventModifierFlagShift)){if(version>=3)[self sendKey:0x7b];else{[self sendKey:0x204];[self sendKey:0x205];}}
    else [super keyDown:event];
}
@end
@interface X64ArmBalloonPanel : NSPanel
@end
@implementation X64ArmBalloonPanel
- (BOOL)canBecomeKeyWindow{return NO;}
- (BOOL)canBecomeMainWindow{return NO;}
@end
@interface X64ArmBalloonTarget : NSObject {
@public
    std::vector<x64arm::desktop::Event>* events;
    std::uint64_t token;
    __weak NSPanel* panel;
}
- (void)clicked:(id)sender;
@end
@implementation X64ArmBalloonTarget
- (void)clicked:(id)sender{(void)sender;if(events)events->push_back({token,0x405,point()});[panel orderOut:nil];}
@end
namespace x64arm {
namespace {
struct Entry {NSStatusItem* item{};X64ArmStatusEvents* view{};desktop::Icon model;};
struct Highlight {__weak NSWindow* window{};NSColor* background{};BOOL transparent{};bool caption{};};
class CocoaDesktopNotifications final:public NativeDesktopNotifications {
    std::map<std::uint64_t,Entry> entries_;
    std::map<std::uint64_t,Highlight> highlights_;
    std::set<std::uint64_t> dock_owners_;
    NSInteger dock_request_{-1};
    std::vector<desktop::Event> events_;
    std::deque<desktop::Icon> pending_;
    std::optional<desktop::Icon> active_;
    X64ArmBalloonPanel* panel_{};
    X64ArmBalloonTarget* target_{};
    std::uint64_t last_tick_{},remaining_{};
    bool session_active_{true},screen_awake_{true};
    NSMutableArray* observers_{};
    void dismiss(std::uint32_t code){
        if(active_ && code){desktop::Event event{active_->token,code,point()};if(code==0x403)event.retired=desktop::Callback{active_->owner,active_->id,active_->callback,active_->version};events_.push_back(std::move(event));}
        [panel_ orderOut:nil];active_.reset();remaining_=0;
    }
    void show_next(){
        if(active_ || pending_.empty())return;
        const auto current=state();if(!current || *current==1 || *current==2 || *current==3 || *current==4)return;
        auto icon=std::move(pending_.front());pending_.pop_front();
        if(!entries_.contains(icon.token)){show_next();return;}
        if(*current==6 && (icon.info_flags&128)){events_.push_back({icon.token,0x403,point()});show_next();return;}
        const auto bounds=NSScreen.mainScreen.visibleFrame;
        const auto font=[NSFont systemFontOfSize:13];
        const auto message=text(icon.info);
        const auto measure=[message boundingRectWithSize:NSMakeSize(286,500) options:NSStringDrawingUsesLineFragmentOrigin attributes:@{NSFontAttributeName:font}];
        const CGFloat height=std::clamp<CGFloat>(measure.size.height+76,110,500);
        if(!panel_)panel_=[[X64ArmBalloonPanel alloc] initWithContentRect:NSMakeRect(0,0,360,height) styleMask:NSWindowStyleMaskBorderless|NSWindowStyleMaskNonactivatingPanel backing:NSBackingStoreBuffered defer:NO];
        panel_.releasedWhenClosed=NO;panel_.level=NSStatusWindowLevel;panel_.hidesOnDeactivate=NO;panel_.backgroundColor=NSColor.windowBackgroundColor;panel_.hasShadow=YES;
        panel_.collectionBehavior=NSWindowCollectionBehaviorCanJoinAllSpaces|NSWindowCollectionBehaviorFullScreenAuxiliary;
        [panel_ setFrame:NSMakeRect(NSMaxX(bounds)-370,NSMaxY(bounds)-height-12,360,height) display:YES];panel_.title=@"MacWin 通知";
        NSView* content=[[NSView alloc] initWithFrame:NSMakeRect(0,0,360,height)];panel_.contentView=content;
        const CGFloat image_size=(icon.info_flags&32)?32:16;
        NSImageView* image=[[NSImageView alloc] initWithFrame:NSMakeRect(12,height-20-image_size,image_size,image_size)];
        const auto type=icon.info_flags&15U;
        if(type==4)image.image=native_gui_icon_image(icon.balloon);
        else if(type){NSString* symbol=type==1?@"info.circle.fill":type==2?@"exclamationmark.triangle.fill":@"xmark.octagon.fill";image.image=[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil];}
        if(icon.title.empty())image.image=nil;image.imageScaling=NSImageScaleProportionallyDown;[content addSubview:image];
        NSTextField* title=[NSTextField wrappingLabelWithString:text(icon.title)];title.font=[NSFont boldSystemFontOfSize:13];title.frame=NSMakeRect(54,height-40,286,24);[content addSubview:title];
        NSTextField* info=[NSTextField wrappingLabelWithString:message];info.font=font;info.frame=NSMakeRect(54,18,286,height-62);[content addSubview:info];
        if(!target_)target_=[X64ArmBalloonTarget new];target_->events=&events_;target_->token=icon.token;target_->panel=panel_;
        NSButton* click=[[NSButton alloc] initWithFrame:content.bounds];click.bordered=NO;click.transparent=YES;click.title=@"";click.target=target_;click.action=@selector(clicked:);click.accessibilityLabel=message;[content addSubview:click];
        active_=std::move(icon);remaining_=active_->timeout;[panel_ orderFrontRegardless];events_.push_back({active_->token,0x402,point()});
        if(!(active_->info_flags&16))NSBeep();
    }
public:
    CocoaDesktopNotifications(){
        observers_=[NSMutableArray new];const auto center=NSWorkspace.sharedWorkspace.notificationCenter;
        [observers_ addObject:[center addObserverForName:NSWorkspaceSessionDidResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification* notification){(void)notification;session_active_=false;}]];
        [observers_ addObject:[center addObserverForName:NSWorkspaceSessionDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification* notification){(void)notification;session_active_=true;}]];
        [observers_ addObject:[center addObserverForName:NSWorkspaceScreensDidSleepNotification object:nil queue:nil usingBlock:^(NSNotification* notification){(void)notification;screen_awake_=false;}]];
        [observers_ addObject:[center addObserverForName:NSWorkspaceScreensDidWakeNotification object:nil queue:nil usingBlock:^(NSNotification* notification){(void)notification;screen_awake_=true;}]];
    }
    ~CocoaDesktopNotifications()override{
        for(id observer in observers_)[NSWorkspace.sharedWorkspace.notificationCenter removeObserver:observer];
        if(target_)target_->events=nullptr;dismiss(0);
        for(auto& [token,entry]:entries_){(void)token;entry.view->events=nullptr;[NSStatusBar.systemStatusBar removeStatusItem:entry.item];}
        for(auto& [token,value]:highlights_){(void)token;if(value.window){value.window.backgroundColor=value.background;value.window.titlebarAppearsTransparent=value.transparent;}}
        if(dock_request_>=0)[NSApp cancelUserAttentionRequest:dock_request_];
    }
    bool update(const std::map<std::uint64_t,desktop::Icon>& icons,std::uint64_t now)override{
        @autoreleasepool{
            const auto elapsed=now>=last_tick_?now-last_tick_:0;last_tick_=now;
            const auto current=state();
            if(active_ && current && *current!=1){if(elapsed>=remaining_)dismiss(0x404);else remaining_-=elapsed;}
            auto requests=pending_;bool cancel_active=false;std::vector<desktop::Event> hidden;
            const auto remove=[&](std::uint64_t token){std::erase_if(requests,[&](const auto& item){return item.token==token;});if(active_ && active_->token==token)cancel_active=true;};
            for(const auto& [token,entry]:entries_){(void)entry;if(!icons.contains(token))remove(token);}
            for(const auto& [token,icon]:icons){const auto found=entries_.find(token);if(icon.serial && (found==entries_.end() || found->second.model.serial!=icon.serial))remove(token);}
            for(const auto& [token,icon]:icons){const auto found=entries_.find(token);const auto serial=found==entries_.end()?0:found->second.model.serial;
                if(icon.serial==serial || icon.info.empty())continue;
                if(current && *current==6 && (icon.info_flags&128))hidden.push_back({token,0x403,point()});
                else if(!(icon.flags&64) || ((!active_ || cancel_active) && requests.empty() && current && *current==5))requests.push_back(icon);
            }
            if(requests.size()>128)return false;
            auto next=entries_;std::erase_if(next,[&](const auto& item){return !icons.contains(item.first);});
            for(const auto& [token,icon]:icons)next[token].model=icon;
            events_.reserve(events_.size()+hidden.size()+requests.size()+4);
            // 圖示轉換先於介面修改，無效像素不留下半套註冊。
            std::map<std::uint64_t,NSImage*> images;
            for(const auto& [token,icon]:icons){const bool present=icon.image.width || icon.image.height || icon.image.system || !icon.image.image.empty() || !icon.image.rgba.empty();NSImage* image=present?native_gui_icon_image(icon.image):nil;if(present && !image)return false;images.emplace(token,image);}
            std::vector<std::uint64_t> created;created.reserve(icons.size());
            const auto failed=[&](){for(const auto token:created){auto& entry=next.at(token);if(entry.view)entry.view->events=nullptr;[NSStatusBar.systemStatusBar removeStatusItem:entry.item];}return false;};
            for(auto& [token,entry]:next)if(!entry.item){entry.item=[NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];if(!entry.item)return failed();created.push_back(token);entry.item.visible=NO;
                entry.view=[[X64ArmStatusEvents alloc] initWithFrame:entry.item.button.bounds];if(!entry.view)return failed();entry.view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;entry.view->events=&events_;entry.view->token=token;[entry.item.button addSubview:entry.view];}
            for(auto& [token,entry]:entries_)if(!icons.contains(token)){entry.view->events=nullptr;[NSStatusBar.systemStatusBar removeStatusItem:entry.item];}
            if(cancel_active)dismiss(0x403);pending_=std::move(requests);events_.insert(events_.end(),hidden.begin(),hidden.end());entries_=std::move(next);
            for(auto& [token,entry]:entries_){const auto& icon=entry.model;
                entry.view->version=icon.version;entry.item.button.image=images.at(token);entry.item.button.imageScaling=NSImageScaleProportionallyDown;
                entry.item.visible=!(icon.state&1);entry.item.button.toolTip=icon.version!=4 || (icon.flags&128)?text(icon.tip):nil;entry.view.toolTip=entry.item.button.toolTip;
            }
            show_next();return true;
        }
    }
    bool focus(std::uint64_t token)override{
        const auto found=entries_.find(token);return found!=entries_.end() && found->second.item.visible && [found->second.view.window makeFirstResponder:found->second.view];
    }
    bool attention(std::uint64_t owner,void* window,bool caption,bool tray)override{
        NSWindow* native=(__bridge NSWindow*)window;if(!native)return false;
        if(caption){auto found=highlights_.find(owner);if(found==highlights_.end())found=highlights_.emplace(owner,Highlight{native,native.backgroundColor,native.titlebarAppearsTransparent,true}).first;
            native.titlebarAppearsTransparent=YES;native.backgroundColor=NSColor.systemOrangeColor;
        }else if(const auto found=highlights_.find(owner);found!=highlights_.end()){
            if(found->second.window){found->second.window.backgroundColor=found->second.background;found->second.window.titlebarAppearsTransparent=found->second.transparent;}highlights_.erase(found);
        }
        if(tray)dock_owners_.insert(owner);else dock_owners_.erase(owner);
        if(!dock_owners_.empty() && dock_request_<0)dock_request_=[NSApp requestUserAttention:NSCriticalRequest];
        if(dock_owners_.empty() && dock_request_>=0){[NSApp cancelUserAttentionRequest:dock_request_];dock_request_=-1;}return true;
    }
    std::optional<std::uint32_t> state()const override{
        if(!session_active_ || !screen_awake_)return 1;
        CFDictionaryRef session=CGSessionCopyCurrentDictionary();if(!session)return {};
        const bool console=CFDictionaryGetValue(session,kCGSessionOnConsoleKey)==kCFBooleanTrue;CFRelease(session);if(!console)return 1;
        const auto options=NSApp.currentSystemPresentationOptions;
        if(options&(NSApplicationPresentationDisableProcessSwitching|NSApplicationPresentationDisableHideApplication))return 4;
        if(options&NSApplicationPresentationFullScreen)return 2;
        CFArrayRef list=CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly|kCGWindowListExcludeDesktopElements,kCGNullWindowID);
        if(list){NSArray* windows=(__bridge NSArray*)list;const auto pid=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
            for(NSDictionary* window in windows){if([window[(__bridge NSString*)kCGWindowOwnerPID] intValue]!=pid || [window[(__bridge NSString*)kCGWindowLayer] intValue]!=0)continue;
                CGRect frame{};if(CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)window[(__bridge NSString*)kCGWindowBounds],&frame))for(NSScreen* screen in NSScreen.screens){const auto display=[screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];if(CGRectContainsRect(frame,CGDisplayBounds(display))){CFRelease(list);return 2;}}break;
            }CFRelease(list);
        }return 5;
    }
    std::vector<desktop::Event> poll(std::uint64_t now)override{
        @autoreleasepool{
            const auto elapsed=now>=last_tick_?now-last_tick_:0;last_tick_=now;
            if(active_ && !panel_.visible)dismiss(0);
            // 桌面狀態只影響作用中的氣球計時；沒有氣球時不查詢 WindowServer。
            // 待顯示的通知仍由 show_next 查詢即時狀態，明確的 state 呼叫亦不快取。
            if(active_){const auto current=state();if(current && *current!=1){if(elapsed>=remaining_)dismiss(0x404);else remaining_-=elapsed;}}
            show_next();auto result=std::move(events_);events_.clear();return result;
        }
    }
    void* icon_view(std::uint64_t token)const override{const auto it=entries_.find(token);return it==entries_.end()?nullptr:(__bridge void*)it->second.view;}
    void* balloon_window()const override{return (__bridge void*)panel_;}
};
}
std::shared_ptr<NativeDesktopNotifications> make_native_desktop_notifications(){return std::make_shared<CocoaDesktopNotifications>();}
}
