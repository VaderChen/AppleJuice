#include "application.hpp"
#include "modifier_keys.hpp"
#import <AppKit/AppKit.h>
#include "x64arm/native_console.hpp"
#include <algorithm>
#include <cmath>

namespace x64arm {
struct ConsoleViewState {ConsoleSnapshot screen;std::vector<ConsoleRecord> events;bool closed{},updating{};};
static NSString* console_string(std::u16string_view text){
    return [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(text.data()) length:text.size()];
}
static std::uint32_t console_modifiers(NSEvent* event){
    const auto flags=event.modifierFlags;std::uint32_t result=0;
    if(flags&NSEventModifierFlagShift)result|=0x10;
    if(flags&NSEventModifierFlagControl){if(flags&NX_DEVICERCTLKEYMASK)result|=4;if((flags&NX_DEVICELCTLKEYMASK) || !(flags&(NX_DEVICELCTLKEYMASK|NX_DEVICERCTLKEYMASK)))result|=8;}
    if(flags&NSEventModifierFlagOption){if(flags&NX_DEVICERALTKEYMASK)result|=1;if((flags&NX_DEVICELALTKEYMASK) || !(flags&(NX_DEVICELALTKEYMASK|NX_DEVICERALTKEYMASK)))result|=2;}
    if(flags&NSEventModifierFlagCapsLock)result|=0x80;return result;
}
static std::array<std::uint16_t,2> console_virtual_key(unsigned code,NSString* text){
    if(const auto* key=native_modifier_key(code))return {key->key,static_cast<std::uint16_t>(key->scan&255U)};
    struct Key{unsigned mac,win,scan;};
    static constexpr Key keys[]{
        {0,65,30},{1,83,31},{2,68,32},{3,70,33},{4,72,35},{5,71,34},{6,90,44},{7,88,45},{8,67,46},{9,86,47},{11,66,48},
        {12,81,16},{13,87,17},{14,69,18},{15,82,19},{16,89,21},{17,84,20},{18,49,2},{19,50,3},{20,51,4},{21,52,5},
        {22,54,7},{23,53,6},{24,187,13},{25,57,10},{26,55,8},{27,189,12},{28,56,9},{29,48,11},{30,221,27},
        {31,79,24},{32,85,22},{33,219,26},{34,73,23},{35,80,25},{36,13,28},{37,76,38},{38,74,36},{39,222,40},
        {40,75,37},{41,186,39},{42,220,43},{43,188,51},{44,191,53},{45,78,49},{46,77,50},{47,190,52},
        {48,9,15},{49,32,57},{50,192,41},{51,8,14},{53,27,1},{55,91,91},{57,20,58},
        {76,13,28},{96,116,63},{97,117,64},{98,118,65},{99,114,61},{100,119,66},
        {101,120,67},{103,122,87},{109,121,68},{111,123,88},{115,36,71},{116,33,73},{117,46,83},{118,115,62},
        {119,35,79},{120,113,60},{121,34,81},{122,112,59},{123,37,75},{124,39,77},{125,40,80},{126,38,72}
    };
    for(const auto& key:keys)if(key.mac==code)return {static_cast<std::uint16_t>(key.win),static_cast<std::uint16_t>(key.scan)};
    const auto character=text.length?[text characterAtIndex:0]:0;return {static_cast<std::uint16_t>(character),0};
}
}

@interface X64ArmConsoleView : NSView {
    std::array<bool,256> pressedModifiers;
}
@property(nonatomic) x64arm::ConsoleViewState* model;
@end
@implementation X64ArmConsoleView
- (BOOL)isFlipped{return YES;}
- (BOOL)acceptsFirstResponder{return YES;}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;if(!self.model)return;const auto& screen=self.model->screen;
    const CGFloat width=9,height=18;NSFont* font=[NSFont monospacedSystemFontOfSize:14 weight:NSFontWeightRegular];
    const auto color=[](unsigned bits)->NSColor*{const CGFloat low=bits&8?1:0.5,zero=bits&8?0.25:0;
        if((bits&7)==7 && !(bits&8))return [NSColor colorWithCalibratedWhite:0.75 alpha:1];
        return [NSColor colorWithCalibratedRed:bits&4?low:zero green:bits&2?low:zero blue:bits&1?low:zero alpha:1];};
    for(unsigned y=0;y<screen.view_rows;++y)for(unsigned x=0;x<screen.view_columns;++x){
        const auto& cell=screen.cells[static_cast<std::size_t>(y+screen.top)*screen.columns+x+screen.left];unsigned fg=cell.attributes&15,bg=(cell.attributes>>4)&15;
        if(cell.attributes&0x4000)std::swap(fg,bg);[color(bg) setFill];NSRectFill(NSMakeRect(x*width,y*height,width,height));
    }
    for(unsigned y=0;y<screen.view_rows;++y)for(unsigned x=0;x<screen.view_columns;++x){
        const auto at=static_cast<std::size_t>(y+screen.top)*screen.columns+x+screen.left;
        const auto& cell=screen.cells[at];const unsigned fg=cell.attributes&0x4000?(cell.attributes>>4)&15:cell.attributes&15;
        auto c=cell.character;if(c>=0xdc00 && c<=0xdfff && x && screen.cells[at-1].character>=0xd800 && screen.cells[at-1].character<=0xdbff)continue;
        std::u16string text(1,c?c:u' ');if(c>=0xd800 && c<=0xdbff && x+1<screen.view_columns){const auto next=screen.cells[at+1].character;if(next>=0xdc00 && next<=0xdfff)text+=next;}
        NSDictionary* attributes=@{NSFontAttributeName:font,NSForegroundColorAttributeName:color(fg),
            NSUnderlineStyleAttributeName:@((cell.attributes&0x8000)?NSUnderlineStyleSingle:NSUnderlineStyleNone)};
        NSString* string=x64arm::console_string(text);const auto natural=[string sizeWithAttributes:attributes].width;
        [NSGraphicsContext saveGraphicsState];auto context=NSGraphicsContext.currentContext.CGContext;
        const auto extent=width*static_cast<CGFloat>(text.size());
        CGContextTranslateCTM(context,x*width,y*height);if(natural>extent)CGContextScaleCTM(context,extent/natural,1);
        [string drawAtPoint:NSZeroPoint withAttributes:attributes];[NSGraphicsContext restoreGraphicsState];
        [color(fg) setFill];if(cell.attributes&0x400)NSRectFill(NSMakeRect(x*width,y*height,width,1));
        if(cell.attributes&0x800)NSRectFill(NSMakeRect(x*width,y*height,1,height));
        if(cell.attributes&0x1000)NSRectFill(NSMakeRect((x+1)*width-1,y*height,1,height));
    }
    if(screen.cursor_visible && screen.x>=screen.left && screen.x<screen.left+screen.view_columns && screen.y>=screen.top && screen.y<screen.top+screen.view_rows){const auto& cell=screen.cells[static_cast<std::size_t>(screen.y)*screen.columns+screen.x];
        [color(cell.attributes&15) setFill];const CGFloat size=height*screen.cursor_size/100.0;
        NSRectFill(NSMakeRect((screen.x-screen.left)*width,(screen.y-screen.top+1)*height-size,width,size));}
}
- (void)recordKey:(NSEvent*)event down:(BOOL)down {
    if(!self.model)return;NSString* text=event.characters?:@"";const auto key=x64arm::console_virtual_key(event.keyCode,event.charactersIgnoringModifiers?:text);
    auto modifiers=x64arm::console_modifiers(event);if(key[0]>=33 && key[0]<=46)modifiers|=0x100;
    if(!text.length || [text characterAtIndex:0]>=0xf700){self.model->events.push_back(x64arm::console_key(0,down,key[0],key[1],modifiers));return;}
    for(NSUInteger i=0;i<text.length;++i)self.model->events.push_back(x64arm::console_key([text characterAtIndex:i],down,key[0],key[1],modifiers));
}
- (void)keyDown:(NSEvent*)event {[self recordKey:event down:YES];}
- (void)keyUp:(NSEvent*)event {[self recordKey:event down:NO];}
- (void)flagsChanged:(NSEvent*)event {
    if(const auto* key=x64arm::native_modifier_key(event.keyCode)){
        if(!self.model)return;auto& previous=pressedModifiers[event.keyCode];
        const bool pressed=x64arm::native_modifier_pressed(*key,event.modifierFlags,previous);if(pressed==previous)return;previous=pressed;
        const auto state=x64arm::console_modifiers(event)|((key->scan&0x100U)?0x100U:0U);
        self.model->events.push_back(x64arm::console_key(0,pressed,key->key,static_cast<std::uint16_t>(key->scan&255U),state));return;
    }
    if(!self.model)return;const auto key=x64arm::console_virtual_key(event.keyCode,@"");const auto mask=key[0]==16?NSEventModifierFlagShift:key[0]==17?NSEventModifierFlagControl:key[0]==18?NSEventModifierFlagOption:key[0]==20?NSEventModifierFlagCapsLock:NSEventModifierFlagCommand;
    self.model->events.push_back(x64arm::console_key(0,(event.modifierFlags&mask)!=0,key[0],key[1],x64arm::console_modifiers(event)));
}
- (void)recordMouse:(NSEvent*)event flags:(unsigned)flags {
    if(!self.model)return;const auto point=[self convertPoint:event.locationInWindow fromView:nil];x64arm::ConsoleRecord value{};
    x64arm::set_console_field(value,0,2,2);x64arm::set_console_field(value,4,self.model->screen.left+static_cast<unsigned>(std::clamp<long>(static_cast<long>(std::floor(point.x/9)),0,self.model->screen.view_columns-1)),2);
    x64arm::set_console_field(value,6,self.model->screen.top+static_cast<unsigned>(std::clamp<long>(static_cast<long>(std::floor(point.y/18)),0,self.model->screen.view_rows-1)),2);
    auto buttons=static_cast<std::uint32_t>(NSEvent.pressedMouseButtons)&7U;
    if(flags&12){const auto delta=flags&8?event.scrollingDeltaX:event.scrollingDeltaY;buttons|=static_cast<std::uint32_t>(delta<0?-120:120)<<16;}
    x64arm::set_console_field(value,8,buttons);x64arm::set_console_field(value,12,x64arm::console_modifiers(event));x64arm::set_console_field(value,16,flags);
    self.model->events.push_back(value);
}
- (void)mouseDown:(NSEvent*)event {[self recordMouse:event flags:event.clickCount>1?2:0];}
- (void)mouseUp:(NSEvent*)event {[self recordMouse:event flags:0];}
- (void)rightMouseDown:(NSEvent*)event {[self mouseDown:event];}
- (void)rightMouseUp:(NSEvent*)event {[self mouseUp:event];}
- (void)otherMouseDown:(NSEvent*)event {[self mouseDown:event];}
- (void)otherMouseUp:(NSEvent*)event {[self mouseUp:event];}
- (void)mouseMoved:(NSEvent*)event {[self recordMouse:event flags:1];}
- (void)mouseDragged:(NSEvent*)event {[self mouseMoved:event];}
- (void)rightMouseDragged:(NSEvent*)event {[self mouseMoved:event];}
- (void)otherMouseDragged:(NSEvent*)event {[self mouseMoved:event];}
- (void)scrollWheel:(NSEvent*)event {[self recordMouse:event flags:event.scrollingDeltaY!=0?4:8];}
@end

@interface X64ArmConsoleDelegate : NSObject<NSWindowDelegate>
@property(nonatomic) x64arm::ConsoleViewState* model;
@end
@implementation X64ArmConsoleDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {(void)sender;if(self.model)self.model->closed=true;return YES;}
- (void)windowWillClose:(NSNotification*)notification {(void)notification;if(self.model)self.model->closed=true;}
- (void)windowDidResize:(NSNotification*)notification {
    if(!self.model || self.model->updating)return;NSWindow* window=notification.object;const auto size=window.contentView.bounds.size;
    x64arm::ConsoleRecord value{};x64arm::set_console_field(value,0,4,2);
    x64arm::set_console_field(value,4,static_cast<unsigned>(std::clamp<long>(static_cast<long>(size.width/9),1,1024)),2);
    x64arm::set_console_field(value,6,static_cast<unsigned>(std::clamp<long>(static_cast<long>(size.height/18),1,1024)),2);self.model->events.push_back(value);
}
- (void)recordFocus:(BOOL)focused {
    if(!self.model)return;x64arm::ConsoleRecord value{};x64arm::set_console_field(value,0,16,2);x64arm::set_console_field(value,4,focused?1:0);self.model->events.push_back(value);
}
- (void)windowDidBecomeKey:(NSNotification*)notification {(void)notification;[self recordFocus:YES];}
- (void)windowDidResignKey:(NSNotification*)notification {(void)notification;[self recordFocus:NO];}
@end

namespace x64arm {
class CocoaConsoleWindow final : public ConsoleWindow {
    ConsoleViewState state_;
    NSWindow* window_;X64ArmConsoleView* view_;X64ArmConsoleDelegate* delegate_;
public:
    CocoaConsoleWindow(){
        @autoreleasepool {
            state_.screen.cells.resize(static_cast<std::size_t>(state_.screen.columns)*state_.screen.rows);
            prepare_native_application();
            auto content=NSMakeRect(120,120,720,450);
            if(NSScreen* screen=native_background_display()){const auto frame=screen.visibleFrame;content.origin=NSMakePoint(NSMidX(frame)-360,NSMidY(frame)-225);}
            window_=[[NSWindow alloc] initWithContentRect:content
                styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
                backing:NSBackingStoreBuffered defer:NO];window_.releasedWhenClosed=NO;
            view_=[[X64ArmConsoleView alloc] initWithFrame:NSMakeRect(0,0,720,450)];view_.model=&state_;
            view_.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;window_.contentView=view_;window_.acceptsMouseMovedEvents=YES;
            window_.contentMinSize=NSMakeSize(9,18);window_.contentResizeIncrements=NSMakeSize(9,18);
            delegate_=[X64ArmConsoleDelegate new];delegate_.model=&state_;window_.delegate=delegate_;
            [window_ makeFirstResponder:view_];window_.title=@"MacWin Console";[window_ orderFront:nil];
        }
    }
    ~CocoaConsoleWindow() override {@autoreleasepool {[NSNotificationCenter.defaultCenter removeObserver:delegate_];view_.model=nullptr;delegate_.model=nullptr;if(window_.delegate==delegate_)window_.delegate=nil;[window_ close];}}
    void* native_view() override {return (__bridge void*)view_;}
    bool bind_native_window(void* native) override {
        if(!native || !NSThread.isMainThread)return false;
        @autoreleasepool {
            [NSNotificationCenter.defaultCenter removeObserver:delegate_];window_.delegate=nil;[view_ removeFromSuperview];[window_ close];
            window_=(__bridge NSWindow*)native;window_.contentView=view_;window_.acceptsMouseMovedEvents=YES;
            window_.contentMinSize=NSMakeSize(18,36);window_.contentResizeIncrements=NSMakeSize(9,18);[window_ makeFirstResponder:view_];
            auto* center=NSNotificationCenter.defaultCenter;
            [center addObserver:delegate_ selector:@selector(windowDidResize:) name:NSWindowDidResizeNotification object:window_];
            [center addObserver:delegate_ selector:@selector(windowDidBecomeKey:) name:NSWindowDidBecomeKeyNotification object:window_];
            [center addObserver:delegate_ selector:@selector(windowDidResignKey:) name:NSWindowDidResignKeyNotification object:window_];
            [center addObserver:delegate_ selector:@selector(windowWillClose:) name:NSWindowWillCloseNotification object:window_];
            return true;
        }
    }
    std::array<std::uint16_t,2> maximum_size() const override {
        const auto screen=window_.screen?:NSScreen.mainScreen;const auto size=screen.visibleFrame.size;
        return {static_cast<std::uint16_t>(std::clamp<long>(static_cast<long>(size.width/9),2,32767)),static_cast<std::uint16_t>(std::clamp<long>(static_cast<long>(size.height/18),2,32767))};
    }
    bool update(const ConsoleSnapshot& screen) override {
        if(!NSThread.isMainThread || state_.closed)return false;
        @autoreleasepool {state_.screen=screen;window_.title=console_string(screen.title);state_.updating=true;
            const auto size=NSMakeSize(screen.view_columns*9,screen.view_rows*18);if(!NSEqualSizes(size,window_.contentView.bounds.size))[window_ setContentSize:size];
            state_.updating=false;view_.needsDisplay=YES;[view_ displayIfNeeded];return true;}
    }
    std::vector<ConsoleRecord> poll(unsigned milliseconds) override {
        if(!NSThread.isMainThread)return {};
        @autoreleasepool {
            NSEvent* event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:milliseconds?[NSDate dateWithTimeIntervalSinceNow:milliseconds/1000.0]:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
            if(event)[NSApp sendEvent:event];[NSApp updateWindows];std::vector<ConsoleRecord> result;result.swap(state_.events);return result;
        }
    }
    bool closed() const override{return state_.closed;}
    bool fullscreen() const override{return (window_.styleMask&NSWindowStyleMaskFullScreen)!=0;}
};
std::unique_ptr<ConsoleWindow> create_console_window(){
    if(!NSThread.isMainThread)return {};return std::make_unique<CocoaConsoleWindow>();
}
}
