#include "x64arm/raster_surface.hpp"
#include "gui_timing.hpp"
#include "application.hpp"
#include "modifier_keys.hpp"
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <WebKit/WebKit.h>
// X64ARM_TRACE_WEBVIEW：記錄 WebView2 對應 WKWebView 的導覽結果，供診斷內容未顯示等問題。
static bool webview_trace_enabled(){static const bool enabled=std::getenv("X64ARM_TRACE_WEBVIEW")!=nullptr;return enabled;}
@interface X64ArmWebViewDelegate : NSObject<WKNavigationDelegate>
@end
@implementation X64ArmWebViewDelegate
- (void)webView:(WKWebView*)view didFinishNavigation:(WKNavigation*)navigation {(void)navigation;if(webview_trace_enabled())std::fprintf(stderr,"[webview] finish url=%s frame=%.0fx%.0f hidden=%d window=%p\n",view.URL.absoluteString.UTF8String?:"",view.frame.size.width,view.frame.size.height,view.hiddenOrHasHiddenAncestor?1:0,(__bridge void*)view.window);
    // X64ARM_TRACE_WEBVIEW_SNAPSHOT=路徑：以 WebKit 自身快照確認內容實際繪製（與視窗是否被遮蔽無關）。
    if(const char* path=std::getenv("X64ARM_TRACE_WEBVIEW_SNAPSHOT");path && *path){
        NSString* target=[NSString stringWithUTF8String:path];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,static_cast<int64_t>(500*NSEC_PER_MSEC)),dispatch_get_main_queue(),^{
            [view takeSnapshotWithConfiguration:nil completionHandler:^(NSImage* image,NSError* error){
                if(!image){std::fprintf(stderr,"[webview] snapshot failed %s\n",error.localizedDescription.UTF8String?:"");return;}
                NSBitmapImageRep* bitmap=[[NSBitmapImageRep alloc] initWithData:image.TIFFRepresentation];
                [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:target atomically:YES];std::fprintf(stderr,"[webview] snapshot saved\n");
            }];
        });
    }
}
- (void)webView:(WKWebView*)view didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {(void)view;(void)navigation;if(webview_trace_enabled())std::fprintf(stderr,"[webview] fail %s\n",error.localizedDescription.UTF8String?:"");}
- (void)webView:(WKWebView*)view didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {(void)view;(void)navigation;if(webview_trace_enabled())std::fprintf(stderr,"[webview] provisional fail %s\n",error.localizedDescription.UTF8String?:"");}
- (void)webViewWebContentProcessDidTerminate:(WKWebView*)view {(void)view;if(webview_trace_enabled())std::fprintf(stderr,"[webview] content process terminated\n");}
@end
#include "x64arm/windows_gui.hpp"
#include "x64arm/windows_control_images.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/native_font_data.hpp"
#include "x64arm/windows_keyboard.hpp"
#include "x64arm/native_console.hpp"
#include "x64arm/native_desktop_notifications.hpp"
#include <chrono>
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_theme.hpp"
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <functional>
#include <atomic>
#include <charconv>
#include <tuple>
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <thread>

namespace x64arm { NSFont* native_gui_font(const GuiFont&); }

namespace {
NSImage* list_icon(const x64arm::GuiIcon&);
// 離屏仍保留原生字型／GDI 及客體視窗模型，GPU 表面不綁定螢幕 drawable。
bool offline_render_enabled(){static const bool enabled=[] {const auto* value=std::getenv("X64ARM_GRAPHICS_OFFLINE");return value && std::string_view(value)=="1";}();return enabled;}
// 自動驗證可投影到非作用中螢幕；客體仍使用自身桌面的相對座標。
bool background_display_enabled(){return x64arm::native_background_display_enabled();}
NSScreen* background_display(){return x64arm::native_background_display();}
// X64ARM_DISPLAY_SIZE=<寬>x<高>：客體看到的虛擬顯示器解析度（例如 1280x720，讓預設全螢幕解析度較高的遊戲降低負載）。
// 只改變公開給客體的桌面、工作區與顯示模式；宿主視窗依客體座標放置，不切換實體顯示器模式。
std::optional<std::pair<std::uint32_t,std::uint32_t>> virtual_display_size(){
    static const auto value=[]()->std::optional<std::pair<std::uint32_t,std::uint32_t>>{
        const auto* text=std::getenv("X64ARM_DISPLAY_SIZE");if(!text || !*text)return {};
        unsigned width=0,height=0;char separator=0;
        if(std::sscanf(text,"%u%c%u",&width,&separator,&height)!=3 || (separator!='x' && separator!='X') || width<320 || height<200 || width>16384 || height>16384)return {};
        return std::pair{width,height};
    }();
    return value;
}
// ICO 的 DIB 與 AND mask 各自以 DWORD 對齊；避免宿主解碼器把非 32 倍數寬度的遮罩錯位。
NSImage* guest_icon_image(const std::vector<std::uint8_t>& bytes) {
    const auto fallback=[&](){return [[NSImage alloc] initWithData:[NSData dataWithBytes:bytes.data() length:bytes.size()]];};
    const auto read=[&](std::size_t at,unsigned count)->std::uint32_t{if(at>bytes.size() || count>bytes.size()-at)throw std::out_of_range("ICO");std::uint32_t value=0;for(unsigned n=0;n<count;++n)value|=std::uint32_t{bytes[at+n]}<<(n*8);return value;};
    try {
        if(bytes.size()<22 || read(0,2)!=0 || read(2,2)!=1 || read(4,2)!=1)return fallback();
        const auto length=read(14,4),offset=read(18,4);
        if(offset>bytes.size() || length>bytes.size()-offset || length<40)return fallback();
        const auto header=read(offset,4),width=read(offset+4,4),double_height=read(offset+8,4),bpp=read(offset+14,2),compression=read(offset+16,4);
        if(header<40 || header>length || !width || width>4096 || !double_height || double_height>8192 || double_height%2 || compression!=0 || (bpp!=1 && bpp!=4 && bpp!=8 && bpp!=24 && bpp!=32))return fallback();
        const auto height=double_height/2,colors=bpp<=8?(read(offset+32,4)?read(offset+32,4):(1U<<bpp)):0U;
        const std::uint64_t start=header+colors*4ULL,stride=(width*static_cast<std::uint64_t>(bpp)+31)/32*4,mask_stride=(width+31ULL)/32*4,mask=start+stride*height;
        if(mask>length || mask_stride*height>length-mask)return fallback();
        std::vector<std::uint8_t> dib(bytes.begin()+offset,bytes.begin()+offset+length);
        for(unsigned n=0;n<4;++n)dib[8+n]=static_cast<std::uint8_t>(height>>(n*8));
        const auto decoded=x64arm::decode_dib_bitmap(dib);
        bool alpha=false;if(bpp==32)for(std::uint32_t y=0;y<height && !alpha;++y)for(std::uint32_t x=0;x<width;++x)if(bytes[offset+start+y*stride+x*4+3]){alpha=true;break;}
        auto* bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:width*4 bitsPerPixel:32];
        for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){const auto i=static_cast<std::size_t>(y)*width+x;const auto color=decoded.pixels[i];auto* p=bitmap.bitmapData+i*4;
            for(unsigned n=0;n<3;++n)p[n]=static_cast<std::uint8_t>(color>>(n*8));
            p[3]=alpha?bytes[offset+start+(height-y-1)*stride+x*4+3]:(bytes[offset+mask+(height-y-1)*mask_stride+x/8]&(0x80U>>(x%8)))?0:255;
        }
        auto* image=[[NSImage alloc] initWithSize:NSMakeSize(width,height)];[image addRepresentation:bitmap];return image;
    }catch(const std::exception&){return fallback();}
}
NSData* automation_data(const char* path, const char* descriptor) {
    if (!descriptor) return [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
    // 測試程序可由匿名 pipe 傳入操作，讓密語等輸入只存在記憶體中。
    int fd=-1; const std::string value(descriptor);
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),fd);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || fd<3)
        throw std::runtime_error("GUI 自動操作 descriptor 無效");
    struct Descriptor {int value;~Descriptor(){::close(value);}} owned{fd};
    NSMutableData* data=[NSMutableData data];
    for(;;){
        pollfd ready{fd,POLLIN,0};int status;
        do{status=::poll(&ready,1,10000);}while(status<0 && errno==EINTR);
        if(status<=0 || (ready.revents&(POLLERR|POLLNVAL)))throw std::runtime_error("GUI 自動操作 pipe 讀取失敗或逾時");
        char buffer[8192];ssize_t size;
        do{size=::read(fd,buffer,sizeof(buffer));}while(size<0 && errno==EINTR);
        if(size<0)throw std::runtime_error("GUI 自動操作 pipe 讀取失敗");
        if(!size)return data;
        if(data.length+static_cast<std::size_t>(size)>4U*1024U*1024U)throw std::runtime_error("GUI 自動操作資料超出上限");
        [data appendBytes:buffer length:static_cast<NSUInteger>(size)];
    }
}
// 診斷只擷取目前程序自身視窗；像素來自 WindowServer，包含實際 AppKit／Metal 合成結果。
struct NativeSnapshotRequest {
    CGWindowID window;
    std::size_t width,height;
    std::filesystem::path path;
};
struct NativeSnapshotBatch {
    std::atomic<bool> active{true},pending{false};
    std::atomic<std::size_t> remaining{},succeeded{};
    void finish(){if(remaining.fetch_sub(1)==1)pending=false;}
};
void capture_native_windows_async(std::shared_ptr<NativeSnapshotBatch> batch,std::vector<NativeSnapshotRequest> requests){
    if(requests.empty() || !batch->active || batch->pending.exchange(true))return;
    const bool trace_capture=std::getenv("X64ARM_TRACE_GUI_CAPTURE")!=nullptr;
    if(@available(macOS 14.4,*)){
        // 只攜帶不可變的 WindowServer 身分及輸出路徑；回呼不持有 backend 或 NSWindow。
        // 同一批尚未完成時不再新增，服務延遲也不能阻塞客體 GUI／心跳排程。
        [SCShareableContent getCurrentProcessShareableContentWithCompletionHandler:^(SCShareableContent* content,NSError* error){
            if(error || !content || !batch->active){
                if(trace_capture && batch->active)std::fprintf(stderr,"[gui_capture_failure] pid=%d stage=content domain=%s code=%ld\n",::getpid(),error?error.domain.UTF8String:"none",error?static_cast<long>(error.code):0L);
                batch->pending=false;return;
            }
            batch->remaining=requests.size();
            for(const auto& entry:requests){
                const auto request=entry;
                SCWindow* target=nil;for(SCWindow* item in content.windows)if(item.windowID==request.window){target=item;break;}
                if(!target || !batch->active){
                    if(trace_capture && batch->active)std::fprintf(stderr,"[gui_capture_failure] pid=%d stage=window window=%u candidates=%lu\n",::getpid(),request.window,static_cast<unsigned long>(content.windows.count));
                    batch->finish();continue;
                }
                auto filter=[[SCContentFilter alloc] initWithDesktopIndependentWindow:target];auto config=[SCStreamConfiguration new];
                config.colorSpaceName=kCGColorSpaceSRGB;config.width=request.width;config.height=request.height;
                config.showsCursor=NO;config.ignoreShadowsSingleWindow=YES;
                [SCScreenshotManager captureImageWithFilter:filter configuration:config completionHandler:^(CGImageRef image,NSError* failure){
                    if(!image || failure || !batch->active){
                        if(trace_capture && batch->active)std::fprintf(stderr,"[gui_capture_failure] pid=%d stage=image window=%u domain=%s code=%ld\n",::getpid(),request.window,failure?failure.domain.UTF8String:"none",failure?static_cast<long>(failure.code):0L);
                        batch->finish();return;
                    }
                    // CGImageRef 只保證在 capture 回呼內有效；編碼與寫檔前保留實際像素。
                    const auto retained=CGImageRetain(image);
                    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
                        @autoreleasepool {
                            if(batch->active){
                                auto bitmap=[[NSBitmapImageRep alloc] initWithCGImage:retained];
                                auto png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                                if(batch->active && png && [png writeToFile:[NSString stringWithUTF8String:request.path.c_str()] atomically:YES])++batch->succeeded;
                                else if(trace_capture && batch->active)std::fprintf(stderr,"[gui_capture_failure] pid=%d stage=write window=%u\n",::getpid(),request.window);
                            }
                            CGImageRelease(retained);batch->finish();
                        }
                    });
                }];
            }
        }];
    }else batch->pending=false;
}
NSString* nsstring(const std::u16string& text){return [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(text.data()) length:text.size()];}
std::u16string gueststring(NSString* text){std::u16string result(text.length,u'\0');[text getCharacters:reinterpret_cast<unichar*>(result.data()) range:NSMakeRange(0,text.length)];return result;}
NSView* native_window_hit(NSWindow* window,NSPoint location){
    NSView* content=window.contentView;if(!content)return nil;
    // NSView.hitTest 接收父視圖座標；contentView 可翻轉，不能先轉成它自己的座標。
    const auto point=content.superview?[content.superview convertPoint:location fromView:nil]:[content convertPoint:location fromView:nil];
    return [content hitTest:point];
}
NSFont* guest_font(const x64arm::GuiFont& model){return x64arm::native_gui_font(model);}
NSString* menu_title(const std::u16string& text) {
    std::u16string display;
    for(std::size_t i=0;i<text.size() && text[i]!=u'\t';++i){
        if(text[i]==u'&'){if(i+1<text.size() && text[i+1]==u'&'){display.push_back(u'&');++i;}}else display.push_back(text[i]);
    }
    return nsstring(display);
}
CGFloat average_advance(NSFont* font) {
    NSString* sample = font.fixedPitch ? @"0" : @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    return std::max<CGFloat>(1, [sample sizeWithAttributes:@{NSFontAttributeName:font}].width / static_cast<CGFloat>(sample.length));
}
CGFloat font_width_scale(const x64arm::GuiFont& model, NSFont* font) {
    return model.width ? static_cast<CGFloat>(std::clamp<std::int64_t>(std::abs(static_cast<std::int64_t>(model.width)), 1, 512)) / average_advance(font) : 1;
}
NSEventModifierFlags automation_modifiers(NSDictionary* step) {
    id specified=step[@"modifiers"];
    NSArray* names=[specified isKindOfClass:[NSArray class]]?specified:[specified isKindOfClass:[NSString class]]?@[specified]:@[];
    return (([step[@"shift"] boolValue] || [names containsObject:@"shift"])?NSEventModifierFlagShift:0)|
        (([step[@"control"] boolValue] || [names containsObject:@"control"])?NSEventModifierFlagControl:0)|
        (([step[@"alt"] boolValue] || [names containsObject:@"alt"])?NSEventModifierFlagOption:0);
}
NSPasteboard* guest_pasteboard() {
    // 自動化可指定獨立 pasteboard，預設仍與使用者的 macOS 剪貼簿互通。
    if(const char* name=std::getenv("X64ARM_GUI_PASTEBOARD");name && *name)
        return [NSPasteboard pasteboardWithName:[NSString stringWithUTF8String:name]];
    return [NSPasteboard generalPasteboard];
}
x64arm::GuiFontMetrics guest_metrics(const x64arm::GuiFont& model) {
    NSFont* font = guest_font(model); x64arm::GuiFontMetrics metrics;
    metrics.ascent = std::max(1, static_cast<std::int32_t>(std::ceil(font.ascender)));
    metrics.descent = std::max(0, static_cast<std::int32_t>(std::ceil(-font.descender)));
    metrics.height = metrics.ascent + metrics.descent;
    metrics.external_leading = std::max(0, static_cast<std::int32_t>(std::ceil(font.leading)));
    const auto scale = font_width_scale(model, font);
    metrics.average_width = std::max(1, static_cast<std::int32_t>(std::lround(average_advance(font) * scale)));
    metrics.fixed_pitch = font.fixedPitch;
    metrics.maximum_width = metrics.fixed_pitch ? metrics.average_width : std::max(metrics.average_width,
        static_cast<std::int32_t>(std::ceil([@"W" sizeWithAttributes:@{NSFontAttributeName:font}].width * scale)));
    return metrics;
}
CGFloat guest_text_width(const x64arm::GuiFont& model, NSFont* font, NSString* text) {
    const auto measured=[text sizeWithAttributes:@{NSFontAttributeName:font}].width;
    return font.fixedPitch?std::round(measured/average_advance(font))*guest_metrics(model).average_width:
        std::ceil(measured*font_width_scale(model,font));
}
std::uint16_t coordinate_word(CGFloat value) {
    if (!std::isfinite(value)) return 0;
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(value, static_cast<CGFloat>(-32768), static_cast<CGFloat>(32767))));
}
std::uint32_t guest_mouse_buttons(NSUInteger native) {
    return static_cast<std::uint32_t>(native&3U)|((native&4U)?16U:0U);
}
}

void native_guest_modifier(std::uint64_t handle,std::vector<x64arm::GuiMessage>* events,std::array<std::uint32_t,256>& pressedGuestKeys,NSEvent* event) {
    const auto* key=x64arm::native_modifier_key(event.keyCode);if(!events || !key)return;
    auto& saved=pressedGuestKeys[event.keyCode];const bool previous=saved!=0;
    const bool pressed=x64arm::native_modifier_pressed(*key,event.modifierFlags,previous);if(pressed==previous)return;
    const std::uint32_t modifiers=((event.modifierFlags&NSEventModifierFlagShift)?1U:0U)|((event.modifierFlags&NSEventModifierFlagControl)?2U:0U)|((event.modifierFlags&NSEventModifierFlagOption)?4U:0U);
    const bool system=pressed?((modifiers&4U) && !(modifiers&2U)):(saved&0x1000000U)!=0;
    const auto lparam=UINT64_C(1)|(std::uint64_t{key->scan}<<16)|(pressed?0:UINT64_C(0xc0000000))|
        ((key->key==18 || (modifiers&4U))?UINT64_C(1)<<29:0);
    saved=pressed?((std::uint32_t{key->scan}<<8)|key->key|(system?0x1000000U:0U)):0;
    events->push_back({handle,key->key,lparam,system?(pressed?0x104U:0x105U):(pressed?0x100U:0x101U),{},modifiers});
}
void native_guest_key(std::uint64_t handle,std::vector<x64arm::GuiMessage>* events,std::array<std::uint32_t,256>& pressedGuestKeys,NSEvent* event,bool pressed) {
    if(!events)return;
    NSString* chars = event.characters;
    const std::uint32_t modifiers=((event.modifierFlags&NSEventModifierFlagShift)?1U:0U)|((event.modifierFlags&NSEventModifierFlagControl)?2U:0U)|((event.modifierFlags&NSEventModifierFlagOption)?4U:0U);
    // Ctrl／Alt 組合使用修飾前的鍵，不把控制字元或 Option 產生的文字當成 VK。
    // 也涵蓋數字、標點，以及尚未產生文字的 Option dead key。
    if((modifiers&6U) && event.charactersIgnoringModifiers)chars=event.charactersIgnoringModifiers;
    if(!pressed){
        // 修飾鍵或輸入布局可在放開前改變；以實體 keyCode 配對，不能重新猜字元。
        const auto code=event.keyCode;if(code>=pressedGuestKeys.size() || !pressedGuestKeys[code])return;
        const auto saved=pressedGuestKeys[code];pressedGuestKeys[code]=0;
        const auto lparam=UINT64_C(0xc0000001)|(std::uint64_t{(saved>>8)&65535U}<<16)|((modifiers&4)?UINT64_C(1)<<29:0);
        events->push_back({handle,saved&255U,lparam,(saved&0x1000000U)?0x105U:0x101U,{},modifiers});return;
    }
    for (NSUInteger i = 0; i < chars.length; ++i){const auto c=[chars characterAtIndex:i];std::uint64_t key=0,scan=0;
        switch(c){case '\r':case '\n':key=13;scan=0x1c;break;case '\t':key=9;scan=0xf;break;case 0x1b:key=27;scan=1;break;case 0x7f:case 8:key=8;scan=0xe;break;case NSUpArrowFunctionKey:key=38;scan=0x148;break;case NSDownArrowFunctionKey:key=40;scan=0x150;break;case NSLeftArrowFunctionKey:key=37;scan=0x14b;break;case NSRightArrowFunctionKey:key=39;scan=0x14d;break;case NSHomeFunctionKey:key=36;scan=0x147;break;case NSEndFunctionKey:key=35;scan=0x14f;break;case NSPageUpFunctionKey:key=33;scan=0x149;break;case NSPageDownFunctionKey:key=34;scan=0x151;break;case NSDeleteFunctionKey:key=46;scan=0x153;break;case NSInsertFunctionKey:key=45;scan=0x152;break;default:break;}
        if(c>=NSF1FunctionKey && c<=NSF12FunctionKey){key=0x70+c-NSF1FunctionKey;scan=key<=0x79?0x3b+key-0x70:0x57+key-0x7a;}
        if((modifiers&2) && c>=1 && c<=26 && !key)key='A'+c-1;
        // macOS 的數字鍵盤使用獨立實體 keyCode；字元相同仍須保留 Windows VK_NUMPAD／VK_ADD。
        switch(event.keyCode){case 82:key=0x60;break;case 83:key=0x61;break;case 84:key=0x62;break;case 85:key=0x63;break;case 86:key=0x64;break;case 87:key=0x65;break;case 88:key=0x66;break;case 89:key=0x67;break;case 91:key=0x68;break;case 92:key=0x69;break;case 69:key=0x6b;break;case 78:key=0x6d;break;case 67:key=0x6a;break;case 75:key=0x6f;break;case 65:key=0x6e;break;default:break;}
        // 一般可列印按鍵也必須經過 WM_KEYDOWN / TranslateMessage。
        // 使用與客體 HKL 共用的 VK 表，文字提交不再另外重複送 WM_CHAR。
        if(!key && c>=32 && c<127){
            const auto base=static_cast<unsigned>((c>='a' && c<='z')?c-'a'+'A':c);
            constexpr std::u16string_view shifted=u")!@#$%^&*(+:<_>?~{|}\"";
            constexpr std::array<unsigned,21> keys{48,49,50,51,52,53,54,55,56,57,0xbb,0xba,0xbc,0xbd,0xbe,0xbf,0xc0,0xdb,0xdc,0xdd,0xde};
            const auto at=shifted.find(c);if((modifiers&1) && at!=shifted.npos)key=keys[at];
            if(!key)for(unsigned candidate=32;candidate<255;++candidate)
                if(x64arm::windows_map_virtual_key(candidate,2)==base){key=candidate;break;}
        }
        if(key && !scan)scan=x64arm::windows_map_virtual_key(static_cast<unsigned>(key),0);

        if(key){
            const bool system=((modifiers&4U) && !(modifiers&2U)) || key==0x79;
            if(event.keyCode<pressedGuestKeys.size())pressedGuestKeys[event.keyCode]=static_cast<std::uint32_t>((scan<<8)|key|(system?0x1000000U:0U));
            // 實體按住狀態由真正 keyUp 結束；自動重複保留 previous-key-state。
            const auto lparam=1|(scan<<16)|((modifiers&4)?UINT64_C(1)<<29:0)|
                (event.isARepeat?UINT64_C(1)<<30:0);
            events->push_back({handle,key,lparam,system?0x104U:0x100U,{},modifiers});
        }else events->push_back({handle,c,1,0x102,{},modifiers});
    }
}

@interface X64ArmGuestView : NSView<NSTextInputClient,NSViewToolTipOwner> {
@public
    x64arm::GuiImeData imeData;
    NSRange imeSelection;
    NSPoint imePoint;
    BOOL imeSilent;
    BOOL imeFromInputMethod;
    std::array<std::uint32_t,256> pressedGuestKeys;
}
@property(nonatomic,strong) NSTextField* imeOverlay;
@property(nonatomic,strong) NSEvent* imeKeyEvent;
@property(nonatomic,strong) NSCursor* guestCursor;
@property(nonatomic) x64arm::GuiWindow* model;
@property(nonatomic) unsigned long long guestHandle;
@property(nonatomic) std::vector<x64arm::GuiMessage>* events;
@property(nonatomic) BOOL openGLManaged;
@property(nonatomic) BOOL rasterManaged;
@property(nonatomic) BOOL guestUpdating;
@property(nonatomic) unsigned int buttons;
@property(nonatomic) double verticalRemainder;
@property(nonatomic) double horizontalRemainder;
- (void)guestMouse:(NSEvent*)event message:(unsigned int)message button:(unsigned int)button pressed:(BOOL)pressed;
- (void)guestScrollAt:(NSPoint)point vertical:(CGFloat)vertical horizontal:(CGFloat)horizontal precise:(BOOL)precise modifiers:(NSEventModifierFlags)modifiers;
- (void)guestKeyDown:(NSEvent*)event;
- (void)guestKey:(NSEvent*)event pressed:(BOOL)pressed;
- (void)updateImeOverlay;
@end
@implementation X64ArmGuestView
- (void)resetCursorRects {if(self.guestCursor)[self addCursorRect:self.bounds cursor:self.guestCursor];else [super resetCursorRects];}
- (NSString*)view:(NSView*)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point userData:(void*)data {
    (void)view;(void)tag;(void)point;
    const auto index=reinterpret_cast<std::uintptr_t>(data);
    if(!self.model || x64arm::gui_control_class(*self.model)!=u"msctls_statusbar32" || self.model->status_simple || !index || index>self.model->status_parts.size())return @"";
    return nsstring(self.model->status_parts[index-1].tooltip);
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder {const BOOL result=[super becomeFirstResponder];if(result && !self.guestUpdating && self.events)self.events->push_back({self.guestHandle,0,0,0x10013});return result;}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {(void)event;return YES;}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if(self.openGLManaged || self.rasterManaged)return;
    if(self.model && (self.model->ex_style&0x80000U) && !self.model->layered_pixels.empty()){[[NSColor clearColor] setFill];NSRectFillUsingOperation(self.bounds,NSCompositingOperationCopy);return;}
    if(self.model && ((self.model->ex_style&0x20U) || (x64arm::gui_control_class(*self.model)==u"syslink" && (self.model->style&1U)))){
        // WS_EX_TRANSPARENT 沿用已繪製的下層內容；保留此 view 的命中與輸入，不能加上宿主白底。
        [[NSColor clearColor] setFill];NSRectFillUsingOperation(self.bounds,NSCompositingOperationCopy);
    }else {[(self.model && self.model->dialog ? [NSColor windowBackgroundColor] : [NSColor whiteColor]) setFill];NSRectFill(self.bounds);}
    if (!self.model) return;
    if(self.model->calendar){
        const auto color=[](std::uint32_t value){return [NSColor colorWithRed:(value&255)/255.0 green:((value>>8)&255)/255.0 blue:((value>>16)&255)/255.0 alpha:1.0];};
        const auto& model=*self.model;const auto& calendar=*model.calendar;NSFont* font=guest_font(model.control_font);const auto scale=font_width_scale(model.control_font,font);
        [color(calendar.picker?0xffffff:calendar.colors[4]) setFill];NSRectFill(self.bounds);
        for(const auto& cell:calendar.cells){if(calendar.picker && ((calendar.editing && cell.action>=0) || (calendar.updown_window && cell.action==-5 && (model.style&1U))))continue;const auto& r=cell.rect;const auto rect=NSMakeRect(r[0],r[1],r[2]-r[0],r[3]-r[1]);if(NSIsEmptyRect(rect))continue;[NSGraphicsContext saveGraphicsState];NSRectClip(rect);
            NSFont* cellFont=cell.bold?([[NSFontManager sharedFontManager]convertFont:font toHaveTrait:NSBoldFontMask]?:font):font;const auto cellScale=cell.bold?font_width_scale(model.control_font,cellFont):scale;
            NSColor* text=cell.disabled || !model.enabled?NSColor.disabledControlTextColor:color(calendar.colors[cell.trailing?5:1]);
            if(!calendar.picker && (cell.action==-6 || cell.action==-7)){[color(calendar.colors[2])setFill];NSRectFill(rect);text=color(calendar.colors[3]);}
            if(cell.selected && !cell.disabled){[NSColor.selectedTextBackgroundColor setFill];NSRectFill(NSInsetRect(rect,1,1));text=NSColor.selectedTextColor;}
            if(calendar.picker && cell.action==-4){[(model.enabled?NSColor.labelColor:NSColor.disabledControlTextColor) setStroke];[[NSBezierPath bezierPathWithRect:NSInsetRect(rect,1,1)]stroke];if(!calendar.none){auto* path=[NSBezierPath bezierPath];[path moveToPoint:NSMakePoint(NSMinX(rect)+3,NSMidY(rect))];[path lineToPoint:NSMakePoint(NSMinX(rect)+6,NSMidY(rect)+3)];[path lineToPoint:NSMakePoint(NSMaxX(rect)-2,NSMinY(rect)+2)];[path stroke];}}
            else if(calendar.picker && cell.action==-5){[NSColor.controlBackgroundColor setFill];NSRectFill(rect);[(model.enabled?NSColor.labelColor:NSColor.disabledControlTextColor) setFill];const auto x=NSMidX(rect),y=NSMidY(rect);auto* path=[NSBezierPath bezierPath];[path moveToPoint:NSMakePoint(x-4,y-2)];[path lineToPoint:NSMakePoint(x+4,y-2)];[path lineToPoint:NSMakePoint(x,y+3)];[path closePath];[path fill];if(model.style&1U){auto* up=[NSBezierPath bezierPath];[up moveToPoint:NSMakePoint(x-4,y-4)];[up lineToPoint:NSMakePoint(x+4,y-4)];[up lineToPoint:NSMakePoint(x,y-9)];[up closePath];[up fill];}}
            else {NSMutableParagraphStyle* paragraph=[NSMutableParagraphStyle new];paragraph.alignment=calendar.picker?NSTextAlignmentLeft:NSTextAlignmentCenter;paragraph.lineBreakMode=cell.text.find(u'\n')==std::u16string::npos?NSLineBreakByTruncatingTail:NSLineBreakByWordWrapping;
                const auto lines=1+std::count(cell.text.begin(),cell.text.end(),u'\n');
                auto* transform=[NSAffineTransform transform];[transform translateXBy:NSMinX(rect) yBy:NSMidY(rect)-(cellFont.ascender-cellFont.descender)*static_cast<CGFloat>(lines)/2];[transform scaleXBy:cellScale yBy:1];[transform concat];
                [nsstring(cell.text) drawInRect:NSMakeRect(0,0,NSWidth(rect)/cellScale,NSHeight(rect)) withAttributes:@{NSFontAttributeName:cellFont,NSForegroundColorAttributeName:text,NSParagraphStyleAttributeName:paragraph}];
            }
            [NSGraphicsContext restoreGraphicsState];if(!calendar.picker && cell.today && !(model.style&8U)){[NSColor.systemRedColor setStroke];[[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect,3,2)]stroke];}
        }
        if(calendar.picker){[NSColor.separatorColor setStroke];[[NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds,0.5,0.5)]stroke];
            for(const auto& cell:calendar.cells)if(cell.action==-5){auto* divider=[NSBezierPath bezierPath];[divider moveToPoint:NSMakePoint(cell.rect[0]+0.5,0.5)];[divider lineToPoint:NSMakePoint(cell.rect[0]+0.5,NSHeight(self.bounds)-0.5)];[divider stroke];}}
        return;
    }
    if(x64arm::gui_control_class(*self.model)==u"msctls_statusbar32"){
        const auto& model=*self.model;const auto background=model.status_background;
        NSColor* color=background==0xffffffffU?NSColor.windowBackgroundColor:[NSColor colorWithRed:(background&255)/255.0 green:((background>>8)&255)/255.0 blue:((background>>16)&255)/255.0 alpha:1];
        [color setFill];NSRectFill(self.bounds);NSFont* font=guest_font(model.control_font);
        const auto count=model.status_simple?1:model.status_parts.size();
        for(std::size_t index=0;index<count;++index){const auto& part=model.status_simple?model.status_simple_part:model.status_parts[index];const auto rectangle=x64arm::gui_status_rectangle(model,model.status_simple?255:index);
            const NSRect bounds=NSMakeRect(rectangle[0],rectangle[1],std::max(0,rectangle[2]-rectangle[0]),std::max(0,rectangle[3]-rectangle[1]));if(NSIsEmptyRect(bounds))continue;
            [NSGraphicsContext saveGraphicsState];NSRectClip(bounds);
            if(!(part.flags&0x100U)){const BOOL pop=(part.flags&0x200U)!=0;
                [(pop?NSColor.whiteColor:NSColor.shadowColor) setFill];NSRectFill(NSMakeRect(bounds.origin.x,bounds.origin.y,bounds.size.width,1));NSRectFill(NSMakeRect(bounds.origin.x,bounds.origin.y,1,bounds.size.height));
                [(pop?NSColor.shadowColor:NSColor.whiteColor) setFill];NSRectFill(NSMakeRect(NSMaxX(bounds)-1,bounds.origin.y,1,bounds.size.height));NSRectFill(NSMakeRect(bounds.origin.x,NSMaxY(bounds)-1,bounds.size.width,1));
            }
            if(!(part.flags&0x1000U)){
                CGFloat x=bounds.origin.x+3;const CGFloat height=std::max<CGFloat>(0,bounds.size.height-4);
                if(part.icon_handle){NSImage* icon=list_icon(part.icon);const auto size=std::min<CGFloat>(16,height);[icon drawInRect:NSMakeRect(x,bounds.origin.y+(bounds.size.height-size)/2,size,size) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];x+=size+3;}
                NSString* text=nsstring(part.text);NSArray<NSString*>* segments=(part.flags&0x800U)?@[[text stringByReplacingOccurrencesOfString:@"\t" withString:@""]]:[text componentsSeparatedByString:@"\t"];
                const BOOL rtl=((model.ex_style&0x400000U)!=0)!=((part.flags&0x400U)!=0);
                for(NSUInteger column=0;column<std::min<NSUInteger>(3,segments.count);++column){NSMutableParagraphStyle* paragraph=[[NSMutableParagraphStyle alloc] init];paragraph.lineBreakMode=NSLineBreakByClipping;paragraph.alignment=column==1?NSTextAlignmentCenter:column==2?NSTextAlignmentRight:rtl?NSTextAlignmentRight:NSTextAlignmentLeft;paragraph.baseWritingDirection=rtl?NSWritingDirectionRightToLeft:NSWritingDirectionLeftToRight;
                    NSMutableDictionary* attributes=[@{NSFontAttributeName:font,NSForegroundColorAttributeName:model.enabled?NSColor.controlTextColor:NSColor.disabledControlTextColor,NSParagraphStyleAttributeName:paragraph} mutableCopy];
                    if(model.control_font.underline)attributes[NSUnderlineStyleAttributeName]=@(NSUnderlineStyleSingle);if(model.control_font.strikeout)attributes[NSStrikethroughStyleAttributeName]=@(NSUnderlineStyleSingle);
                    const auto text_height=[segments[column] sizeWithAttributes:attributes].height;[segments[column] drawInRect:NSMakeRect(x,bounds.origin.y+std::max<CGFloat>(2,(bounds.size.height-text_height)/2),std::max<CGFloat>(0,NSMaxX(bounds)-3-x),height) withAttributes:attributes];
                }
            }
            [NSGraphicsContext restoreGraphicsState];
        }
        return;
    }
    if(x64arm::gui_control_class(*self.model)==u"tooltips_class32" && self.model->tooltip){
        const auto& tip=*self.model->tooltip;
        const auto color=[](std::uint32_t value){return [NSColor colorWithRed:(value&255)/255.0 green:((value>>8)&255)/255.0 blue:((value>>16)&255)/255.0 alpha:1.0];};
        [color(tip.background) setFill];NSRectFill(self.bounds);
        const auto radius=(self.model->style&0x40U)?8:1;
        NSBezierPath* border=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,0.5,0.5) xRadius:radius yRadius:radius];
        [[NSColor colorWithWhite:0.35 alpha:1] setStroke];[border stroke];
        NSFont* font=guest_font(self.model->control_font);NSMutableParagraphStyle* paragraph=[[NSMutableParagraphStyle alloc] init];
        paragraph.lineBreakMode=NSLineBreakByClipping;paragraph.alignment=tip.rtl?NSTextAlignmentRight:NSTextAlignmentLeft;
        if(tip.rtl)paragraph.baseWritingDirection=NSWritingDirectionRightToLeft;
        const CGFloat left=tip.margin[0]+4,right=self.bounds.size.width-tip.margin[2]-4;
        CGFloat y=tip.margin[1]+4;
        if(!tip.title.empty()){
            const auto iconSize=tip.icon>=4 && tip.icon<=6?32:16;CGFloat x=left;
            if(tip.icon){NSImage* icon=nil;
                if(tip.icon>6 && !self.model->list_icons.empty())icon=list_icon(self.model->list_icons.front());
                else {const auto kind=(tip.icon-1)%3;icon=[NSImage imageWithSystemSymbolName:kind==0?@"info.circle.fill":kind==1?@"exclamationmark.triangle.fill":@"xmark.octagon.fill" accessibilityDescription:nil];}
                [icon drawInRect:NSMakeRect(x,y,iconSize,iconSize) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];x+=iconSize+4;
            }
            const BOOL close=(self.model->style&0x80U)!=0;
            NSFont* bold=[[NSFontManager sharedFontManager] convertFont:font toHaveTrait:NSBoldFontMask];
            [nsstring(tip.title) drawInRect:NSMakeRect(x,y,std::max<CGFloat>(0,right-x-(close?20:0)),tip.title_height) withAttributes:@{NSFontAttributeName:bold,NSForegroundColorAttributeName:color(tip.color),NSParagraphStyleAttributeName:paragraph}];
            if(close)[@"×" drawInRect:NSMakeRect(self.bounds.size.width-20,2,18,20) withAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:16],NSForegroundColorAttributeName:color(tip.color)}];
            y+=tip.title_height;
        }
        NSDictionary* attributes=@{NSFontAttributeName:font,NSForegroundColorAttributeName:color(tip.color),NSParagraphStyleAttributeName:paragraph};
        for(const auto& line:tip.lines){[nsstring(line) drawInRect:NSMakeRect(left,y,std::max<CGFloat>(0,right-left),tip.line_height+2) withAttributes:attributes];y+=tip.line_height;}
        return;
    }
    if(x64arm::gui_control_class(*self.model)==u"systabcontrol32" && self.model->tab){
        const auto& tab=*self.model->tab;NSFont* font=guest_font(self.model->control_font);
        [[NSColor windowBackgroundColor] setFill];NSRectFill(self.bounds);
        if(!(self.model->style&0x100U)){
            const auto& r=tab.display;[[NSColor separatorColor] setStroke];
            [NSBezierPath strokeRect:NSMakeRect(static_cast<CGFloat>(r[0])-1,static_cast<CGFloat>(r[1])-1,std::max<CGFloat>(0,static_cast<CGFloat>(r[2])-r[0]+2),std::max<CGFloat>(0,static_cast<CGFloat>(r[3])-r[1]+2))];
        }
        for(std::size_t i=0;i<tab.items.size() && i<tab.rectangles.size();++i){
            const auto& item=tab.items[i];const auto& r=tab.rectangles[i];const NSRect item_rect=NSMakeRect(r[0],r[1],static_cast<CGFloat>(r[2])-r[0],static_cast<CGFloat>(r[3])-r[1]);NSRect rect=item_rect;
            if(!NSIntersectsRect(rect,dirty))continue;
            const bool selected=static_cast<std::int32_t>(i)==self.model->selection || ((self.model->style&0x100U) && (item.state&1U));
            [(selected?[NSColor controlBackgroundColor]:[NSColor windowBackgroundColor]) setFill];
            NSBezierPath* shape=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect,0.5,0.5) xRadius:3 yRadius:3];[shape fill];[[NSColor separatorColor] setStroke];[shape stroke];
            if(selected){[(self.model->enabled?[NSColor controlAccentColor]:[NSColor disabledControlTextColor]) setFill];NSRectFill(NSMakeRect(rect.origin.x+2,rect.origin.y+1,std::max<CGFloat>(0,rect.size.width-4),2));}
            if(self.model->style&0x2000U)continue; // TCS_OWNERDRAWFIXED：項目內容由 WM_DRAWITEM 提供。
            [NSGraphicsContext saveGraphicsState];NSRectClip(NSInsetRect(rect,2,2));
            const bool sideways=(self.model->style&0x80U)!=0;
            if(sideways){NSAffineTransform* transform=[NSAffineTransform transform];[transform translateXBy:NSMinX(rect) yBy:NSMaxY(rect)];[transform rotateByDegrees:-90];[transform concat];rect=NSMakeRect(0,0,rect.size.height,rect.size.width);}
            NSMutableParagraphStyle* paragraph=[[NSMutableParagraphStyle alloc] init];paragraph.lineBreakMode=NSLineBreakByTruncatingTail;paragraph.alignment=NSTextAlignmentCenter;
            if(item.rtl)paragraph.baseWritingDirection=NSWritingDirectionRightToLeft;
            const auto color=!self.model->enabled?[NSColor disabledControlTextColor]:(item.state&2U)?[NSColor controlAccentColor]:[NSColor labelColor];
            NSDictionary* attributes=@{NSFontAttributeName:font,NSForegroundColorAttributeName:color,NSParagraphStyleAttributeName:paragraph};
            NSString* title=menu_title(item.text);const auto text_height=[title sizeWithAttributes:attributes].height;
            NSRect text=NSMakeRect(NSMinX(rect)+tab.padding_x,NSMidY(rect)-text_height/2,std::max<CGFloat>(0,rect.size.width-2*tab.padding_x),text_height);
            if(item.image>=0 && static_cast<std::size_t>(item.image)<self.model->list_icons.size()){
                const auto& source=self.model->list_icons[static_cast<std::size_t>(item.image)];NSImage* icon=[list_icon(source) copy];if(source.width && source.height)icon.size=NSMakeSize(source.width,source.height);
                if(icon){[icon drawInRect:NSMakeRect(NSMinX(text),NSMidY(rect)-icon.size.height/2,icon.size.width,icon.size.height) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:self.model->enabled?1:0.4 respectFlipped:YES hints:nil];text.origin.x+=icon.size.width+4;text.size.width=std::max<CGFloat>(0,text.size.width-icon.size.width-4);}
            }
            [title drawInRect:text withAttributes:attributes];[NSGraphicsContext restoreGraphicsState];
            if(tab.focused && static_cast<std::int32_t>(i)==tab.focus && !(self.model->style&0x8000U)){
                [[NSColor keyboardFocusIndicatorColor] setStroke];NSBezierPath* focus=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(item_rect,3,3) xRadius:2 yRadius:2];const CGFloat dash[]{2,2};[focus setLineDash:dash count:2 phase:0];[focus stroke];
            }
        }
        return;
    }
    if(x64arm::gui_control_class(*self.model)==u"syslink"){
        for(const auto& run:self.model->link_layout.runs){
            const auto& r=run.rectangle;NSColor* color=[NSColor labelColor];bool focused=false;
            if(run.link>=0 && static_cast<std::size_t>(run.link)<self.model->links.size()){
                const auto state=self.model->links[static_cast<std::size_t>(run.link)].state;focused=(state&1U)!=0;
                color=!self.model->enabled || !(state&2U)?[NSColor disabledControlTextColor]:(state&16U)?[NSColor labelColor]:(state&8U)?[NSColor selectedControlColor]:(state&4U)?[NSColor purpleColor]:[NSColor linkColor];
            }
            NSFont* font=guest_font(self.model->control_font);NSMutableDictionary* attributes=[@{NSFontAttributeName:font,NSForegroundColorAttributeName:color} mutableCopy];
            if(run.link>=0)attributes[NSUnderlineStyleAttributeName]=@(NSUnderlineStyleSingle);
            [NSGraphicsContext saveGraphicsState];NSAffineTransform* transform=[NSAffineTransform transform];[transform translateXBy:r[0] yBy:r[1]];[transform scaleXBy:font_width_scale(self.model->control_font,font) yBy:1];[transform concat];
            [nsstring(self.model->link_text.substr(run.start,run.length)) drawAtPoint:NSZeroPoint withAttributes:attributes];[NSGraphicsContext restoreGraphicsState];
            if(focused){[color setStroke];NSBezierPath* outline=[NSBezierPath bezierPathWithRect:NSMakeRect(r[0],r[1],r[2]-r[0],r[3]-r[1])];CGFloat dash[]{1,2};[outline setLineDash:dash count:2 phase:0];[outline stroke];}
        }
        return;
    }
    for (const auto& draw : self.model->drawing) {
        NSColor* color = [NSColor colorWithRed:(draw.color & 255) / 255.0 green:((draw.color >> 8) & 255) / 255.0 blue:((draw.color >> 16) & 255) / 255.0 alpha:1.0];
        [NSGraphicsContext saveGraphicsState];if(draw.clipped)NSRectClip(NSMakeRect(draw.clip[0],draw.clip[1],std::max(0,draw.clip[2]-draw.clip[0]),std::max(0,draw.clip[3]-draw.clip[1])));
        for(const auto& rect:draw.excluded){NSBezierPath* region=[NSBezierPath bezierPathWithRect:self.bounds];region.windingRule=NSWindingRuleEvenOdd;[region appendBezierPathWithRect:NSIntersectionRect(self.bounds,NSMakeRect(rect[0],rect[1],std::max(0,rect[2]-rect[0]),std::max(0,rect[3]-rect[1])))];[region addClip];}
        if(draw.transform!=x64arm::gui_identity){NSAffineTransform* world=[NSAffineTransform transform];world.transformStruct={draw.transform[0],draw.transform[1],draw.transform[2],draw.transform[3],draw.transform[4],draw.transform[5]};[world concat];}
        if(draw.kind==x64arm::GuiDraw::Kind::Fill){[color setFill];NSRectFill(NSMakeRect(draw.x,draw.y,static_cast<CGFloat>(draw.right)-draw.x,static_cast<CGFloat>(draw.bottom)-draw.y));}
        else if(draw.kind==x64arm::GuiDraw::Kind::Line || draw.kind==x64arm::GuiDraw::Kind::Rectangle){[color setStroke];NSBezierPath* path=[NSBezierPath bezierPath];if(draw.kind==x64arm::GuiDraw::Kind::Line){[path moveToPoint:NSMakePoint(draw.x,draw.y)];[path lineToPoint:NSMakePoint(draw.right,draw.bottom)];}else [path appendBezierPathWithRect:NSMakeRect(draw.x,draw.y,draw.right-draw.x,draw.bottom-draw.y)];[path stroke];}
        else if(draw.kind==x64arm::GuiDraw::Kind::Icon){NSImage* icon=nil;
            if(!draw.image.empty())icon=guest_icon_image(draw.image);
            else if(draw.system_icon==32513 || draw.system_icon==32514 || draw.system_icon==32516)icon=[NSImage imageWithSystemSymbolName:draw.system_icon==32513?@"xmark.octagon.fill":draw.system_icon==32514?@"questionmark.circle":@"info.circle" accessibilityDescription:nil];
            else icon=[NSImage imageNamed:draw.system_icon==32515?NSImageNameCaution:NSImageNameApplicationIcon];
            [icon drawInRect:NSMakeRect(draw.x,draw.y,draw.right-draw.x,draw.bottom-draw.y) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];}
        else if(draw.glyph_indices){
            const auto raster=x64arm::native_gdi_glyph_raster(draw);if(!raster.alpha.empty()){
                const auto width=raster.bounds[2]-raster.bounds[0],height=raster.bounds[3]-raster.bounds[1];
                auto* rep=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:nullptr pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:width*4 bitsPerPixel:32];
                if(!rep || !rep.bitmapData)throw std::bad_alloc();
                for(std::size_t i=0;i<static_cast<std::size_t>(width)*static_cast<std::size_t>(height);++i){auto* pixel=rep.bitmapData+i*4;for(unsigned c=0;c<3;++c)pixel[c]=static_cast<std::uint8_t>(draw.color>>(c*8));unsigned alpha=0;for(unsigned c=0;c<raster.channels;++c)alpha+=raster.alpha[i*raster.channels+c];pixel[3]=static_cast<std::uint8_t>((alpha+raster.channels/2)/raster.channels);}
                auto* image=[[NSImage alloc]initWithSize:NSMakeSize(width,height)];[image addRepresentation:rep];[image drawInRect:NSMakeRect(static_cast<CGFloat>(draw.x)+raster.bounds[0],static_cast<CGFloat>(draw.y)+raster.bounds[1],width,height) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
            }
        }else {
            NSString* text=[[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(draw.text.data()) length:draw.text.size()];
            NSFont* font=guest_font(draw.font);NSMutableDictionary* attributes=[@{NSFontAttributeName:font,NSForegroundColorAttributeName:color} mutableCopy];
            if(draw.font.underline)attributes[NSUnderlineStyleAttributeName]=@(NSUnderlineStyleSingle);
            if(draw.font.strikeout)attributes[NSStrikethroughStyleAttributeName]=@(NSUnderlineStyleSingle);
            if(draw.rtl_reading)attributes[NSWritingDirectionAttributeName]=@[@(static_cast<NSInteger>(NSWritingDirectionRightToLeft)|static_cast<NSInteger>(NSWritingDirectionEmbedding))];
            const auto scale=font_width_scale(draw.font,font);NSAffineTransform* transform=[NSAffineTransform transform];[transform translateXBy:draw.x yBy:draw.y];[transform scaleXBy:scale yBy:1];[transform concat];
            if(draw.cell_width || !draw.advances.empty()){CGFloat x=0,y=0;for(NSUInteger i=0;i<text.length;){const auto range=[text rangeOfComposedCharacterSequenceAtIndex:i];NSString* cluster=[text substringWithRange:range];[cluster drawAtPoint:NSMakePoint(x/scale,y) withAttributes:attributes];
                if(draw.advances.empty())x+=std::round([cluster sizeWithAttributes:attributes].width/average_advance(font))*draw.cell_width;
                else for(NSUInteger j=0;j<range.length;++j){x+=i+j<draw.advances.size()?draw.advances[i+j]:guest_metrics(draw.font).average_width;y+=i+j<draw.advances_y.size()?draw.advances_y[i+j]:0;}
                i+=range.length;}}
            else [text drawAtPoint:NSZeroPoint withAttributes:attributes];
        }
        [NSGraphicsContext restoreGraphicsState];
    }
}
- (void)mouseDown:(NSEvent*)event {
    [self guestMouse:event message:event.clickCount>1?0x203U:0x201U button:1 pressed:YES];
}
- (void)guestMouse:(NSEvent*)event message:(unsigned int)message button:(unsigned int)button pressed:(BOOL)pressed {
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    self.buttons=pressed?self.buttons|button:self.buttons&~button;
    const std::uint32_t modifiers=((event.modifierFlags&NSEventModifierFlagShift)?1U:0U)|((event.modifierFlags&NSEventModifierFlagControl)?2U:0U)|((event.modifierFlags&NSEventModifierFlagOption)?4U:0U);
    const auto buttons=self.buttons|((modifiers&1)?4U:0U)|((modifiers&2)?8U:0U);
    x64arm::GuiMessage input{self.guestHandle,buttons,(static_cast<std::uint64_t>(coordinate_word(point.y))<<16)|coordinate_word(point.x),message,{},modifiers};
    // 每個 NSEvent 保存自己的完整 client POINT；批次 pump 時不能用較晚的游標位置替代。
    input.has_point=true;input.point={static_cast<std::int32_t>(std::clamp<CGFloat>(point.x,INT32_MIN,INT32_MAX)),static_cast<std::int32_t>(std::clamp<CGFloat>(point.y,INT32_MIN,INT32_MAX))};
    self.events->push_back(std::move(input));
}
- (void)mouseUp:(NSEvent*)event { [self guestMouse:event message:0x202 button:1 pressed:NO]; }
- (void)mouseMoved:(NSEvent*)event {
    self.buttons=guest_mouse_buttons(NSEvent.pressedMouseButtons);
    [self guestMouse:event message:0x200 button:0 pressed:NO];
}
- (void)mouseDragged:(NSEvent*)event { [self guestMouse:event message:0x200 button:1 pressed:YES]; }
- (void)rightMouseDown:(NSEvent*)event { [self guestMouse:event message:event.clickCount>1?0x206U:0x204U button:2 pressed:YES]; }
- (void)rightMouseUp:(NSEvent*)event { [self guestMouse:event message:0x205 button:2 pressed:NO]; }
- (void)rightMouseDragged:(NSEvent*)event { [self guestMouse:event message:0x200 button:2 pressed:YES]; }
- (void)otherMouseDown:(NSEvent*)event { if(event.buttonNumber==2){[self guestMouse:event message:event.clickCount>1?0x209U:0x207U button:16 pressed:YES];} }
- (void)otherMouseUp:(NSEvent*)event { if(event.buttonNumber==2)[self guestMouse:event message:0x208 button:16 pressed:NO]; }
- (void)otherMouseDragged:(NSEvent*)event { if(event.buttonNumber==2)[self guestMouse:event message:0x200 button:16 pressed:YES]; }
- (void)guestScrollAt:(NSPoint)point vertical:(CGFloat)vertical horizontal:(CGFloat)horizontal precise:(BOOL)precise modifiers:(NSEventModifierFlags)flags {
    if(!std::isfinite(vertical) || !std::isfinite(horizontal))return;
    const std::uint32_t modifiers=((flags&NSEventModifierFlagShift)?1U:0U)|((flags&NSEventModifierFlagControl)?2U:0U)|((flags&NSEventModifierFlagOption)?4U:0U);
    const auto buttons=self.buttons|((modifiers&1)?4U:0U)|((modifiers&2)?8U:0U);
    // WM_MOUSE(W|H)WHEEL 的 lParam 是客體螢幕座標，不能沿用按鍵訊息的 client 座標。
    for(NSView* view=self;view;view=view.superview)if([view isKindOfClass:[X64ArmGuestView class]]){const auto* model=((X64ArmGuestView*)view).model;if(model){point.x+=model->x;point.y+=model->y;}}
    const auto position=(static_cast<std::uint64_t>(coordinate_word(point.y))<<16)|coordinate_word(point.x);
    const auto send=[&](double amount,unsigned message,double remainder){
        const auto total=std::clamp(amount*(precise?3.0:120.0)+remainder,-32767.0,32767.0);
        const auto delta=static_cast<std::int16_t>(total);
        if(delta)self.events->push_back({self.guestHandle,buttons|(static_cast<std::uint64_t>(static_cast<std::uint16_t>(delta))<<16),position,message,{},modifiers});
        return total-delta;
    };
    self.verticalRemainder=send(vertical,0x20a,self.verticalRemainder);
    self.horizontalRemainder=send(-horizontal,0x20e,self.horizontalRemainder);
}
- (void)scrollWheel:(NSEvent*)event {
    [self guestScrollAt:[self convertPoint:event.locationInWindow fromView:nil] vertical:event.scrollingDeltaY horizontal:event.scrollingDeltaX precise:event.hasPreciseScrollingDeltas modifiers:event.modifierFlags];
}
- (void)keyDown:(NSEvent*)event {
    self.imeKeyEvent=event;
    if(event.modifierFlags&(NSEventModifierFlagControl|NSEventModifierFlagOption)){[self guestKeyDown:event];self.imeKeyEvent=nil;return;}
    [self interpretKeyEvents:@[event]];
    self.imeKeyEvent=nil;
}
- (void)guestKeyDown:(NSEvent*)event {[self guestKey:event pressed:YES];}
- (void)keyUp:(NSEvent*)event {[self guestKey:event pressed:NO];}
- (void)flagsChanged:(NSEvent*)event {native_guest_modifier(self.guestHandle,self.events,pressedGuestKeys,event);}
- (void)guestKey:(NSEvent*)event pressed:(BOOL)pressed {native_guest_key(self.guestHandle,self.events,pressedGuestKeys,event,pressed);}
- (BOOL)hasMarkedText {return imeData.composing;}
- (NSRange)markedRange {return imeData.composing?NSMakeRange(0,imeData.text.size()):NSMakeRange(NSNotFound,0);}
- (NSRange)selectedRange {return imeSelection;}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {return @[NSUnderlineStyleAttributeName,NSMarkedClauseSegmentAttributeName];}
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if(!imeData.composing || range.location>imeData.text.size())return nil;
    const auto bounded=NSMakeRange(range.location,std::min<NSUInteger>(range.length,imeData.text.size()-range.location));
    if(actualRange)*actualRange=bounded;return [[NSAttributedString alloc] initWithString:nsstring(imeData.text.substr(bounded.location,bounded.length))];
}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if(actualRange)*actualRange=NSMakeRange(std::min<NSUInteger>(range.location,imeData.text.size()),0);
    NSRect rect=[self convertRect:NSMakeRect(imePoint.x,imePoint.y,1,20) toView:nil];return [self.window convertRectToScreen:rect];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {(void)point;return imeSelection.location;}
- (void)updateImeOverlay {
    if(!imeData.composing || imeData.text.empty()){self.imeOverlay.hidden=YES;return;}
    if(!self.imeOverlay){self.imeOverlay=[NSTextField labelWithString:@""];self.imeOverlay.backgroundColor=[NSColor textBackgroundColor];self.imeOverlay.drawsBackground=YES;[self addSubview:self.imeOverlay];}
    auto value=[[NSMutableAttributedString alloc] initWithString:nsstring(imeData.text) attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:14],NSUnderlineStyleAttributeName:@(NSUnderlineStyleSingle)}];
    self.imeOverlay.attributedStringValue=value;self.imeOverlay.hidden=NO;[self.imeOverlay sizeToFit];
    auto frame=self.imeOverlay.frame;frame.origin=imePoint;self.imeOverlay.frame=frame;
}
- (void)setMarkedText:(id)value selectedRange:(NSRange)selected replacementRange:(NSRange)replacement {
    if(!imeSilent)imeFromInputMethod=YES;
    NSString* string=[value isKindOfClass:[NSAttributedString class]]?[value string]:value;
    if(![string isKindOfClass:[NSString class]] || string.length>32767)return;
    auto text=gueststring(string);
    if(replacement.location!=NSNotFound && replacement.location<=imeData.text.size() && replacement.length<=imeData.text.size()-replacement.location){auto old=imeData.text;old.replace(replacement.location,replacement.length,text);text=std::move(old);}
    const auto original=imeData.composing?imeData.original:text;imeData={};imeData.text=std::move(text);imeData.original=original;imeData.composing=!imeData.text.empty();
    imeSelection=NSMakeRange(std::min<NSUInteger>(selected.location,imeData.text.size()),0);imeSelection.length=std::min<NSUInteger>(selected.length,imeData.text.size()-imeSelection.location);
    imeData.cursor=static_cast<std::uint32_t>(imeSelection.location);imeData.attributes.assign(imeData.text.size(),0);imeData.clauses.push_back(0);
    for(std::uint32_t i=0;i<imeData.text.size();++i){const auto attr=i>=imeSelection.location && i<NSMaxRange(imeSelection)?3U:0U;imeData.attributes[i]=static_cast<std::uint8_t>(attr);if(i && imeData.attributes[i]!=imeData.attributes[i-1])imeData.clauses.push_back(i);}
    imeData.clauses.push_back(static_cast<std::uint32_t>(imeData.text.size()));imeData.reading_clauses={0,0};[self updateImeOverlay];
    if(!imeSilent && self.events){x64arm::GuiMessage msg{self.guestHandle,0,imeData.composing?0x1bfU:0U,0x10f};msg.ime=imeData;self.events->push_back(std::move(msg));}
}
- (void)insertText:(id)value replacementRange:(NSRange)replacement {
    (void)replacement;NSString* string=[value isKindOfClass:[NSAttributedString class]]?[value string]:value;
    if(![string isKindOfClass:[NSString class]] || string.length>32767)return;const auto text=gueststring(string);
    if(imeData.composing){imeData.result=text;imeData.result_clauses={0,static_cast<std::uint32_t>(text.size())};imeData.result_reading=imeData.reading;imeData.result_reading_clauses=imeData.reading_clauses;imeData.has_result=true;imeData.composing=false;imeData.text.clear();
        if(!imeSilent && self.events){x64arm::GuiMessage msg{self.guestHandle,0,0x1e00,0x10f};msg.ime=imeData;self.events->push_back(std::move(msg));}}
    else if(!imeSilent && self.events){
        // 只有原生單鍵直接提交的 ASCII 走按鍵翻譯；IME、貼上及多字元提交保留文字路徑。
        if(self.imeKeyEvent && !(self.imeKeyEvent.modifierFlags&NSEventModifierFlagCapsLock) && text.size()==1 && text[0]>=32 && text[0]<127 &&
           [string isEqualToString:self.imeKeyEvent.characters])[self guestKeyDown:self.imeKeyEvent];
        else for(const auto c:text)self.events->push_back({self.guestHandle,c,1,0x102});
    }
    [self updateImeOverlay];imeSelection=NSMakeRange(0,0);
}
- (void)unmarkText {if(imeData.composing)[self insertText:nsstring(imeData.text) replacementRange:NSMakeRange(NSNotFound,0)];}
- (void)doCommandBySelector:(SEL)selector {(void)selector;if(self.imeKeyEvent)[self guestKeyDown:self.imeKeyEvent];}
@end
// 一般按鈕保留 AppKit 行為；BS_OWNERDRAW 將原始輸入交回客體，不在 tracking loop 阻塞繪圖。
@interface X64ArmGuestButton : NSButton {
    std::array<std::uint32_t,256> pressedGuestKeys;
}
@property(nonatomic) BOOL guestUpdating;
@property(nonatomic) x64arm::GuiWindow* model;
@property(nonatomic) unsigned long long guestHandle;
@property(nonatomic) std::vector<x64arm::GuiMessage>* events;
- (BOOL)isOwnerDraw;
- (void)buttonMouse:(NSEvent*)event message:(unsigned)message;
@end
@implementation X64ArmGuestButton
- (BOOL)isOwnerDraw {return self.model && (self.model->style&15U)==11;}
- (void)drawRect:(NSRect)dirty {if(!self.isOwnerDraw)[super drawRect:dirty];}
- (BOOL)acceptsFirstResponder {if(!self.model || !self.events)return NO;return self.enabled;}
- (BOOL)becomeFirstResponder {const BOOL result=[super becomeFirstResponder];if(result && !self.guestUpdating && self.events)self.events->push_back({self.guestHandle,0,0,0x10013});return result;}
- (BOOL)resignFirstResponder {return [super resignFirstResponder];}
- (void)buttonMouse:(NSEvent*)event message:(unsigned)message {
    if(!self.enabled || !self.events)return;
    NSPoint point=[self convertPoint:event.locationInWindow fromView:nil];if(!self.isFlipped)point.y=self.bounds.size.height-point.y;
    const std::uint32_t modifiers=((event.modifierFlags&NSEventModifierFlagShift)?1U:0U)|((event.modifierFlags&NSEventModifierFlagControl)?2U:0U)|((event.modifierFlags&NSEventModifierFlagOption)?4U:0U);
    self.events->push_back({self.guestHandle,(message==0x202?0U:1U)|((modifiers&1U)?4U:0U)|((modifiers&2U)?8U:0U),(static_cast<std::uint64_t>(coordinate_word(point.y))<<16)|coordinate_word(point.x),message,{},modifiers});
}
- (void)mouseDown:(NSEvent*)event {if(!self.model || !self.events)return;if(!self.isOwnerDraw){[super mouseDown:event];return;}if(self.enabled){[self.window makeFirstResponder:self];[self buttonMouse:event message:0x201];}}
- (void)mouseDragged:(NSEvent*)event {if(!self.model || !self.events)return;if(self.isOwnerDraw)[self buttonMouse:event message:0x200];else [super mouseDragged:event];}
- (void)mouseUp:(NSEvent*)event {if(!self.model || !self.events)return;if(self.isOwnerDraw)[self buttonMouse:event message:0x202];else [super mouseUp:event];}
- (void)keyDown:(NSEvent*)event {if(self.model && self.events && self.enabled)native_guest_key(self.guestHandle,self.events,pressedGuestKeys,event,true);}
- (void)keyUp:(NSEvent*)event {if(self.model && self.events && self.enabled)native_guest_key(self.guestHandle,self.events,pressedGuestKeys,event,false);}
- (void)flagsChanged:(NSEvent*)event {if(self.model && self.events && self.enabled)native_guest_modifier(self.guestHandle,self.events,pressedGuestKeys,event);}
- (void)performClick:(id)sender {if(!self.model || !self.events)return;if(self.isOwnerDraw){if(self.enabled && self.events){[self.window makeFirstResponder:self];self.events->push_back({self.guestHandle,0,0,0xf5});}}else [super performClick:sender];}
@end
@interface X64ArmMenuContainer : NSView
@end
@implementation X64ArmMenuContainer
- (BOOL)isFlipped {return YES;}
@end
@interface X64ArmMenuBar : X64ArmMenuContainer
@end
@implementation X64ArmMenuBar
- (void)drawRect:(NSRect)rect {(void)rect;[[NSColor controlBackgroundColor] setFill];NSRectFill(self.bounds);}
@end
@interface X64ArmWindowDelegate : NSObject<NSWindowDelegate>
@property(nonatomic) unsigned long long guestHandle;
@property(nonatomic) std::vector<x64arm::GuiMessage>* events;
@property(nonatomic,strong) NSMenu* mainMenu;
@property(nonatomic,strong) NSView* clientView;
@property(nonatomic) BOOL updating;
@property(nonatomic) BOOL moving;
@property(nonatomic) x64arm::GuiWindow* model;
@end
@implementation X64ArmWindowDelegate
- (void)windowDidBecomeKey:(NSNotification*)notification {(void)notification;if(self.mainMenu)NSApp.mainMenu=self.mainMenu;if(!self.updating && self.events)self.events->push_back({self.guestHandle,1,0,0x1000d});}
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender; self.events->push_back({self.guestHandle, 0, 0, 0x10}); return NO;
}
- (void)publishGeometry:(NSWindow*)window state:(unsigned)state {
    if(self.updating || !self.model)return;
    const auto size=self.clientView?self.clientView.bounds.size:window.contentView.bounds.size;
    const auto screen=background_display()?:window.screen?:NSScreen.mainScreen;
    x64arm::GuiMessage event{self.guestHandle,state,(static_cast<std::uint64_t>(coordinate_word(size.height))<<16)|coordinate_word(size.width),0x1000c};
    event.has_point=true;event.point={static_cast<std::int32_t>(window.frame.origin.x-(background_display()?screen.frame.origin.x:0)),static_cast<std::int32_t>(NSMaxY(screen.frame)-NSMaxY(window.frame))};
    self.model->show_command=state==1?2:state==2?3:1;
    if(state!=1){self.model->width=static_cast<std::uint32_t>(size.width);self.model->height=static_cast<std::uint32_t>(size.height);self.model->x=event.point[0];self.model->y=event.point[1];}
    self.events->push_back(std::move(event));
}
- (void)windowDidResize:(NSNotification*)notification {[self publishGeometry:notification.object state:[notification.object isMiniaturized]?1:[notification.object isZoomed]?2:0];}
- (void)windowWillMove:(NSNotification*)notification {(void)notification;self.moving=!self.updating;}
- (void)windowDidMove:(NSNotification*)notification {if(self.moving || NSApp.currentEvent.type==NSEventTypeLeftMouseDragged){[self publishGeometry:notification.object state:[notification.object isMiniaturized]?1:self.model->show_command==3?2:0];self.moving=NO;}}
- (void)windowDidMiniaturize:(NSNotification*)notification {[self publishGeometry:notification.object state:1];}
- (void)windowDidDeminiaturize:(NSNotification*)notification {[self publishGeometry:notification.object state:self.model->restore_maximized?2:0];}
- (void)windowWillStartLiveResize:(NSNotification*)notification {(void)notification;if(!self.updating)self.events->push_back({self.guestHandle,0,0,0x231});}
- (void)windowDidEndLiveResize:(NSNotification*)notification {(void)notification;if(!self.updating)self.events->push_back({self.guestHandle,0,0,0x232});}

@end
namespace {
NSString* label(const std::u16string& text){std::u16string result;for(std::size_t i=0;i<text.size();++i){if(text[i]!=u'&')result.push_back(text[i]);else if(i+1<text.size() && text[i+1]==u'&'){result.push_back(u'&');++i;}}return nsstring(result);}
NSImage* list_icon(const x64arm::GuiIcon& icon){
    if(icon.width && icon.height && icon.rgba.size()==static_cast<std::size_t>(icon.width)*icon.height){
        auto* bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:icon.width pixelsHigh:icon.height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:icon.width*4 bitsPerPixel:32];
        auto* pixels=bitmap.bitmapData;for(std::size_t i=0;i<icon.rgba.size();++i)for(unsigned n=0;n<4;++n)pixels[i*4+n]=static_cast<unsigned char>(icon.rgba[i]>>(n*8U));
        auto* image=[[NSImage alloc] initWithSize:NSMakeSize(icon.width,icon.height)];[image addRepresentation:bitmap];return image;
    }
    if(!icon.image.empty())return guest_icon_image(icon.image);
    if(icon.system>=0x20000U && icon.system<0x20900U){
        NSArray<NSString*>* symbols=nil;const auto group=(icon.system-0x20000U)/256U,index=(icon.system-0x20000U)%256U;
        if(group==0)symbols=@[@"scissors",@"doc.on.doc",@"doc.on.clipboard",@"arrow.uturn.backward",@"arrow.uturn.forward",@"trash",@"doc.badge.plus",@"folder",@"square.and.arrow.down",@"doc.text.magnifyingglass",@"info.circle",@"questionmark.circle",@"magnifyingglass",@"arrow.triangle.2.circlepath",@"printer"];
        else if(group==4)symbols=@[@"square.grid.2x2",@"square.grid.3x3",@"list.bullet",@"list.bullet.rectangle",@"textformat.abc",@"arrow.up.arrow.down",@"calendar",@"doc",@"arrow.up",@"network",@"network.slash",@"folder.badge.plus",@"list.bullet"];
        else if(group==8)symbols=@[@"arrow.left",@"arrow.right",@"star",@"star.badge.plus",@"list.bullet.indent"];
        if(index<symbols.count){NSImage* image=[[NSImage imageWithSystemSymbolName:symbols[index] accessibilityDescription:nil] copy];image.size=NSMakeSize(icon.width,icon.height);return image;}
    }
    // 平台內部 stock 圖示分類；不依副檔名或個別程式選擇圖示。
    if(icon.system==0x10001)return [NSImage imageNamed:NSImageNameMultipleDocuments];
    if(icon.system==0x10002)return [NSImage imageNamed:NSImageNameFolder];
    if(icon.system==0x10003)return [NSImage imageNamed:NSImageNameComputer];
    return [NSImage imageNamed:NSImageNameApplicationIcon];
}
NSImage* menu_bitmap(const x64arm::GuiIcon& bitmap,bool blank=false){
    if(bitmap.rgba.empty())return blank?[[NSImage alloc] initWithSize:NSMakeSize(18,18)]:nil;
    NSImage* image=list_icon(bitmap);[image setTemplate:bitmap.monochrome];return image;
}
void menu_bitmaps(NSMenuItem* item,const x64arm::GuiPopupItem& entry){
    if(item.separatorItem)return;
    item.image=menu_bitmap(entry.bitmap);
    if(entry.custom_checkmarks){item.onStateImage=menu_bitmap(entry.checked_bitmap,true);item.offStateImage=menu_bitmap(entry.unchecked_bitmap,true);}
}
NSDictionary* menu_bitmap_details(NSImage* image){
    if(!image)return @{@"present":@NO};
    std::uint64_t opaque=0,transparent=0,checksum=UINT64_C(14695981039346656037);
    for(NSImageRep* representation in image.representations)if([representation isKindOfClass:NSBitmapImageRep.class]){
        auto* bitmap=(NSBitmapImageRep*)representation;
        if(bitmap.samplesPerPixel!=4 || bitmap.bitsPerSample!=8)continue;
        for(NSInteger y=0;y<bitmap.pixelsHigh;++y)for(NSInteger x=0;x<bitmap.pixelsWide;++x){NSUInteger pixel[4]{};[bitmap getPixel:pixel atX:x y:y];if(pixel[3])++opaque;else ++transparent;
            for(unsigned component=0;component<4;++component){checksum^=pixel[component];checksum*=UINT64_C(1099511628211);}}
    }
    return @{@"present":@YES,@"width":@(image.size.width),@"height":@(image.size.height),@"template":@([image isTemplate]),@"opaque_pixels":@(opaque),@"transparent_pixels":@(transparent),@"pixel_checksum":@(checksum)};
}
}
// 原生 Header 只繪製與送入實際事件；欄位變更、否決及拖曳由共用 Win32 流程決定。
@interface X64ArmHeaderView : NSTableHeaderView
@property(nonatomic,assign) x64arm::GuiWindow* model;
@property(nonatomic,assign) const x64arm::GuiHeaderRenderStyle* paintStyle;
@property(nonatomic) BOOL rasterManaged;
@property(nonatomic,assign) std::vector<x64arm::GuiMessage>* events;
@property(nonatomic,assign) std::uint64_t guestHandle;
@property(nonatomic,assign) unsigned int buttons;
- (void)guestMouse:(NSEvent*)event message:(unsigned int)message button:(unsigned int)button pressed:(BOOL)pressed;
@end
@implementation X64ArmHeaderView
- (BOOL)isFlipped{return YES;}
- (BOOL)acceptsFirstResponder{return YES;}
- (BOOL)acceptsFirstMouse:(NSEvent*)event{(void)event;return YES;}
- (void)drawRect:(NSRect)dirty{
    if(self.rasterManaged)return;
    // NSTableHeaderView 的快照與離屏繪製仍須限制在客體控制項邊界。
    (void)dirty;NSRectClip(self.bounds);[NSColor.controlBackgroundColor setFill];NSRectFill(self.bounds);if(!self.model || !self.model->header)return;
    const auto& model=*self.model;const auto& header=*model.header;NSFont* font=guest_font(model.control_font);
    for(std::size_t i=0;i<model.columns.size() && i<header.rectangles.size();++i){const auto& item=model.columns[i];const auto& r=header.rectangles[i];const auto full=NSMakeRect(r[0],r[1],std::max(0,r[2]-r[0]),std::max(0,r[3]-r[1]));auto rect=full;if(model.style&0x100U)rect.size.height=(r[3]-r[1])/2;if(NSIsEmptyRect(rect))continue;
        [NSGraphicsContext saveGraphicsState];NSRectClip(rect);
        if(self.paintStyle && self.paintStyle->background_color){const auto color=*self.paintStyle->background_color;[[NSColor colorWithRed:(color&255)/255.0 green:((color>>8)&255)/255.0 blue:((color>>16)&255)/255.0 alpha:1]setFill];NSRectFill(rect);}
        if(header.pressed==item.identity && !header.tracking){[NSColor.selectedControlColor setFill];NSRectFill(rect);}
        [NSColor.separatorColor setStroke];NSBezierPath* border=[NSBezierPath bezierPathWithRect:NSInsetRect(rect,0.5,0.5)];[border stroke];
        if(!self.paintStyle || !self.paintStyle->frame_only){
        CGFloat left=NSMinX(rect)+6,right=NSMaxX(rect)-6;
        if((model.style&0x400U) && (item.format&0x40)){const auto box=NSMakeRect(left,NSMidY(rect)-6,12,12);[NSColor.labelColor setStroke];[[NSBezierPath bezierPathWithRect:box] stroke];if(item.format&0x80){NSBezierPath* check=[NSBezierPath bezierPath];[check moveToPoint:NSMakePoint(left+2,NSMidY(rect))];[check lineToPoint:NSMakePoint(left+5,NSMidY(rect)+3)];[check lineToPoint:NSMakePoint(left+10,NSMidY(rect)-4)];[check stroke];}left+=18;}
        if(item.format&0x600){const auto x=right-5,y=NSMidY(rect);NSBezierPath* triangle=[NSBezierPath bezierPath];[triangle moveToPoint:NSMakePoint(x-4,y+((item.format&0x400)?3:-3))];[triangle lineToPoint:NSMakePoint(x+4,y+((item.format&0x400)?3:-3))];[triangle lineToPoint:NSMakePoint(x,y+((item.format&0x400)?-3:3))];[triangle closePath];[NSColor.labelColor setFill];[triangle fill];right-=14;}
        if(item.format&0x1000000){[NSColor.labelColor setStroke];NSBezierPath* arrow=[NSBezierPath bezierPath];const auto center=(model.ex_style&0x400000U)?left+6:right-6;[arrow moveToPoint:NSMakePoint(center-4,NSMidY(rect)-2)];[arrow lineToPoint:NSMakePoint(center,NSMidY(rect)+2)];[arrow lineToPoint:NSMakePoint(center+4,NSMidY(rect)-2)];[arrow stroke];if(model.ex_style&0x400000U)left+=18;else right-=18;}
        NSImage* image=nil;if((item.format&0x800) && item.image>=0 && static_cast<std::size_t>(item.image)<model.list_icons.size())image=list_icon(model.list_icons[static_cast<std::size_t>(item.image)]);
        if((item.format&0x2000) && i<header.bitmap_images.size() && !header.bitmap_images[i].rgba.empty())image=list_icon(header.bitmap_images[i]);
        if(image){const auto height=std::min(image.size.height,std::max<CGFloat>(0,NSHeight(rect)-4));const auto width=image.size.height>0?image.size.width*height/image.size.height:0;const bool trailing=(item.format&0x1000)!=0;const auto x=trailing?right-width:left;[image drawInRect:NSMakeRect(x,NSMidY(rect)-height/2,width,height) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:model.enabled?1:0.5 respectFlipped:YES hints:nil];if(trailing)right-=width+header.bitmap_margin;else left+=width+header.bitmap_margin;}
        if((item.format&0x4000) && right>left){NSMutableParagraphStyle* paragraph=[NSMutableParagraphStyle new];paragraph.lineBreakMode=NSLineBreakByTruncatingTail;paragraph.alignment=(item.format&3)==1?NSTextAlignmentRight:(item.format&3)==2?NSTextAlignmentCenter:NSTextAlignmentLeft;paragraph.baseWritingDirection=((model.ex_style&0x2000U)!=0)^((item.format&4)!=0)?NSWritingDirectionRightToLeft:NSWritingDirectionLeftToRight;
            const auto scale=font_width_scale(model.control_font,font);NSAffineTransform* transform=[NSAffineTransform transform];[transform translateXBy:left yBy:std::max<CGFloat>(0,NSMidY(rect)-(font.ascender-font.descender)/2)];[transform scaleXBy:scale yBy:1];[transform concat];
            NSColor* textColor=model.enabled?NSColor.labelColor:NSColor.disabledControlTextColor;if(self.paintStyle && self.paintStyle->text_color){const auto color=*self.paintStyle->text_color;textColor=[NSColor colorWithRed:(color&255)/255.0 green:((color>>8)&255)/255.0 blue:((color>>16)&255)/255.0 alpha:1];}
            [nsstring(item.text) drawInRect:NSMakeRect(0,0,(right-left)/scale,NSHeight(rect)) withAttributes:@{NSFontAttributeName:font,NSForegroundColorAttributeName:textColor,NSParagraphStyleAttributeName:paragraph}];
        }
        }
        [NSGraphicsContext restoreGraphicsState];
        if(model.style&0x100U){
            auto filter=full;filter.origin.y+=rect.size.height;filter.size.height-=rect.size.height;auto button=filter;button.size.width=std::min<CGFloat>(18,NSWidth(filter));
            if(model.ex_style&0x400000U){filter.origin.x+=button.size.width;filter.size.width-=button.size.width;}else {filter.size.width-=button.size.width;button.origin.x+=filter.size.width;}
            [NSGraphicsContext saveGraphicsState];NSRectClip(full);[NSColor.textBackgroundColor setFill];NSRectFill(filter);[NSColor.separatorColor setStroke];[[NSBezierPath bezierPathWithRect:NSInsetRect(filter,0.5,0.5)] stroke];[[NSBezierPath bezierPathWithRect:NSInsetRect(button,0.5,0.5)] stroke];
            NSBezierPath* funnel=[NSBezierPath bezierPath];const auto x=NSMidX(button),y=NSMidY(button);[funnel moveToPoint:NSMakePoint(x-5,y-4)];[funnel lineToPoint:NSMakePoint(x+5,y-4)];[funnel lineToPoint:NSMakePoint(x+1,y)];[funnel lineToPoint:NSMakePoint(x+1,y+4)];[funnel lineToPoint:NSMakePoint(x-1,y+4)];[funnel lineToPoint:NSMakePoint(x-1,y)];[funnel closePath];[(model.enabled?NSColor.labelColor:NSColor.disabledControlTextColor) setStroke];[funnel stroke];
            NSRectClip(NSInsetRect(filter,4,1));NSMutableParagraphStyle* paragraph=[NSMutableParagraphStyle new];paragraph.lineBreakMode=NSLineBreakByTruncatingTail;
            [nsstring(item.filter_display) drawInRect:NSMakeRect(NSMinX(filter)+4,NSMidY(filter)-(font.ascender-font.descender)/2,std::max<CGFloat>(0,NSWidth(filter)-8),NSHeight(filter)) withAttributes:@{NSFontAttributeName:font,NSForegroundColorAttributeName:model.enabled?NSColor.labelColor:NSColor.disabledControlTextColor,NSParagraphStyleAttributeName:paragraph}];[NSGraphicsContext restoreGraphicsState];
        }
    }
    if(header.overflow_item>=0){const auto& r=header.overflow_rectangle;const auto rect=NSMakeRect(r[0],r[1],r[2]-r[0],r[3]-r[1]);[NSGraphicsContext saveGraphicsState];NSRectClip(rect);
        [(header.overflow_pressed?NSColor.selectedControlColor:NSColor.controlBackgroundColor) setFill];NSRectFill(rect);[NSColor.separatorColor setStroke];[[NSBezierPath bezierPathWithRect:NSInsetRect(rect,0.5,0.5)] stroke];
        [(model.enabled?NSColor.labelColor:NSColor.disabledControlTextColor) setStroke];const auto x=NSMidX(rect),y=NSMidY(rect),direction=(model.ex_style&0x400000U)?-1.0:1.0;
        for(unsigned n=0;n<2;++n){const auto center=x+(static_cast<CGFloat>(n)*5-2.5);NSBezierPath* arrow=[NSBezierPath bezierPath];[arrow moveToPoint:NSMakePoint(center-direction*2,y-4)];[arrow lineToPoint:NSMakePoint(center+direction*2,y)];[arrow lineToPoint:NSMakePoint(center-direction*2,y+4)];[arrow stroke];}[NSGraphicsContext restoreGraphicsState];
    }
}
- (void)guestMouse:(NSEvent*)event message:(unsigned int)message button:(unsigned int)button pressed:(BOOL)pressed{
    if(!self.model || !self.events || !self.model->enabled)return;const auto point=[self convertPoint:event.locationInWindow fromView:nil];self.buttons=pressed?self.buttons|button:self.buttons&~button;
    const std::uint32_t modifiers=((event.modifierFlags&NSEventModifierFlagShift)?1U:0U)|((event.modifierFlags&NSEventModifierFlagControl)?2U:0U);
    x64arm::GuiMessage input{self.guestHandle,self.buttons|((modifiers&1U)?4U:0U)|((modifiers&2U)?8U:0U),(static_cast<std::uint64_t>(coordinate_word(point.y))<<16)|coordinate_word(point.x),message,{},modifiers};input.has_point=true;input.point={static_cast<std::int32_t>(std::clamp<CGFloat>(point.x,INT32_MIN,INT32_MAX)),static_cast<std::int32_t>(std::clamp<CGFloat>(point.y,INT32_MIN,INT32_MAX))};self.events->push_back(std::move(input));
}
- (void)mouseDown:(NSEvent*)event{[self guestMouse:event message:event.clickCount>1?0x203U:0x201U button:1 pressed:YES];}
- (void)mouseDragged:(NSEvent*)event{[self guestMouse:event message:0x200 button:1 pressed:YES];}
- (void)mouseMoved:(NSEvent*)event{self.buttons=guest_mouse_buttons(NSEvent.pressedMouseButtons);[self guestMouse:event message:0x200 button:0 pressed:NO];}
- (void)mouseUp:(NSEvent*)event{[self guestMouse:event message:0x202 button:1 pressed:NO];}
- (void)rightMouseDown:(NSEvent*)event{[self guestMouse:event message:0x204 button:2 pressed:YES];}
- (void)rightMouseUp:(NSEvent*)event{[self guestMouse:event message:0x205 button:2 pressed:NO];}
@end

// 同一列的文字與附件共用字型基線，再把整個內容框置於列高中央。
@interface X64ArmCenteredTextCell : NSTextFieldCell
@end
@implementation X64ArmCenteredTextCell
- (NSRect)drawingRectForBounds:(NSRect)bounds {
    NSRect rect=[super drawingRectForBounds:bounds];const auto height=std::min(rect.size.height,[super cellSizeForBounds:bounds].height);
    rect.origin.y+=(rect.size.height-height)/2;rect.size.height=height;return rect;
}
@end
// 工具列的客體矩形已按字型及圖示量測，不能再套用 AppKit 一般按鈕的額外 bezel 留白。
// 保留 NSButton 的 action／鍵盤／accessibility，僅將內容繪製映射至同一份客體幾何。
@interface X64ArmToolbarButton : NSButton
@property(nonatomic) BOOL guestPressed;
@property(nonatomic) BOOL guestIndeterminate;
@property(nonatomic) CGFloat guestTextScale;
- (void)contentTitle:(NSRect*)title image:(NSRect*)image;
@end
@implementation X64ArmToolbarButton
- (BOOL)isFlipped{return YES;}
- (void)contentTitle:(NSRect*)title image:(NSRect*)image {
    const auto area=NSInsetRect(self.bounds,4,3);const auto width=std::max<CGFloat>(0,area.size.width),height=std::max<CGFloat>(0,area.size.height);
    auto measured=[self.title sizeWithAttributes:@{NSFontAttributeName:self.font?:[NSFont systemFontOfSize:13]}];measured.width*=self.guestTextScale;
    const bool hasTitle=self.title.length && self.imagePosition!=NSImageOnly,hasImage=self.image && self.imagePosition!=NSNoImage;
    NSSize icon=hasImage?self.image.size:NSZeroSize;
    if(icon.width>0 && icon.height>0){const auto scale=std::min<CGFloat>(1,std::min(width/icon.width,height/icon.height));icon.width*=scale;icon.height*=scale;}
    const auto gap=hasTitle && hasImage?4.0:0.0;
    if(self.imagePosition==NSImageAbove){
        const auto th=hasTitle?std::min(measured.height,height):0;
        if(hasImage && icon.height+gap+th>height && icon.height>0){const auto scale=std::max<CGFloat>(0,height-gap-th)/icon.height;icon.width*=scale;icon.height*=scale;}
        const auto top=NSMidY(area)-(icon.height+gap+th)/2;
        *image=NSMakeRect(NSMidX(area)-icon.width/2,top,icon.width,icon.height);
        *title=NSMakeRect(area.origin.x,top+icon.height+gap,width,th);
    }else{
        const auto tw=hasTitle?std::min(measured.width,std::max<CGFloat>(0,width-icon.width-gap)):0;
        const auto left=NSMidX(area)-(icon.width+gap+tw)/2;
        *image=NSMakeRect(left,NSMidY(area)-icon.height/2,icon.width,icon.height);
        *title=NSMakeRect(left+icon.width+gap,NSMidY(area)-measured.height/2,tw,std::min(height,measured.height));
    }
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;const bool selected=self.state!=NSControlStateValueOff,pressed=self.guestPressed || self.cell.highlighted;const bool enabled=self.enabled && !self.guestIndeterminate;
    NSBezierPath* shape=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,0.5,0.5) xRadius:2 yRadius:2];
    if(self.bordered || selected || pressed){
        NSColor* fill=selected || pressed?[[NSColor controlAccentColor] colorWithAlphaComponent:pressed?0.24:0.14]:[NSColor controlBackgroundColor];
        [fill setFill];[shape fill];[(selected?[NSColor controlAccentColor]:[NSColor separatorColor]) setStroke];[shape stroke];
    }
    NSRect title{},icon{};[self contentTitle:&title image:&icon];
    if(self.image && icon.size.width>0 && icon.size.height>0)[self.image drawInRect:icon fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:enabled?1:0.4 respectFlipped:YES hints:nil];
    if(title.size.width>0 && title.size.height>0){
        auto* paragraph=[[NSMutableParagraphStyle alloc] init];paragraph.alignment=NSTextAlignmentCenter;paragraph.lineBreakMode=NSLineBreakByTruncatingTail;
        [NSGraphicsContext saveGraphicsState];NSAffineTransform* transform=[NSAffineTransform transform];[transform translateXBy:title.origin.x yBy:title.origin.y];[transform scaleXBy:self.guestTextScale yBy:1];[transform concat];
        [self.title drawInRect:NSMakeRect(0,0,title.size.width/self.guestTextScale,title.size.height) withAttributes:@{NSFontAttributeName:self.font?:[NSFont systemFontOfSize:13],NSForegroundColorAttributeName:enabled?[NSColor controlTextColor]:[NSColor disabledControlTextColor],NSParagraphStyleAttributeName:paragraph}];[NSGraphicsContext restoreGraphicsState];
    }
    if(self.window.firstResponder==self){[[NSColor keyboardFocusIndicatorColor] setStroke];[NSBezierPath strokeRect:NSInsetRect(self.bounds,3,3)];}
}
@end
@interface X64ArmControlTarget : NSObject<NSTextViewDelegate,NSTextFieldDelegate,NSComboBoxDelegate,NSTableViewDataSource,NSTableViewDelegate,NSOutlineViewDataSource,NSOutlineViewDelegate>
@property(nonatomic) x64arm::GuiWindow* model;
@property(nonatomic) unsigned long long guestHandle;
@property(nonatomic) std::vector<x64arm::GuiMessage>* events;
@property(nonatomic) BOOL updating;
@property(nonatomic,strong) NSString* imeText;
- (void)clicked:(id)sender;
- (void)doubleClicked:(id)sender;
- (void)listClicked:(id)sender;
- (void)sendListEvent:(NSTableView*)table message:(unsigned)code;
- (void)toolbarClicked:(NSButton*)sender;
- (BOOL)publishIme:(NSTextView*)editor;
- (unsigned long long)editHandle;
- (void)publishEditSelection:(NSTextView*)editor;
- (BOOL)editCommand:(SEL)command modifiers:(NSEventModifierFlags)modifiers;
@end
@implementation X64ArmControlTarget
- (void)toolbarClicked:(NSButton*)sender{
    if(self.updating || !sender.enabled)return;
    const auto command=static_cast<std::uint64_t>(sender.tag);
    self.events->push_back({self.guestHandle,[sender.cell.representedObject unsignedLongLongValue],command,0x10011});
}
- (void)spinChanged:(NSStepper*)sender{if(!self.updating)self.events->push_back({self.guestHandle,static_cast<std::uint64_t>(static_cast<std::int64_t>(sender.doubleValue)),0,0x1000b});sender.doubleValue=0;}
- (void)sliderChanged:(NSSlider*)sender{
    if(self.updating || !sender.enabled)return;
    auto position=static_cast<std::int32_t>(std::llround(sender.doubleValue));
    if(self.model->style&2U)position=static_cast<std::int32_t>(static_cast<std::int64_t>(self.model->track_min)+self.model->track_max-position);
    const auto event=NSApp.currentEvent;unsigned code=event.type==NSEventTypeLeftMouseUp?4U:5U;
    if(event.type==NSEventTypeKeyDown){switch(event.keyCode){case 123:case 126:code=0;break;case 124:case 125:code=1;break;case 116:code=2;break;case 121:code=3;break;case 115:code=6;break;case 119:code=7;break;default:code=8;break;}}
    if(event.type==NSEventTypeKeyDown){
        const auto& model=*self.model;std::int64_t value=model.track_position;
        const auto page=model.track_page?model.track_page:std::max<std::int64_t>(1,(static_cast<std::int64_t>(model.track_max)-model.track_min)/5);
        switch(code){case 0:value-=model.track_line;break;case 1:value+=model.track_line;break;case 2:value-=page;break;case 3:value+=page;break;case 6:value=model.track_min;break;case 7:value=model.track_max;break;default:break;}
        position=static_cast<std::int32_t>(std::clamp(value,static_cast<std::int64_t>(model.track_min),static_cast<std::int64_t>(model.track_max)));
    }
    sender.doubleValue=(self.model->style&2U)?static_cast<double>(self.model->track_min)+self.model->track_max-position:position;
    self.events->push_back({self.guestHandle,static_cast<std::uint32_t>(position),code,0x1000b});
    if(code!=5)self.events->push_back({self.guestHandle,static_cast<std::uint32_t>(position),8,0x1000b});
}
- (void)clicked:(id)sender{(void)sender;if(!self.updating)self.events->push_back({self.guestHandle,0,0,0x10003});}
- (void)controlTextDidChange:(NSNotification*)notification{
    if(self.updating)return;NSControl* sender=notification.object;
    if([sender.currentEditor isKindOfClass:[NSTextView class]] && [self publishIme:(NSTextView*)sender.currentEditor])return;
    self.events->push_back({self.guestHandle,0,0,0x10001,gueststring(sender.stringValue)});
    if([sender.currentEditor isKindOfClass:[NSTextView class]]){const auto range=((NSTextView*)sender.currentEditor).selectedRange;
        self.events->back().indices={static_cast<std::uint32_t>(std::min<NSUInteger>(range.location,UINT32_MAX)),static_cast<std::uint32_t>(std::min<NSUInteger>(NSMaxRange(range),UINT32_MAX))};}
}
- (void)textDidChange:(NSNotification*)notification{if(self.updating)return;NSTextView* view=notification.object;if(![self publishIme:view]){
    self.events->push_back({self.guestHandle,0,0,0x10001,gueststring(view.string)});const auto range=view.selectedRange;
    self.events->back().indices={static_cast<std::uint32_t>(std::min<NSUInteger>(range.location,UINT32_MAX)),static_cast<std::uint32_t>(std::min<NSUInteger>(NSMaxRange(range),UINT32_MAX))};}}
- (unsigned long long)editHandle {return self.model && x64arm::gui_control_class(*self.model)==u"comboboxex32" && self.model->edit_control?self.model->edit_control:self.guestHandle;}
- (void)publishEditSelection:(NSTextView*)editor {
    if(self.updating || !self.events || !editor || editor.hasMarkedText)return;
    const auto range=editor.selectedRange;if(range.location==NSNotFound)return;
    self.events->push_back({[self editHandle],std::min<NSUInteger>(range.location,UINT32_MAX),std::min<NSUInteger>(NSMaxRange(range),UINT32_MAX),0x10014});
}
- (void)controlTextDidBeginEditing:(NSNotification*)notification {
    if(self.updating || !self.events)return;self.events->push_back({[self editHandle],0,0,0x10013});NSControl* control=notification.object;
    if([control.currentEditor isKindOfClass:[NSTextView class]])[self publishEditSelection:(NSTextView*)control.currentEditor];
}
- (void)textDidBeginEditing:(NSNotification*)notification {if(!self.updating && self.events){self.events->push_back({[self editHandle],0,0,0x10013});[self publishEditSelection:notification.object];}}
- (void)textViewDidChangeSelection:(NSNotification*)notification {[self publishEditSelection:notification.object];}
- (BOOL)publishIme:(NSTextView*)editor {
    if(editor.hasMarkedText){const auto range=editor.markedRange;if(range.location>editor.string.length || range.length>editor.string.length-range.location)return NO;
        self.imeText=[editor.string substringWithRange:range];x64arm::GuiImeData data;data.text=gueststring(self.imeText);data.composing=true;data.native_edit=true;data.cursor=static_cast<std::uint32_t>(std::min<NSUInteger>(data.text.size(),editor.selectedRange.location>=range.location?editor.selectedRange.location-range.location:0));
        data.attributes.assign(data.text.size(),3);data.clauses={0,static_cast<std::uint32_t>(data.text.size())};data.reading_clauses={0,0};x64arm::GuiMessage msg{self.guestHandle,0,0x1bf,0x10f};msg.ime=std::move(data);self.events->push_back(std::move(msg));return YES;
    }
    if(!self.imeText)return NO;
    self.events->push_back({self.guestHandle,0,0,0x10001,gueststring(editor.string)});
    const auto full=gueststring(editor.string),old=self.model?self.model->title:std::u16string{};std::size_t prefix=0,suffix=0;
    while(prefix<full.size() && prefix<old.size() && full[prefix]==old[prefix])++prefix;
    while(suffix<full.size()-prefix && suffix<old.size()-prefix && full[full.size()-1-suffix]==old[old.size()-1-suffix])++suffix;
    x64arm::GuiImeData data;data.result=full.substr(prefix,full.size()-prefix-suffix);data.has_result=!data.result.empty();data.native_edit=true;data.result_clauses={0,static_cast<std::uint32_t>(data.result.size())};data.result_reading_clauses={0,0};
    x64arm::GuiMessage msg{self.guestHandle,0,data.has_result?0x1e00U:0U,0x10f};msg.ime=std::move(data);self.events->push_back(std::move(msg));self.imeText=nil;return YES;
}
- (void)comboBoxSelectionDidChange:(NSNotification*)notification{
    if(self.updating)return;NSComboBox* sender=notification.object;
    self.events->push_back({self.guestHandle,static_cast<std::uint64_t>(sender.indexOfSelectedItem),0,0x10002});
}
- (BOOL)control:(NSControl*)control textView:(NSTextView*)editor doCommandBySelector:(SEL)command{
    (void)control;[self publishEditSelection:editor];return [self editCommand:command modifiers:NSApp.currentEvent.modifierFlags];
}
- (BOOL)textView:(NSTextView*)editor doCommandBySelector:(SEL)command {[self publishEditSelection:editor];return [self editCommand:command modifiers:NSApp.currentEvent.modifierFlags];}
- (BOOL)editCommand:(SEL)command modifiers:(NSEventModifierFlags)flags {
    if(self.updating || !self.model || !self.events)return NO;
    const auto cls=x64arm::gui_control_class(*self.model);if(cls!=u"edit" && cls!=u"combobox" && cls!=u"comboboxex32")return NO;
    if(cls==u"edit"){
        const unsigned clipboard=command==@selector(copy:)?0x301U:command==@selector(cut:)?0x300U:command==@selector(paste:) || command==@selector(pasteAsPlainText:)?0x302U:0;
        if(clipboard){if(!self.model->enabled)return YES;const auto handle=[self editHandle];self.events->push_back({handle,0,0,0x10013});self.events->push_back({handle,0,0,clipboard});return YES;}
    }
    const bool backwards=command==@selector(insertBacktab:);
    const unsigned key=command==@selector(insertNewline:)?13U:command==@selector(cancelOperation:)?27U:command==@selector(insertTab:) || backwards?9U:0U;if(!key)return NO;
    const std::uint64_t scan=key==13?0x1cU:key==9?0xfU:1U;
    const std::uint32_t modifiers=(((flags&NSEventModifierFlagShift) || backwards)?1U:0U)|((flags&NSEventModifierFlagControl)?2U:0U)|((flags&NSEventModifierFlagOption)?4U:0U);
    const auto handle=[self editHandle];self.events->push_back({handle,0,0,0x10013});
    // 導覽必須先經過客體 WM_GETDLGCODE；不能由 AppKit 的 key-view 次序自行跳轉。
    self.events->push_back({handle,key,1U|(scan<<16),0x100,{},modifiers});
    self.events->push_back({handle,key,0xc0000001U|(scan<<16),0x101,{},modifiers});return YES;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView*)view{(void)view;return static_cast<NSInteger>(self.model->items.size());}
- (id)tableView:(NSTableView*)view objectValueForTableColumn:(NSTableColumn*)column row:(NSInteger)row{
    (void)view;if(row<0 || static_cast<std::size_t>(row)>=self.model->items.size())return @"";
    const auto& item=self.model->items[static_cast<std::size_t>(row)];
    if(x64arm::gui_control_class(*self.model)!=u"syslistview32")return nsstring(item.text);
    const auto index=static_cast<std::size_t>([column.identifier integerValue]);
    const auto subitem=index<self.model->columns.size()?static_cast<std::uint32_t>(self.model->columns[index].subitem):0;
    if(x64arm::gui_list_text_callback(item,subitem) && !item.resolved_subitems.contains(subitem)){
        if(std::none_of(self.events->begin(),self.events->end(),[&](const auto& event){return event.window==self.guestHandle && event.message==0x10007 && event.wparam==static_cast<std::uint64_t>(row) && event.lparam==subitem;}))
            self.events->push_back({self.guestHandle,static_cast<std::uint64_t>(row),subitem,0x10007});
    }
    NSString* text=subitem==0?nsstring(item.text):subitem<item.subitems.size()?nsstring(item.subitems[subitem]):@"";
    if(subitem==0 && item.image>=0 && static_cast<std::size_t>(item.image)<self.model->list_icons.size()){
        const auto& source=self.model->list_icons[static_cast<std::size_t>(item.image)];
        NSFont* font=guest_font(self.model->control_font);const auto width=std::clamp(source.width,1U,128U),height=std::clamp(source.height,1U,128U);
        NSImage* icon=[list_icon(source) copy];icon.size=NSMakeSize(width,height);
        NSTextAttachment* attachment=[[NSTextAttachment alloc] init];attachment.image=icon;attachment.bounds=NSMakeRect(0,std::round((font.capHeight-height)/2),width,height);
        NSMutableAttributedString* value=[[NSAttributedString attributedStringWithAttachment:attachment] mutableCopy];
        [value appendAttributedString:[[NSAttributedString alloc] initWithString:[@" " stringByAppendingString:text] attributes:@{NSFontAttributeName:font}]];return value;
    }
    return text;
}
- (void)tableViewSelectionDidChange:(NSNotification*)notification{
    if(self.updating)return;NSTableView* sender=notification.object;
    x64arm::GuiMessage message{self.guestHandle,static_cast<std::uint64_t>(sender.selectedRow),0,x64arm::gui_control_class(*self.model)==u"syslistview32"?0x10005U:0x10002U};
    for(NSUInteger row=sender.selectedRowIndexes.firstIndex;row!=NSNotFound;row=[sender.selectedRowIndexes indexGreaterThanIndex:row])message.indices.push_back(static_cast<std::uint32_t>(row));
    self.events->push_back(std::move(message));
}
- (void)sendListEvent:(NSTableView*)table message:(unsigned)code{
    if(self.updating || x64arm::gui_control_class(*self.model)!=u"syslistview32")return;
    const auto row=table.clickedRow>=0?table.clickedRow:table.selectedRow;
    const auto column=table.clickedColumn>=0?static_cast<std::size_t>(table.clickedColumn):0;
    const auto subitem=column<self.model->columns.size()?static_cast<std::uint32_t>(self.model->columns[column].subitem):0;
    x64arm::GuiMessage event{self.guestHandle,static_cast<std::uint64_t>(row),subitem,code};
    NSPoint point=NSZeroPoint;NSEvent* current=NSApp.currentEvent;
    if(current && (current.type==NSEventTypeLeftMouseDown || current.type==NSEventTypeLeftMouseUp))point=[table.enclosingScrollView convertPoint:current.locationInWindow fromView:nil];
    else if(row>=0){const auto cell=[table frameOfCellAtColumn:static_cast<NSInteger>(column) row:row];point=[table convertPoint:NSMakePoint(NSMinX(cell)+8,NSMidY(cell)) toView:table.enclosingScrollView];}
    event.point={static_cast<std::int32_t>(point.x),static_cast<std::int32_t>(point.y)};event.has_point=true;
    event.modifiers=((current.modifierFlags&NSEventModifierFlagShift)?1U:0U)|((current.modifierFlags&NSEventModifierFlagControl)?2U:0U)|((current.modifierFlags&NSEventModifierFlagOption)?4U:0U);
    [table.window makeFirstResponder:table];self.events->push_back(std::move(event));
}
- (void)listClicked:(id)sender{[self sendListEvent:sender message:0x10009];}
- (void)doubleClicked:(id)sender{[self sendListEvent:sender message:0x10006];}
- (void)tableView:(NSTableView*)table didClickTableColumn:(NSTableColumn*)column{
    (void)table;if(self.updating || x64arm::gui_control_class(*self.model)!=u"syslistview32")return;
    const auto index=static_cast<std::size_t>([column.identifier integerValue]);
    self.events->push_back({self.guestHandle,UINT64_MAX,index<self.model->columns.size()?static_cast<std::uint32_t>(self.model->columns[index].subitem):0,0x10008});
}
- (NSInteger)outlineView:(NSOutlineView*)view numberOfChildrenOfItem:(id)item{
    (void)view;const auto parent=item?[item unsignedLongLongValue]:0;NSInteger count=0;for(const auto& entry:self.model->items)if(entry.parent==parent)++count;return count;
}
- (id)outlineView:(NSOutlineView*)view child:(NSInteger)index ofItem:(id)item{
    (void)view;const auto parent=item?[item unsignedLongLongValue]:0;for(const auto& entry:self.model->items)if(entry.parent==parent && index--==0)return @(entry.handle);return nil;
}
- (BOOL)outlineView:(NSOutlineView*)view isItemExpandable:(id)item{
    (void)view;const auto identity=[item unsignedLongLongValue];for(const auto& entry:self.model->items){if(entry.parent==identity)return YES;if(entry.handle==identity && (entry.tree_children==-1?entry.tree_display[2]:entry.tree_children)>0)return YES;}return NO;
}
- (id)outlineView:(NSOutlineView*)view objectValueForTableColumn:(NSTableColumn*)column byItem:(id)item{
    (void)view;(void)column;for(const auto& entry:self.model->items)if(entry.handle==[item unsignedLongLongValue]){
        if(!self.updating && ((entry.callback_subitems.contains(0) && !entry.resolved_subitems.contains(0)) || (entry.image==-1 && !(entry.state&2U) && !entry.resolved_subitems.contains(1)) || (entry.selected_image==-1 && (entry.state&2U) && !entry.resolved_subitems.contains(2)) || (entry.tree_children==-1 && !entry.resolved_subitems.contains(3))) && self.events && std::none_of(self.events->begin(),self.events->end(),[&](const auto& event){return event.window==self.guestHandle && event.message==0x1001b && event.wparam==entry.handle;}))self.events->push_back({self.guestHandle,entry.handle,0,0x1001b});
        NSString* text=nsstring(entry.text);const bool selected=(entry.state&2U)!=0;const auto source=selected?entry.selected_image:entry.image;const auto image=source==-1?entry.tree_display[selected?1:0]:source;
        if(image>=0 && static_cast<std::size_t>(image)<self.model->list_icons.size()){
            const auto& icon=self.model->list_icons[static_cast<std::size_t>(image)];NSFont* font=guest_font(self.model->control_font);const auto width=std::clamp(icon.width,1U,128U),height=std::clamp(icon.height,1U,128U);NSImage* native=[list_icon(icon) copy];native.size=NSMakeSize(width,height);
            NSTextAttachment* attachment=[NSTextAttachment new];attachment.image=native;attachment.bounds=NSMakeRect(0,std::round((font.capHeight-height)/2),width,height);NSMutableAttributedString* value=[[NSAttributedString attributedStringWithAttachment:attachment] mutableCopy];[value appendAttributedString:[[NSAttributedString alloc]initWithString:[@" " stringByAppendingString:text] attributes:@{NSFontAttributeName:font}]];return value;
        }return text;
    }
    return @"";
}
- (BOOL)outlineView:(NSOutlineView*)view shouldExpandItem:(id)item{
    (void)view;if(self.updating)return YES;if(!self.model || !self.model->enabled || !self.events)return NO;self.events->push_back({self.guestHandle,2,[item unsignedLongLongValue],0x1001a});return NO;
}
- (BOOL)outlineView:(NSOutlineView*)view shouldCollapseItem:(id)item{
    (void)view;if(self.updating)return YES;if(!self.model || !self.model->enabled || !self.events)return NO;self.events->push_back({self.guestHandle,1,[item unsignedLongLongValue],0x1001a});return NO;
}
- (void)outlineViewSelectionDidChange:(NSNotification*)notification{
    if(self.updating)return;NSOutlineView* view=notification.object;id item=[view itemAtRow:view.selectedRow];if(!item)return;
    const auto action=NSApp.currentEvent.type==NSEventTypeKeyDown?2U:1U;
    // 使用穩定 HTREEITEM；父視窗在派送前插入／刪除節點時，舊索引不能選到別的項目。
    self.events->push_back({self.guestHandle,action,[item unsignedLongLongValue],0x10010});
}
@end
@interface X64ArmNativeMenuItem : NSMenuItem
@property(nonatomic) unsigned long long guestMenu;
@property(nonatomic) unsigned int guestPosition;
@property(nonatomic) BOOL notifyByPosition;
@end
@implementation X64ArmNativeMenuItem
@end
// GroupBox／分隔線是裝飾；位於兄弟欄位上方時，不能吃掉框內的滑鼠事件。
@interface X64ArmDecorationBox : NSBox
@end
@implementation X64ArmDecorationBox
- (NSView*)hitTest:(NSPoint)point{NSView* hit=[super hitTest:point];return hit==self || hit==self.contentView?nil:hit;}
- (void)drawRect:(NSRect)dirty{
    if(self.boxType!=NSBoxCustom){[super drawRect:dirty];return;}
    // NSBoxCustom 不繪製 title；依同一原生字型畫出有標題缺口的 GroupBox，背景保持透明。
    NSFont* font=self.titleFont?:[NSFont systemFontOfSize:12];const auto size=[self.title sizeWithAttributes:@{NSFontAttributeName:font}];
    const auto height=std::ceil(font.ascender-font.descender+font.leading);const auto bounds=self.bounds;
    const auto title=NSMakeRect(NSMinX(bounds)+7,NSMaxY(bounds)-height,std::min(size.width,std::max<CGFloat>(0,NSWidth(bounds)-14)),height);
    auto rectangle=NSInsetRect(bounds,0.5,0.5);rectangle.size.height=std::max<CGFloat>(0,rectangle.size.height-height/2);
    [NSGraphicsContext saveGraphicsState];NSRectClip(dirty);
    if(self.title.length){auto* clip=[NSBezierPath bezierPathWithRect:bounds];[clip appendBezierPathWithRect:NSInsetRect(title,-2,0)];clip.windingRule=NSWindingRuleEvenOdd;[clip addClip];}
    [self.borderColor setStroke];auto* border=[NSBezierPath bezierPathWithRoundedRect:rectangle xRadius:self.cornerRadius yRadius:self.cornerRadius];border.lineWidth=self.borderWidth;[border stroke];[NSGraphicsContext restoreGraphicsState];
    if(self.title.length)[self.title drawInRect:title withAttributes:@{NSFontAttributeName:font,NSForegroundColorAttributeName:((NSCell*)self.titleCell).enabled?NSColor.controlTextColor:NSColor.disabledControlTextColor}];
}
@end
// Windows 選單列在按下時開始追蹤；不能等一般 NSButton 的放開 tracking loop 才通知客體。
@interface X64ArmMenuButton : NSButton
@end
@implementation X64ArmMenuButton
- (BOOL)acceptsFirstMouse:(NSEvent*)event{(void)event;return YES;}
- (BOOL)mouseDownCanMoveWindow{return NO;}
- (void)mouseDown:(NSEvent*)event{(void)event;if(self.enabled){[self highlight:YES];[self sendAction:self.action to:self.target];}}
- (void)mouseUp:(NSEvent*)event{(void)event;[self highlight:NO];}
@end
@interface X64ArmPopupMenu : NSMenu
@property(nonatomic) unsigned long long guestMenu;
@property(nonatomic) unsigned int guestPosition;
@property(nonatomic) BOOL guestInitialized;
@property(nonatomic) BOOL deferGuestRefresh;
@end
@implementation X64ArmPopupMenu
@end
@interface X64ArmMenuTarget : NSObject<NSMenuDelegate> {
@public std::function<void()> changed;
@public std::function<void(X64ArmPopupMenu*,bool)> popupChanged;
@public std::function<bool(NSMenuItem*,bool)> highlightChanged;
@public std::function<bool(NSMenuItem*)> canChoose;
}
@property(nonatomic,strong) NSMutableDictionary* record;
@property(nonatomic) unsigned long long selected;
@property(nonatomic) unsigned long long selectedMenu;
@property(nonatomic) unsigned int selectedPosition;
@property(nonatomic) BOOL didSelect;
@property(nonatomic,strong) NSMenuItem* highlighted;
@property(nonatomic) BOOL highlightedMouse;
@property(nonatomic,strong) NSMenuItem* pendingChoice;
@property(nonatomic,strong) NSMenuItem* lastHighlighted;
@property(nonatomic) BOOL expectedRejection;
- (void)recordLifecycle:(NSString*)event menu:(NSMenu*)menu;
- (BOOL)finishChoice;
- (BOOL)highlight:(NSMenuItem*)item mouse:(BOOL)mouse;
- (void)choose:(NSMenuItem*)sender;
@end
@implementation X64ArmMenuTarget
- (void)recordLifecycle:(NSString*)event menu:(NSMenu*)menu {
    if(!self.record[@"native_lifecycle"])self.record[@"native_lifecycle"]=[NSMutableArray array];
    auto* popup=[menu isKindOfClass:X64ArmPopupMenu.class]?(X64ArmPopupMenu*)menu:nil;
    if([self.record[@"native_lifecycle"] count]<256)[self.record[@"native_lifecycle"] addObject:@{@"event":event,@"menu":@(popup.guestMenu),@"initialized":@(popup.guestInitialized)}];
}
- (BOOL)highlight:(NSMenuItem*)item mouse:(BOOL)mouse {
    if(!item){self.highlighted=nil;return YES;}
    if(self.highlighted==item && self.highlightedMouse==mouse)return YES;
    self.highlighted=item;self.highlightedMouse=mouse;self.lastHighlighted=item;
    if(highlightChanged && !highlightChanged(item,mouse)){self.highlighted=nil;return NO;}
    return YES;
}
- (void)menu:(NSMenu*)menu willHighlightItem:(NSMenuItem*)item {
    (void)menu;const auto type=NSApp.currentEvent.type;
    if(item){self.record[@"delegate_highlight_count"]=@([self.record[@"delegate_highlight_count"] unsignedIntValue]+1);
        if(!self.record[@"native_highlights"])self.record[@"native_highlights"]=[NSMutableArray array];
        if([self.record[@"native_highlights"] count]<256)[self.record[@"native_highlights"] addObject:@{@"id":item.representedObject?:[NSNull null],@"event_type":@(type)}];}
    const BOOL mouse=type==NSEventTypeMouseMoved || type==NSEventTypeLeftMouseDown || type==NSEventTypeLeftMouseUp || type==NSEventTypeLeftMouseDragged || type==NSEventTypeRightMouseDown || type==NSEventTypeRightMouseUp;
    (void)[self highlight:item mouse:mouse];
}
- (void)menuNeedsUpdate:(NSMenu*)menu {
    [self recordLifecycle:@"needs_update" menu:menu];
    if([menu isKindOfClass:X64ArmPopupMenu.class]){auto* popup=(X64ArmPopupMenu*)menu;
        if(popup.guestMenu && !popup.guestInitialized){popup.guestInitialized=YES;if(popupChanged)popupChanged(popup,true);}}
}
- (void)choose:(NSMenuItem*)sender {
    if(!sender.enabled || sender.separatorItem || sender.submenu)return;
    // 關閉動畫會清空目前高亮；已通知過的候選不應在 action 階段重送成鍵盤來源。
    if(self.lastHighlighted!=sender && ![self highlight:sender mouse:NO])return;
    self.pendingChoice=sender;self.record[@"native_action_received_count"]=@([self.record[@"native_action_received_count"] unsignedIntValue]+1);
}
- (BOOL)finishChoice {
    NSMenuItem* sender=self.pendingChoice;self.pendingChoice=nil;
    if(!sender)return NO;
    if(canChoose && !canChoose(sender)){self.record[@"selection_rejected"]=@YES;if(changed)changed();return NO;}
    self.selected=[sender.representedObject unsignedLongLongValue];self.didSelect=YES;
    if([sender isKindOfClass:X64ArmNativeMenuItem.class]){auto* item=(X64ArmNativeMenuItem*)sender;self.selectedMenu=item.guestMenu;self.selectedPosition=item.guestPosition;}
    self.record[@"selected"]=@(self.selected);self.record[@"native_action_count"]=@([self.record[@"native_action_count"] unsignedIntValue]+1);
    if(changed)changed();return YES;
}
- (void)menuWillOpen:(NSMenu*)menu {
    [self recordLifecycle:@"open" menu:menu];
    if([menu isKindOfClass:X64ArmPopupMenu.class]){auto* popup=(X64ArmPopupMenu*)menu;
        if(popup.guestMenu && !popup.guestInitialized){
            popup.guestInitialized=YES;popup.deferGuestRefresh=YES;
            if(popupChanged)popupChanged(popup,true);
            popup.deferGuestRefresh=NO;
        }}
    self.record[@"opened"]=@([self.record[@"opened"] unsignedIntValue]+1);if(changed)changed();
}
- (void)menuDidClose:(NSMenu*)menu {
    [self recordLifecycle:@"close" menu:menu];
    if([menu isKindOfClass:X64ArmPopupMenu.class]){auto* popup=(X64ArmPopupMenu*)menu;
        if(popup.guestMenu && popup.guestInitialized){popup.guestInitialized=NO;if(popupChanged)popupChanged(popup,false);}}
    self.record[@"close_event_type"]=@(NSApp.currentEvent.type);
    self.record[@"closed"]=@([self.record[@"closed"] unsignedIntValue]+1);if(changed)changed();
}
@end
@interface X64ArmMainMenuTarget : NSObject
@property(nonatomic) unsigned long long owner;
@property(nonatomic) std::vector<x64arm::GuiMessage>* events;
- (void)choose:(NSMenuItem*)sender;
- (void)openMenu:(NSButton*)sender;
@end
@implementation X64ArmMainMenuTarget
- (void)openMenu:(NSButton*)sender {
    if(sender.enabled)self.events->push_back({self.owner,static_cast<std::uint64_t>(sender.tag),static_cast<std::uint32_t>(sender.frame.origin.x),0x1000a});
}
- (void)choose:(NSMenuItem*)sender {
    if(sender.enabled && !sender.separatorItem && !sender.submenu){
        auto* item=(X64ArmNativeMenuItem*)sender;
        self.events->push_back({self.owner,item.notifyByPosition?item.guestPosition:[sender.representedObject unsignedLongLongValue],item.notifyByPosition?item.guestMenu:0,item.notifyByPosition?0x126U:0x111U});
    }
}
@end
// MDI 子視窗的非客戶區使用獨立容器，客戶區仍保留原本 GDI／Metal view。
@interface X64ArmMdiFrameView : X64ArmGuestView
@property(nonatomic) CGFloat originX;
@property(nonatomic) CGFloat originY;
@end
@implementation X64ArmMdiFrameView
- (void)drawRect:(NSRect)dirty {
    (void)dirty;if(!self.model)return;
    [[NSColor windowBackgroundColor] setFill];NSRectFill(self.bounds);
    if((self.model->style&0xc00000U)!=0xc00000U)return;
    const auto inset=x64arm::gui_frame_insets(*self.model);const CGFloat border=inset.left;
    const NSRect caption=NSMakeRect(border,border,std::max<CGFloat>(0,self.bounds.size.width-2*border),std::max<CGFloat>(0,inset.top-border));
    [(self.model->mdi_active?[NSColor selectedContentBackgroundColor]:[NSColor controlColor]) setFill];NSRectFill(caption);
    NSString* title=[[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(self.model->title.data()) length:self.model->title.size()];
    const auto color=self.model->mdi_active?[NSColor whiteColor]:[NSColor labelColor];
    const auto attributes=@{NSFontAttributeName:[NSFont systemFontOfSize:12],NSForegroundColorAttributeName:color};
    [NSGraphicsContext saveGraphicsState];NSRectClip(NSInsetRect(caption,22,0));[title drawAtPoint:NSMakePoint(border+22,border+4) withAttributes:attributes];[NSGraphicsContext restoreGraphicsState];
    if(self.model->style&0x80000U){[@"▣" drawAtPoint:NSMakePoint(border+3,border+4) withAttributes:attributes];[@"×" drawAtPoint:NSMakePoint(NSMaxX(caption)-16,border+4) withAttributes:attributes];}
    if(self.model->style&0x10000U)[@"□" drawAtPoint:NSMakePoint(NSMaxX(caption)-36,border+4) withAttributes:attributes];
    if(self.model->style&0x20000U)[@"−" drawAtPoint:NSMakePoint(NSMaxX(caption)-((self.model->style&0x10000U)?56:36),border+4) withAttributes:attributes];
}
- (unsigned)hit:(NSPoint)p {
    if(!self.model)return 0;const auto style=self.model->style;const CGFloat width=self.bounds.size.width,height=self.bounds.size.height,border=style&0x40000U?4:1;
    if(style&0x40000U){const bool l=p.x<border,r=p.x>=width-border,t=p.y<border,b=p.y>=height-border;if(t)return l?13:r?14:12;if(b)return l?16:r?17:15;if(l)return 10;if(r)return 11;}
    if((style&0xc00000U)==0xc00000U && p.y<24+border){if((style&0x80000U) && p.x<border+20)return 3;if((style&0x80000U) && p.x>=width-border-20)return 20;if((style&0x10000U) && p.x>=width-border-40)return 9;if((style&0x20000U) && p.x>=width-border-((style&0x10000U)?60:40))return 8;return 2;}return 18;
}
- (void)sendMouse:(NSEvent*)event message:(unsigned)message {
    if(!self.events || !self.model || !self.model->enabled)return;const auto p=[self convertPoint:event.locationInWindow fromView:nil];
    const auto x=coordinate_word(self.originX+p.x),y=coordinate_word(self.originY+p.y);
    self.events->push_back({self.guestHandle,[self hit:p],static_cast<std::uint64_t>(x)|(static_cast<std::uint64_t>(y)<<16),message});
}
- (void)mouseDown:(NSEvent*)event {[self.window makeFirstResponder:self];[self sendMouse:event message:event.clickCount>1?0xa3:0xa1];}
- (void)mouseUp:(NSEvent*)event {[self sendMouse:event message:0xa2];}
- (void)mouseDragged:(NSEvent*)event {[self sendMouse:event message:0xa0];}
@end
// 在客體視窗統一處理 Command，避免依賴客體是否提供 macOS 編輯選單。
@interface X64ArmGuestWindow : NSWindow
@property(nonatomic,copy) BOOL (^commandShortcut)(NSEvent*);
@property(nonatomic) x64arm::GuiWindow* guestModel;
@end
@implementation X64ArmGuestWindow
- (instancetype)initWithContentRect:(NSRect)rect styleMask:(NSWindowStyleMask)style backing:(NSBackingStoreType)backing defer:(BOOL)defer {
    self=[super initWithContentRect:rect styleMask:style backing:backing defer:defer];
    if(self)self.acceptsMouseMovedEvents=YES;
    return self;
}
- (BOOL)canBecomeKeyWindow {return self.guestModel?self.guestModel->enabled && !(self.guestModel->ex_style&0x08000000U):[super canBecomeKeyWindow];}
- (BOOL)canBecomeMainWindow {return self.guestModel?[self canBecomeKeyWindow] && !(self.guestModel->ex_style&0x80U):[super canBecomeMainWindow];}
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if(self.commandShortcut && self.commandShortcut(event))return YES;
    return [super performKeyEquivalent:event];
}
@end

namespace x64arm {
NSImage* native_gui_icon_image(const GuiIcon& icon){
    return icon.width || icon.height || icon.system || !icon.image.empty() || !icon.rgba.empty()?list_icon(icon):nil;
}
namespace {
NSWindowStyleMask native_window_style(const GuiWindow& model) {
    const auto inset=gui_frame_insets(model);
    if((model.frame_insets && !inset.left && !inset.top && !inset.right && !inset.bottom) || ((model.style&0x80000000U) && (model.style&0xc00000U)!=0xc00000U && !model.dialog))return NSWindowStyleMaskBorderless;
    return NSWindowStyleMaskTitled|((model.style&0x80000U)?NSWindowStyleMaskClosable:0)|((model.style&0x20000U)?NSWindowStyleMaskMiniaturizable:0)|((model.style&0x40000U)?NSWindowStyleMaskResizable:0);
}
NSRect native_child_rect(const GuiWindow& model,bool client_view) {
    const auto inset=gui_frame_insets(model);const auto outer=gui_outer_size(model);
    return client_view?NSMakeRect(static_cast<double>(model.x)+inset.left,static_cast<double>(model.y)+inset.top,model.width,model.height):NSMakeRect(model.x,model.y,outer[0],outer[1]);
}
struct Window {
    X64ArmGuestWindow* __strong native{};
    NSView* __strong view{};
    X64ArmMdiFrameView* __strong mdi_frame{};
    NSView* __strong menu_container{};NSView* __strong menu_bar{};
    X64ArmWindowDelegate* __strong delegate{};
    X64ArmControlTarget* __strong target{};
    X64ArmMainMenuTarget* __strong menu_target{};
    NSMenu* __strong main_menu{};
    NSControl* __strong control{};
    NSTextView* __strong editor{};
    NSMutableDictionary<NSNumber*,X64ArmToolbarButton*>* __strong native_toolbar_buttons{};
    std::vector<std::pair<NSToolTipTag,std::size_t>> status_tooltips{};
    GuiWindow model{};
    bool alias{},shaped{},fullscreen{};
};
void apply_window_region(Window& w){
    NSView* target=w.native?(w.native.contentView.superview?:w.native.contentView):(w.mdi_frame?:w.view);
    if(!target)return;
    if(!w.model.window_region && !w.shaped)return;
    target.wantsLayer=YES;
    if(!w.model.window_region){w.shaped=false;target.layer.mask=nil;if(w.native){w.native.opaque=YES;w.native.backgroundColor=NSColor.windowBackgroundColor;[w.native invalidateShadow];}return;}
    w.shaped=true;CGMutablePathRef path=CGPathCreateMutable();const auto outer=gui_outer_size(w.model);
    const auto sx=outer[0]?target.bounds.size.width/outer[0]:1.0,sy=outer[1]?target.bounds.size.height/outer[1]:1.0;
    for(const auto& r:*w.model.window_region){
        const auto x=r[0]*sx,y=target.isFlipped?r[1]*sy:target.bounds.size.height-r[3]*sy;
        CGPathAddRect(path,nullptr,CGRectMake(x,y,(static_cast<double>(r[2])-r[0])*sx,(static_cast<double>(r[3])-r[1])*sy));
    }
    auto* mask=[CAShapeLayer layer];mask.frame=target.bounds;mask.path=path;mask.fillColor=NSColor.blackColor.CGColor;target.layer.mask=mask;CGPathRelease(path);
    if(w.native){w.native.opaque=NO;w.native.backgroundColor=NSColor.clearColor;[w.native invalidateShadow];}
}
void sync_native_frame(Window& w){
    if(!w.native || w.model.parent==gui_message_parent)return;const BOOL updating=w.delegate.updating;w.delegate.updating=YES;const auto menu_height=w.model.menu?20U:0U;
    if(menu_height && !w.menu_container){
        w.menu_container=[[X64ArmMenuContainer alloc] initWithFrame:NSMakeRect(0,0,w.model.width,w.model.height+20)];
        w.menu_bar=[[X64ArmMenuBar alloc] initWithFrame:NSMakeRect(0,0,w.model.width,20)];w.menu_bar.autoresizingMask=NSViewWidthSizable;
        [w.view removeFromSuperview];w.native.contentView=w.menu_container;[w.menu_container addSubview:w.view];[w.menu_container addSubview:w.menu_bar];
    }else if(!menu_height && w.menu_container){[w.view removeFromSuperview];w.native.contentView=w.view;w.menu_container=nil;w.menu_bar=nil;}
    w.view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    const auto size=NSMakeSize(w.model.width,w.model.height+menu_height);
    if(!NSEqualSizes(w.native.contentView.bounds.size,size))[w.native setContentSize:size];
    w.view.frame=NSMakeRect(0,menu_height,w.model.width,w.model.height);
    if(w.menu_bar)w.menu_bar.frame=NSMakeRect(0,0,w.model.width,20);
    w.delegate.clientView=w.view;
    const auto screen=background_display()?:w.native.screen?:NSScreen.mainScreen;
    // AppKit 的 frame 必須位於有號 32 位元矩形內；保留客體座標，只限制原生投影的原點。
    const auto origin=NSMakePoint(std::clamp<CGFloat>(w.model.x+(background_display()?screen.frame.origin.x:0),INT32_MIN,static_cast<CGFloat>(INT32_MAX)-w.native.frame.size.width),std::clamp<CGFloat>(NSMaxY(screen.frame)-w.model.y-w.native.frame.size.height,INT32_MIN,static_cast<CGFloat>(INT32_MAX)-w.native.frame.size.height));
    if(!NSEqualPoints(w.native.frame.origin,origin))[w.native setFrameOrigin:origin];
    w.delegate.updating=updating;
}
void create_table_view(Window& window,const NSRect& rect,X64ArmControlTarget* target) {
    const auto& model=window.model;const auto& cls=gui_control_class(model);
    auto* scroll=[[NSScrollView alloc] initWithFrame:rect];scroll.hasVerticalScroller=YES;scroll.borderType=NSBezelBorder;scroll.autohidesScrollers=YES;
    NSTableView* table=cls==u"systreeview32"?[[NSOutlineView alloc] initWithFrame:NSMakeRect(0,0,model.width,model.height)]:[[NSTableView alloc] initWithFrame:NSMakeRect(0,0,model.width,model.height)];
    auto* column=[[NSTableColumn alloc] initWithIdentifier:@"text"];column.width=std::max<CGFloat>(20,model.width-4);column.dataCell=[[X64ArmCenteredTextCell alloc] initTextCell:@""];[(NSTextFieldCell*)column.dataCell setFont:[NSFont systemFontOfSize:12]];[table addTableColumn:column];table.headerView=nil;table.rowHeight=17;table.intercellSpacing=NSMakeSize(0,0);table.dataSource=target;table.delegate=target;
    table.allowsMultipleSelection=cls==u"syslistview32"?!(model.style&4U):cls==u"listbox" && (model.style&0x808U)!=0;
    if(cls==u"syslistview32"){table.target=target;table.action=@selector(listClicked:);table.doubleAction=@selector(doubleClicked:);scroll.hasHorizontalScroller=YES;}
    if(cls==u"systreeview32"){auto* tree=(NSOutlineView*)table;tree.outlineTableColumn=column;tree.indentationPerLevel=12;tree.dataSource=target;tree.delegate=target;
        // Windows 的選取／焦點屬於項目，不使用 AppKit 額外的整個控制項藍框。
        tree.focusRingType=NSFocusRingTypeNone;scroll.focusRingType=NSFocusRingTypeNone;}
    scroll.documentView=table;window.control=table;window.view=scroll;
}
class CocoaGui final : public NativeGui {
    std::uint64_t fullscreen_window_{};
    CGDirectDisplayID fullscreen_display_{};
    CGDisplayModeRef original_display_mode_{};
    std::uint32_t written_display_mode_{};
    static std::uint32_t display_mode_id(CGDisplayModeRef mode){return static_cast<std::uint32_t>(CGDisplayModeGetIODisplayModeID(mode));}
    static GuiDisplayMode describe_display_mode(CGDisplayModeRef mode){
        GuiDisplayMode result;result.width=static_cast<std::uint32_t>(CGDisplayModeGetWidth(mode));result.height=static_cast<std::uint32_t>(CGDisplayModeGetHeight(mode));
        auto rate=CGDisplayModeGetRefreshRate(mode);
        if(!(rate>0)){CVDisplayLinkRef link=nullptr;if(CVDisplayLinkCreateWithCGDisplay(CGMainDisplayID(),&link)==kCVReturnSuccess){const auto duration=CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);if(!(duration.flags&kCVTimeIsIndefinite) && duration.timeValue>0)rate=static_cast<double>(duration.timeScale)/static_cast<double>(duration.timeValue);CVDisplayLinkRelease(link);}}
        if(rate>0 && std::isfinite(rate)){result.refresh_numerator=static_cast<std::uint32_t>(std::llround(rate*1000));result.refresh_denominator=1000;}
        result.native_id=display_mode_id(mode);return result;
    }
    static bool configure_display(CGDirectDisplayID display,CGDisplayModeRef mode){
        CGDisplayConfigRef config=nullptr;if(CGBeginDisplayConfiguration(&config)!=kCGErrorSuccess)return false;
        if(CGConfigureDisplayWithDisplayMode(config,display,mode,nullptr)!=kCGErrorSuccess){CGCancelDisplayConfiguration(config);return false;}
        return CGCompleteDisplayConfiguration(config,kCGConfigureForAppOnly)==kCGErrorSuccess;
    }
    bool restore_display_mode(){
        if(original_display_mode_){const auto current=CGDisplayCopyDisplayMode(fullscreen_display_);const bool ours=current && display_mode_id(current)==written_display_mode_;if(current)CGDisplayModeRelease(current);
            if(ours && written_display_mode_!=display_mode_id(original_display_mode_) && !configure_display(fullscreen_display_,original_display_mode_))return false;
            CGDisplayModeRelease(original_display_mode_);original_display_mode_=nullptr;
        }
        fullscreen_window_=0;fullscreen_display_=0;written_display_mode_=0;return true;
    }
    std::shared_ptr<NativeDesktopNotifications> desktop_notifications_;
    id activity_{};
    std::shared_ptr<NativeWinHelp> help_;
    static std::uint64_t desktop_time(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    bool raw_keyboard_{},raw_mouse_{},raw_noactivate_{};
    std::uint64_t pending_activation_{};
    bool interactive_activation_{},application_launched_{};
    id __strong app_deactivate_observer_{};
    std::map<CGDirectDisplayID,std::vector<float>> original_gamma_,written_gamma_;
    static std::vector<float> display_gamma(CGDirectDisplayID display){
        const auto capacity=CGDisplayGammaTableCapacity(display);if(!capacity || capacity>16384)return {};
        std::vector<float> tables(static_cast<std::size_t>(capacity)*3);uint32_t count=0;
        if(CGGetDisplayTransferByTable(display,capacity,tables.data(),tables.data()+capacity,tables.data()+2*capacity,&count)!=kCGErrorSuccess || !count || count>capacity)return {};
        if(count!=capacity){std::vector<float> compact(static_cast<std::size_t>(count)*3);for(unsigned c=0;c<3;++c)std::copy_n(tables.begin()+c*capacity,count,compact.begin()+c*count);return compact;}return tables;
    }
    void restore_gamma(){
        for(const auto& [display,original]:original_gamma_){
            try {
                const auto last=written_gamma_.find(display);if(last==written_gamma_.end() || display_gamma(display)!=last->second)continue;
                const auto count=static_cast<uint32_t>(original.size()/3);(void)CGSetDisplayTransferByTable(display,count,original.data(),original.data()+count,original.data()+2*count);
            }catch(const std::bad_alloc&){} // 解構時無法確認目前曲線的歸屬，不覆蓋其他應用程式的設定。
        }
    }
    bool cursor_hidden_{};NSCursor* cursor_{};
    std::map<std::uint64_t, Window> windows_;
    // WebView2 嵌入檢視：鍵為服務層的 Controller id，檢視為父視窗 client view 的子 view。
    std::map<std::uint64_t,WKWebView* __strong> webviews_;
    X64ArmWebViewDelegate* __strong webview_delegate_{[X64ArmWebViewDelegate new]};
    std::vector<std::uint64_t> z_order_;
    bool command_shortcut(std::uint64_t owner,NSEvent* event) {
        if(event.type!=NSEventTypeKeyDown || !(event.modifierFlags&NSEventModifierFlagCommand))return false;
        NSString* chars=event.charactersIgnoringModifiers.uppercaseString;
        if(chars.length!=1)return false;
        const auto key=[chars characterAtIndex:0];
        if(!((key>='A' && key<='Z') || (key>='0' && key<='9')))return false;
        const auto top=windows_.find(owner);if(top==windows_.end())return false;
        NSResponder* responder=top->second.native.firstResponder;
        NSView* focused_view=[responder isKindOfClass:NSView.class]?(NSView*)responder:nil;
        Window* focused=nullptr;std::uint64_t handle=owner;
        for(auto& [candidate,item]:windows_){
            if(item.alias || item.view.window!=top->second.native)continue;
            // 單行 Edit／ComboBox 使用共享 field editor，多行 Edit 有自己的 NSTextView。
            if(responder && (responder==item.editor || (item.control && responder==item.control.currentEditor))){focused=&item;handle=candidate;break;}
            if(focused_view && [focused_view isDescendantOf:item.view] && (!focused || [item.view isDescendantOf:focused->view])){focused=&item;handle=candidate;}
        }
        if(!focused)return false;
        const Window* ancestor=focused;
        for(std::size_t depth=0;ancestor && depth<=windows_.size();++depth){
            if(!ancestor->model.enabled || ancestor->view.hidden)return true;
            const auto parent=windows_.find(ancestor->model.parent);ancestor=parent==windows_.end()?nullptr:&parent->second;
        }
        const auto flags=event.modifierFlags;
        if([responder isKindOfClass:NSTextView.class] && !(flags&(NSEventModifierFlagControl|NSEventModifierFlagOption)) && (!(flags&NSEventModifierFlagShift) || key=='Z')){
            auto* editor=(NSTextView*)responder;editor.allowsUndo=YES;
            if(x64arm::gui_control_class(focused->model)==u"edit" && (key=='C' || key=='X' || key=='V')){
                [focused->target publishEditSelection:editor];return [focused->target editCommand:key=='C'?@selector(copy:):key=='X'?@selector(cut:):@selector(paste:) modifiers:flags];
            }
            // 使用 AppKit 原生編輯動作，保留選取、Unicode、undo 及密碼欄位保護。
            switch(key){
                case 'A':[editor selectAll:nil];return true;
                case 'C':[editor copy:nil];return true;
                case 'X':if(editor.editable)[editor cut:nil];return true;
                case 'V':if(editor.editable)[editor pasteAsPlainText:nil];return true;
                case 'Z':case 'Y':
                    if(editor.editable){NSUndoManager* undo=editor.undoManager;
                        if(key=='Y' || (flags&NSEventModifierFlagShift)){if(undo.canRedo)[undo redo];}
                        else if(undo.canUndo)[undo undo];
                    }return true;
                default:break;
            }
        }
        // 保留既有宿主選單快捷鍵（例如 Cmd+H 隱藏），其餘才送往客體。
        if([NSApp.mainMenu performKeyEquivalent:event])return true;
        // 客體自繪視窗、清單與應用程式快捷鍵沿用 Windows Ctrl／accelerator 路徑。
        constexpr unsigned scans[]={0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
        const unsigned scan=key>='A'?scans[key-'A']:key=='0'?0x0bU:static_cast<unsigned>(key-'1')+2U;
        const std::uint32_t modifiers=2U|((flags&NSEventModifierFlagShift)?1U:0U)|((flags&NSEventModifierFlagOption)?4U:0U);
        const std::uint64_t lparam=1U|(static_cast<std::uint64_t>(scan)<<16)|((modifiers&4U)?UINT64_C(1)<<29:0)|(event.isARepeat?UINT64_C(1)<<30:0);
        events_.push_back({handle,key,lparam,0x100,{},modifiers});
        events_.push_back({handle,key,lparam|0xc0000000U,0x101,{},modifiers});
        return true;
    }
    void position_owned_table(Window& window) {
        if(!window.native || !window.model.owner || ![window.control isKindOfClass:[NSTableView class]])return;
        const auto found=windows_.find(window.model.owner);if(found==windows_.end() || !found->second.view.window)return;
        const auto& owner=found->second;std::int64_t x=0,y=0;const Window* ancestor=&owner;
        // 客體螢幕原點可與 AppKit 視窗位置不同，先對 owner client 求相對位置。
        for(std::size_t depth=0;ancestor && depth<=windows_.size();++depth){
            const auto inset=gui_frame_insets(ancestor->model);x+=static_cast<std::int64_t>(ancestor->model.x)+inset.left;y+=static_cast<std::int64_t>(ancestor->model.y)+inset.top;
            const auto parent=windows_.find(ancestor->model.parent);ancestor=parent==windows_.end()?nullptr:&parent->second;
        }
        const auto dx=static_cast<CGFloat>(static_cast<std::int64_t>(window.model.x)-x),dy=static_cast<CGFloat>(static_cast<std::int64_t>(window.model.y)-y);
        const NSPoint point=[owner.view convertPoint:NSMakePoint(dx,owner.view.flipped?dy:owner.view.bounds.size.height-dy) toView:nil];
        [window.native setFrameTopLeftPoint:[owner.view.window convertPointToScreen:point]];
    }
    void apply_z_order(std::uint64_t handle) {
        const auto found=windows_.find(handle);if(found==windows_.end() || found->second.alias)return;
        auto& target=found->second;Window* above=nullptr;Window* below=nullptr;bool seen=false;
        for(const auto h:z_order_){if(h==handle){seen=true;continue;}const auto other=windows_.find(h);
            if(other==windows_.end() || other->second.alias || other->second.model.parent!=target.model.parent)continue;
            auto& candidate=other->second;
            if(target.native){if(!candidate.native || !candidate.native.visible)continue;}
            else if((candidate.mdi_frame?:candidate.view).superview!=(target.mdi_frame?:target.view).superview)continue;
            if(!seen)above=&candidate;else {below=&candidate;break;}}
        if(target.native){
            // 排序不得順便顯示 hidden HWND；show 後再套用已保存的相對位置。
            if(!target.native.visible)return;
            target.native.level=target.fullscreen?NSMainMenuWindowLevel+1:(target.model.ex_style&8U)?NSFloatingWindowLevel:NSNormalWindowLevel;
            // AppKit 可能保持剛成為 key 的視窗在前；同步錨點的相對順序。
            if(above){[target.native orderWindow:NSWindowBelow relativeTo:above->native.windowNumber];[above->native orderWindow:NSWindowAbove relativeTo:target.native.windowNumber];}
            else if(below)[target.native orderWindow:NSWindowAbove relativeTo:below->native.windowNumber];
        } else {NSView* outer=target.mdi_frame?:target.view;if(outer.superview){NSView* anchor=above?(above->mdi_frame?:above->view):below?(below->mdi_frame?:below->view):nil;[outer.superview addSubview:outer positioned:above?NSWindowBelow:NSWindowAbove relativeTo:anchor];}}
        dirty_=true;
    }
    std::map<std::uint64_t,std::shared_ptr<RasterSurface>> graphics_surfaces_;
    NSDate* graphics_snapshot_time_{};
    std::vector<GuiMessage> events_;
    NSView* __strong mouse_target_{};
    std::filesystem::path snapshot_;
    std::filesystem::path native_snapshot_directory_;
    NSDate* native_snapshot_time_{};
    std::shared_ptr<NativeSnapshotBatch> native_snapshot_batch_{std::make_shared<NativeSnapshotBatch>()};
    GuiIcon dock_icon_{};
    bool dirty_{};
    NSArray* __strong automation_{};
    std::array<std::int32_t,2> automation_cursor_{};
    void automation_cursor_at(const Window& window,NSPoint point){
        if(!std::isfinite(point.x) || !std::isfinite(point.y))throw std::runtime_error("GUI 測試游標座標無效");
        const Window* ancestor=&window;
        for(std::size_t depth=0;ancestor && depth<=windows_.size();++depth){
            const auto inset=gui_frame_insets(ancestor->model);point.x+=ancestor->model.x+static_cast<CGFloat>(inset.left);point.y+=ancestor->model.y+static_cast<CGFloat>(inset.top);
            const auto parent=windows_.find(ancestor->model.parent);ancestor=parent==windows_.end()?nullptr:&parent->second;
        }
        automation_cursor_={static_cast<std::int32_t>(std::clamp<CGFloat>(point.x,INT32_MIN,INT32_MAX)),static_cast<std::int32_t>(std::clamp<CGFloat>(point.y,INT32_MIN,INT32_MAX))};
    }
    NSString* __strong automation_live_path_{};
    bool automation_input_active_{}; // 背景顯示下由自動化 window_activate 模擬的前景輸入狀態。
    NSDate* automation_read_time_{};
    id __strong automation_input_monitor_{};
    NSMutableSet<NSEvent*>* __strong automation_posted_events_{[NSMutableSet set]};
    std::shared_ptr<NativeSnapshotBatch> automation_snapshot_;
    NSDate* __strong automation_snapshot_started_{};
    NSMutableArray* __strong automation_errors_{[NSMutableArray array]};
    std::size_t action_{};
    NSDate* __strong last_action_{};
    NSString* __strong state_path_{};
    NSMutableArray* __strong popup_history_{[NSMutableArray array]};
    struct PopupFrame {NSMenu* __strong menu;NSMutableDictionary* __strong record;};
    std::vector<PopupFrame> popup_stack_;
    NSBitmapImageRep* capture_view(NSView* view){
        if(!view || !(view.bounds.size.width>0) || !(view.bounds.size.height>0))return nil;
        [view displayIfNeeded];NSBitmapImageRep* bitmap=[view bitmapImageRepForCachingDisplayInRect:view.bounds];
        if(!bitmap)return nil;
        std::memset(bitmap.bitmapData,0,static_cast<std::size_t>(bitmap.bytesPerRow)*static_cast<std::size_t>(bitmap.pixelsHigh));
        [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
        std::vector<std::pair<std::size_t,std::uint64_t>> layers;
        for(const auto& [handle,surface]:graphics_surfaces_){(void)surface;const auto window=windows_.find(handle);if(window==windows_.end() || ![window->second.view isDescendantOf:view])continue;
            std::size_t depth=0;for(NSView* current=window->second.view;current && current!=view;current=current.superview)++depth;layers.emplace_back(depth,handle);
        }
        // 子視窗先補底，再由父視窗補上剩餘透明區域，保留最近祖先的 GDI 背景。
        std::sort(layers.rbegin(),layers.rend());
        for(const auto& [depth,handle]:layers){(void)depth;const auto window=windows_.find(handle);const auto& surface=graphics_surfaces_.at(handle);
            if(window->second.view.hiddenOrHasHiddenAncestor)continue;
            if([window->second.view isKindOfClass:[X64ArmGuestButton class]] && ![(X64ArmGuestButton*)window->second.view isOwnerDraw])continue;
            auto pixels=surface->snapshot();auto rep=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:nullptr pixelsWide:pixels.width pixelsHigh:pixels.height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:pixels.width*4 bitsPerPixel:32];std::memcpy(rep.bitmapData,pixels.pixels.data(),pixels.pixels.size()*4);
            auto image=[[NSImage alloc]initWithSize:NSMakeSize(pixels.width,pixels.height)];[image addRepresentation:rep];
            const auto rectangle=[&](NSView* item){auto frame=[item convertRect:item.bounds toView:view];frame.origin.y=view.bounds.size.height-frame.origin.y-frame.size.height;return frame;};
            const auto frame=rectangle(window->second.view);
            [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];
            NSRectClip(frame);
            std::function<void(NSView*)> exclude=[&](NSView* item){if(item.hiddenOrHasHiddenAncestor)return;
                if([item isKindOfClass:[X64ArmGuestView class]]){auto* guest=(X64ArmGuestView*)item;
                    if(guest.model && (guest.model->ex_style&0x20U) && !guest.rasterManaged && !guest.openGLManaged && guest.model->drawing.empty() && guest.model->layered_pixels.empty()){
                        // 透明輸入 view 不遮擋背景，但它的真正繪圖子視窗仍可能遮擋。
                        for(NSView* child in item.subviews)exclude(child);return;
                    }
                }
                const auto area=NSIntersectionRect(frame,rectangle(item));if(NSIsEmptyRect(area))return;
                // 逐一相交，避免重疊排除矩形的 even-odd 交集重新露出父表面。
                auto* clip=[NSBezierPath bezierPathWithRect:frame];[clip appendBezierPathWithRect:area];clip.windingRule=NSWindingRuleEvenOdd;[clip addClip];
            };
            // Raster 是宿主 view 的背景，不能覆蓋它的原生控制項或客體子視窗。
            for(NSView* child in window->second.view.subviews)if(![child.identifier isEqualToString:@"x64arm-raster-presentation"])exclude(child);
            for(NSView* current=window->second.view;current && current!=view;current=current.superview){
                bool above=false;for(NSView* sibling in current.superview.subviews){if(above)exclude(sibling);else if(sibling==current)above=true;}
                if(current.superview)NSRectClip(rectangle(current.superview));
            }
            [image drawInRect:frame fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];[NSGraphicsContext restoreGraphicsState];
            // AppKit 的按鈕／ComboBox 快照含透明底色；不能排除其矩形後留下空洞。
            // 從下方合成祖先 Raster，讓原生文字與半透明外觀保留在上層。
            [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];NSRectClip(frame);
            for(NSView* current=window->second.view;current && current!=view;current=current.superview)if(current.superview)NSRectClip(rectangle(current.superview));
            [image drawInRect:frame fromRect:NSZeroRect operation:NSCompositingOperationDestinationOver fraction:1];[NSGraphicsContext restoreGraphicsState];
        }
        return bitmap;
    }
    void snapshot(NSView* view,const std::filesystem::path& path){
        if(path.empty() || !view.window.visible)return;
        NSBitmapImageRep* bitmap=capture_view(view);if(!bitmap)return;
        NSData* png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if(!png || ![png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES])throw std::runtime_error("GUI PNG 快照寫入失敗");
    }
    void diagnostics(){
        if(state_path_){
            NSMutableArray* entries=[NSMutableArray array];
            for(const auto& [h,w]:windows_){NSMutableArray* items=[NSMutableArray array];for(const auto& item:w.model.items){NSMutableArray* subitems=[NSMutableArray array];for(const auto& text:item.subitems)[subitems addObject:nsstring(text)];[items addObject:@{@"handle":@(item.handle),@"parent":@(item.parent),@"text":nsstring(item.text),@"state":@(item.state),@"image":@(item.image),@"selected":@((item.state&((gui_control_class(w.model)==u"syslistview32" || gui_control_class(w.model)==u"systreeview32")?2U:1U))!=0),@"subitems":subitems}];}
                NSMutableArray* columns=[NSMutableArray array];for(const auto& column:w.model.columns)[columns addObject:@{@"text":nsstring(column.text),@"width":@(column.width),@"format":@(column.format),@"subitem":@(column.subitem),@"order":@(column.order),@"image":@(column.image),@"data":@(column.data),@"bitmap":@(column.bitmap),@"state":@(column.state),@"filter_type":@(column.filter.type),@"filter_display":nsstring(column.filter_display)}];
                NSMutableArray* native_columns=[NSMutableArray array];if([w.control isKindOfClass:NSTableView.class]){auto* table=(NSTableView*)w.control;
                    for(NSUInteger position=0;position<table.tableColumns.count;++position){NSTableColumn* column=table.tableColumns[position];const auto index=static_cast<std::size_t>(column.identifier.integerValue);const auto rect=[table rectOfColumn:static_cast<NSInteger>(position)];
                        [native_columns addObject:@{@"text":column.title,@"logical_index":@(index),@"subitem":@(index<w.model.columns.size()?w.model.columns[index].subitem:0),@"rect":@[@(rect.origin.x),@(rect.origin.y),@(rect.size.width),@(rect.size.height)]}];}}
                NSMutableArray* toolbar=[NSMutableArray array];for(const auto& button:w.model.toolbar_buttons)[toolbar addObject:@{@"command":@(button.command),@"text":nsstring(button.text),@"image":@(button.image),@"state":@(button.state),@"style":@(button.style),@"rect":@[@(button.rectangle[0]),@(button.rectangle[1]),@(button.rectangle[2]),@(button.rectangle[3])]}];NSMutableArray* status=[NSMutableArray array];for(const auto& part:w.model.status_parts)[status addObject:@{@"text":nsstring(part.text),@"edge":@(part.edge)}];
                NSMutableArray* drawing=[NSMutableArray array];for(const auto& draw:w.model.drawing)if(draw.kind==GuiDraw::Kind::Text)[drawing addObject:@{@"text":nsstring(draw.text),@"x":@(draw.x),@"y":@(draw.y),@"color":@(draw.color),@"font_face":nsstring(draw.font.face),@"font_height":@(draw.font.height),@"font_weight":@(draw.font.weight),@"font_italic":@(draw.font.italic),@"cell_width":@(draw.cell_width)}];
                NSMutableArray* native_buttons=[NSMutableArray array];for(NSView* child in w.view.subviews)if([child isKindOfClass:[NSButton class]]){NSButton* b=(NSButton*)child;const bool is_toolbar=[b isKindOfClass:X64ArmToolbarButton.class];[native_buttons addObject:@{@"command":@(b.tag),@"title":b.title,@"guest_index":@(is_toolbar?[b.cell.representedObject unsignedLongLongValue]:0),@"enabled":@(b.enabled),@"checked":@(b.state!=NSControlStateValueOff),@"pressed":@(is_toolbar && ((X64ArmToolbarButton*)b).guestPressed),@"indeterminate":@(is_toolbar && ((X64ArmToolbarButton*)b).guestIndeterminate),@"has_image":@(b.image!=nil),@"font_size":@(b.font.pointSize),@"text_width":@([b.title sizeWithAttributes:@{NSFontAttributeName:b.font}].width*(is_toolbar?((X64ArmToolbarButton*)b).guestTextScale:1)),@"width":@(b.frame.size.width),@"height":@(b.frame.size.height),@"image_width":@(b.image.size.width),@"image_height":@(b.image.size.height),@"image_position":@(b.imagePosition)}];}
                NSMutableArray* menu_titles=[NSMutableArray array];for(NSView* child in w.menu_bar.subviews)if([child isKindOfClass:[NSButton class]])[menu_titles addObject:((NSButton*)child).title];
                NSMutableArray* native_tooltips=[NSMutableArray array];for(const auto& [tag,index]:w.status_tooltips){NSString* text=[(id<NSViewToolTipOwner>)w.view view:w.view stringForToolTip:tag point:NSZeroPoint userData:reinterpret_cast<void*>(index+1)];[native_tooltips addObject:@{@"part":@(index),@"text":text}];}
                NSMutableArray* native_lines=[NSMutableArray array];if(w.editor)if(const auto lines=text_line_ranges(h))for(const auto& line:*lines)[native_lines addObject:@[@(line[0]),@(line[1])]];
                // 分頁控制項的項目與版面矩形，供自動化點擊與診斷。
                NSMutableArray* tabs=[NSMutableArray array];if(w.model.tab){const auto& t=*w.model.tab;for(std::size_t i=0;i<t.items.size();++i){const auto r=i<t.rectangles.size()?t.rectangles[i]:std::array<std::int32_t,4>{};[tabs addObject:@{@"text":nsstring(t.items[i].text),@"state":@(t.items[i].state),@"rect":@[@(r[0]),@(r[1]),@(r[2]),@(r[3])]}];}}
                NSMutableDictionary* calendar=[NSMutableDictionary dictionary];if(w.model.calendar){const auto& c=*w.model.calendar;NSMutableArray* cells=[NSMutableArray array];for(const auto& cell:c.cells)[cells addObject:@{@"date":@[@(cell.date[0]),@(cell.date[1]),@(cell.date[3])],@"rect":@[@(cell.rect[0]),@(cell.rect[1]),@(cell.rect[2]),@(cell.rect[3])],@"text":nsstring(cell.text),@"selected":@(cell.selected),@"disabled":@(cell.disabled),@"action":@(cell.action),@"bold":@(cell.bold),@"row":@(cell.row),@"column":@(cell.column),@"calendar":@(cell.calendar),@"end":@[@(cell.end[0]),@(cell.end[1]),@(cell.end[3])]}];[calendar addEntriesFromDictionary:@{@"value":@[@(c.value[0]),@(c.value[1]),@(c.value[3]),@(c.value[4]),@(c.value[5]),@(c.value[6])],@"last":@[@(c.last[0]),@(c.last[1]),@(c.last[3])],@"none":@(c.none),@"open":@(c.open),@"month_window":@(c.month_window),@"calendar_count":@(c.calendar_count),@"view":@(c.view),@"border":@(c.border),@"cells":cells}];}
                [entries addObject:@{@"handle":@(h),@"id":@(w.model.id),@"parent":@(w.model.parent),@"class":nsstring(w.model.class_name),@"control_class":nsstring(gui_control_class(w.model)),@"text":nsstring(w.model.title),@"visible":@(w.view.window.visible && !w.view.hidden),@"raster_surface":@(graphics_surfaces_.contains(h)),@"native_key":@(w.view.window.keyWindow),@"native_active":@(NSApp.active),@"native_owner_pid":@(::getpid()),@"native_window_number":@(w.view.window.windowNumber),@"native_screen_display":@([w.view.window.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]),@"native_sharing_type":@(w.view.window.sharingType),@"display_affinity":@(w.model.display_affinity),@"native_activation_allowed":@(interactive_activation_),@"native_pending_activation":@(pending_activation_),@"native_application_launched":@(application_launched_),@"enabled":@(w.model.enabled),@"x":@(w.model.x),@"y":@(w.model.y),@"width":@(w.model.width),@"height":@(w.model.height),@"style":@(w.model.style),@"ex_style":@(w.model.ex_style),@"check":@(w.model.check),@"selection":@(w.model.selection),@"items":items,@"columns":columns,@"native_columns":native_columns,@"header_visible":@(w.model.header && w.model.visible),@"toolbar":toolbar,@"native_buttons":native_buttons,@"native_tooltips":native_tooltips,@"native_edit_line_ranges":native_lines,@"native_edit_wrap":@(w.editor && w.editor.textContainer.widthTracksTextView),@"native_edit_font_size":@(w.editor.font.pointSize),@"image_count":@(w.model.list_icons.size()),@"native_tree_rows":@([w.control isKindOfClass:NSOutlineView.class]?[(NSOutlineView*)w.control numberOfRows]:0),@"menu_titles":menu_titles,@"icon":@(w.model.icon),@"status":status,@"drawing":drawing,@"calendar":calendar,@"tabs":tabs}];}
            NSData* data=[NSJSONSerialization dataWithJSONObject:@{@"windows":entries,@"actions":@(action_),@"automation_errors":automation_errors_,@"popups":popup_history_} options:NSJSONWritingPrettyPrinted error:nil];[data writeToFile:state_path_ atomically:YES];
        }
    }
    void automate(){
        if(automation_live_path_ && (!automation_read_time_ || -automation_read_time_.timeIntervalSinceNow>=0.25)){
            automation_read_time_=[NSDate date];NSData* data=[NSData dataWithContentsOfFile:automation_live_path_];
            if(data.length>4U*1024U*1024U)throw std::runtime_error("GUI 即時操作資料超出上限");
            id value=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
            if([value isKindOfClass:[NSArray class]] && [value count]>=action_){
                NSArray* next=value;
                for(std::size_t i=0;i<action_;++i)if(![automation_[i] isEqual:next[i]])throw std::runtime_error("GUI 即時操作不能修改已執行步驟");
                automation_=next;
            }
        }
        if(!automation_ || action_>=automation_.count || -last_action_.timeIntervalSinceNow<0.25)return;
        NSDictionary* step=automation_[action_];NSString* operation=step[@"action"];Window* selected=nullptr;
        if(([operation isEqualToString:@"snapshot"] || [operation isEqualToString:@"snapshot_native"]) && (![step[@"path"] isKindOfClass:NSString.class] || ![step[@"path"] length]))throw std::runtime_error("GUI 快照測試必須指定輸出路徑");
        if(step[@"delay"] && -last_action_.timeIntervalSinceNow<[step[@"delay"] doubleValue])return;
        for(auto& [h,w]:windows_){(void)h;if((!w.view.window.visible || w.view.hiddenOrHasHiddenAncestor) && ![operation isEqualToString:@"window_restore"] && ![operation isEqualToString:@"assert_window"])continue;
            const Window* top=&w;bool enabled=w.model.enabled;
            for(unsigned depth=0;top->model.parent && depth<64;++depth){const auto parent=windows_.find(top->model.parent);if(parent==windows_.end())break;top=&parent->second;enabled=enabled && top->model.enabled;}
            if(!enabled && ![operation isEqualToString:@"snapshot"])continue;
            if(step[@"parent_text"] && ![label(top->model.title) isEqualToString:step[@"parent_text"]])continue;
            if(step[@"owner_text"]){const auto owner=windows_.find(top->model.owner);if(owner==windows_.end() || ![label(owner->second.model.title) isEqualToString:step[@"owner_text"]])continue;}
            if(step[@"handle"] && h!=[step[@"handle"] unsignedLongLongValue])continue;
            if(step[@"id"] && w.model.id!=[step[@"id"] unsignedLongLongValue])continue;
            if(step[@"text"] && ![label(w.model.title) isEqualToString:step[@"text"]])continue;
            if(step[@"class"] && ![nsstring(w.model.class_name) isEqualToString:step[@"class"]])continue;
            if(step[@"draw_contains"]){NSMutableString* painted=[NSMutableString string];for(const auto& draw:w.model.drawing)if(draw.kind==GuiDraw::Kind::Text)[painted appendString:nsstring(draw.text)];if(![painted containsString:step[@"draw_contains"]])continue;}
            selected=&w;break;
        }
        if(!selected)return;auto& w=*selected;
        if(step[@"calendar_owner"]){if(!w.model.calendar || !w.model.calendar->picker_owner)return;const auto owner=windows_.find(w.model.calendar->picker_owner);if(owner==windows_.end() || owner->second.model.id!=[step[@"calendar_owner"] unsignedLongLongValue] || !owner->second.model.calendar || !owner->second.model.calendar->open)return;}
        if([operation isEqualToString:@"window_activate"]){
            if(!background_display_enabled()){if(@available(macOS 14.0,*))[NSApp activate];else [NSApp activateIgnoringOtherApps:YES];[w.view.window makeKeyAndOrderFront:nil];[w.view.window makeFirstResponder:w.view];}
            // 背景顯示不搶宿主焦點：只送與 windowDidBecomeKey 相同的客體啟用事件，讓依前景狀態接收輸入的程式（如 UE4）可測。
            else {
                // 同時視為輸入作用中：依前景投遞的 Raw Input（WM_INPUT）在宿主未啟用時本會被丟棄。
                automation_input_active_=true;
                for(const auto& [handle,window]:windows_)if(&window==&w){events_.push_back({handle,1,0,0x1000d});break;}
            }
        }
        else if([operation isEqualToString:@"window_resize"] && w.native){[w.native setContentSize:NSMakeSize([step[@"width"] doubleValue],[step[@"height"] doubleValue]+(w.model.menu?20:0))];}
        else if([operation isEqualToString:@"window_minimize"] && w.native){[w.native performMiniaturize:nil];}
        else if([operation isEqualToString:@"window_maximize"] && w.native){[w.native zoom:nil];}
        else if([operation isEqualToString:@"window_restore"] && w.native){if(w.native.miniaturized)[w.native deminiaturize:nil];else if(w.model.show_command==3)[w.native zoom:nil];}
        else if([operation isEqualToString:@"assert_window"] && w.native){
            const bool matches=(!step[@"minimized"] || w.native.miniaturized==[step[@"minimized"] boolValue]) &&
                (!step[@"state"] || w.model.show_command==[step[@"state"] unsignedIntValue]) &&
                (!step[@"width"] || w.model.width==[step[@"width"] unsignedIntValue]) &&
                (!step[@"height"] || w.model.height==[step[@"height"] unsignedIntValue]) &&
                (!step[@"native_key"] || w.native.keyWindow==[step[@"native_key"] boolValue]) &&
                (!step[@"native_active"] || NSApp.active==[step[@"native_active"] boolValue]);
            // 沒有新的客體繪圖時，舊快照不代表等待中的真實 key／active 狀態。
            if(!matches){diagnostics();return;}
        }
        else if([operation isEqualToString:@"dock_icon_snapshot"]){if(dock_icon_.image.empty() && dock_icon_.rgba.empty() && !dock_icon_.system)return;
            NSBitmapImageRep* bitmap=[[NSBitmapImageRep alloc] initWithData:NSApp.applicationIconImage.TIFFRepresentation];NSData* png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];if(![png writeToFile:step[@"path"] atomically:YES])throw std::runtime_error("Dock 圖示快照失敗");
        }
        else if([operation isEqualToString:@"edit"] && [w.control isKindOfClass:[NSTextField class]]){
            w.control.stringValue=step[@"value"]?:@"";[w.target controlTextDidChange:[NSNotification notificationWithName:NSControlTextDidChangeNotification object:w.control]];
        }else if([operation isEqualToString:@"spin"] && [w.control isKindOfClass:NSStepper.class]){
            const auto delta=[step[@"value"] intValue];if(delta!=1 && delta!=-1)throw std::runtime_error("Up-Down 測試只接受單次增加或減少");auto* spin=(NSStepper*)w.control;spin.doubleValue=delta;[spin sendAction:spin.action to:spin.target];
        }else if([operation isEqualToString:@"calendar_mouse"] && w.model.calendar && [w.view isKindOfClass:[X64ArmGuestView class]]){
            const x64arm::GuiDateCell* chosen=nullptr;
            for(const auto& cell:w.model.calendar->cells){bool match=false;
                if([step[@"date"] isKindOfClass:NSArray.class] && [step[@"date"] count]==3){NSArray* date=step[@"date"];match=cell.action==1 && cell.date[0]==[date[0] unsignedIntValue] && cell.date[1]==[date[1] unsignedIntValue] && cell.date[3]==[date[2] unsignedIntValue];}
                else if([step[@"cell"] isKindOfClass:NSArray.class] && [step[@"cell"] count]==2){NSArray* index=step[@"cell"];match=!w.model.calendar->picker && cell.row==[index[0] intValue] && cell.column==[index[1] intValue] && cell.calendar==[step[@"calendar"] intValue] && cell.action>0;}
                else if(step[@"field"])match=w.model.calendar->picker && cell.action==[step[@"field"] intValue]+1;
                else if(step[@"part"]){NSString* part=step[@"part"];match=cell.action==([part isEqualToString:@"checkbox"]?-4:[part isEqualToString:@"arrow"]?-5:[part isEqualToString:@"previous"]?-1:[part isEqualToString:@"next"]?-2:[part isEqualToString:@"today"]?-3:[part isEqualToString:@"header"]?-6:INT_MIN);}
                if(match && !cell.disabled){chosen=&cell;if(!cell.trailing)break;}
            }
            if(!chosen)throw std::runtime_error("日期測試找不到可操作的欄位或日期");const auto& r=chosen->rect;CGFloat y=(r[1]+r[3])/2.0;if([step[@"half"] isEqualToString:@"top"])y=r[1]+(r[3]-r[1])/4.0;if([step[@"half"] isEqualToString:@"bottom"])y=r[1]+(r[3]-r[1])*3/4.0;
            NSPoint local=NSMakePoint((r[0]+r[2])/2.0,y);automation_cursor_at(w,local);const auto point=[w.view convertPoint:local toView:nil];NSString* phase=step[@"phase"]?:@"click";
            const auto event=[&](NSEventType type){return [NSEvent mouseEventWithType:type location:point modifierFlags:automation_modifiers(step) timestamp:0 windowNumber:w.view.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];};
            if([phase isEqualToString:@"click"] || [phase isEqualToString:@"down"])[w.view mouseDown:event(NSEventTypeLeftMouseDown)];
            if([phase isEqualToString:@"drag"])[w.view mouseDragged:event(NSEventTypeLeftMouseDragged)];
            if([phase isEqualToString:@"click"] || [phase isEqualToString:@"up"])[w.view mouseUp:event(NSEventTypeLeftMouseUp)];
        }else if([operation isEqualToString:@"key"] && [w.control isKindOfClass:X64ArmGuestButton.class]){
            NSDictionary* keys=@{@"space":@(32),@"tab":@(9),@"return":@(13),@"escape":@(27),@"left":@(NSLeftArrowFunctionKey),@"right":@(NSRightArrowFunctionKey),@"up":@(NSUpArrowFunctionKey),@"down":@(NSDownArrowFunctionKey)};NSNumber* code=keys[step[@"value"]];if(!code)throw std::runtime_error("按鈕測試的鍵名無效");
            const unichar value=static_cast<unichar>(code.unsignedIntValue);NSString* chars=[NSString stringWithCharacters:&value length:1];focus(w.target.guestHandle);events_.push_back({w.target.guestHandle,0,0,0x10013});
            for(const auto type:{NSEventTypeKeyDown,NSEventTypeKeyUp}){auto* event=[NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:automation_modifiers(step) timestamp:0 windowNumber:w.view.window.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:0];if(type==NSEventTypeKeyDown)[w.control keyDown:event];else [w.control keyUp:event];}
        }else if([operation isEqualToString:@"key"] && (w.editor || [w.control isKindOfClass:[NSTextField class]])){
            const auto modifiers=automation_modifiers(step);NSString* key=step[@"value"];
            const SEL command=[key isEqualToString:@"return"]?@selector(insertNewline:):[key isEqualToString:@"escape"]?@selector(cancelOperation:):[key isEqualToString:@"tab"]?((modifiers&NSEventModifierFlagShift)?@selector(insertBacktab:):@selector(insertTab:)):
                [key isEqualToString:@"copy"] || ([key isEqualToString:@"c"] && (modifiers&(NSEventModifierFlagControl|NSEventModifierFlagCommand)))?@selector(copy:):
                [key isEqualToString:@"cut"] || ([key isEqualToString:@"x"] && (modifiers&(NSEventModifierFlagControl|NSEventModifierFlagCommand)))?@selector(cut:):
                [key isEqualToString:@"paste"] || ([key isEqualToString:@"v"] && (modifiers&(NSEventModifierFlagControl|NSEventModifierFlagCommand)))?@selector(paste:):nullptr;
            focus(w.target.guestHandle);auto* editor=w.editor?:(NSTextView*)w.control.currentEditor;
            if(!command || !editor || !(w.editor?[w.target textView:editor doCommandBySelector:command]:[w.target control:w.control textView:editor doCommandBySelector:command]))throw std::runtime_error("文字控制項測試按鍵未由原生 delegate 處理");
        }else if([operation isEqualToString:@"resize"] && w.native){const auto width=[step[@"width"] unsignedIntValue],height=[step[@"height"] unsignedIntValue];if(!width || !height || width>4096 || height>4096)throw std::runtime_error("GUI 測試視窗尺寸超出範圍");[w.native setContentSize:NSMakeSize(width,height+(w.model.menu?20:0))];}
        else if([operation isEqualToString:@"type"] && [w.view isKindOfClass:[X64ArmGuestView class]]){
            [w.view.window makeFirstResponder:w.view];NSString* value=step[@"value"]?:@"";
            [(X64ArmGuestView*)w.view insertText:value replacementRange:NSMakeRange(NSNotFound,0)];
        }else if([operation isEqualToString:@"key"] && [w.view isKindOfClass:[X64ArmGuestView class]]){
            NSDictionary* keys=@{@"space":@(32),@"tab":@(9),@"backspace":@(8),@"insert":@(NSInsertFunctionKey),@"delete":@(NSDeleteFunctionKey),@"home":@(NSHomeFunctionKey),@"end":@(NSEndFunctionKey),@"pageup":@(NSPageUpFunctionKey),@"pagedown":@(NSPageDownFunctionKey),@"up":@(NSUpArrowFunctionKey),@"down":@(NSDownArrowFunctionKey),@"left":@(NSLeftArrowFunctionKey),@"right":@(NSRightArrowFunctionKey),@"return":@(13),@"escape":@(27),@"add":@(43),@"subtract":@(45)};
            NSNumber* code=keys[step[@"value"]];
            // 單字元快捷鍵沿用正常 keyDown／keyUp，避免只送 WM_CHAR。
            NSString* keyName=step[@"value"];
            if(!code && keyName.length==1){const unichar value=[keyName characterAtIndex:0];if(value>=32 && value<=126)code=@(value);}
            // 自動化沿用真實鍵盤的 F1–F12 路徑，供程式內建診斷及快捷鍵 Smoke 使用。
            if(!code)for(unsigned function=1;function<=12;++function)
                if([step[@"value"] isEqualToString:[NSString stringWithFormat:@"f%u",function]]){code=@(NSF1FunctionKey+function-1);break;}
            if(!code)throw std::runtime_error("GUI 測試按鍵名稱無效");
            const unichar character=static_cast<unichar>(code.unsignedIntValue);NSString* chars=[NSString stringWithCharacters:&character length:1];
            const NSEventModifierFlags modifiers=automation_modifiers(step);
            const unsigned short hardware=[keyName isEqualToString:@"add"]?69:[keyName isEqualToString:@"subtract"]?78:0;
            NSString* phase=step[@"phase"]?:@"tap";
            if(![phase isEqualToString:@"tap"] && ![phase isEqualToString:@"down"] && ![phase isEqualToString:@"up"])throw std::runtime_error("GUI 測試按鍵階段無效");
            [w.view.window makeFirstResponder:w.view];
            // 自動化按鍵不經實體鍵盤狀態，標成注入：服務端比照 SendInput 一併產生 Raw Input（WM_INPUT）。
            const auto first_event=events_.size();
            if(![phase isEqualToString:@"up"]){NSEvent* event=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:modifiers timestamp:0 windowNumber:w.view.window.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:hardware];[w.view keyDown:event];}
            if(![phase isEqualToString:@"down"]){NSEvent* event=[NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:modifiers timestamp:0 windowNumber:w.view.window.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:hardware];[w.view keyUp:event];}
            for(auto i=first_event;i<events_.size();++i)if(events_[i].message>=0x100 && events_[i].message<=0x105)events_[i].injected=true;
        }else if([operation isEqualToString:@"mouse"] && [w.control isKindOfClass:NSOutlineView.class] && [step[@"phase"] isEqualToString:@"tap"]){
            auto* target=(NSOutlineView*)w.control;const auto point=[target convertPoint:NSMakePoint([step[@"x"] doubleValue],[step[@"y"] doubleValue]) toView:nil];const auto timestamp=NSProcessInfo.processInfo.systemUptime;
            const auto event=[&](NSEventType type){return [NSEvent mouseEventWithType:type location:point modifierFlags:automation_modifiers(step) timestamp:timestamp windowNumber:target.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];};
            // AppKit 的 disclosure tracking 使用真正的配對 NSEvent，客體仍可同步否決。
            [NSApp postEvent:event(NSEventTypeLeftMouseUp) atStart:YES];[target mouseDown:event(NSEventTypeLeftMouseDown)];[target mouseUp:event(NSEventTypeLeftMouseUp)];
        }else if([operation isEqualToString:@"mouse"] && ([w.view isKindOfClass:[X64ArmGuestView class]] || [w.view isKindOfClass:[X64ArmHeaderView class]])){
            // 與自動化按鍵／滾輪相同標記為注入：服務層依此產生 Raw Input 按鍵旗標。
            const auto first_event=events_.size();
            struct MarkInjected {std::vector<GuiMessage>& events;std::size_t first;~MarkInjected(){for(auto i=first;i<events.size();++i)if(events[i].message>=0x200 && events[i].message<=0x209)events[i].injected=true;}} mark{events_,first_event};
            NSString* phase=step[@"phase"]?:@"down";NSString* button=step[@"button"]?:@"left";
            const bool down=[phase isEqualToString:@"down"],up=[phase isEqualToString:@"up"],move=[phase isEqualToString:@"move"],right=[button isEqualToString:@"right"],middle=[button isEqualToString:@"middle"];
            if((!down && !up && !move && ![phase isEqualToString:@"drag"]) || (!right && !middle && ![button isEqualToString:@"left"]))throw std::runtime_error("GUI 滑鼠測試的按鍵或動作無效");
            const auto type=right?(down?NSEventTypeRightMouseDown:up?NSEventTypeRightMouseUp:NSEventTypeRightMouseDragged):middle?(down?NSEventTypeOtherMouseDown:up?NSEventTypeOtherMouseUp:NSEventTypeOtherMouseDragged):(down?NSEventTypeLeftMouseDown:up?NSEventTypeLeftMouseUp:NSEventTypeLeftMouseDragged);
            const auto local=NSMakePoint([step[@"x"] doubleValue],[step[@"y"] doubleValue]);automation_cursor_at(w,local);
            const auto point=[w.view convertPoint:local toView:nil];
            const NSEventModifierFlags modifiers=automation_modifiers(step);
            NSEvent* event=[NSEvent mouseEventWithType:move?NSEventTypeMouseMoved:type location:point modifierFlags:modifiers timestamp:0 windowNumber:w.view.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
            if(middle){CGEventRef native=CGEventCreateCopy(event.CGEvent);if(!native)throw std::runtime_error("無法建立中鍵測試事件");CGEventSetIntegerValueField(native,kCGMouseEventButtonNumber,2);event=[NSEvent eventWithCGEvent:native];CFRelease(native);}
            if(move)[(id)w.view guestMouse:event message:0x200 button:0 pressed:NO];
            else if(right){if(down)[w.view rightMouseDown:event];else if(up)[w.view rightMouseUp:event];else [w.view rightMouseDragged:event];}
            else if(middle){if(down)[w.view otherMouseDown:event];else if(up)[w.view otherMouseUp:event];else [w.view otherMouseDragged:event];}
            else {if(down)[w.view mouseDown:event];else if(up)[w.view mouseUp:event];else [w.view mouseDragged:event];}
        }else if([operation isEqualToString:@"mouse"] && [step[@"phase"] isEqualToString:@"move"]){
            const auto local=NSMakePoint([step[@"x"] doubleValue],[step[@"y"] doubleValue]);automation_cursor_at(w,local);
            const auto x=coordinate_word(local.x),y=coordinate_word(local.y);
            events_.push_back({w.target.guestHandle,0x200,static_cast<std::uint64_t>(x)|(static_cast<std::uint64_t>(y)<<16),0x10012});
        }else if([operation isEqualToString:@"scroll"] && [w.view isKindOfClass:[X64ArmGuestView class]]){
            // 與自動化按鍵相同標記為注入：沒有實體滑鼠狀態，Raw Input（WM_INPUT 滾輪）由服務層依此產生。
            const auto first_event=events_.size();
            struct MarkInjected {std::vector<GuiMessage>& events;std::size_t first;~MarkInjected(){for(auto i=first;i<events.size();++i)if(events[i].message==0x20a || events[i].message==0x20e)events[i].injected=true;}} mark{events_,first_event};
            automation_cursor_at(w,NSMakePoint([step[@"x"] doubleValue],[step[@"y"] doubleValue]));
            [(X64ArmGuestView*)w.view guestScrollAt:NSMakePoint([step[@"x"] doubleValue],[step[@"y"] doubleValue]) vertical:[step[@"delta_y"] doubleValue] horizontal:[step[@"delta_x"] doubleValue] precise:[step[@"precise"] boolValue] modifiers:automation_modifiers(step)];
        }else if([operation isEqualToString:@"clipboard"]){if(!set_clipboard_text(gueststring(step[@"value"]?:@"")))throw std::runtime_error("GUI 測試無法設定剪貼簿");
        }else if([operation isEqualToString:@"clipboard_assert"]){const auto text=clipboard_text();if(!text)return;
            if(step[@"contains"] && ![nsstring(*text) containsString:step[@"contains"]])return;
            if(step[@"value"] && ![nsstring(*text) isEqualToString:step[@"value"]])return;
            if(!step[@"contains"] && !step[@"value"])throw std::runtime_error("GUI 剪貼簿測試缺少預期文字");
        }else if([operation isEqualToString:@"clipboard_snapshot"]){const char* name=std::getenv("X64ARM_GUI_PASTEBOARD");if(!name || !*name)throw std::runtime_error("剪貼簿快照只允許使用獨立測試 pasteboard");const auto text=clipboard_text();if(!text)return;NSString* path=step[@"path"];if(![path isKindOfClass:[NSString class]] || ![nsstring(*text) writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil])throw std::runtime_error("GUI 剪貼簿快照寫入失敗");
        }else if([operation isEqualToString:@"click"] && [w.control isKindOfClass:[NSButton class]])[(NSButton*)w.control performClick:nil];
        else if([operation isEqualToString:@"select"] && gui_control_class(w.model)==u"systabcontrol32" && w.model.tab){
            // 分頁控制項：依索引或標籤文字，在該分頁矩形中心送出真實的滑鼠按下／放開。
            const auto& t=*w.model.tab;std::size_t index=t.items.size();
            if(step[@"index"])index=[step[@"index"] unsignedLongLongValue];else for(std::size_t i=0;i<t.items.size();++i)if([nsstring(t.items[i].text) isEqualToString:step[@"value"]]){index=i;break;}
            if(index>=t.items.size() || index>=t.rectangles.size())return;
            const auto& r=t.rectangles[index];const auto point=[w.view convertPoint:NSMakePoint((r[0]+r[2])/2.0,(r[1]+r[3])/2.0) toView:nil];
            for(const auto type:{NSEventTypeLeftMouseDown,NSEventTypeLeftMouseUp}){NSEvent* event=[NSEvent mouseEventWithType:type location:point modifierFlags:0 timestamp:0 windowNumber:w.view.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];if(type==NSEventTypeLeftMouseDown)[w.view mouseDown:event];else [w.view mouseUp:event];}
        }
        else if([operation isEqualToString:@"select"]){
            std::size_t index=0;if(step[@"index"])index=[step[@"index"] unsignedLongLongValue];else if(!step[@"indices"]){for(;index<w.model.items.size();++index)if([nsstring(w.model.items[index].text) isEqualToString:step[@"value"]])break;}if(index>=w.model.items.size() && !step[@"indices"])return;
            if([w.control isKindOfClass:[NSOutlineView class]]){auto* tree=(NSOutlineView*)w.control;[tree expandItem:nil expandChildren:YES];const auto row=[tree rowForItem:@(w.model.items[index].handle)];if(row<0)return;[tree selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];}
            else if([w.control isKindOfClass:[NSTableView class]]){auto* table=(NSTableView*)w.control;NSMutableIndexSet* rows=[NSMutableIndexSet indexSet];if(step[@"indices"]){if(![step[@"indices"] isKindOfClass:[NSArray class]])throw std::runtime_error("列表多選測試必須指定索引陣列");for(NSNumber* entry in step[@"indices"]){const auto row=entry.unsignedLongLongValue;if(row>=w.model.items.size())throw std::runtime_error("列表測試索引超出範圍");[rows addIndex:static_cast<NSUInteger>(row)];}if(rows.count>1 && !table.allowsMultipleSelection)throw std::runtime_error("單選列表不能執行多選測試");}else [rows addIndex:index];[table selectRowIndexes:rows byExtendingSelection:NO];if(gui_control_class(w.model)==u"syslistview32")[table sendAction:table.action to:table.target];}
            else if([w.control isKindOfClass:[NSComboBox class]]){[(NSComboBox*)w.control selectItemAtIndex:static_cast<NSInteger>(index)];[w.target comboBoxSelectionDidChange:[NSNotification notificationWithName:NSComboBoxSelectionDidChangeNotification object:w.control]];}
            else return;
        }else if([operation isEqualToString:@"double_click"] && gui_control_class(w.model)==u"syslistview32" && [w.control isKindOfClass:[NSTableView class]]){
            auto* table=(NSTableView*)w.control;
            if(step[@"index"]){const auto row=[step[@"index"] unsignedLongLongValue];if(row>=w.model.items.size())return;[table selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];}
            [table sendAction:table.doubleAction to:table.target];
        }else if([operation isEqualToString:@"column_click"] && gui_control_class(w.model)==u"syslistview32" && [w.control isKindOfClass:[NSTableView class]]){
            auto* table=(NSTableView*)w.control;const auto column=[step[@"index"] unsignedLongLongValue];if(column>=table.tableColumns.count)return;
            const auto header=windows_.find(w.model.list_header);if(header==windows_.end() || !header->second.model.header || ![header->second.view isKindOfClass:X64ArmHeaderView.class])return;
            const auto logical=static_cast<std::size_t>([table.tableColumns[static_cast<NSUInteger>(column)].identifier integerValue]);if(logical>=header->second.model.header->rectangles.size())return;const auto& rect=header->second.model.header->rectangles[logical];
            const auto point=[header->second.view convertPoint:NSMakePoint((static_cast<CGFloat>(rect[0])+rect[2])/2,(static_cast<CGFloat>(rect[1])+rect[3])/2) toView:nil];
            for(const auto type:{NSEventTypeLeftMouseDown,NSEventTypeLeftMouseUp}){NSEvent* event=[NSEvent mouseEventWithType:type location:point modifierFlags:0 timestamp:0 windowNumber:w.view.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];if(type==NSEventTypeLeftMouseDown)[header->second.view mouseDown:event];else [header->second.view mouseUp:event];}
        }else if([operation isEqualToString:@"toolbar_click"] && gui_control_class(w.model)==u"toolbarwindow32"){
            NSButton* button=nil;for(NSView* view in w.view.subviews)if([view isKindOfClass:[NSButton class]]){auto* candidate=(NSButton*)view;const bool matches=step[@"index"]?[candidate.cell.representedObject unsignedLongLongValue]==[step[@"index"] unsignedLongLongValue]:candidate.tag==[step[@"command"] longLongValue];if(matches){button=candidate;break;}}if(!button || !button.enabled)return;[button performClick:nil];
        }else if([operation isEqualToString:@"menu_bar"] && w.menu_bar){
            NSButton* chosen_button=nil;for(NSView* view in w.menu_bar.subviews)if([view isKindOfClass:[NSButton class]]){NSButton* button=(NSButton*)view;if(step[@"index"]?button.tag==[step[@"index"] integerValue]:[button.title isEqualToString:step[@"value"]]){chosen_button=button;break;}}
            if(!chosen_button || !chosen_button.enabled)return;
            if(step[@"phase"]){
                NSString* phase=step[@"phase"];if(![phase isEqualToString:@"tap"] && ![phase isEqualToString:@"down"] && ![phase isEqualToString:@"up"])throw std::runtime_error("主選單測試滑鼠階段無效");
                const auto point=[chosen_button convertPoint:NSMakePoint(NSMidX(chosen_button.bounds),NSMidY(chosen_button.bounds)) toView:nil];
                NSView* hit=native_window_hit(chosen_button.window,point);if(hit!=chosen_button && ![hit isDescendantOf:chosen_button])throw std::runtime_error("主選單顯示位置無法命中原生按鈕");
                const auto event=[&](NSEventType type){return [NSEvent mouseEventWithType:type location:point modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:chosen_button.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];};
                if(![phase isEqualToString:@"up"])[hit mouseDown:event(NSEventTypeLeftMouseDown)];
                if(![phase isEqualToString:@"down"])[hit mouseUp:event(NSEventTypeLeftMouseUp)];
            }else [chosen_button performClick:nil];
        }else if([operation isEqualToString:@"main_menu"] && w.main_menu){
            const auto command=[step[@"command"] unsignedLongLongValue];
            std::function<bool(NSMenu*)> invoke=[&](NSMenu* menu){for(NSMenuItem* item in menu.itemArray){if(!item.enabled)continue;if(item.submenu){if(invoke(item.submenu))return true;}else if(!item.separatorItem && item.representedObject && [item.representedObject unsignedLongLongValue]==command){[menu performActionForItemAtIndex:[menu indexOfItem:item]];return true;}}return false;};
            if(!invoke(w.main_menu))return;
        }else if([operation isEqualToString:@"snapshot_native"]){
            // 擷取服務可能延遲或視窗已消失；診斷失敗不能終止客體或阻塞其 GUI 執行緒。
            if(!automation_snapshot_){
                [NSApp updateWindows];automation_snapshot_=std::make_shared<NativeSnapshotBatch>();automation_snapshot_started_=[NSDate date];
                const auto window=w.view.window;
                capture_native_windows_async(automation_snapshot_,{{static_cast<CGWindowID>(window.windowNumber),static_cast<std::size_t>(std::max<CGFloat>(1,std::ceil(window.frame.size.width))),static_cast<std::size_t>(std::max<CGFloat>(1,std::ceil(window.frame.size.height))),[step[@"path"] UTF8String]}});
                return;
            }
            if(automation_snapshot_->pending && -automation_snapshot_started_.timeIntervalSinceNow<10)return;
            const bool captured=automation_snapshot_->succeeded.load()==1;automation_snapshot_->active=false;automation_snapshot_.reset();
            if(!captured)[automation_errors_ addObject:@{@"action":@(action_),@"operation":@"snapshot_native",@"path":step[@"path"],@"error":@"原生視窗擷取失敗或逾時"}];
            diagnostics();if(state_path_){NSData* state=[NSData dataWithContentsOfFile:state_path_];[state writeToFile:[step[@"path"] stringByAppendingString:@".json"] atomically:YES];}
        }
        else if([operation isEqualToString:@"snapshot"]){snapshot(w.view.window.contentView,[step[@"path"] UTF8String]);diagnostics();if(state_path_){NSData* state=[NSData dataWithContentsOfFile:state_path_];[state writeToFile:[step[@"path"] stringByAppendingString:@".json"] atomically:YES];}}
        else if([operation isEqualToString:@"close"]) [w.view.window performClose:nil];
        else return;
        ++action_;last_action_=[NSDate date];diagnostics();
    }
public:
    void activate(std::uint64_t handle) override {
        const auto found=windows_.find(handle);if(found==windows_.end() || !found->second.native || !found->second.native.visible)return;
        auto& w=found->second;w.delegate.updating=YES;
        if(background_display_enabled() && !interactive_activation_){[w.native orderFront:nil];w.delegate.updating=NO;dirty_=true;return;}
        // AppKit 啟用為非同步；只在真的進入 active 後完成原請求的 key window。
        // 不能在 inactive 時 makeKey 後，任由 AppKit 恢復先前另一個 key window。
        if(!application_launched_){[NSApp finishLaunching];application_launched_=true;}
        pending_activation_=NSApp.active?0:handle;
        if(!NSApp.active)[NSApp activateIgnoringOtherApps:YES];
        [w.native makeKeyAndOrderFront:nil];w.delegate.updating=NO;apply_z_order(handle);dirty_=true;
    }
    void focus(std::uint64_t handle) override {
        // 客體 SetFocus 比先前尚未交付的原生焦點通知更新；保留文字與其他輸入。
        // 否則下一次事件泵會把焦點改回舊視窗，導致日期／篩選編輯器提早提交。
        std::erase_if(events_,[](const auto& event){return event.message==0x10013;});
        if(!handle){
            // 非作用中背景視窗沒有 NSApp.keyWindow，仍必須解除它的 field editor。
            std::set<NSWindow*> hosts;for(auto& [id,window]:windows_){(void)id;if(window.view.window)hosts.insert(window.view.window);}
            for(NSWindow* host:hosts)[host makeFirstResponder:nil];return;
        }
        const auto found=windows_.find(handle);if(found==windows_.end())return;
        auto& window=found->second;const BOOL updating=window.target.updating;window.target.updating=YES;
        NSResponder* responder=window.view.window.firstResponder;
        NSTextView* editor=window.editor?:([window.control.currentEditor isKindOfClass:NSTextView.class]?(NSTextView*)window.control.currentEditor:nil);
        // 客體 SetFocus 不產生另一筆原生選取事件；保留 EM_SETSEL 指定的範圍。
        NSView* target=window.editor?:window.control?:window.view;
        X64ArmGuestButton* button=[target isKindOfClass:X64ArmGuestButton.class]?(X64ArmGuestButton*)target:nil;const BOOL buttonUpdating=button.guestUpdating;button.guestUpdating=YES;
        X64ArmGuestView* view=[target isKindOfClass:X64ArmGuestView.class]?(X64ArmGuestView*)target:nil;const BOOL viewUpdating=view.guestUpdating;view.guestUpdating=YES;
        if(responder!=target && (!editor || responder!=editor))[window.view.window makeFirstResponder:target];
        editor=window.editor?:([window.control.currentEditor isKindOfClass:NSTextView.class]?(NSTextView*)window.control.currentEditor:nil);
        if(gui_control_class(window.model)==u"edit" && editor && !editor.hasMarkedText){
            const auto length=editor.string.length,begin=std::min<NSUInteger>(std::min(window.model.edit_start,window.model.edit_end),length),end=std::min<NSUInteger>(std::max(window.model.edit_start,window.model.edit_end),length);
            editor.selectedRange=NSMakeRange(begin,end-begin);
        }
        view.guestUpdating=viewUpdating;button.guestUpdating=buttonUpdating;window.target.updating=updating;dirty_=true;
    }
    // begin_activity=false：延遲建立時由外層代理在程序啟動就持有 App Nap 防護，這裡不重複開始。
    explicit CocoaGui(std::filesystem::path snapshot,bool begin_activity=true) : snapshot_(std::move(snapshot)) {
        if (![NSThread isMainThread]) throw std::runtime_error("AppKit GUI 必須在主執行緒執行");
        @autoreleasepool {
            prepare_native_application();
            if(background_display_enabled()){
                // sharedApplication 已初始化 AppKit；此後由 poll 手動派送事件。
                // finishLaunching 本身會排程啟用應用程式，先 deactivate 也不能取消它。
                startup_profile::Scope measurement("gui_init","background_display");(void)background_display();
            }else {startup_profile::Scope measurement("gui_init","finish_launching");[NSApp finishLaunching];application_launched_=true;}
            app_deactivate_observer_=[NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidResignActiveNotification object:NSApp queue:nil usingBlock:^(NSNotification* note){(void)note;interactive_activation_=false;pending_activation_=0;dirty_=true;}];
            {startup_profile::Scope measurement("gui_init","desktop_notifications");desktop_notifications_=make_native_desktop_notifications();}
            const char* automation_path=std::getenv("X64ARM_GUI_AUTOMATION");
            const char* automation_fd=std::getenv("X64ARM_GUI_AUTOMATION_FD");
            if(automation_path || automation_fd){
                NSData* data=automation_data(automation_path,automation_fd);NSError* error=nil;
                id value=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:&error]:nil;
                if(![value isKindOfClass:[NSArray class]])throw std::runtime_error("GUI 自動操作檔必須為 JSON 陣列");automation_=value;last_action_=[NSDate date];
                if(automation_path && std::getenv("X64ARM_GUI_AUTOMATION_LIVE"))automation_live_path_=[NSString stringWithUTF8String:automation_path];
                // 自動化直接呼叫原生 view／control；隔離本測試程序收到的真實輸入，避免混入測試資料。
                const NSEventMask input=NSEventMaskKeyDown|NSEventMaskKeyUp|NSEventMaskFlagsChanged|NSEventMaskLeftMouseDown|NSEventMaskLeftMouseUp|NSEventMaskRightMouseDown|NSEventMaskRightMouseUp|NSEventMaskOtherMouseDown|NSEventMaskOtherMouseUp|NSEventMaskMouseMoved|NSEventMaskLeftMouseDragged|NSEventMaskRightMouseDragged|NSEventMaskOtherMouseDragged|NSEventMaskScrollWheel;
                const char* manual_input=std::getenv("X64ARM_GUI_AUTOMATION_ALLOW_INPUT");
                if(!manual_input || std::string_view(manual_input)!="1")automation_input_monitor_=[NSEvent addLocalMonitorForEventsMatchingMask:input handler:^NSEvent*(NSEvent* event){
                    if([automation_posted_events_ containsObject:event])return event;
                    return nil;
                }];
            }
            if(const char* path=std::getenv("X64ARM_GUI_STATE"))state_path_=[NSString stringWithUTF8String:path];
            if(const char* path=std::getenv("X64ARM_GUI_NATIVE_SNAPSHOT_DIR");path && *path){native_snapshot_directory_=path;std::filesystem::create_directories(native_snapshot_directory_);}
            // 原生 GUI 的主程序與隱藏輔助程序同屬使用者啟動的互動工作。
            // App Nap 可能停滯協作式客體排程，讓另一程序誤判它沒有回應；活動範圍隨 backend 釋放。
            // 仍允許系統閒置睡眠，且不變更客體的背景／QoS 契約；診斷可用 0 明確停用。
            startup_profile::Scope activity_measurement("gui_init","begin_activity");
            if(const char* value=std::getenv("X64ARM_GUI_ACTIVITY");begin_activity && (!value || std::string_view(value)!="0"))
                activity_=[NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"執行使用者啟動的 Windows 互動程式"];
        }
    }
    std::optional<std::array<std::uint16_t,768>> gamma_ramp() override {
        const auto source=display_gamma(CGMainDisplayID());if(source.empty())return {};
        const auto count=source.size()/3;std::array<std::uint16_t,768> ramp{};
        for(unsigned c=0;c<3;++c)for(unsigned i=0;i<256;++i){
            const auto position=static_cast<double>(i)*static_cast<double>(count-1)/255;const auto low=static_cast<std::size_t>(position),high=std::min(low+1,count-1);const auto fraction=position-static_cast<double>(low);
            const double value=source[c*count+low]*(1-fraction)+source[c*count+high]*fraction;if(!std::isfinite(value))return {};
            ramp[c*256+i]=static_cast<std::uint16_t>(std::lround(std::clamp(value,0.0,1.0)*65535));
        }
        return ramp;
    }
    bool set_gamma_ramp(std::span<const std::uint16_t,768> ramp) override {
        const auto display=CGMainDisplayID();auto before=display_gamma(display);if(before.empty())return false;
        std::array<float,768> values{};for(std::size_t i=0;i<values.size();++i)values[i]=static_cast<float>(ramp[i])/65535.0F;
        original_gamma_.try_emplace(display,before);
        if(CGSetDisplayTransferByTable(display,256,values.data(),values.data()+256,values.data()+512)!=kCGErrorSuccess)return false;
        try {
            auto actual=display_gamma(display);if(!actual.empty()){written_gamma_[display]=std::move(actual);return true;}
        }catch(const std::bad_alloc&){} // 修改後記錄失敗時立即還原，不能留下無法回收的宿主曲線。
        const auto count=static_cast<uint32_t>(before.size()/3);(void)CGSetDisplayTransferByTable(display,count,before.data(),before.data()+count,before.data()+2*count);return false;
    }
    ~CocoaGui() override { native_snapshot_batch_->active=false;if(automation_snapshot_)automation_snapshot_->active=false;(void)restore_display_mode();restore_gamma(); @autoreleasepool { if(activity_)[NSProcessInfo.processInfo endActivity:activity_];help_.reset();desktop_notifications_.reset();if(cursor_hidden_)[NSCursor unhide]; if(automation_input_monitor_)[NSEvent removeMonitor:automation_input_monitor_];if(app_deactivate_observer_)[NSNotificationCenter.defaultCenter removeObserver:app_deactivate_observer_];for (auto& [handle, window] : windows_) { (void)handle; window.native.commandShortcut=nil;window.native.guestModel=nullptr;window.native.delegate = nil; [window.native close]; } } }
    void create(std::uint64_t handle, const GuiWindow& model) override {
        @autoreleasepool {
            auto& window=windows_[handle];window.model=model;std::erase(z_order_,handle);z_order_.insert(z_order_.begin(),handle);
            if(model.combo_owner){const auto owner=windows_.find(model.combo_owner);if(owner==windows_.end())throw std::runtime_error("ComboBoxEx 內部控制項缺少外層視窗");window.alias=true;window.view=owner->second.view;window.control=owner->second.control;window.target=owner->second.target;return;}
            const bool top_level=!model.parent || model.parent==gui_message_parent;
            const auto parent=windows_.find(model.parent);
            if(!top_level && parent==windows_.end())throw std::runtime_error("原生控制項缺少父視窗");
            if(top_level){
                window.native=[[X64ArmGuestWindow alloc] initWithContentRect:NSMakeRect(120,120,model.width,model.height) styleMask:native_window_style(model) backing:NSBackingStoreBuffered defer:NO];window.native.releasedWhenClosed=NO;
                window.native.guestModel=&window.model;
                window.native.commandShortcut=^BOOL(NSEvent* event){return command_shortcut(handle,event);};
            }
            // 頂層與子視窗共用控制項映射，標準類別不再退化為空白客體畫布。
            const NSRect rect=top_level?NSMakeRect(0,0,model.width,model.height):native_child_rect(model,false);
            auto* target=[[X64ArmControlTarget alloc] init];target.model=&window.model;target.guestHandle=handle;target.events=&events_;window.target=target;
            const auto& cls=gui_control_class(model);
            if(model.console && model.console->window){
                window.view=(__bridge NSView*)model.console->window->native_view();
                if(!window.native || !window.view || !model.console->window->bind_native_window((__bridge void*)window.native))throw std::runtime_error("無法連接 Console 原生視窗");
            }else if(!model.builtin || model.mdi || model.dialog || cls==u"toolbarwindow32" || cls==u"msctls_statusbar32" || cls==u"rebarwindow32" || cls==u"syslink" || cls==u"systabcontrol32" || cls==u"tooltips_class32" || cls==u"sysdatetimepick32" || cls==u"sysmonthcal32"){
                auto* view=[[X64ArmGuestView alloc] initWithFrame:rect];view.model=&window.model;view.guestHandle=handle;view.events=&events_;view.guestCursor=cursor_;window.view=view;
            }else if(cls==u"msctls_updown32"){
                auto* spin=[[NSStepper alloc] initWithFrame:rect];spin.minValue=-1;spin.maxValue=1;spin.increment=1;spin.doubleValue=0;spin.valueWraps=NO;spin.target=target;spin.action=@selector(spinChanged:);window.control=spin;window.view=spin;
            }else if(cls==u"msctls_trackbar32"){
                auto* slider=[[NSSlider alloc] initWithFrame:rect];slider.sliderType=NSSliderTypeLinear;slider.vertical=(model.style&2U)!=0;slider.continuous=YES;
                slider.minValue=model.track_min;slider.maxValue=model.track_max;slider.doubleValue=(model.style&2U)?static_cast<double>(model.track_min)+model.track_max-model.track_position:model.track_position;slider.target=target;slider.action=@selector(sliderChanged:);window.control=slider;window.view=slider;
            }else if(cls==u"msctls_progress32"){
                auto* progress=[[NSProgressIndicator alloc] initWithFrame:rect];progress.style=NSProgressIndicatorStyleBar;progress.indeterminate=NO;progress.minValue=model.progress_min;progress.maxValue=model.progress_max;progress.doubleValue=model.progress_position;window.view=progress;
            }else if(cls==u"button" && (model.style&15)!=7){auto* button=[[X64ArmGuestButton alloc] initWithFrame:rect];button.model=&window.model;button.guestHandle=handle;button.events=&events_;const auto kind=model.style&15;
                button.buttonType=(model.style&0x1000U)?NSButtonTypePushOnPushOff:(kind==2 || kind==3 || kind==5 || kind==6)?NSButtonTypeSwitch:(kind==4 || kind==9)?NSButtonTypeRadio:NSButtonTypeMomentaryPushIn;
                if(kind==4 || kind==9){button.buttonType=NSButtonTypeMomentaryChange;button.bordered=NO;button.imagePosition=NSImageLeft;}
                if(kind<=1 || (model.style&0x1000U))button.bezelStyle=NSBezelStyleRounded;button.keyEquivalent=@"";button.font=guest_font(model.control_font);button.title=label(model.title);button.target=target;button.action=@selector(clicked:);window.control=button;window.view=button;
            }else if(cls==u"static" && ((model.style&31)==13 || (model.style&31)==3)){auto* view=[[X64ArmGuestView alloc] initWithFrame:rect];view.model=&window.model;view.guestHandle=handle;view.events=&events_;view.guestCursor=cursor_;window.view=view;
            }else if(cls==u"sysheader32"){
                auto* view=[[X64ArmHeaderView alloc] initWithFrame:rect];view.model=&window.model;view.guestHandle=handle;view.events=&events_;window.view=view;
            }else if(cls==u"edit" && (model.style&4)){
                auto* scroll=[[NSScrollView alloc] initWithFrame:rect];scroll.autohidesScrollers=YES;
                auto* editor=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,model.width,model.height)];editor.editable=(model.style&0x800)==0;editor.richText=NO;editor.font=guest_font(model.control_font);editor.delegate=target;
                editor.verticallyResizable=YES;editor.maxSize=NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);scroll.documentView=editor;window.editor=editor;window.view=scroll;
            }else if(cls==u"edit"){NSTextField* field=(model.style&0x20)?[[NSSecureTextField alloc] initWithFrame:rect]:[[NSTextField alloc] initWithFrame:rect];field.delegate=target;field.font=guest_font(model.control_font);field.editable=(model.style&0x800)==0;window.control=field;window.view=field;
            }else if(cls==u"combobox" || cls==u"comboboxex32"){auto* combo=[[NSComboBox alloc] initWithFrame:rect];combo.delegate=target;combo.usesDataSource=NO;combo.editable=(model.style&3U)!=3U;combo.font=[NSFont systemFontOfSize:12];window.control=combo;window.view=combo;
            }else if(cls==u"listbox" || cls==u"systreeview32" || cls==u"syslistview32"){
                create_table_view(window,rect,target);
            }else if(cls==u"button" || (cls==u"static" && (model.style&31)==16)){
                auto* box=[[X64ArmDecorationBox alloc] initWithFrame:rect];box.boxType=cls==u"button"?NSBoxCustom:NSBoxSeparator;box.title=label(model.title);box.titleFont=guest_font(model.control_font);
                if(cls==u"button"){box.fillColor=NSColor.clearColor;box.borderColor=NSColor.separatorColor;box.cornerRadius=3;}
                window.view=box;
            }else {auto* text=[NSTextField labelWithString:label(model.title)];text.frame=rect;text.font=guest_font(model.control_font);text.lineBreakMode=NSLineBreakByWordWrapping;text.maximumNumberOfLines=0;window.control=text;window.view=text;}
            if(top_level){
                window.delegate=[[X64ArmWindowDelegate alloc] init];window.delegate.guestHandle=handle;window.delegate.events=&events_;window.delegate.model=&window.model;
                window.native.collectionBehavior=NSWindowCollectionBehaviorFullScreenNone;
                window.native.delegate=window.delegate;window.native.contentView=window.view;window.native.title=nsstring(model.title);sync_native_frame(window);
            }else {
                if(model.mdi_child){
                    window.mdi_frame=[[X64ArmMdiFrameView alloc] initWithFrame:native_child_rect(model,false)];window.mdi_frame.model=&window.model;window.mdi_frame.events=&events_;window.mdi_frame.guestHandle=handle;
                    const auto inset=gui_frame_insets(model);window.view.frame=NSMakeRect(inset.left,inset.top,model.width,model.height);[window.mdi_frame addSubview:window.view];[parent->second.view addSubview:window.mdi_frame];
                }else {
                    if([window.view isKindOfClass:[X64ArmGuestView class]])window.view.frame=native_child_rect(model,true);
                    if(model.header_list && [parent->second.control isKindOfClass:NSTableView.class])((NSTableView*)parent->second.control).headerView=(NSTableHeaderView*)window.view;
                    else if([window.view isKindOfClass:[NSBox class]])[parent->second.view addSubview:window.view positioned:NSWindowBelow relativeTo:nil];else [parent->second.view addSubview:window.view];
                }
            }
            present(handle,model);
        }
    }
    void reorder(std::uint64_t handle,std::uint64_t insert_after) override {
        @autoreleasepool {
            const auto found=windows_.find(handle);if(found==windows_.end() || insert_after==handle)return;
            const auto anchor=windows_.find(insert_after);
            if(insert_after>1 && (anchor==windows_.end() || anchor->second.model.parent!=found->second.model.parent))return;
            std::erase(z_order_,handle);
            if(insert_after==1)z_order_.push_back(handle);
            else if(!insert_after)z_order_.insert(z_order_.begin(),handle);
            else {const auto position=std::find(z_order_.begin(),z_order_.end(),insert_after);z_order_.insert(position==z_order_.end()?z_order_.end():position+1,handle);}
            apply_z_order(handle);
        }
    }
    bool set_region(std::uint64_t handle,const std::optional<GuiRegion>& region) override {
        @autoreleasepool {const auto found=windows_.find(handle);if(found==windows_.end() || found->second.alias)return false;
            found->second.model.window_region=region;apply_window_region(found->second);dirty_=true;return true;
        }
    }
    bool set_opacity(std::uint64_t handle,std::uint8_t alpha) override {
        @autoreleasepool {const auto found=windows_.find(handle);if(found==windows_.end() || found->second.alias)return false;
            auto& window=found->second;const auto value=static_cast<CGFloat>(alpha)/255.0;
            if(window.native)window.native.alphaValue=value;else (window.mdi_frame?:window.view).alphaValue=value;
            dirty_=true;return true;
        }
    }
    std::optional<GuiThemeMetrics> theme_metrics(std::u16string_view cls,std::int32_t part,std::int32_t state,std::uint32_t dpi,std::array<std::int32_t,2> size) override {return native_theme_metrics(cls,part,state,dpi,size);}
    std::optional<GuiBitmap> theme_background(std::u16string_view cls,std::int32_t part,std::int32_t state,std::uint32_t dpi,std::array<std::int32_t,2> size) override {return native_theme_background(cls,part,state,dpi,size);}
    void graphics_text(std::uint64_t handle,const GuiDraw& draw) override {
        if(!state_path_ && !automation_)return;
        const auto found=windows_.find(handle);if(found==windows_.end())return;
        auto& history=found->second.model.drawing;
        if(history.size()>=256)history.erase(history.begin(),history.begin()+128);
        history.push_back(draw);dirty_=true;
    }
    std::optional<GuiBitmap> capture_client(std::uint64_t handle) override {
        @autoreleasepool {const auto found=windows_.find(handle);if(found==windows_.end())return {};
            if(found->second.model.display_affinity)return {};
            NSBitmapImageRep* source=capture_view(found->second.view);if(!source || source.pixelsWide<=0 || source.pixelsHigh<=0 || source.pixelsWide>16384 || source.pixelsHigh>16384 || static_cast<std::uint64_t>(source.pixelsWide)*static_cast<std::uint64_t>(source.pixelsHigh)>4194304)return {};
            GuiBitmap result{found->second.model.width,found->second.model.height,{}};if(!result.width || !result.height || static_cast<std::uint64_t>(result.width)*result.height>4194304)return {};result.pixels.resize(static_cast<std::size_t>(result.width)*result.height);
            for(unsigned y=0;y<result.height;++y){for(unsigned x=0;x<result.width;++x){const auto sx=static_cast<NSInteger>(static_cast<std::uint64_t>(x)*static_cast<std::uint64_t>(source.pixelsWide)/result.width),sy=static_cast<NSInteger>(static_cast<std::uint64_t>(y)*static_cast<std::uint64_t>(source.pixelsHigh)/result.height);NSColor* c=[[source colorAtX:sx y:sy] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];if(!c)return {};
                const auto channel=[alpha=c.alphaComponent](CGFloat v){return static_cast<std::uint32_t>(std::clamp(std::lround((v*alpha+1-alpha)*255),0L,255L));};result.pixels[static_cast<std::size_t>(y)*result.width+x]=0xff000000U|channel(c.redComponent)|(channel(c.greenComponent)<<8)|(channel(c.blueComponent)<<16);
            }}
            return result;
        }
    }
    void graphics_surface(std::uint64_t handle,std::shared_ptr<RasterSurface> surface) override {
        const bool attached=static_cast<bool>(surface);if(attached)graphics_surfaces_[handle]=std::move(surface);else graphics_surfaces_.erase(handle);
        const auto window=windows_.find(handle);
        if(window!=windows_.end() && [window->second.view isKindOfClass:X64ArmGuestView.class]){
            ((X64ArmGuestView*)window->second.view).rasterManaged=attached;window->second.view.canDrawSubviewsIntoLayer=NO;[window->second.view setNeedsDisplay:YES];
        }
        if(window!=windows_.end() && [window->second.view isKindOfClass:X64ArmHeaderView.class]){((X64ArmHeaderView*)window->second.view).rasterManaged=attached;[window->second.view setNeedsDisplay:YES];}
        dirty_=true;
    }
    void* graphics_view(std::uint64_t handle) override {if(offline_render_enabled())return nullptr;const auto f=windows_.find(handle);return f==windows_.end()?nullptr:(__bridge void*)f->second.view;}
    std::optional<GuiDisplayMode> current_display_mode() override {
        const auto mode=CGDisplayCopyDisplayMode(CGMainDisplayID());if(!mode)return {};auto result=describe_display_mode(mode);CGDisplayModeRelease(mode);
        if(const auto size=virtual_display_size()){result.width=size->first;result.height=size->second;}
        return result;
    }
    std::vector<GuiDisplayMode> display_modes() override {
        std::vector<GuiDisplayMode> result;const auto modes=CGDisplayCopyAllDisplayModes(CGMainDisplayID(),nullptr);if(!modes)return result;
        for(CFIndex i=0;i<CFArrayGetCount(modes);++i){const auto mode=static_cast<CGDisplayModeRef>(const_cast<void*>(CFArrayGetValueAtIndex(modes,i)));if(!CGDisplayModeIsUsableForDesktopGUI(mode))continue;
            auto value=describe_display_mode(mode);if(!value.width || !value.height || value.width>16384 || value.height>16384)continue;
            if(std::none_of(result.begin(),result.end(),[&](const auto& a){return a.width==value.width && a.height==value.height && a.refresh_numerator==value.refresh_numerator && a.refresh_denominator==value.refresh_denominator;}))result.push_back(value);
        }
        CFRelease(modes);
        // 虛擬解析度：只公開不超過它的模式，並保證該解析度本身存在（沿用目前更新率與原生模式識別）。
        if(const auto size=virtual_display_size()){
            std::erase_if(result,[&](const auto& m){return m.width>size->first || m.height>size->second;});
            if(const auto current=current_display_mode();current && std::none_of(result.begin(),result.end(),[&](const auto& m){return m.width==size->first && m.height==size->second;}))result.push_back(*current);
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return std::tie(a.width,a.height,a.refresh_numerator)<std::tie(b.width,b.height,b.refresh_numerator);});return result;
    }
    bool display_fullscreen(std::uint64_t handle,bool enabled,const std::optional<GuiDisplayMode>& desired={}) override {
        const auto found=windows_.find(handle);if(found==windows_.end() || !found->second.native)return !enabled && (!fullscreen_window_ || fullscreen_window_==handle) && restore_display_mode();
        if(offline_render_enabled()){found->second.fullscreen=enabled;return true;}
        auto& window=found->second;if(fullscreen_window_ && fullscreen_window_!=handle)return false;
        if(!enabled){if(!restore_display_mode())return false;window.fullscreen=false;apply_z_order(handle);return true;}
        if(!window.native.visible || window.native.miniaturized)return false;
        const auto display=CGMainDisplayID();const auto current=CGDisplayCopyDisplayMode(display);if(!current)return false;
        CGDisplayModeRef target=nullptr;
        if(desired && desired->native_id!=display_mode_id(current)){
            const auto modes=CGDisplayCopyAllDisplayModes(display,nullptr);if(modes){for(CFIndex i=0;i<CFArrayGetCount(modes);++i){const auto mode=static_cast<CGDisplayModeRef>(const_cast<void*>(CFArrayGetValueAtIndex(modes,i)));if(display_mode_id(mode)==desired->native_id){target=CGDisplayModeRetain(mode);break;}}CFRelease(modes);}
            if(!target || !configure_display(display,target)){if(target)CGDisplayModeRelease(target);CGDisplayModeRelease(current);return false;}
        }
        if(!original_display_mode_)original_display_mode_=CGDisplayModeRetain(current);
        written_display_mode_=display_mode_id(target?target:current);if(target)CGDisplayModeRelease(target);CGDisplayModeRelease(current);
        fullscreen_display_=display;fullscreen_window_=handle;window.fullscreen=true;apply_z_order(handle);return true;
    }
    std::array<std::int32_t,4> work_area() override {if(const auto size=virtual_display_size())return {0,0,static_cast<std::int32_t>(size->first),static_cast<std::int32_t>(size->second)};const auto screen=background_display()?:NSScreen.mainScreen;const auto frame=screen.visibleFrame;return {static_cast<std::int32_t>(frame.origin.x-(background_display()?screen.frame.origin.x:0)),static_cast<std::int32_t>(NSMaxY(screen.frame)-NSMaxY(frame)),static_cast<std::int32_t>(frame.size.width),static_cast<std::int32_t>(frame.size.height)};}
    std::array<std::int32_t,4> screen_area() override {if(const auto size=virtual_display_size())return {0,0,static_cast<std::int32_t>(size->first),static_cast<std::int32_t>(size->second)};const auto r=(background_display()?:NSScreen.mainScreen).frame;return {0,0,static_cast<std::int32_t>(r.size.width),static_cast<std::int32_t>(r.size.height)};}
    std::optional<shell::Dock> shell_dock() override {return shell::dock();}
    std::uint32_t shell_reveal(std::span<const std::shared_ptr<HostFile>> files,std::uint32_t flags) override {return shell::reveal(files,flags);}
    std::vector<shell::Application> shell_applications(const std::shared_ptr<HostFile>& file,std::string_view uri) override {return shell::applications(file,uri);}
    std::uint32_t shell_open(const std::shared_ptr<HostFile>& file,std::string_view uri,const shell::Application& app) override {return shell::open(file,uri,app);}
    std::optional<shell::Application> shell_default_application(const std::shared_ptr<HostFile>& file,std::string_view uri) override {return shell::default_application(file,uri);}
    void show(std::uint64_t handle,bool visible) override {
        @autoreleasepool {const auto f=windows_.find(handle);if(f==windows_.end())return;auto& w=f->second;w.model.visible=visible;
            if(w.alias)return;
            if(w.model.parent==gui_message_parent || offline_render_enabled())visible=false;
            if(w.native){w.delegate.updating=YES;if(visible){
                if(w.model.show_command==2){if(!w.native.miniaturized)[w.native miniaturize:nil];w.delegate.updating=NO;dirty_=true;return;}
                if(w.native.miniaturized)[w.native deminiaturize:nil];
                position_owned_table(w);
                const auto owner=windows_.find(w.model.owner);
                if([w.control isKindOfClass:[NSTableView class]] && owner!=windows_.end() && owner->second.view.window){
                    if(w.native.parentWindow!=owner->second.view.window){[w.native.parentWindow removeChildWindow:w.native];[owner->second.view.window addChildWindow:w.native ordered:NSWindowAbove];}
                    [w.native orderFront:nil];
                }else if(automation_ || (background_display_enabled() && !interactive_activation_) || (w.model.ex_style&0x08000000U))[w.native orderFront:nil];else {[w.native makeKeyAndOrderFront:nil];if(@available(macOS 14.0,*))[NSApp activate];}
                if(!w.model.dialog && !(w.model.ex_style&0x08000000U))[w.native makeFirstResponder:w.control?w.control:w.view];
            }else {[w.native.parentWindow removeChildWindow:w.native];[w.native orderOut:nil];}w.delegate.updating=NO;}
            else {w.view.hidden=!visible;w.mdi_frame.hidden=!visible;}if(visible)apply_z_order(handle);dirty_=true;
        }
    }
    void present(std::uint64_t handle,const GuiWindow& model) override {
        @autoreleasepool {const auto f=windows_.find(handle);if(f==windows_.end())return;auto& w=f->second;const auto old=w.model;w.model=model;if(w.alias)return;w.target.updating=YES;
            if(w.native)w.native.sharingType=model.display_affinity?NSWindowSharingNone:NSWindowSharingReadOnly;
            if(!model.parent && !model.owner && model.application_icon!=dock_icon_ && (!model.application_icon.image.empty() || !model.application_icon.rgba.empty() || model.application_icon.system)){
                NSImage* icon=list_icon(model.application_icon);if(icon){NSApp.applicationIconImage=icon;dock_icon_=model.application_icon;}
            }
            // ES_PASSWORD 變更時更換原生欄位類型，保留焦點、內容及 UTF-16 選取範圍。
            if(gui_control_class(model)==u"edit" && !(model.style&4U) && [w.control isKindOfClass:NSTextField.class] &&
               ((model.style&0x20U)!=0)!=[w.control isKindOfClass:NSSecureTextField.class]){
                auto* previous=(NSTextField*)w.control;auto* editor=previous.currentEditor;
                const bool focused=editor!=nil;const NSRange selection=focused?editor.selectedRange:NSMakeRange(0,0);
                NSWindow* host=previous.window;NSView* parent=previous.superview;NSView* priorKey=previous.previousKeyView;
                NSTextField* field=(model.style&0x20U)?[[NSSecureTextField alloc] initWithFrame:previous.frame]:[[NSTextField alloc] initWithFrame:previous.frame];
                field.delegate=w.target;field.font=previous.font;field.editable=(model.style&0x800U)==0;
                field.alignment=previous.alignment;field.stringValue=nsstring(model.title);field.nextKeyView=previous.nextKeyView;
                if(focused)[host makeFirstResponder:nil];
                [parent addSubview:field positioned:NSWindowAbove relativeTo:previous];
                if(priorKey && priorKey!=previous)priorKey.nextKeyView=field;
                [previous removeFromSuperview];previous.delegate=nil;w.control=field;w.view=field;
                if(focused){[host makeFirstResponder:field];auto* current=field.currentEditor;
                    const auto start=std::min(selection.location,field.stringValue.length);
                    current.selectedRange=NSMakeRange(start,std::min(selection.length,field.stringValue.length-start));}
            }
            // 更換父視窗時保留 NSView、控制項與 Metal layer，只調整原生容器。
            if(old.parent!=model.parent){
                const bool root=!model.parent || model.parent==gui_message_parent;
                if(!root && w.native){
                    [w.native orderOut:nil];w.native.contentView=[[NSView alloc] init];[w.native.parentWindow removeChildWindow:w.native];w.native.commandShortcut=nil;w.native.guestModel=nullptr;w.native.delegate=nil;[w.native close];w.native=nil;
                }
                if(root && !w.native){
                    [w.view removeFromSuperview];w.native=[[X64ArmGuestWindow alloc] initWithContentRect:NSMakeRect(120,120,model.width,model.height) styleMask:native_window_style(model) backing:NSBackingStoreBuffered defer:NO];w.native.releasedWhenClosed=NO;
                    w.native.guestModel=&w.model;
                    w.native.commandShortcut=^BOOL(NSEvent* event){return command_shortcut(handle,event);};
                    if(!w.delegate){w.delegate=[[X64ArmWindowDelegate alloc] init];w.delegate.guestHandle=handle;w.delegate.events=&events_;w.delegate.model=&w.model;}
                    w.native.delegate=w.delegate;w.native.contentView=w.view;w.native.collectionBehavior=NSWindowCollectionBehaviorFullScreenNone;
                }
                if(root)w.view.hidden=NO;
            }
            if(w.control)w.control.font=guest_font(model.control_font);if(w.editor)w.editor.font=guest_font(model.control_font);
            if(w.native){
                // styleMask 也會同步送出 resize 通知；整個客體投影須共用更新範圍，避免舊尺寸回寫 model。
                const BOOL updating=w.delegate.updating;w.delegate.updating=YES;
                w.native.ignoresMouseEvents=!model.enabled;w.native.title=nsstring(model.title);
                const auto mask=native_window_style(model);if(w.native.styleMask!=mask)w.native.styleMask=mask;
                sync_native_frame(w);position_owned_table(w);[w.view setNeedsDisplay:YES];
                w.delegate.updating=updating;
            }
            else {
                NSView* outer=w.mdi_frame?:w.view;
                if(old.parent!=model.parent){const auto parent=windows_.find(model.parent);if(parent!=windows_.end()){[outer removeFromSuperview];[parent->second.view addSubview:outer];}}
                if(w.mdi_frame){
                    w.mdi_frame.frame=native_child_rect(model,false);const auto inset=gui_frame_insets(model);w.view.frame=NSMakeRect(inset.left,inset.top,model.width,model.height);
                    std::int64_t x=model.x,y=model.y;auto parent=model.parent;
                    for(unsigned depth=0;parent && depth<64;++depth){const auto a=windows_.find(parent);if(a==windows_.end())break;const auto frame_inset=gui_frame_insets(a->second.model);x+=static_cast<std::int64_t>(a->second.model.x)+frame_inset.left;y+=static_cast<std::int64_t>(a->second.model.y)+frame_inset.top;parent=a->second.model.parent;}
                    w.mdi_frame.originX=static_cast<CGFloat>(x);w.mdi_frame.originY=static_cast<CGFloat>(y);w.mdi_frame.hidden=!model.visible;[w.mdi_frame setNeedsDisplay:YES];
                }else if(!model.header_list)w.view.frame=native_child_rect(model,[w.view isKindOfClass:[X64ArmGuestView class]] || [w.view isKindOfClass:[X64ArmHeaderView class]]);
                else {auto frame=w.view.frame;frame.size.height=model.height;w.view.frame=frame;const auto parent=windows_.find(model.header_list);if(parent!=windows_.end() && [parent->second.view isKindOfClass:NSScrollView.class])[(NSScrollView*)parent->second.view tile];}
                w.view.hidden=!model.visible;
            }
            apply_window_region(w);
            if(model.console && model.console->window){model.console->title=model.title;w.target.updating=NO;dirty_=true;return;}
            if(w.native){const bool transparent=w.shaped || ((model.ex_style&0x80000U) && !model.layered_pixels.empty());w.native.opaque=!transparent;w.native.backgroundColor=transparent?[NSColor clearColor]:[NSColor windowBackgroundColor];}
            if((model.ex_style&0x20U) && [w.view isKindOfClass:[X64ArmGuestView class]]){
                // 透明子 view 使用自己的 backing，清除其背景不應清掉下層兄弟視窗的像素。
                w.view.wantsLayer=YES;w.view.canDrawSubviewsIntoLayer=NO;w.view.layer.backgroundColor=NSColor.clearColor.CGColor;
            }
            set_opacity(handle,(model.ex_style&0x80000U) && model.layered_initialized?model.layered_alpha:255);
            w.control.enabled=model.enabled;
            if(gui_control_class(model)==u"toolbarwindow32"){
                    if(!w.native_toolbar_buttons)w.native_toolbar_buttons=[NSMutableDictionary dictionary];NSMutableSet<NSNumber*>* used=[NSMutableSet set];
                    const bool common_changed=old.toolbar_font!=model.toolbar_font || old.toolbar_text_rows!=model.toolbar_text_rows || old.enabled!=model.enabled || old.style!=model.style || old.list_icons!=model.list_icons;
                    for(std::size_t index=0;index<model.toolbar_buttons.size();++index){const auto& item=model.toolbar_buttons[index];
                        if(item.state&8U)continue;
                        if(item.style&1U)continue;
                        NSNumber* key=@(index);[used addObject:key];auto* button=w.native_toolbar_buttons[key];const bool created=button==nil;
                        if(!created && !common_changed && index<old.toolbar_buttons.size() && old.toolbar_buttons[index]==item)continue;
                        const auto& rect=item.rectangle;
                        if(created){button=[[X64ArmToolbarButton alloc] initWithFrame:NSZeroRect];button.identifier=@"x64arm-toolbar-button";w.native_toolbar_buttons[key]=button;[w.view addSubview:button];}
                        button.frame=NSMakeRect(rect[0],rect[1]+2,rect[2]-rect[0],rect[3]-rect[1]);
                        button.title=model.toolbar_text_rows?label(item.text):@"";button.toolTip=label(item.text);button.bezelStyle=NSBezelStyleRegularSquare;button.bordered=(model.style&0x800U)==0;button.font=guest_font(model.toolbar_font);
                        const auto titleWidth=[button.title sizeWithAttributes:@{NSFontAttributeName:button.font}].width,guestWidth=guest_text_width(model.toolbar_font,button.font,button.title);button.guestTextScale=titleWidth>0 && guestWidth>0?guestWidth/titleWidth:1;
                        button.buttonType=(item.style&2U)?NSButtonTypePushOnPushOff:NSButtonTypeMomentaryPushIn;
                        button.state=(item.state&1U)?NSControlStateValueOn:NSControlStateValueOff;button.enabled=model.enabled && (item.state&4U);button.tag=item.command;button.cell.representedObject=@(index);
                        button.guestPressed=(item.state&2U)!=0;button.guestIndeterminate=(item.state&16U)!=0;
                        if(created || old.list_icons!=model.list_icons || index>=old.toolbar_buttons.size() || old.toolbar_buttons[index].image!=item.image)button.image=item.image>=0 && static_cast<std::size_t>(item.image)<model.list_icons.size()?list_icon(model.list_icons[static_cast<std::size_t>(item.image)]):nil;
                        button.imageScaling=NSImageScaleProportionallyDown;button.imagePosition=button.image?(button.title.length?((model.style&0x1000U)?NSImageLeft:NSImageAbove):NSImageOnly):NSNoImage;
                        button.target=w.target;button.action=@selector(toolbarClicked:);[button setNeedsDisplay:YES];
                    }
                    for(NSNumber* key in [w.native_toolbar_buttons.allKeys copy])if(![used containsObject:key]){[w.native_toolbar_buttons[key] removeFromSuperview];[w.native_toolbar_buttons removeObjectForKey:key];}
                }else if(gui_control_class(model)==u"msctls_statusbar32"){
                    [w.view removeAllToolTips];w.status_tooltips.clear();
                    if((model.style&0x800U) && !model.status_simple){NSFont* font=guest_font(model.control_font);
                        for(std::size_t index=0;index<model.status_parts.size();++index){const auto& part=model.status_parts[index];if(part.tooltip.empty())continue;
                            const auto& rect=gui_status_rectangle(model,index);const CGFloat width=std::max(0,rect[2]-rect[0]);const CGFloat available=std::max<CGFloat>(0,width-6-(part.icon_handle?19:0));
                            NSString* text=nsstring(part.text);const auto measured=[text sizeWithAttributes:@{NSFontAttributeName:font}].width*font_width_scale(model.control_font,font);
                            if(!(part.text.empty() && part.icon_handle) && ((part.flags&0x1000U) || measured<=available))continue;
                            const auto tag=[w.view addToolTipRect:NSMakeRect(rect[0],rect[1],width,std::max(0,rect[3]-rect[1])) owner:w.view userData:reinterpret_cast<void*>(index+1)];w.status_tooltips.emplace_back(tag,index);
                        }
                    }
                    [w.view setNeedsDisplay:YES];
                }else if([w.control isKindOfClass:[NSSlider class]]){
                    auto* slider=(NSSlider*)w.control;slider.minValue=model.track_min;slider.maxValue=model.track_max;slider.doubleValue=(model.style&2U)?static_cast<double>(model.track_min)+model.track_max-model.track_position:model.track_position;
                    const auto span=static_cast<std::int64_t>(model.track_max)-model.track_min;
                    slider.numberOfTickMarks=(model.track_ticks_cleared || (model.style&16U))?0:static_cast<NSInteger>(std::min<std::int64_t>(4096,2+((model.style&1U) && span>0?(span-1)/std::max(1,model.track_tick_frequency):0)));
                    slider.allowsTickMarkValuesOnly=NO;slider.tickMarkPosition=(model.style&4U)?NSTickMarkPositionAbove:NSTickMarkPositionBelow;
                }else if([w.view isKindOfClass:[NSProgressIndicator class]]) {
                    auto* progress=(NSProgressIndicator*)w.view;progress.minValue=model.progress_min;progress.maxValue=model.progress_max;progress.doubleValue=model.progress_position;
                    progress.indeterminate=model.progress_marquee;if(model.progress_marquee)[progress startAnimation:nil];else [progress stopAnimation:nil];
                }else if(w.editor){
                    const bool wrap=!(model.style&(0x80U|0x100000U));auto* scroll=(NSScrollView*)w.view;
                    scroll.hasVerticalScroller=(model.style&0x200000U)!=0;scroll.hasHorizontalScroller=(model.style&0x100000U)!=0;
                    w.editor.horizontallyResizable=!wrap;w.editor.autoresizingMask=wrap?NSViewWidthSizable:NSViewNotSizable;
                    w.editor.textContainer.widthTracksTextView=wrap;const auto size=scroll.contentSize;
                    w.editor.minSize=size;w.editor.textContainer.containerSize=NSMakeSize(wrap?size.width:CGFLOAT_MAX,CGFLOAT_MAX);
                    w.editor.alignment=(model.style&3U)==2?NSTextAlignmentRight:(model.style&3U)==1?NSTextAlignmentCenter:NSTextAlignmentLeft;
                    if(![w.editor.string isEqualToString:nsstring(model.title)])w.editor.string=nsstring(model.title);
                    if(old.title!=model.title || old.control_font!=model.control_font || old.width!=model.width || old.height!=model.height || old.style!=model.style)[w.editor sizeToFit];
                    w.editor.editable=model.enabled && !(model.style&0x800);
                }
                else if([w.control isKindOfClass:[NSButton class]]){auto* button=(NSButton*)w.control;button.title=label(model.title);const auto kind=model.style&15;
                    if([button isKindOfClass:[X64ArmGuestButton class]]){
                        // 自繪按鈕的焦點外觀由客體負責，避免 AppKit 疊上藍色圓角框。
                        button.focusRingType=kind==11?NSFocusRingTypeNone:NSFocusRingTypeDefault;
                        for(CALayer* layer in button.layer.sublayers)if([layer isKindOfClass:[CAMetalLayer class]])layer.hidden=kind!=11;
                        if((old.style&15U)!=kind){[button setNeedsDisplay:YES];if(kind==11){GuiMessage paint{handle,0,0,15};paint.queue_priority=2;events_.push_back(std::move(paint));}}
                    }
                    if(kind==4 || kind==9){
                        // 由客體的 WS_GROUP／BM_SETCHECK 管理群組；AppKit 的自動分組會跨越不同 Win32 群組。
                        button.image=[NSImage imageWithSystemSymbolName:model.check?@"largecircle.fill.circle":@"circle" accessibilityDescription:nil];button.alternateImage=button.image;button.state=NSControlStateValueOff;
                    }else button.state=model.check?NSControlStateValueOn:NSControlStateValueOff;
                }
                else if([w.control isKindOfClass:[NSComboBox class]]){auto* combo=(NSComboBox*)w.control;if(old.items!=model.items){[combo removeAllItems];for(const auto& i:model.items)[combo addItemWithObjectValue:nsstring(i.text)];}if(model.selection>=0)[combo selectItemAtIndex:model.selection];if(![combo.stringValue isEqualToString:nsstring(model.title)])combo.stringValue=nsstring(model.title);}
                else if([w.control isKindOfClass:[NSTableView class]]){auto* table=(NSTableView*)w.control;
                    NSFont* font=guest_font(model.control_font);CGFloat content_height=std::ceil(font.ascender-font.descender);
                    if(gui_control_class(model)==u"syslistview32")for(const auto& icon:model.list_icons)content_height=std::max(content_height,static_cast<CGFloat>(std::clamp(icon.height,1U,128U)));
                    table.rowHeight=content_height+4;for(NSTableColumn* column in table.tableColumns){NSTextFieldCell* cell=column.dataCell;cell.font=font;cell.usesSingleLineMode=YES;cell.wraps=NO;}

                    if(gui_control_class(model)==u"syslistview32" && (old.columns!=model.columns || old.style!=model.style || [table.tableColumns.firstObject.identifier isEqualToString:@"text"])){
                        for(NSTableColumn* column in [table.tableColumns copy])[table removeTableColumn:column];
                        const bool report=(model.style&3)==1;
                        const auto count=report?std::max<std::size_t>(1,model.columns.size()):1;
                        std::vector<std::size_t> order(count);std::iota(order.begin(),order.end(),0);if(report)std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return a<model.columns.size() && b<model.columns.size() && model.columns[a].order<model.columns[b].order;});
                        for(const auto i:order){
                            auto* column=[[NSTableColumn alloc] initWithIdentifier:[NSString stringWithFormat:@"%zu",i]];
                            column.width=i<model.columns.size()?std::max(0,model.columns[i].width):std::max<CGFloat>(20,model.width-4);column.minWidth=0;
                            column.title=i<model.columns.size()?nsstring(model.columns[i].text):@"";
                            auto* cell=[[X64ArmCenteredTextCell alloc] initTextCell:@""];cell.font=guest_font(model.control_font);cell.usesSingleLineMode=YES;cell.wraps=NO;cell.lineBreakMode=NSLineBreakByTruncatingTail;
                            const auto format=i<model.columns.size()?model.columns[i].format&3:0;cell.alignment=format==1?NSTextAlignmentRight:format==2?NSTextAlignmentCenter:NSTextAlignmentLeft;
                            column.dataCell=cell;[table addTableColumn:column];
                        }
                        const auto header=windows_.find(model.list_header);table.headerView=report && !(model.style&0x4000) && header!=windows_.end()?(NSTableHeaderView*)header->second.view:nil;
                        table.allowsMultipleSelection=!(model.style&4U);table.usesAlternatingRowBackgroundColors=NO;
                    }
                    if(old.items!=model.items || old.columns!=model.columns || old.list_icons!=model.list_icons || table.numberOfRows==0)[table reloadData];
                    if([table isKindOfClass:[NSOutlineView class]]){auto* tree=(NSOutlineView*)table;
                        std::set<std::uint64_t> previously_expanded;for(const auto& item:old.items)if(item.state&0x20U)previously_expanded.insert(item.handle);
                        for(const auto& item:model.items){if(item.state&0x20U)[tree expandItem:@(item.handle)];else if(previously_expanded.contains(item.handle))[tree collapseItem:@(item.handle)];}
                        const auto row=model.selection>=0 && static_cast<std::size_t>(model.selection)<model.items.size()?[tree rowForItem:@(model.items[static_cast<std::size_t>(model.selection)].handle)]:-1;
                        if(row>=0)[tree selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];else [tree deselectAll:nil];
                    }
                    else if(table.allowsMultipleSelection || gui_control_class(model)==u"syslistview32"){NSMutableIndexSet* selected=[NSMutableIndexSet indexSet];for(std::size_t i=0;i<model.items.size();++i)if(model.items[i].state&(gui_control_class(model)==u"syslistview32"?2U:1U))[selected addIndex:i];[table selectRowIndexes:selected byExtendingSelection:NO];}
                    else if(model.selection>=0)[table selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(model.selection)] byExtendingSelection:NO];else [table deselectAll:nil];
                }else if([w.view isKindOfClass:NSBox.class]){auto* box=(NSBox*)w.view;box.title=label(model.title);box.titleFont=guest_font(model.control_font);[(NSCell*)box.titleCell setEnabled:model.enabled];[box setNeedsDisplay:YES];
                }else if([w.control isKindOfClass:[NSTextField class]]){NSString* text=gui_control_class(model)==u"edit"?nsstring(model.title):label(model.title);if(![w.control.stringValue isEqualToString:text])w.control.stringValue=text;
                    if(gui_control_class(model)==u"edit"){auto* field=(NSTextField*)w.control;field.editable=(model.style&0x800U)==0;field.selectable=YES;}}
            if(gui_control_class(model)==u"edit" && (old.edit_start!=model.edit_start || old.edit_end!=model.edit_end || old.title!=model.title)){
                NSTextView* editor=w.editor?:([w.control.currentEditor isKindOfClass:[NSTextView class]]?(NSTextView*)w.control.currentEditor:nil);
                if(editor && !editor.hasMarkedText){const auto length=editor.string.length;const auto begin=std::min<NSUInteger>(std::min(model.edit_start,model.edit_end),length),end=std::min<NSUInteger>(std::max(model.edit_start,model.edit_end),length);
                    editor.selectedRange=NSMakeRange(begin,end-begin);}
            }
            [w.view setNeedsDisplay:YES];w.target.updating=NO;dirty_=true;
        }
    }
    std::optional<std::array<std::int32_t,4>> menu_rectangle(std::uint64_t handle,std::int32_t index) override {
        @autoreleasepool {const auto found=windows_.find(handle);if(found==windows_.end() || !found->second.menu_bar)return {};
            const auto& w=found->second;NSRect rect=w.menu_bar.bounds;
            if(index>=0){NSView* item=nil;for(NSView* child in w.menu_bar.subviews)if([child isKindOfClass:NSButton.class] && [(NSButton*)child tag]==index){item=child;break;}if(!item)return {};rect=item.frame;}
            const auto origin=[w.view convertPoint:NSMakePoint(rect.origin.x,rect.origin.y) fromView:w.menu_bar];
            return std::array<std::int32_t,4>{static_cast<std::int32_t>(std::floor(origin.x)),static_cast<std::int32_t>(std::floor(origin.y)),static_cast<std::int32_t>(std::ceil(origin.x+rect.size.width)),static_cast<std::int32_t>(std::ceil(origin.y+rect.size.height))};
        }
    }
    void set_menu(std::uint64_t handle,const std::vector<GuiPopupItem>& items) override {
        @autoreleasepool {
            const auto found=windows_.find(handle);if(found==windows_.end() || !found->second.native)return;auto& window=found->second;
            auto* target=[[X64ArmMainMenuTarget alloc] init];target.owner=handle;target.events=&events_;
            std::size_t count=0;
            std::function<NSMenu*(const std::vector<GuiPopupItem>&,unsigned)> build=[&](const auto& entries,unsigned depth)->NSMenu*{
                if(depth>64)return nil;auto* menu=[[NSMenu alloc] initWithTitle:@""];menu.autoenablesItems=NO;
                for(const auto& entry:entries){if(++count>65536)return nil;const bool submenu=(entry.flags&0x10U)!=0 || !entry.children.empty();
                    auto* item=(entry.flags&0x800U)?[NSMenuItem separatorItem]:[[X64ArmNativeMenuItem alloc] initWithTitle:menu_title(entry.text) action:submenu?nullptr:@selector(choose:) keyEquivalent:@""];
                    item.enabled=(entry.flags&3U)==0 && !item.separatorItem;item.state=(entry.flags&8U)?NSControlStateValueOn:NSControlStateValueOff;item.representedObject=@(entry.id);item.target=target;
                    menu_bitmaps(item,entry);
                    if([item isKindOfClass:X64ArmNativeMenuItem.class]){auto* native=(X64ArmNativeMenuItem*)item;native.guestMenu=entry.menu;native.guestPosition=entry.position;native.notifyByPosition=(entry.flags&0x08000000U)!=0;}
                    if(entry.flags&0x1000U)item.attributedTitle=[[NSAttributedString alloc] initWithString:item.title attributes:@{NSFontAttributeName:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]}];
                    if(submenu){item.submenu=build(entry.children,depth+1);if(!item.submenu)return nil;item.submenu.title=item.title;}
                    [menu addItem:item];
                }return menu;
            };
            NSMenu* menu=build(items,0);if(!menu)throw std::runtime_error("主選單層級或項目數超過上限");
            // 第一個 AppKit 選單保留應用程式項，客體各頂層選單依原始順序接在其後。
            auto* app=[[NSMenuItem alloc] initWithTitle:@"" action:nullptr keyEquivalent:@""];app.submenu=[[NSMenu alloc] initWithTitle:@""];
            [app.submenu addItemWithTitle:@"隱藏" action:@selector(hide:) keyEquivalent:@"h"];[menu insertItem:app atIndex:0];
            sync_native_frame(window);
            if(window.menu_bar){
                for(NSView* child in [window.menu_bar.subviews copy])[child removeFromSuperview];CGFloat x=2;
                for(std::size_t i=0;i<items.size();++i){const auto& entry=items[i];
                    auto* button=[[X64ArmMenuButton alloc] initWithFrame:NSZeroRect];button.title=menu_title(entry.text);button.font=[NSFont systemFontOfSize:12];button.bordered=NO;button.bezelStyle=NSBezelStyleRegularSquare;button.focusRingType=NSFocusRingTypeNone;
                    button.image=entry.custom_checkmarks?menu_bitmap((entry.flags&8U)?entry.checked_bitmap:entry.unchecked_bitmap):menu_bitmap(entry.bitmap);button.imagePosition=NSImageLeft;
                    const auto width=[button.title sizeWithAttributes:@{NSFontAttributeName:button.font}].width+16+(button.image?button.image.size.width+4:0);
                    button.frame=NSMakeRect(x,0,width,20);button.enabled=window.model.enabled && !(entry.flags&3U);button.tag=static_cast<NSInteger>(i);button.target=target;button.action=@selector(openMenu:);[window.menu_bar addSubview:button];x+=width;
                }
            }
            window.menu_target=target;window.main_menu=menu;window.delegate.mainMenu=menu;
            if(window.native.keyWindow || !NSApp.keyWindow || automation_)NSApp.mainMenu=menu;dirty_=true;
        }
    }
    static NSString* webview_text(const std::u16string& value){return [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(value.data()) length:value.size()];}
    WKWebView* webview(std::uint64_t id){const auto found=webviews_.find(id);return found==webviews_.end()?nil:found->second;}
    bool webview_create(std::uint64_t id,std::uint64_t parent) override {
        @autoreleasepool {
            if(offline_render_enabled())return false;
            const auto found=windows_.find(parent);if(found==windows_.end() || !found->second.view)return false;
            webview_destroy(id);
            WKWebViewConfiguration* configuration=[[WKWebViewConfiguration alloc] init];
            configuration.websiteDataStore=[WKWebsiteDataStore nonPersistentDataStore];
            // window.chrome.webview 的頁面端介面：主機傳入的訊息以 message 事件派送。
            NSString* bootstrap=@"(function(){if(window.chrome&&window.chrome.webview)return;var t=new EventTarget();window.chrome=window.chrome||{};"
                "window.chrome.webview={addEventListener:t.addEventListener.bind(t),removeEventListener:t.removeEventListener.bind(t),postMessage:function(){},"
                "__dispatch:function(d){var e=new Event('message');e.data=d;t.dispatchEvent(e);}};})();";
            [configuration.userContentController addUserScript:[[WKUserScript alloc] initWithSource:bootstrap injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO]];
            WKWebView* view=[[WKWebView alloc] initWithFrame:NSZeroRect configuration:configuration];
            view.autoresizingMask=NSViewNotSizable;view.navigationDelegate=webview_delegate_;
            if(webview_trace_enabled())std::fprintf(stderr,"[webview] create parent=%llu view=%s flipped=%d layer=%s\n",static_cast<unsigned long long>(parent),
                NSStringFromClass(found->second.view.class).UTF8String,found->second.view.isFlipped?1:0,found->second.view.layer?NSStringFromClass(found->second.view.layer.class).UTF8String:"none");
            [found->second.view addSubview:view positioned:NSWindowAbove relativeTo:nil];
            webviews_[id]=view;dirty_=true;return true;
        }
    }
    void webview_destroy(std::uint64_t id) override {
        @autoreleasepool {const auto found=webviews_.find(id);if(found==webviews_.end())return;[found->second stopLoading];[found->second removeFromSuperview];webviews_.erase(found);dirty_=true;}
    }
    void webview_visible(std::uint64_t id,bool visible) override {if(auto* view=webview(id))view.hidden=!visible;}
    void webview_bounds(std::uint64_t id,const std::array<std::int32_t,4>& rect) override {
        // 父 view 為翻轉座標（左上原點），與客體 client 座標一致。
        if(auto* view=webview(id)){view.frame=NSMakeRect(rect[0],rect[1],std::max(0,rect[2]-rect[0]),std::max(0,rect[3]-rect[1]));
            if(webview_trace_enabled())std::fprintf(stderr,"[webview] bounds %d,%d,%d,%d superview=%.0fx%.0f\n",rect[0],rect[1],rect[2],rect[3],view.superview.bounds.size.width,view.superview.bounds.size.height);}
    }
    void webview_zoom(std::uint64_t id,double zoom) override {if(auto* view=webview(id))view.pageZoom=std::clamp(zoom,0.25,5.0);}
    void webview_focus(std::uint64_t id) override {if(auto* view=webview(id))[view.window makeFirstResponder:view];}
    void webview_navigate(std::uint64_t id,const std::u16string& uri) override {
        @autoreleasepool {auto* view=webview(id);if(!view)return;NSURL* url=[NSURL URLWithString:webview_text(uri)];if(!url)return;
            if(url.isFileURL)[view loadFileURL:url allowingReadAccessToURL:url.URLByDeletingLastPathComponent];else [view loadRequest:[NSURLRequest requestWithURL:url]];}
    }
    void webview_load_html(std::uint64_t id,const std::u16string& html) override {@autoreleasepool {if(auto* view=webview(id)){if(webview_trace_enabled())std::fprintf(stderr,"[webview] load html=%zu chars\n",html.size());[view loadHTMLString:webview_text(html) baseURL:[NSURL URLWithString:@"about:blank"]];}}}
    void webview_command(std::uint64_t id,unsigned command) override {
        auto* view=webview(id);if(!view)return;
        if(command==0)[view reload];else if(command==1)[view stopLoading];else if(command==2)[view goBack];else if(command==3)[view goForward];
    }
    void webview_script(std::uint64_t id,const std::u16string& script) override {@autoreleasepool {if(auto* view=webview(id))[view evaluateJavaScript:webview_text(script) completionHandler:nil];}}
    void webview_user_script(std::uint64_t id,const std::u16string& script) override {
        @autoreleasepool {if(auto* view=webview(id))[view.configuration.userContentController addUserScript:[[WKUserScript alloc] initWithSource:webview_text(script) injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];}
    }
    void webview_post_message(std::uint64_t id,const std::u16string& message,bool json) override {
        @autoreleasepool {
            auto* view=webview(id);if(!view)return;NSString* payload=webview_text(message);
            if(!json){NSData* data=[NSJSONSerialization dataWithJSONObject:payload options:NSJSONWritingFragmentsAllowed error:nil];if(!data)return;payload=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];}
            [view evaluateJavaScript:[NSString stringWithFormat:@"window.chrome&&window.chrome.webview&&window.chrome.webview.__dispatch(%@);",payload] completionHandler:nil];
        }
    }
    std::optional<std::uint32_t> text_line_count(std::uint64_t handle) override {
        const auto lines=text_line_ranges(handle);return lines?std::optional<std::uint32_t>{static_cast<std::uint32_t>(lines->size())}:std::nullopt;
    }
    std::optional<std::vector<std::array<std::uint32_t,2>>> text_line_ranges(std::uint64_t handle) override {
        @autoreleasepool {
            const auto found=windows_.find(handle);if(found==windows_.end() || !found->second.editor)return std::nullopt;
            NSTextView* editor=found->second.editor;NSLayoutManager* layout=editor.layoutManager;
            [layout ensureLayoutForTextContainer:editor.textContainer];std::vector<std::array<std::uint32_t,2>> lines;NSUInteger index=0;
            while(index<layout.numberOfGlyphs){NSRange range;[layout lineFragmentRectForGlyphAtIndex:index effectiveRange:&range];const auto next=NSMaxRange(range);if(next<=index)break;
                const auto characters=[layout characterRangeForGlyphRange:range actualGlyphRange:nullptr];auto end=std::min(NSMaxRange(characters),editor.string.length);
                while(end>characters.location){const auto c=[editor.string characterAtIndex:end-1];if(c!='\r' && c!='\n')break;--end;}
                lines.push_back({static_cast<std::uint32_t>(characters.location),static_cast<std::uint32_t>(end)});index=next;
            }
            if(layout.extraLineFragmentTextContainer || lines.empty()){const auto length=static_cast<std::uint32_t>(editor.string.length);lines.push_back({length,length});}
            return lines;
        }
    }
    std::optional<GuiFontMetrics> font_metrics(const GuiFont& font) override {
        @autoreleasepool { return guest_metrics(font); }
    }
    std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags) override {
        return popup_menu(owner,items,x,y,flags,{});
    }
    std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle) override {
        return popup_menu(owner,items,x,y,flags,idle,{});
    }
    std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle,const std::function<bool(std::uint64_t,std::uint32_t,bool)>& popup) override {
        return popup_menu(owner,items,x,y,flags,idle,popup,{});
    }
    std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle,const std::function<bool(std::uint64_t,std::uint32_t,bool)>& popup,const std::function<bool(const GuiMenuHighlight&)>& highlight) override {
        @autoreleasepool {
            // 根選單開始新的 AppKit 追蹤前，先完成前一次追蹤留下的放開／視窗事件。
            // 輪詢產生的 Windows 訊息留在原佇列；遞迴選單由既有追蹤迴圈持有事件，不能重入輪詢。
            if(popup_stack_.empty()){
                auto pending=poll_for(0);events_=std::move(pending);
            }
            const auto found=windows_.find(owner);if(found==windows_.end() || !found->second.view.window.visible)return std::nullopt;
            NSView* popupOwnerView=found->second.view;
            auto* target=[[X64ArmMenuTarget alloc] init];NSMutableDictionary* record=[@{@"owner":@(owner),@"x":@(x),@"y":@(y),@"flags":@(flags),@"opened":@0,@"closed":@0,@"native_action_count":@0,@"selected":[NSNull null],@"tracking":@YES,@"cancelled":@NO} mutableCopy];
            target.record=record;target->changed=[this]{diagnostics();};
            std::size_t count=0;
            std::function<NSMenu*(const std::vector<GuiPopupItem>&,NSMutableArray*,unsigned)> build=[&](const auto& entries,NSMutableArray* description,unsigned depth)->NSMenu*{
                if(depth>64)return nil;NSMenu* menu=[[X64ArmPopupMenu alloc] initWithTitle:@""];menu.autoenablesItems=NO;menu.delegate=target;
                for(const auto& entry:entries){if(++count>65536)return nil;
                    const bool separator=(entry.flags&0x800U)!=0,submenu=(entry.flags&0x10U)!=0 || !entry.children.empty();
                    NSMenuItem* item=separator?[NSMenuItem separatorItem]:[[X64ArmNativeMenuItem alloc] initWithTitle:menu_title(entry.text) action:submenu?nullptr:@selector(choose:) keyEquivalent:@""];
                    item.enabled=(entry.flags&3U)==0 && !separator;item.state=(entry.flags&8U)?NSControlStateValueOn:NSControlStateValueOff;item.representedObject=@(entry.id);item.target=target;
                    menu_bitmaps(item,entry);
                    if([item isKindOfClass:X64ArmNativeMenuItem.class]){auto* native=(X64ArmNativeMenuItem*)item;native.guestMenu=entry.menu;native.guestPosition=entry.position;native.notifyByPosition=(entry.flags&0x08000000U)!=0;}
                    if(entry.flags&0x1000U)item.attributedTitle=[[NSAttributedString alloc] initWithString:item.title attributes:@{NSFontAttributeName:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]}];
                    NSMutableArray* children=[NSMutableArray array];if(submenu){item.submenu=build(entry.children,children,depth+1);if(!item.submenu)return nil;auto* nested=(X64ArmPopupMenu*)item.submenu;nested.guestMenu=entry.id;nested.guestPosition=entry.position;}
                    [menu addItem:item];[description addObject:@{@"id":@(entry.id),@"flags":@(entry.flags),@"title":item.title,@"enabled":@(item.enabled),@"checked":@(item.state==NSControlStateValueOn),@"separator":@(separator),@"children":children,@"custom_checkmarks":@(entry.custom_checkmarks),@"checked_bitmap":menu_bitmap_details(item.onStateImage),@"unchecked_bitmap":menu_bitmap_details(item.offStateImage),@"bitmap":menu_bitmap_details(item.image)}];
                }return menu;
            };
            // 保留仍可沿用的項目與子選單身分；非空內容不經過暫時清空，避免 AppKit 結束該層鍵盤追蹤。
            std::function<void(NSMenu*,NSMenu*)> reconcileMenu=[&](NSMenu* destination,NSMenu* replacement){
                bool compatible=destination.numberOfItems==replacement.numberOfItems;
                if(compatible)for(NSInteger i=0;i<destination.numberOfItems;++i){
                    NSMenuItem* current=[destination itemAtIndex:i];NSMenuItem* next=[replacement itemAtIndex:i];
                    if(current.separatorItem!=next.separatorItem || (current.submenu!=nil)!=(next.submenu!=nil) ||
                       (current.submenu && ((X64ArmPopupMenu*)current.submenu).guestMenu!=((X64ArmPopupMenu*)next.submenu).guestMenu)){compatible=false;break;}
                }
                if(compatible){
                    for(NSInteger i=0;i<destination.numberOfItems;++i){
                        NSMenuItem* current=[destination itemAtIndex:i];NSMenuItem* next=[replacement itemAtIndex:i];
                        if(current.separatorItem)continue;
                        current.title=next.title;current.attributedTitle=next.attributedTitle;current.enabled=next.enabled;current.state=next.state;
                        current.image=next.image;current.onStateImage=next.onStateImage;current.offStateImage=next.offStateImage;
                        current.representedObject=next.representedObject;current.target=next.target;current.action=next.action;
                        auto* native=(X64ArmNativeMenuItem*)current;auto* updated=(X64ArmNativeMenuItem*)next;
                        native.guestMenu=updated.guestMenu;native.guestPosition=updated.guestPosition;native.notifyByPosition=updated.notifyByPosition;
                        if(current.submenu)reconcileMenu(current.submenu,next.submenu);
                    }
                }else {
                    NSArray<NSMenuItem*>* previous=[destination.itemArray copy];
                    for(NSMenuItem* item in [replacement.itemArray copy]){[replacement removeItem:item];[destination addItem:item];}
                    for(NSMenuItem* item in previous)[destination removeItem:item];
                }
            };
            NSMutableArray* description=[NSMutableArray array];NSMenu* menu=build(items,description,0);if(!menu)return std::nullopt;record[@"items"]=description;
            if(popup_history_.count>=128)[popup_history_ removeObjectAtIndex:0];[popup_history_ addObject:record];diagnostics();
            NSPoint location=NSMakePoint(x,y);std::uint64_t handle=owner;
            for(unsigned depth=0;handle && depth<64;++depth){const auto window=windows_.find(handle);if(window==windows_.end())break;const auto inset=gui_frame_insets(window->second.model);location.x-=static_cast<double>(window->second.model.x)+inset.left;location.y-=static_cast<double>(window->second.model.y)+inset.top;handle=window->second.model.parent;}
            [menu update];const auto size=menu.size;
            if(flags&8U)location.x-=size.width;else if(flags&4U)location.x-=size.width/2;
            if(flags&0x20U)location.y-=size.height;else if(flags&0x10U)location.y-=size.height/2;
            // popUpMenuPositioningItem 執行 AppKit tracking loop；測試也經 NSMenu 本身送 action，絕不直接注入客體命令。
            NSTimer* idleTimer=nil;auto idleError=std::make_shared<std::exception_ptr>();__block bool idleBusy=false;
            __block std::uint64_t idleCycles=0;
            auto shownItems=std::make_shared<std::vector<GuiPopupItem>>(items);
            auto* itemCount=&count;
            const auto* liveItems=&items;
            struct NotificationScope {unsigned& depth;explicit NotificationScope(unsigned& value):depth(value){++depth;}~NotificationScope(){--depth;}};
            auto notificationDepth=std::make_shared<unsigned>(0);
            target->canChoose=[&](NSMenuItem* item){
                if(![item isKindOfClass:X64ArmNativeMenuItem.class])return false;
                auto* selected=(X64ArmNativeMenuItem*)item;bool valid=false;
                std::function<void(const std::vector<GuiPopupItem>&)> find=[&](const auto& entries){for(const auto& entry:entries){
                    if(entry.menu==selected.guestMenu && entry.position==selected.guestPosition && entry.id==[selected.representedObject unsignedLongLongValue]){valid=!(entry.flags&(3U|0x800U|0x10U));return;}
                    find(entry.children);
                }};find(*liveItems);return valid;
            };
            NSMutableArray* highlights=[NSMutableArray array];
            target->highlightChanged=[&](NSMenuItem* item,bool mouse){
                if(highlights.count>=4096){*idleError=std::make_exception_ptr(std::runtime_error("選單高亮佇列超過上限"));[menu cancelTrackingWithoutAnimation];return false;}
                [highlights addObject:@{@"item":item,@"mouse":@(mouse)}];return true;
            };
            const auto flushHighlights=[&]{
                NSArray* pending=[highlights copy];[highlights removeAllObjects];
                for(NSDictionary* event in pending){
                    NSMenuItem* item=event[@"item"];if(![item isKindOfClass:X64ArmNativeMenuItem.class] || !highlight)continue;
                    auto* selected=(X64ArmNativeMenuItem*)item;NotificationScope scope{*notificationDepth};
                    if(!highlight({selected.guestMenu,selected.guestPosition,[event[@"mouse"] boolValue]})){
                        record[@"guest_cancel_requested"]=@YES;
                        if(automation_ && !target.pendingChoice && action_<automation_.count && [automation_[action_][@"cancel_on_highlight"] boolValue]){++action_;last_action_=[NSDate date];}
                        [menu cancelTrackingWithoutAnimation];return false;
                    }
                    record[@"highlight_count"]=@([record[@"highlight_count"] unsignedIntValue]+1);
                }
                if(pending.count)diagnostics();return true;
            };
            NSMutableArray<X64ArmPopupMenu*>* deferredPopups=[NSMutableArray array];
            const auto refreshPopup=[&](X64ArmPopupMenu* changed){
                const std::vector<GuiPopupItem>* children=nullptr;
                std::function<void(const std::vector<GuiPopupItem>&)> find=[&](const auto& entries){for(const auto& entry:entries){if(entry.id==changed.guestMenu && (entry.flags&0x10U)){children=&entry.children;return;}if(!children)find(entry.children);}};
                find(*liveItems);if(!children){[menu cancelTrackingWithoutAnimation];return;}
                const auto* updatedChildren=children;children=nullptr;find(*shownItems);const auto* visibleChildren=children;
                if(visibleChildren && *visibleChildren==*updatedChildren)return;
                count=0;NSMutableArray* refreshed=[NSMutableArray array];NSMenu* replacement=build(*updatedChildren,refreshed,0);
                if(!replacement)throw std::runtime_error("子選單初始化後內容超過上限");
                reconcileMenu(changed,replacement);
                // 只標記已投影的子樹；同一回呼修改的其他分支仍由一般更新流程處理。
                std::function<bool(std::vector<GuiPopupItem>&)> acknowledge=[&](auto& entries){for(auto& entry:entries){if(entry.id==changed.guestMenu && (entry.flags&0x10U)){entry.children=*updatedChildren;return true;}if(acknowledge(entry.children))return true;}return false;};
                (void)acknowledge(*shownItems);
                record[@"submenu_rebuilds"]=@([record[@"submenu_rebuilds"] unsignedIntValue]+1);
            };
            const auto flushPopupUpdates=[&]{
                if(*notificationDepth || !deferredPopups.count)return;
                NSArray<X64ArmPopupMenu*>* pending=[deferredPopups copy];[deferredPopups removeAllObjects];
                for(X64ArmPopupMenu* changed in pending)if(changed.guestInitialized){refreshPopup(changed);record[@"submenu_deferred_updates"]=@([record[@"submenu_deferred_updates"] unsignedIntValue]+1);}
                diagnostics();
            };
            target->popupChanged=[&](X64ArmPopupMenu* changed,bool opening){
                if(*idleError || !popup)return;
                NotificationScope scope{*notificationDepth};
                try{
                    if(!popup(changed.guestMenu,changed.guestPosition,opening)){record[@"guest_cancel_requested"]=@YES;[menu cancelTrackingWithoutAnimation];return;}
                    if(!opening)return;
                    record[@"submenu_updates"]=@([record[@"submenu_updates"] unsignedIntValue]+1);
                    // AppKit 重新開啟已快取分支時省略 menuNeedsUpdate；willOpen 中只通知客體，結構更新留到回呼外。
                    if(changed.deferGuestRefresh){if(![deferredPopups containsObject:changed])[deferredPopups addObject:changed];}
                    else refreshPopup(changed);
                    diagnostics();
                }catch(...){*idleError=std::current_exception();[menu cancelTrackingWithoutAnimation];}
            };
            if(idle){idleTimer=[NSTimer timerWithTimeInterval:0.01 repeats:YES block:^(NSTimer* current){
                (void)current;if(idleBusy || *idleError || *notificationDepth)return;idleBusy=true;
                try{flushPopupUpdates();if(!flushHighlights() || !idle()){record[@"guest_cancel_requested"]=@YES;[menu cancelTrackingWithoutAnimation];}
                    else {++idleCycles;
                        if(*shownItems!=*liveItems){
                        *itemCount=0;NSMutableArray* refreshed=[NSMutableArray array];NSMenu* replacement=build(*liveItems,refreshed,0);
                        if(!replacement)throw std::runtime_error("動態選單層級或項目數超過上限");
                        reconcileMenu(menu,replacement);
                        [menu update];*shownItems=*liveItems;record[@"items"]=refreshed;record[@"dynamic_updates"]=@([record[@"dynamic_updates"] unsignedIntValue]+1);diagnostics();
                    }}}
                catch(...){*idleError=std::current_exception();[menu cancelTrackingWithoutAnimation];}
                idleBusy=false;
            }];[[NSRunLoop currentRunLoop] addTimer:idleTimer forMode:NSEventTrackingRunLoopMode];[[NSRunLoop currentRunLoop] addTimer:idleTimer forMode:NSModalPanelRunLoopMode];}
            id inputObserver=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskAny handler:^NSEvent*(NSEvent* event){
                try{flushPopupUpdates();}catch(...){*idleError=std::current_exception();[menu cancelTrackingWithoutAnimation];}
                return event;
            }];
            NSTimer* timer=nil;NSMutableArray<NSEvent*>* postedEvents=[NSMutableArray array];__block bool terminalInputPosted=false;
            if(automation_){__block void (^__weak automationStep)(NSTimer*)=nil;
                void (^stepBlock)(NSTimer*)=^(NSTimer* current){
                (void)current;if(terminalInputPosted || *notificationDepth || target.pendingChoice || target.didSelect || action_>=automation_.count || [record[@"opened"] unsignedIntValue]<=[record[@"closed"] unsignedIntValue] || -last_action_.timeIntervalSinceNow<0.1)return;
                NSDictionary* step=automation_[action_];
                if(step[@"delay"] && -last_action_.timeIntervalSinceNow<[step[@"delay"] doubleValue])return;
                const auto fail=[&](NSString* message){record[@"automation_error"]=message;[menu cancelTrackingWithoutAnimation];diagnostics();};
                if([step[@"action"] isEqualToString:@"menu_input"]){
                    NSDictionary* keys=@{@"up":@[@(NSUpArrowFunctionKey),@126],@"down":@[@(NSDownArrowFunctionKey),@125],@"left":@[@(NSLeftArrowFunctionKey),@123],@"right":@[@(NSRightArrowFunctionKey),@124],@"home":@[@(NSHomeFunctionKey),@115],@"end":@[@(NSEndFunctionKey),@119],@"return":@[@13,@36],@"escape":@[@27,@53]};
                    NSArray* key=keys[step[@"key"]];if(!key){fail(@"選單原生輸入的鍵名無效");return;}
                    const unichar value=static_cast<unichar>([key[0] unsignedIntValue]);NSString* text=[NSString stringWithCharacters:&value length:1];
                    for(const auto type:{NSEventTypeKeyDown,NSEventTypeKeyUp}){
                        NSEvent* event=[NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:0 context:nil characters:text charactersIgnoringModifiers:text isARepeat:NO keyCode:static_cast<unsigned short>([key[1] unsignedIntValue])];
                        [postedEvents addObject:event];[automation_posted_events_ addObject:event];[NSApp postEvent:event atStart:NO];
                    }
                    const bool leaf=target.highlighted && target.highlighted.enabled && !target.highlighted.submenu;
                    const bool rootOnly=[record[@"opened"] unsignedIntValue]-[record[@"closed"] unsignedIntValue]<=1;
                    terminalInputPosted=([step[@"key"] isEqualToString:@"return"] && leaf) || ([step[@"key"] isEqualToString:@"escape"] && rootOnly);
                    ++action_;last_action_=[NSDate date];record[@"native_keys_posted"]=@([record[@"native_keys_posted"] unsignedIntValue]+1);diagnostics();return;
                }
                const bool mouseInput=[step[@"action"] isEqualToString:@"menu_mouse"];
                if(!mouseInput && ![step[@"action"] isEqualToString:@"menu"])return;
                if([step[@"cancel"] boolValue]){record[@"cancel_requested"]=@YES;++action_;last_action_=[NSDate date];[menu cancelTrackingWithoutAnimation];diagnostics();return;}
                NSArray* path=step[@"path"];if(![path isKindOfClass:[NSArray class]] || !path.count){fail(@"選單測試缺少 path");return;}
                NSMenu* currentMenu=menu;NSMenuItem* chosen=nil;NSInteger index=-1;bool enabled=true;
                for(NSUInteger depth=0;depth<path.count;++depth){id component=path[depth];chosen=nil;index=-1;
                    if([component isKindOfClass:[NSNumber class]]){index=[component integerValue];if(index>=0 && index<currentMenu.numberOfItems)chosen=[currentMenu itemAtIndex:index];}
                    else if([component isKindOfClass:[NSString class]])for(NSInteger i=0;i<currentMenu.numberOfItems;++i){NSMenuItem* candidate=[currentMenu itemAtIndex:i];if([candidate.title isEqualToString:component]){chosen=candidate;index=i;break;}}
                    if(!chosen){fail(@"選單測試 path 不存在");return;}enabled=enabled && chosen.enabled && !chosen.separatorItem;
                    if(depth+1<path.count){if(!chosen.submenu){fail(@"選單測試 path 穿過非子選單項目");return;}
                        auto* child=[chosen.submenu isKindOfClass:X64ArmPopupMenu.class]?(X64ArmPopupMenu*)chosen.submenu:nil;
                        if([step[@"open_submenus"] boolValue] && child && !child.guestInitialized){
                            [currentMenu cancelTrackingWithoutAnimation];
                            // 正在觸發的 NSTimer 不會在同一回呼內再次排程；子追蹤用另一 timer。
                            NSTimer* childTimer=[NSTimer timerWithTimeInterval:0.05 repeats:YES block:automationStep];
                            [[NSRunLoop currentRunLoop] addTimer:childTimer forMode:NSEventTrackingRunLoopMode];[[NSRunLoop currentRunLoop] addTimer:childTimer forMode:NSModalPanelRunLoopMode];
                            [child popUpMenuPositioningItem:nil atLocation:location inView:popupOwnerView];
                            [childTimer invalidate];return;
                        }
                        currentMenu=chosen.submenu;}
                }
                if(mouseInput){
                    const NSRect frame=chosen.accessibilityFrame;id accessibleWindow=chosen.accessibilityWindow;
                    NSWindow* inputWindow=[accessibleWindow isKindOfClass:NSWindow.class]?accessibleWindow:nil;
                    // NSMenuItem 可提供螢幕座標，卻沒有 accessibilityWindow；由公開 hit-test 找同程序原生視窗。
                    if(!inputWindow && !NSIsEmptyRect(frame))inputWindow=[NSApp windowWithWindowNumber:[NSWindow windowNumberAtPoint:NSMakePoint(NSMidX(frame),NSMidY(frame)) belowWindowWithWindowNumber:0]];
                    if(!inputWindow || NSIsEmptyRect(frame)){
                        // 子選單的 willOpen 早於原生視窗完成顯示；等待公開座標可用再送事件。
                        if(-last_action_.timeIntervalSinceNow<2.0){record[@"mouse_frame_waits"]=@([record[@"mouse_frame_waits"] unsignedIntValue]+1);return;}
                        NSMutableArray* nativeWindows=[NSMutableArray array];for(NSWindow* window in NSApp.windows)[nativeWindows addObject:@{@"class":NSStringFromClass(window.class),@"number":@(window.windowNumber),@"visible":@(window.visible),@"frame":NSStringFromRect(window.frame)}];
                        record[@"mouse_accessibility"]=@{@"frame":NSStringFromRect(frame),@"window_class":accessibleWindow?NSStringFromClass([accessibleWindow class]):@"nil",@"parent_class":chosen.accessibilityParent?NSStringFromClass([chosen.accessibilityParent class]):@"nil",@"windows":nativeWindows};
                        fail(@"原生選單項目無可操作的 accessibility frame／window");return;}
                    const NSPoint point=[inputWindow convertPointFromScreen:NSMakePoint(NSMidX(frame),NSMidY(frame))];
                    const bool click=[step[@"phase"] isEqualToString:@"click"];
                    for(const auto type:click?std::vector<NSEventType>{NSEventTypeLeftMouseDown,NSEventTypeLeftMouseUp}:std::vector<NSEventType>{NSEventTypeMouseMoved}){
                        NSEvent* event=[NSEvent mouseEventWithType:type location:point modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:inputWindow.windowNumber context:nil eventNumber:0 clickCount:click?1:0 pressure:click?1:0];
                        if(event.CGEvent){
                            // AppKit 的選單追蹤也使用底層事件的命中視窗，不能只填 NSEvent.windowNumber。
                            CGEventRef native=CGEventCreateCopy(event.CGEvent);
                            CGEventSetIntegerValueField(native,kCGMouseEventWindowUnderMousePointer,inputWindow.windowNumber);
                            CGEventSetIntegerValueField(native,kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent,inputWindow.windowNumber);
                            event=[NSEvent eventWithCGEvent:native];CFRelease(native);
                            const auto nativePoint=CGEventGetLocation(event.CGEvent);
                            record[@"mouse_event"]=@{@"window":@(inputWindow.windowNumber),@"under_mouse":@(CGEventGetIntegerValueField(event.CGEvent,kCGMouseEventWindowUnderMousePointer)),@"handles_mouse":@(CGEventGetIntegerValueField(event.CGEvent,kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent)),@"cg_point":@[@(nativePoint.x),@(nativePoint.y)],@"screen_point":NSStringFromPoint([inputWindow convertPointToScreen:point]),@"cursor":NSStringFromPoint(NSEvent.mouseLocation)};
                        }
                        [postedEvents addObject:event];[automation_posted_events_ addObject:event];[NSApp postEvent:event atStart:NO];
                    }
                    terminalInputPosted=click && chosen.enabled && !chosen.submenu;
                    record[@"mouse_frame"]=@[@(frame.origin.x),@(frame.origin.y),@(frame.size.width),@(frame.size.height)];record[@"native_mouse_posted"]=@([record[@"native_mouse_posted"] unsignedIntValue]+1);
                    ++action_;last_action_=[NSDate date];diagnostics();return;
                }
                if([step[@"expect_disabled"] boolValue]){if(enabled){fail(@"選單項目未停用");return;}record[@"disabled_verified"]=@YES;++action_;last_action_=[NSDate date];diagnostics();return;}
                if(!enabled || chosen.submenu){fail(@"選單測試項目無法執行");return;}
                // 先交付目前動作，客體回呼若開啟下一層原生選單，才能取得後續動作。
                ++action_;last_action_=[NSDate date];record[@"selected_path"]=path;target.expectedRejection=[step[@"expect_rejected"] boolValue];[currentMenu performActionForItemAtIndex:index];
                if(!target.pendingChoice && !([step[@"cancel_on_highlight"] boolValue] && [record[@"guest_cancel_requested"] boolValue])){fail(@"NSMenu 未觸發原生選取 action");return;}
                [currentMenu cancelTrackingWithoutAnimation];[menu cancelTrackingWithoutAnimation];diagnostics();
            };automationStep=stepBlock;timer=[NSTimer timerWithTimeInterval:0.05 repeats:YES block:stepBlock];
                [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSEventTrackingRunLoopMode];[[NSRunLoop currentRunLoop] addTimer:timer forMode:NSModalPanelRunLoopMode];}
            NSView* ownerView=found->second.view;NSWindow* ownerWindow=ownerView.window;
            // AppKit 同時只追蹤一個 root menu；遞迴客體選單先暫停外層原生追蹤。
            if(!popup_stack_.empty()){const auto parent=popup_stack_.back();parent.record[@"suspended"]=@YES;[parent.menu cancelTrackingWithoutAnimation];}
            popup_stack_.push_back({menu,record});
            while(!target.didSelect && !*idleError){
                record[@"suspended"]=@NO;terminalInputPosted=false;
                record[@"native_tracking_selected"]=@([menu popUpMenuPositioningItem:nil atLocation:location inView:ownerView]);
                try{
                    if(!flushHighlights() || [record[@"guest_cancel_requested"] boolValue])break;
                    if(target.pendingChoice){
                        const bool selected=[target finishChoice];
                        if(target.expectedRejection && selected){record[@"automation_error"]=@"選取時未拒絕已失效項目";break;}
                        target.expectedRejection=NO;if(selected)break;
                        // 通知已停用／移除候選項目，繼續追蹤最新內容。
                        count=0;NSMutableArray* refreshed=[NSMutableArray array];NSMenu* replacement=build(*liveItems,refreshed,0);
                        if(!replacement)throw std::runtime_error("選取通知後選單內容超過上限");
                        reconcileMenu(menu,replacement);
                        [menu update];*shownItems=*liveItems;record[@"items"]=refreshed;diagnostics();continue;
                    }
                    if(![record[@"suspended"] boolValue])break;
                    if(idle && !idle()){record[@"guest_cancel_requested"]=@YES;break;}
                }catch(...){*idleError=std::current_exception();break;}
            }
            [timer invalidate];[idleTimer invalidate];[NSEvent removeMonitor:inputObserver];
            for(NSEvent* event in postedEvents)[automation_posted_events_ removeObject:event];
            record[@"idle_cycles"]=@(idleCycles);
            popup_stack_.pop_back();
            // AppKit 的 tracking loop 會消耗放開事件；以狀態事件同步，避免停用中的 owner 留下按鍵狀態。
            const auto nativeButtons=automation_?0U:static_cast<unsigned>([NSEvent pressedMouseButtons]);
            const std::uint32_t buttons=guest_mouse_buttons(nativeButtons);
            const auto nativeModifiers=automation_?static_cast<NSEventModifierFlags>(0):[NSEvent modifierFlags];
            const std::uint32_t modifiers=((nativeModifiers&NSEventModifierFlagShift)?1U:0U)|((nativeModifiers&NSEventModifierFlagControl)?2U:0U)|((nativeModifiers&NSEventModifierFlagOption)?4U:0U);
            for(auto& [id,window]:windows_){(void)id;if(window.view.window==ownerWindow && [window.view isKindOfClass:[X64ArmGuestView class]])((X64ArmGuestView*)window.view).buttons=buttons;}
            events_.push_back({owner,buttons,0,0x10004,{},modifiers});
            record[@"tracking"]=@NO;record[@"cancelled"]=@(!target.didSelect);target->changed={};target->popupChanged={};target->highlightChanged={};target->canChoose={};diagnostics();
            if(*idleError)std::rethrow_exception(*idleError);
            if(record[@"automation_error"])throw std::runtime_error([record[@"automation_error"] UTF8String]);
            return target.didSelect?std::optional<GuiMenuSelection>{{target.selected,target.selectedMenu,target.selectedPosition}}:std::nullopt;
        }
    }
    std::optional<std::int32_t> text_width(const GuiFont& font, const std::u16string& text) override {
        @autoreleasepool {
            NSFont* selected=guest_font(font);
            // 固定字寬文字採用與 ExtTextOut 相同的整數字格，避免長列因小數 advance 累積而漂移。
            const auto width=guest_text_width(font,selected,nsstring(text));
            return static_cast<std::int32_t>(std::clamp<CGFloat>(width,0,INT32_MAX));
        }
    }
    std::optional<std::vector<std::int32_t>> text_advances(const GuiFont& font, const std::u16string& text) override {
        @autoreleasepool {
            std::vector<std::int32_t> advances;advances.reserve(text.size());
            NSFont* selected=guest_font(font);NSAttributedString* attributed=[[NSAttributedString alloc] initWithString:nsstring(text) attributes:@{NSFontAttributeName:selected}];
            CTLineRef line=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);if(!line)return std::nullopt;
            const auto scale=font_width_scale(font,selected),base=average_advance(selected);const auto cell=guest_metrics(font).average_width;
            for(std::size_t i=0;i<text.size();++i){const auto offset=CTLineGetOffsetForStringIndex(line,static_cast<CFIndex>(i+1),nullptr);
                const auto width=selected.fixedPitch?std::round(offset/base)*cell:std::ceil(offset*scale);
                advances.push_back(static_cast<std::int32_t>(std::clamp<CGFloat>(width,0,INT32_MAX)));
            }
            CFRelease(line);return advances;
        }
    }
    std::vector<std::u16string> font_families(bool fixed_only) override {
        @autoreleasepool {
            std::vector<std::u16string> names;NSFontManager* manager=[NSFontManager sharedFontManager];
            for(NSString* family in [manager.availableFontFamilies sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)]){
                if(!family.length || family.length>=32)continue;
                NSFont* font=[manager fontWithFamily:family traits:0 weight:5 size:13];
                if(font && (!fixed_only || font.fixedPitch))names.push_back(gueststring(family));
            }
            return names;
        }
    }
    std::optional<std::u16string> clipboard_text() override {
        @autoreleasepool {
            NSString* text=[guest_pasteboard() stringForType:NSPasteboardTypeString];
            if(!text || text.length>1048576)return std::nullopt;
            return gueststring(text);
        }
    }
    bool set_clipboard_text(const std::u16string& text) override {
        @autoreleasepool {
            if(text.size()>1048576)return false;
            NSPasteboard* pasteboard=guest_pasteboard();[pasteboard clearContents];
            return [pasteboard setString:nsstring(text) forType:NSPasteboardTypeString];
        }
    }
    std::optional<std::uint64_t> clipboard_sequence() override { @autoreleasepool {return static_cast<std::uint64_t>(guest_pasteboard().changeCount);} }
    bool clear_clipboard() override { @autoreleasepool { [guest_pasteboard() clearContents]; return true; } }
    std::optional<std::array<std::int32_t,2>> cursor_position() override {
        if(automation_)return automation_cursor_;
        @autoreleasepool {
            const NSPoint screen=[NSEvent mouseLocation];
            // Win32 使用客體的虛擬螢幕原點；轉成目前原生視窗的 client 座標再加回客體位置。
            const Window* target=nullptr;
            for(const auto& [handle,window]:windows_){(void)handle;if(!window.native || !window.native.visible)continue;if(!target || window.native==NSApp.keyWindow)target=&window;if(window.native==NSApp.keyWindow)break;}
            if(target){const auto& window=*target;
                const auto point=[window.view convertPoint:[window.native convertPointFromScreen:screen] fromView:nil];
                const auto inset=gui_frame_insets(window.model);return std::array<std::int32_t,2>{static_cast<std::int32_t>(std::clamp<CGFloat>(point.x+window.model.x+inset.left,INT32_MIN,INT32_MAX)),static_cast<std::int32_t>(std::clamp<CGFloat>(point.y+window.model.y+inset.top,INT32_MIN,INT32_MAX))};
            }
            const auto height=NSScreen.screens.firstObject.frame.size.height;
            return std::array<std::int32_t,2>{static_cast<std::int32_t>(screen.x),static_cast<std::int32_t>(height-screen.y)};
        }
    }
    bool set_cursor_position(std::int32_t x,std::int32_t y) override {
        if(automation_){automation_cursor_={x,y};return true;}
        @autoreleasepool {
            const Window* target=nullptr;for(const auto& [handle,window]:windows_){(void)handle;if(!window.native || !window.native.visible)continue;if(!target || window.native==NSApp.keyWindow)target=&window;if(window.native==NSApp.keyWindow)break;}
            const auto height=NSScreen.screens.firstObject.frame.size.height;NSPoint point=NSMakePoint(x,height-y);
            if(target){const auto inset=gui_frame_insets(target->model);const auto local=NSMakePoint(static_cast<CGFloat>(x)-target->model.x-inset.left,static_cast<CGFloat>(y)-target->model.y-inset.top);
                point=[target->native convertPointToScreen:[target->view convertPoint:local toView:nil]];}
            return CGWarpMouseCursorPosition(CGPointMake(point.x,height-point.y))==kCGErrorSuccess;
        }
    }
    std::optional<bool> physical_key_state(unsigned key) override {
        if(automation_)return {};
        if(key==1 || key==2 || key==4 || key==5 || key==6){const auto button=key==1?0:key==2?1:key-2;return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,static_cast<CGMouseButton>(button));}
        const auto down=[](CGKeyCode code){return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState,code);};
        if(key==16)return down(56)||down(60);if(key==17)return down(59)||down(62);if(key==18)return down(58)||down(61);
        // 與客體固定 en-US HKL 相同，映射 Apple ANSI 位置至 Windows VK；同一 VK 可有多個實體鍵。
        static constexpr std::array<unsigned,128> virtual_keys{
            'A','S','D','F','H','G','Z','X','C','V',0xe2,'B','Q','W','E','R',
            'Y','T','1','2','3','4','6','5',0xbb,'9','7',0xbd,'8','0',0xdd,'O',
            'U',0xdb,'I','P',13,'L','J',0xde,'K',0xba,0xdc,0xbc,0xbf,'N','M',0xbe,
            9,32,0xc0,8,0,27,0x5c,0x5b,0xa0,20,0xa4,0xa2,0xa1,0xa5,0xa3,0,
            0x80,0x6e,0,0x6a,0,0x6b,0,12,0xaf,0xae,0xad,0x6f,13,0,0x6d,0x81,
            0x82,0xbb,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x83,0x68,0x69,0xdc,0xe2,0,
            0x74,0x75,0x76,0x72,0x77,0x78,0,0x7a,0,0x7c,0x7f,0x7d,0,0x79,0x5d,0x7b,
            0,0x7e,45,36,33,46,0x73,35,0x71,34,0x70,37,39,40,38,0};
        if(!key || key>=256)return false;for(unsigned code=0;code<virtual_keys.size();++code)if(virtual_keys[code]==key && down(static_cast<CGKeyCode>(code)))return true;return false;
    }
    std::optional<std::uint64_t> input_idle_milliseconds() override {
        if(automation_)return {};const auto seconds=CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState,kCGAnyInputEventType);
        if(!std::isfinite(seconds) || seconds<0 || seconds>=static_cast<double>(UINT64_MAX/1000))return {};return static_cast<std::uint64_t>(seconds*1000);
    }
    bool set_cursor(const GuiIcon* icon,bool visible) override {
        @autoreleasepool {
            NSCursor* next=nil;
            if(icon){if(!icon->system){NSImage* image=list_icon(*icon);if(!image)return false;next=[[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(icon->hotspot_x,icon->hotspot_y)];}
                else {NSString* symbol=nil;switch(icon->system){
                    case 32512:next=NSCursor.arrowCursor;break;case 32513:next=NSCursor.IBeamCursor;break;case 32515:next=NSCursor.crosshairCursor;break;
                    case 32642:next=NSCursor.resizeLeftRightCursor;break;case 32643:next=NSCursor.resizeUpDownCursor;break;case 32645:next=NSCursor.operationNotAllowedCursor;break;case 32649:next=NSCursor.pointingHandCursor;break;
                    case 32514:case 32650:symbol=@"hourglass";break;case 32516:symbol=@"arrow.up";break;case 32640:symbol=@"arrow.up.left.and.arrow.down.right";break;case 32641:symbol=@"arrow.up.right.and.arrow.down.left";break;case 32644:symbol=@"arrow.up.and.down.and.arrow.left.and.right";break;case 32651:symbol=@"questionmark";break;default:return false;}
                    if(symbol){NSImage* image=[[NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil] copy];if(!image)return false;image.size=NSMakeSize(24,24);next=[[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(12,12)];}
                }
                if(!next)return false;
            }
            cursor_=next;for(auto& [handle,window]:windows_){(void)handle;if([window.view isKindOfClass:X64ArmGuestView.class]){((X64ArmGuestView*)window.view).guestCursor=next;[window.view.window invalidateCursorRectsForView:window.view];}}
            if(next)[next set];if(visible && cursor_hidden_){[NSCursor unhide];cursor_hidden_=false;}else if(!visible && !cursor_hidden_){[NSCursor hide];cursor_hidden_=true;}return true;
        }
    }
    void beep() override { @autoreleasepool { NSBeep(); } }
    bool accessibility_event(std::uint64_t handle,std::uint32_t event,std::uint32_t property) override {
        const auto found=windows_.find(handle);if(found==windows_.end())return false;
        NSView* view=found->second.view;if(!view)return false;
        NSString* notification=event==20005?NSAccessibilityFocusedUIElementChangedNotification:
            event==20002?NSAccessibilityLayoutChangedNotification:
            property==30005?NSAccessibilityTitleChangedNotification:NSAccessibilityValueChangedNotification;
        NSAccessibilityPostNotification(view,notification);return true;
    }
    bool accessibility_announcement(std::uint64_t handle,std::u16string_view text,bool important) override {
        const auto found=windows_.find(handle);if(found==windows_.end())return false;
        NSView* view=found->second.view;if(!view)return false;
        @autoreleasepool {
            NSString* message=[[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(text.data()) length:text.size()];
            NSDictionary* info=@{NSAccessibilityAnnouncementKey:message,NSAccessibilityPriorityKey:@(important?NSAccessibilityPriorityHigh:NSAccessibilityPriorityMedium)};
            NSAccessibilityPostNotificationWithUserInfo(view,NSAccessibilityAnnouncementRequestedNotification,info);
        }
        return true;
    }
    void destroy(std::uint64_t handle) override {
        if(fullscreen_window_==handle)(void)display_fullscreen(handle,false);
        if(pending_activation_==handle)pending_activation_=0;
        graphics_surfaces_.erase(handle);
        std::erase(z_order_,handle);
        @autoreleasepool {const auto found=windows_.find(handle);if(found==windows_.end())return;auto& w=found->second;
            if(w.alias){windows_.erase(found);return;}w.target.updating=YES;
            if(mouse_target_==w.view || [mouse_target_ isDescendantOf:w.view])mouse_target_=nil;
            if([w.view isKindOfClass:X64ArmHeaderView.class]){auto* header=(X64ArmHeaderView*)w.view;header.model=nullptr;header.events=nullptr;header.guestHandle=0;}
            if([w.control isKindOfClass:[X64ArmGuestButton class]]){auto* button=(X64ArmGuestButton*)w.control;button.enabled=NO;button.model=nullptr;button.events=nullptr;button.guestHandle=0;button.target=nil;button.action=nullptr;}
            if([w.control isKindOfClass:[NSTableView class]]){[(NSTableView*)w.control setDataSource:nil];[(NSTableView*)w.control setDelegate:nil];}
            [w.mdi_frame removeFromSuperview];[w.view removeFromSuperview];[w.native.parentWindow removeChildWindow:w.native];w.native.commandShortcut=nil;w.native.guestModel=nullptr;w.native.delegate=nil;[w.native close];windows_.erase(found);dirty_=true;
        }
    }
    bool input_active() const override {return NSApp.isActive || automation_input_active_;}
    bool status_icons(const std::map<std::uint64_t,desktop::Icon>& icons)override{return desktop_notifications_->update(icons,desktop_time());}
    std::uint32_t help(const NativeHelpRequest& request)override{
        if(!help_)help_=make_native_winhelp();
        const auto found=windows_.find(request.owner);void* anchor=found==windows_.end()?nullptr:(__bridge void*)found->second.view;
        return help_->execute(request,anchor);
    }
    bool status_focus(std::uint64_t token)override{return desktop_notifications_->focus(token);}
    bool attention(std::uint64_t owner,bool caption,bool tray)override{
        const auto found=windows_.find(owner);return found!=windows_.end() && desktop_notifications_->attention(owner,(__bridge void*)found->second.view.window,caption,tray);
    }
    std::optional<std::uint32_t> notification_state()const override{return desktop_notifications_->state();}
    bool ime_mark(std::uint64_t handle,const GuiImeData& data,std::array<std::int32_t,2> point) override {
        const auto found=windows_.find(handle);if(found==windows_.end())return false;NSView* view=found->second.view;
        if([view isKindOfClass:[X64ArmGuestView class]]){auto client=(X64ArmGuestView*)view;client->imeSilent=YES;client->imeFromInputMethod=NO;client->imeData=data;client->imeSelection=NSMakeRange(std::min<std::size_t>(data.cursor,data.text.size()),0);client->imePoint=NSMakePoint(point[0],point[1]);[client updateImeOverlay];[client.inputContext invalidateCharacterCoordinates];if(!data.composing)[client.inputContext discardMarkedText];client->imeSilent=NO;return true;}
        auto editor=found->second.editor;if(!editor && [view isKindOfClass:[NSTextField class]])editor=(NSTextView*)[view.window fieldEditor:NO forObject:view];
        if(editor){auto target=found->second.target;const auto previous=target.updating;target.updating=YES;
            if(data.composing)[editor setMarkedText:nsstring(data.text) selectedRange:NSMakeRange(std::min<std::size_t>(data.cursor,data.text.size()),0) replacementRange:NSMakeRange(NSNotFound,0)];
            else if(editor.hasMarkedText){[editor insertText:@"" replacementRange:editor.markedRange];target.imeText=nil;}
            target.updating=previous;return true;
        }
        return false;
    }
    bool ime_convert(std::uint64_t handle) override {
        const auto found=windows_.find(handle);if(found==windows_.end() || ![found->second.view isKindOfClass:[X64ArmGuestView class]])return false;
        const auto methods=native_input_methods();if(std::none_of(methods.begin(),methods.end(),[](const auto& item){return item.current;}))return false;
        auto client=(X64ArmGuestView*)found->second.view;if(!client.hasMarkedText || !client->imeFromInputMethod)return false;
        auto event=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:[NSDate timeIntervalSinceReferenceDate] windowNumber:client.window.windowNumber context:nil characters:@" " charactersIgnoringModifiers:@" " isARepeat:NO keyCode:49];
        return [client.inputContext handleEvent:event];
    }
    void raw_input_policy(bool keyboard,bool mouse,bool noactivate) override {raw_keyboard_=keyboard;raw_mouse_=mouse;raw_noactivate_=noactivate;}
    std::vector<GuiMessage> poll(bool wait) override {
        return poll_for(wait?50000000:0);
    }
    std::vector<GuiMessage> poll_for(std::uint64_t nanoseconds) override {
        const bool wait=nanoseconds!=0;
        @autoreleasepool {
            // 第一次最多等待呼叫者的期限；已有待送訊息時不再額外睡眠。
            // 依原順序排空一小批事件，避免每個按鍵／滑鼠事件都重做更新與渲染輪詢。
            auto batch_deadline=std::chrono::steady_clock::time_point::max();
            for(unsigned count=0;count<64;++count){
            NSEvent* event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:(count==0 && events_.empty() && wait)?[NSDate dateWithTimeIntervalSinceNow:static_cast<double>(nanoseconds)*1e-9]:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
            if(!event)break;
            if(!count)batch_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(4);
            {const auto type=event.type;const bool keyboard=type==NSEventTypeKeyDown || type==NSEventTypeKeyUp || type==NSEventTypeFlagsChanged;
                const bool mouse=type==NSEventTypeLeftMouseDown || type==NSEventTypeLeftMouseUp || type==NSEventTypeRightMouseDown || type==NSEventTypeRightMouseUp || type==NSEventTypeOtherMouseDown || type==NSEventTypeOtherMouseUp || type==NSEventTypeMouseMoved || type==NSEventTypeLeftMouseDragged || type==NSEventTypeRightMouseDragged || type==NSEventTypeOtherMouseDragged || type==NSEventTypeScrollWheel;
                const bool guest=event.window && std::any_of(windows_.begin(),windows_.end(),[&](const auto& item){return item.second.native==event.window;});
                bool delivered=false;
                const bool down=type==NSEventTypeLeftMouseDown || type==NSEventTypeRightMouseDown || type==NSEventTypeOtherMouseDown;
                if(guest && down)mouse_target_=nil;
                // 背景只限制啟動時的顯示與程式要求；使用者親自點擊後仍須能編輯及開啟 modal。
                // 自動化隔離中的事件不授予啟用資格，也不改變預先選定的螢幕。
                if(guest && down && !automation_input_monitor_ && !raw_noactivate_){
                    const auto owner=std::find_if(windows_.begin(),windows_.end(),[&](const auto& entry){return entry.second.native==event.window;});
                    if(owner!=windows_.end() && owner->second.model.enabled && !(owner->second.model.ex_style&0x08000000U)){interactive_activation_=true;dirty_=true;}
                }
                // local monitor 由 NSApp.sendEvent 觸發；直接轉交 GuestView 也須遵守隔離。
                if(guest && mouse && !automation_input_monitor_ && !raw_mouse_ && type!=NSEventTypeScrollWheel){
                    const bool tracking=type==NSEventTypeLeftMouseDragged || type==NSEventTypeRightMouseDragged || type==NSEventTypeOtherMouseDragged || type==NSEventTypeLeftMouseUp || type==NSEventTypeRightMouseUp || type==NSEventTypeOtherMouseUp;
                    NSView* target=tracking && mouse_target_?mouse_target_:native_window_hit(event.window,event.locationInWindow);
                    if([target isKindOfClass:[X64ArmGuestView class]] || [target isKindOfClass:[X64ArmHeaderView class]]){
                        auto* view=target;if(down)mouse_target_=view;
                        if(type==NSEventTypeLeftMouseDown)[view mouseDown:event];else if(type==NSEventTypeLeftMouseUp)[view mouseUp:event];else if(type==NSEventTypeLeftMouseDragged)[view mouseDragged:event];
                        else if(type==NSEventTypeRightMouseDown)[view rightMouseDown:event];else if(type==NSEventTypeRightMouseUp)[view rightMouseUp:event];else if(type==NSEventTypeRightMouseDragged)[view rightMouseDragged:event];
                        else if(type==NSEventTypeOtherMouseDown)[view otherMouseDown:event];else if(type==NSEventTypeOtherMouseUp)[view otherMouseUp:event];else if(type==NSEventTypeOtherMouseDragged)[view otherMouseDragged:event];else if(type==NSEventTypeMouseMoved)[view mouseMoved:event];
                        delivered=true;if(type==NSEventTypeLeftMouseUp || type==NSEventTypeRightMouseUp || type==NSEventTypeOtherMouseUp)mouse_target_=nil;
                    }else {
                        if(down && interactive_activation_ && !raw_noactivate_){
                            const auto owner=std::find_if(windows_.begin(),windows_.end(),[&](const auto& entry){return entry.second.native==event.window;});
                            if(owner!=windows_.end() && owner->second.model.enabled && !(owner->second.model.ex_style&0x08000000U))activate(owner->first);
                        }
                        const unsigned message=type==NSEventTypeLeftMouseDown?0x201U:type==NSEventTypeLeftMouseUp?0x202U:type==NSEventTypeRightMouseDown?0x204U:type==NSEventTypeRightMouseUp?0x205U:type==NSEventTypeOtherMouseDown?0x207U:type==NSEventTypeOtherMouseUp?0x208U:0x200U;
                        // 原生欄位／按鈕仍由 AppKit 操作；另外送共用 Tooltip relay，避免重複 click。
                        for(NSView* view=target;view;view=view.superview){
                            const auto owner=std::find_if(windows_.begin(),windows_.end(),[&](const auto& entry){return !entry.second.alias && entry.second.view==view;});
                            if(owner==windows_.end())continue;
                            auto point=[view convertPoint:event.locationInWindow fromView:nil];if(!view.isFlipped)point.y=view.bounds.size.height-point.y;
                            events_.push_back({owner->first,message,static_cast<std::uint64_t>(coordinate_word(point.x))|(static_cast<std::uint64_t>(coordinate_word(point.y))<<16),0x10012});break;
                        }
                    }
                }
                if(!delivered && (!guest || !((keyboard && raw_keyboard_) || (mouse && raw_mouse_))))[NSApp sendEvent:event];
                else if(!delivered && mouse && !automation_input_monitor_ && !raw_noactivate_ && !background_display_enabled() && (type==NSEventTypeLeftMouseDown || type==NSEventTypeRightMouseDown || type==NSEventTypeOtherMouseDown)){if(@available(macOS 14.0,*))[NSApp activate];else [NSApp activateIgnoringOtherApps:YES];[event.window makeKeyAndOrderFront:nil];}
                if(guest && (type==NSEventTypeLeftMouseUp || type==NSEventTypeRightMouseUp || type==NSEventTypeOtherMouseUp))mouse_target_=nil;
            }
            if(std::chrono::steady_clock::now()>=batch_deadline)break;
            }
            [NSApp updateWindows];
            if(pending_activation_ && NSApp.active){
                const auto handle=pending_activation_;pending_activation_=0;
                const auto found=windows_.find(handle);
                if(found!=windows_.end() && found->second.native.visible && found->second.model.enabled && !(found->second.model.ex_style&0x08000000U)){
                    auto& target=found->second;target.delegate.updating=YES;[target.native makeKeyAndOrderFront:nil];target.delegate.updating=NO;apply_z_order(handle);dirty_=true;
                }
            }
            for(const auto& notification:desktop_notifications_->poll(desktop_time())){GuiMessage message;message.status_icon=notification;events_.push_back(std::move(message));}
            if(!snapshot_.empty() && !graphics_surfaces_.empty() && (!graphics_snapshot_time_ || -[graphics_snapshot_time_ timeIntervalSinceNow]>=0.5)){dirty_=true;graphics_snapshot_time_=[NSDate date];}
            if(dirty_){
                for(auto& [h,w]:windows_){if(!w.native || !w.native.visible)continue;
                    try{snapshot(w.view,snapshot_);}catch(const std::exception& error){
                        std::fprintf(stderr,"[gui] 自動快照失敗 window=%llu width=%.0f height=%.0f：%s\n",static_cast<unsigned long long>(h),static_cast<double>(w.view.bounds.size.width),static_cast<double>(w.view.bounds.size.height),error.what());
                    }
                }
                diagnostics();dirty_=false;
            }
            if(!native_snapshot_directory_.empty() && (!native_snapshot_time_ || -native_snapshot_time_.timeIntervalSinceNow>=10)){
                native_snapshot_time_=[NSDate date];
                std::vector<NativeSnapshotRequest> requests;
                for(auto& [h,w]:windows_)if(w.native && w.native.visible && !w.native.miniaturized){
                    const auto width=std::ceil(w.native.frame.size.width),height=std::ceil(w.native.frame.size.height);
                    if(!std::isfinite(width) || !std::isfinite(height) || width<1 || height<1 || width>16384 || height>16384)continue;
                    const auto path=native_snapshot_directory_/("native-"+std::to_string(::getpid())+"-"+std::to_string(h)+".png");
                    requests.push_back({static_cast<CGWindowID>(w.native.windowNumber),static_cast<std::size_t>(width),static_cast<std::size_t>(height),path});
                }
                capture_native_windows_async(native_snapshot_batch_,requests);
            }
            if(wait || automation_live_path_ || (action_<automation_.count && [automation_[action_][@"nonblocking"] boolValue]))automate();
            if(const auto& timing=macos_detail::gui_timing();timing)for(const auto& queued:events_)
                if((queued.message>=0x100 && queued.message<=0x109) || (queued.message>=0x200 && queued.message<=0x20e))
                    timing->input(queued.window,queued.message);
            auto result=std::move(events_);events_.clear();return result;
        }
    }
};
}
std::optional<GuiBitmap> render_gui_header_image(const GuiWindow& window,std::optional<std::size_t> item,const GuiHeaderRenderStyle& style){
    @autoreleasepool {
        if(!window.header || (item && *item>=window.columns.size()))return {};
        GuiWindow model=window;
        if(item){
            const auto index=*item;const auto column=window.columns[index];model.width=static_cast<std::uint32_t>(std::max(0,column.width));model.columns={column};
            model.header->rectangles={{0,0,static_cast<std::int32_t>(model.width),static_cast<std::int32_t>(model.height)}};
            model.header->overflow_item=-1;model.header->overflow_pressed=false;model.style&=~0x1000U;
            model.header->bitmap_images=index<window.header->bitmap_images.size()?std::vector<GuiIcon>{window.header->bitmap_images[index]}:std::vector<GuiIcon>{};
        }
        if(!model.width || !model.height || model.width>16384 || model.height>16384 || static_cast<std::uint64_t>(model.width)*model.height>4194304)return {};
        // 離屏畫面沿用原生 Header 的字型、RTL、文字與圖示；不取得宿主焦點。
        if(!NSApp){prepare_native_application();[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];}
        auto* view=[[X64ArmHeaderView alloc]initWithFrame:NSMakeRect(0,0,model.width,model.height)];view.model=&model;view.paintStyle=&style;
        auto* bitmap=[[NSBitmapImageRep alloc]initWithBitmapDataPlanes:nullptr pixelsWide:model.width pixelsHigh:model.height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:0 bytesPerRow:static_cast<NSInteger>(model.width)*4 bitsPerPixel:32];
        if(!bitmap)return {};std::memset(bitmap.bitmapData,0,static_cast<std::size_t>(bitmap.bytesPerRow)*model.height);
        // CoreGraphics 的繪圖 context 使用 premultiplied alpha；非預乘格式只能作為影像資料。
        auto* bitmapContext=[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];if(!bitmapContext || !bitmapContext.CGContext)return {};
        [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithCGContext:bitmapContext.CGContext flipped:YES]];
        NSAffineTransform* transform=[NSAffineTransform transform];[transform translateXBy:0 yBy:model.height];[transform scaleXBy:1 yBy:-1];[transform concat];[view drawRect:view.bounds];[NSGraphicsContext restoreGraphicsState];view.model=nullptr;
        GuiBitmap result{model.width,model.height,{}};result.pixels.resize(static_cast<std::size_t>(model.width)*model.height);
        for(unsigned y=0;y<model.height;++y)for(unsigned x=0;x<model.width;++x){NSColor* color=[[bitmap colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];if(!color)return {};
            const auto channel=[](CGFloat value){return static_cast<std::uint32_t>(std::clamp(std::lround(value*255),0L,255L));};result.pixels[static_cast<std::size_t>(y)*model.width+x]=channel(color.redComponent)|(channel(color.greenComponent)<<8)|(channel(color.blueComponent)<<16)|(channel(color.alphaComponent)<<24);
        }
        return result;
    }
}

namespace {
// 延遲建立原生 GUI 後端。[NSApplication sharedApplication]（連線 WindowServer、初始化 AppKit）每個程序約 0.12–0.23 秒，
// 是子程序啟動到客體開始執行之間最大的固定成本；沒有視窗的輔助程序（crashpad、renderer 等）不一定會用到 AppKit。
// 代理在第一次呼叫任何原生 GUI 方法時才於同一執行緒建立 CocoaGui，之後全部轉發，結果與立即建立相同。
// App Nap 防護不需要 AppKit，仍在程序啟動時開始，避免無視窗程序被節流而延誤跨程序回應。
// 原始輸入政策先記錄，建立後端時補送。X64ARM_GUI_DEFERRED=0 恢復立即建立；
// X64ARM_TRACE_GUI_DEFERRED=1 於建立時印出觸發的方法與時間。
class DeferredCocoaGui final : public NativeGui {
public:
    explicit DeferredCocoaGui(std::filesystem::path snapshot):snapshot_(std::move(snapshot)){
        @autoreleasepool {
            if(const char* value=std::getenv("X64ARM_GUI_ACTIVITY");!value || std::string_view(value)!="0")
                activity_=[NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"執行使用者啟動的 Windows 互動程式"];
        }
        // sharedApplication 的一部分是建立 WindowServer 連線；CoreGraphics 的顯示與工作階段查詢執行緒安全，
        // 先在背景建立，客體開始執行與第一次使用 GUI 之間的空檔即可完成。主執行緒稍後初始化 AppKit 時沿用同一連線；
        // 兩者同時進行由 CoreGraphics 內部協調。X64ARM_GUI_PREWARM=0 停用。
        static const bool prewarm=[] {const auto* value=std::getenv("X64ARM_GUI_PREWARM");return !value || std::string_view(value)!="0";}();
        if(prewarm)try{
            prewarm_=std::thread([]{@autoreleasepool {
                startup_profile::Scope measurement("gui_init","prewarm_graphics");
                const auto display=CGMainDisplayID();(void)CGDisplayBounds(display);
                if(const auto session=CGSessionCopyCurrentDictionary())CFRelease(session);
            }});
        }catch(const std::system_error&){}
    }
    ~DeferredCocoaGui() override {if(prewarm_.joinable())prewarm_.join();real_.reset();@autoreleasepool {if(activity_)[NSProcessInfo.processInfo endActivity:activity_];}}
    void create(std::uint64_t handle,const GuiWindow& window) override {real("create").create(handle,window);}
    void show(std::uint64_t handle,bool visible) override {real("show").show(handle,visible);}
    void reorder(std::uint64_t a,std::uint64_t b) override {real("reorder").reorder(a,b);}
    void activate(std::uint64_t handle) override {real("activate").activate(handle);}
    void focus(std::uint64_t handle) override {real("focus").focus(handle);}
    bool accessibility_event(std::uint64_t handle,std::uint32_t event,std::uint32_t child) override {return real("accessibility_event").accessibility_event(handle,event,child);}
    bool accessibility_announcement(std::uint64_t handle,std::u16string_view text,bool important) override {return real("accessibility_announcement").accessibility_announcement(handle,text,important);}
    std::array<std::int32_t,4> work_area() override {return real("work_area").work_area();}
    std::array<std::int32_t,4> screen_area() override {return real("screen_area").screen_area();}
    std::vector<GuiDisplayMode> display_modes() override {return real("display_modes").display_modes();}
    std::optional<GuiDisplayMode> current_display_mode() override {return real("current_display_mode").current_display_mode();}
    bool display_fullscreen(std::uint64_t handle,bool enable,const std::optional<GuiDisplayMode>& mode) override {return real("display_fullscreen").display_fullscreen(handle,enable,mode);}
    std::optional<shell::Dock> shell_dock() override {return real("shell_dock").shell_dock();}
    std::uint32_t shell_reveal(std::span<const std::shared_ptr<HostFile>> files,std::uint32_t flags) override {return real("shell_reveal").shell_reveal(files,flags);}
    std::vector<shell::Application> shell_applications(const std::shared_ptr<HostFile>& file,std::string_view verb) override {return real("shell_applications").shell_applications(file,verb);}
    std::uint32_t shell_open(const std::shared_ptr<HostFile>& file,std::string_view verb,const shell::Application& application) override {return real("shell_open").shell_open(file,verb,application);}
    std::optional<shell::Application> shell_default_application(const std::shared_ptr<HostFile>& file,std::string_view verb) override {return real("shell_default_application").shell_default_application(file,verb);}
    bool set_opacity(std::uint64_t handle,std::uint8_t alpha) override {return real("set_opacity").set_opacity(handle,alpha);}
    bool set_region(std::uint64_t handle,const std::optional<GuiRegion>& region) override {return real("set_region").set_region(handle,region);}
    void present(std::uint64_t handle,const GuiWindow& window) override {real("present").present(handle,window);}
    void destroy(std::uint64_t handle) override {real("destroy").destroy(handle);}
    std::vector<GuiMessage> poll(bool wait) override {return real("poll").poll(wait);}
    // 排程器每輪都問前景狀態；沒有建立過視窗的程序（steamsysinfo、driver query、utility 子程序）不該為此
    // 初始化 AppKit（NSApplication 建立會同步向 tccd 查詢並初始化 safe aperture，負載下數百 ms 到數秒）。
    // 尚未建立真正 GUI 就表示沒有可成為前景的視窗。
    bool input_active() const override {return real_ && real_->input_active();}
    bool status_icons(const std::map<std::uint64_t,desktop::Icon>& icons) override {return real("status_icons").status_icons(icons);}
    std::uint32_t help(const NativeHelpRequest& request) override {return real("help").help(request);}
    bool status_focus(std::uint64_t handle) override {return real("status_focus").status_focus(handle);}
    bool attention(std::uint64_t handle,bool flash,bool request) override {return real("attention").attention(handle,flash,request);}
    std::optional<std::uint32_t> notification_state() const override {return real("notification_state").notification_state();}
    bool ime_mark(std::uint64_t handle,const GuiImeData& data,std::array<std::int32_t,2> caret) override {return real("ime_mark").ime_mark(handle,data,caret);}
    bool ime_convert(std::uint64_t handle) override {return real("ime_convert").ime_convert(handle);}
    void raw_input_policy(bool keyboard,bool mouse,bool sink) override {
        if(real_)real_->raw_input_policy(keyboard,mouse,sink);else raw_policy_={keyboard,mouse,sink};
    }
    std::vector<GuiMessage> poll_for(std::uint64_t nanoseconds) override {return real("poll_for").poll_for(nanoseconds);}
    std::optional<GuiFontMetrics> font_metrics(const GuiFont& font) override {return real("font_metrics").font_metrics(font);}
    std::optional<std::int32_t> text_width(const GuiFont& font,const std::u16string& text) override {return real("text_width").text_width(font,text);}
    std::optional<std::vector<std::int32_t>> text_advances(const GuiFont& font,const std::u16string& text) override {return real("text_advances").text_advances(font,text);}
    std::vector<std::u16string> font_families(bool all) override {return real("font_families").font_families(all);}
    std::optional<std::u16string> clipboard_text() override {return real("clipboard_text").clipboard_text();}
    bool set_clipboard_text(const std::u16string& text) override {return real("set_clipboard_text").set_clipboard_text(text);}
    std::optional<std::uint64_t> clipboard_sequence() override {return real("clipboard_sequence").clipboard_sequence();}
    bool clear_clipboard() override {return real("clear_clipboard").clear_clipboard();}
    std::optional<std::array<std::int32_t,2>> cursor_position() override {return real("cursor_position").cursor_position();}
    bool set_cursor_position(std::int32_t x,std::int32_t y) override {return real("set_cursor_position").set_cursor_position(x,y);}
    bool set_cursor(const GuiIcon* icon,bool visible) override {return real("set_cursor").set_cursor(icon,visible);}
    std::optional<bool> physical_key_state(unsigned key) override {return real("physical_key_state").physical_key_state(key);}
    std::optional<std::uint64_t> input_idle_milliseconds() override {return real("input_idle_milliseconds").input_idle_milliseconds();}
    std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags) override {
        return real("popup_menu").popup_menu(owner,items,x,y,flags);
    }
    std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle) override {
        return real("popup_menu").popup_menu(owner,items,x,y,flags,idle);
    }
    std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle,const std::function<bool(std::uint64_t,std::uint32_t,bool)>& popup) override {
        return real("popup_menu").popup_menu(owner,items,x,y,flags,idle,popup);
    }
    std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle,const std::function<bool(std::uint64_t,std::uint32_t,bool)>& popup,const std::function<bool(const GuiMenuHighlight&)>& highlight) override {
        return real("popup_menu").popup_menu(owner,items,x,y,flags,idle,popup,highlight);
    }
    void graphics_text(std::uint64_t handle,const GuiDraw& draw) override {real("graphics_text").graphics_text(handle,draw);}
    std::optional<GuiThemeMetrics> theme_metrics(std::u16string_view name,std::int32_t part,std::int32_t state,std::uint32_t property,std::array<std::int32_t,2> size) override {
        return real("theme_metrics").theme_metrics(name,part,state,property,size);
    }
    std::optional<GuiBitmap> theme_background(std::u16string_view name,std::int32_t part,std::int32_t state,std::uint32_t property,std::array<std::int32_t,2> size) override {
        return real("theme_background").theme_background(name,part,state,property,size);
    }
    std::optional<GuiBitmap> capture_client(std::uint64_t handle) override {return real("capture_client").capture_client(handle);}
    void graphics_surface(std::uint64_t handle,std::shared_ptr<RasterSurface> surface) override {real("graphics_surface").graphics_surface(handle,std::move(surface));}
    void* graphics_view(std::uint64_t handle) override {return real("graphics_view").graphics_view(handle);}
    std::optional<std::array<std::uint16_t,768>> gamma_ramp() override {return real("gamma_ramp").gamma_ramp();}
    bool set_gamma_ramp(std::span<const std::uint16_t,768> ramp) override {return real("set_gamma_ramp").set_gamma_ramp(ramp);}
    void beep() override {real("beep").beep();}
    void set_menu(std::uint64_t handle,const std::vector<GuiPopupItem>& items) override {real("set_menu").set_menu(handle,items);}
    std::optional<std::array<std::int32_t,4>> menu_rectangle(std::uint64_t handle,std::int32_t index) override {return real("menu_rectangle").menu_rectangle(handle,index);}
    std::optional<std::uint32_t> text_line_count(std::uint64_t handle) override {return real("text_line_count").text_line_count(handle);}
    bool webview_create(std::uint64_t id,std::uint64_t parent) override {return real("webview_create").webview_create(id,parent);}
    void webview_destroy(std::uint64_t id) override {real("webview_destroy").webview_destroy(id);}
    void webview_visible(std::uint64_t id,bool visible) override {real("webview_visible").webview_visible(id,visible);}
    void webview_bounds(std::uint64_t id,const std::array<std::int32_t,4>& bounds) override {real("webview_bounds").webview_bounds(id,bounds);}
    void webview_zoom(std::uint64_t id,double zoom) override {real("webview_zoom").webview_zoom(id,zoom);}
    void webview_focus(std::uint64_t id) override {real("webview_focus").webview_focus(id);}
    void webview_navigate(std::uint64_t id,const std::u16string& url) override {real("webview_navigate").webview_navigate(id,url);}
    void webview_load_html(std::uint64_t id,const std::u16string& html) override {real("webview_load_html").webview_load_html(id,html);}
    void webview_command(std::uint64_t id,unsigned command) override {real("webview_command").webview_command(id,command);}
    void webview_script(std::uint64_t id,const std::u16string& script) override {real("webview_script").webview_script(id,script);}
    void webview_user_script(std::uint64_t id,const std::u16string& script) override {real("webview_user_script").webview_user_script(id,script);}
    void webview_post_message(std::uint64_t id,const std::u16string& message,bool json) override {real("webview_post_message").webview_post_message(id,message,json);}
    std::optional<std::vector<std::array<std::uint32_t,2>>> text_line_ranges(std::uint64_t handle) override {return real("text_line_ranges").text_line_ranges(handle);}
private:
    NativeGui& real(const char* method) const {
        if(!real_){
            startup_profile::Scope measurement("startup","native_gui_deferred_init",true);
            real_=std::make_shared<CocoaGui>(snapshot_,false);
            if(raw_policy_)real_->raw_input_policy((*raw_policy_)[0],(*raw_policy_)[1],(*raw_policy_)[2]);
            static const bool trace=[] {const auto* value=std::getenv("X64ARM_TRACE_GUI_DEFERRED");return value && std::string_view(value)=="1";}();
            if(trace)std::fprintf(stderr,"gui_deferred_init pid=%d method=%s unix_us=%lld\n",static_cast<int>(::getpid()),method,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
        }
        return *real_;
    }
    std::filesystem::path snapshot_;
    mutable std::shared_ptr<NativeGui> real_;
    std::optional<std::array<bool,3>> raw_policy_;
    id activity_{};
    std::thread prewarm_;
};
bool native_gui_deferred(){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_GUI_DEFERRED");return !value || std::string_view(value)!="0";}();
    return enabled;
}
}
std::shared_ptr<NativeGui> make_native_gui(const std::filesystem::path& snapshot){
    if(native_gui_deferred())return std::make_shared<DeferredCocoaGui>(snapshot);
    return std::make_shared<CocoaGui>(snapshot);
}
}
