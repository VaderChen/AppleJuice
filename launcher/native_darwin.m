#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>
#import <CoreText/CoreText.h>
#include <termios.h>
#include <sys/sysctl.h>

uint64_t launcher_physical_memory(void) {
    return NSProcessInfo.processInfo.physicalMemory;
}

uint32_t launcher_physical_processors(void) {
    uint32_t count = 0;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.physicalcpu", &count, &size, NULL, 0) == 0 && count) return count;
    return 1;
}

char* launcher_windows_fonts(void) {
    @autoreleasepool {
        CTFontCollectionRef collection = CTFontCollectionCreateFromAvailableFonts(NULL);
        CFArrayRef descriptors = collection ? CTFontCollectionCreateMatchingFontDescriptors(collection) : NULL;
        NSMutableSet<NSString*>* families = [NSMutableSet set];
        for (id value in (__bridge NSArray*)descriptors) {
            CTFontRef font = CTFontCreateWithFontDescriptor((__bridge CTFontDescriptorRef)value, 16, NULL);
            if (!font) continue;
            CFNumberRef format = CTFontCopyAttribute(font, kCTFontFormatAttribute);
            int kind = 0;
            if (format) { CFNumberGetValue(format, kCFNumberIntType, &kind); CFRelease(format); }
            if (kind == kCTFontFormatOpenTypePostScript || kind == kCTFontFormatOpenTypeTrueType || kind == kCTFontFormatTrueType) {
                NSString* family = CFBridgingRelease(CTFontCopyFamilyName(font));
                if (family.length) [families addObject:family];
            }
            CFRelease(font);
        }
        if (descriptors) CFRelease(descriptors);
        if (collection) CFRelease(collection);
        NSData* data = [NSJSONSerialization dataWithJSONObject:families.allObjects options:0 error:nil];
        return data ? strdup([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String) : NULL;
    }
}

// 原生對話框與 WebView 使用同一個已解析語言。
static NSString *launcherLanguage = @"zh-Hant";
void launcher_language(const char *language) {
    NSString *value = language ? [NSString stringWithUTF8String:language] : @"zh-Hant";
    if (![@[@"zh-Hant", @"en", @"ja", @"ko"] containsObject:value]) return;
    if (NSThread.isMainThread) launcherLanguage = value;
    else dispatch_sync(dispatch_get_main_queue(), ^{ launcherLanguage = value; });
}
static NSString *localized(NSString *zh, NSString *en, NSString *ja, NSString *ko) {
    if ([launcherLanguage isEqualToString:@"en"]) return en;
    if ([launcherLanguage isEqualToString:@"ja"]) return ja;
    if ([launcherLanguage isEqualToString:@"ko"]) return ko;
    return zh;
}
static NSInteger runningCount=0;
static __weak NSWindow *launcherWindow;
static NSWindow *backgroundWindow;
static NSScreen *backgroundScreen;
// 先提供既有 NSWindow 給 WebView，避開套件自建視窗時的自動啟用。
void *launcher_background_window(void) {
    [NSApplication sharedApplication];
    NSArray<NSScreen *> *screens=NSScreen.screens;
    NSScreen *active=NSScreen.mainScreen?:screens.firstObject;
    pid_t pid=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    NSArray *windows=CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly|kCGWindowListExcludeDesktopElements,kCGNullWindowID));
    for(NSDictionary *row in windows) {
        if([row[(id)kCGWindowOwnerPID] intValue]!=pid || [row[(id)kCGWindowLayer] intValue]!=0)continue;
        CGRect bounds;
        if(!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)row[(id)kCGWindowBounds],&bounds))continue;
        CGFloat largest=0;
        for(NSScreen *screen in screens) {
            CGRect overlap=CGRectIntersection(bounds,CGDisplayBounds([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]));
            CGFloat area=CGRectIsNull(overlap)?0:overlap.size.width*overlap.size.height;
            if(area>largest){largest=area;active=screen;}
        }
        if(largest>0)break;
    }
    CGFloat largest=0;
    for(NSScreen *screen in screens) {
        if(screen==active)continue;
        CGFloat area=screen.visibleFrame.size.width*screen.visibleFrame.size.height;
        if(area>largest){largest=area;backgroundScreen=screen;}
    }
    if(!backgroundScreen)return NULL;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    backgroundWindow=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1180,800)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO screen:backgroundScreen];
    backgroundWindow.releasedWhenClosed=NO;
    fprintf(stderr,"launcher_background_display=%u active_display=%u no_activate=1\n",
        [backgroundScreen.deviceDescription[@"NSScreenNumber"] unsignedIntValue],
        [active.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
    return (__bridge void *)backgroundWindow;
}
void launcher_close_background_window(void) {
    backgroundWindow.delegate=nil;[backgroundWindow close];backgroundWindow=nil;backgroundScreen=nil;
}
uint32_t launcher_background_display_id(void) {
    return [backgroundScreen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}
static WKWebView *findWebView(NSView *view) {
    if([view isKindOfClass:WKWebView.class])return (WKWebView *)view;
    for(NSView *child in view.subviews){WKWebView *found=findWebView(child);if(found)return found;}
    return nil;
}
@interface LauncherWindowDelegate : NSObject<NSWindowDelegate>
@property(nonatomic,strong) id previous;
@end
@implementation LauncherWindowDelegate
- (void)windowWillClose:(NSNotification *)notification {
    if(notification.object==backgroundWindow) {
        [NSApp stop:nil];
        [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
            modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
    }
    if([self.previous respondsToSelector:_cmd])[self.previous windowWillClose:notification];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    if(runningCount>0){
        NSAlert *alert=[NSAlert new];alert.messageText=localized(@"結束執行中的程式？", @"Stop running apps?", @"実行中のアプリを終了しますか？", @"실행 중인 앱을 종료할까요?");
        alert.informativeText=[NSString stringWithFormat:localized(@"目前有 %ld 個程式仍在執行。離開啟動器時，這些程式也會結束。", @"%ld apps are still running. Quitting the launcher also stops these apps.", @"%ld 個のアプリが実行中です。ランチャーを終了すると、これらのアプリも終了します。", @"앱 %ld개가 실행 중입니다. 런처를 종료하면 이 앱들도 종료됩니다."),(long)runningCount];
        [alert addButtonWithTitle:localized(@"繼續使用", @"Keep using", @"使用を続ける", @"계속 사용")];[alert addButtonWithTitle:localized(@"結束並離開", @"Stop and quit", @"終了して閉じる", @"중지 후 종료")];
        if([alert runModal]!=NSAlertSecondButtonReturn)return NO;
    }
    if([self.previous respondsToSelector:_cmd])return [self.previous windowShouldClose:sender];
    return YES;
}
- (BOOL)respondsToSelector:(SEL)selector {return [super respondsToSelector:selector] || [self.previous respondsToSelector:selector];}
- (id)forwardingTargetForSelector:(SEL)selector {return self.previous;}
@end
@interface LauncherNavigationDelegate : NSObject<WKNavigationDelegate>
@property(nonatomic,strong) id previous;
@end
@implementation LauncherNavigationDelegate
- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    NSString *url=action.request.URL.absoluteString;
    // Go bridge 只供內嵌程式使用；拖放檔案或外部連結不能載入另一個頁面。
    handler(!url || [url isEqualToString:@"about:blank"] ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
- (BOOL)respondsToSelector:(SEL)selector {return [super respondsToSelector:selector] || [self.previous respondsToSelector:selector];}
- (id)forwardingTargetForSelector:(SEL)selector {return self.previous;}
@end

char *launcher_pick(int folder,int probe,const char *title,int single) {
    if(!NSThread.isMainThread){
        __block char *result=NULL;
        dispatch_sync(dispatch_get_main_queue(), ^{result=launcher_pick(folder,probe,title,single);});
        return result;
    }
    @autoreleasepool {
        // 檔案選擇器須由主執行緒顯示，並喚回被其他客體視窗蓋住的啟動器。
        NSWindow *owner=launcherWindow;
        [NSApp activateIgnoringOtherApps:YES];
        if(owner.miniaturized)[owner deminiaturize:nil];
        [owner makeKeyAndOrderFront:nil];
        NSOpenPanel *panel=[NSOpenPanel openPanel];panel.canChooseDirectories=folder;panel.canChooseFiles=!folder;panel.allowsMultipleSelection=!folder && !single;
        if (title && !strcmp(title,"選擇臨時執行的 EXE")) title = [localized(@"選擇臨時執行的 EXE", @"Choose EXE to run once", @"一時実行する EXE を選択", @"임시 실행할 EXE 선택") UTF8String];
        panel.title=title && *title?[NSString stringWithUTF8String:title]:(folder?localized(@"選擇資料夾", @"Choose folder", @"フォルダーを選択", @"폴더 선택"):localized(@"加入 Windows 程式", @"Add Windows apps", @"Windows アプリを追加", @"Windows 앱 추가"));panel.prompt=folder?localized(@"選擇", @"Choose", @"選択", @"선택"):single?localized(@"執行", @"Run", @"実行", @"실행"):localized(@"加入程式庫", @"Add to library", @"ライブラリに追加", @"라이브러리에 추가");
        panel.level=MAX(owner.level,NSModalPanelWindowLevel);
        panel.canCreateDirectories=folder;
        if(!folder)panel.allowedContentTypes=@[[UTType typeWithFilenameExtension:@"exe"]];
        __block BOOL appeared=NO;
        if(probe){NSTimer *timer=[NSTimer timerWithTimeInterval:0.3 repeats:NO block:^(NSTimer *t){(void)t;appeared=panel.visible;[panel cancel:nil];}];[[NSRunLoop mainRunLoop] addTimer:timer forMode:NSModalPanelRunLoopMode];}
        NSModalResponse result=[panel runModal];[owner makeKeyAndOrderFront:nil];NSMutableArray *paths=[NSMutableArray array];
        if(probe){if(appeared)[paths addObject:@"原生檔案選擇器已開啟並取消"];}
        else if(result==NSModalResponseOK){for(NSURL *url in panel.URLs)[paths addObject:url.path];}
        NSData *json=[NSJSONSerialization dataWithJSONObject:paths options:0 error:nil];
        return strdup([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
    }
}
void launcher_configure(void *pointer) {
    NSWindow *window=(__bridge NSWindow *)pointer;launcherWindow=window;
    [NSApp setActivationPolicy:backgroundScreen?NSApplicationActivationPolicyAccessory:NSApplicationActivationPolicyRegular];
    window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];window.backgroundColor=[NSColor colorWithWhite:0.97 alpha:1];
    LauncherWindowDelegate *delegate=[LauncherWindowDelegate new];delegate.previous=window.delegate;window.delegate=delegate;
    objc_setAssociatedObject(window,@selector(windowShouldClose:),delegate,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    WKWebView *web=findWebView(window.contentView);
    LauncherNavigationDelegate *navigation=[LauncherNavigationDelegate new];navigation.previous=web.navigationDelegate;web.navigationDelegate=navigation;
    objc_setAssociatedObject(window,@selector(navigationDelegate),navigation,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSMenu *menu=[NSMenu new];NSMenuItem *appItem=[NSMenuItem new];[menu addItem:appItem];NSMenu *appMenu=[NSMenu new];appItem.submenu=appMenu;
    NSMenuItem *quit=[[NSMenuItem alloc] initWithTitle:@"結束 Win64 Launcher" action:@selector(performClose:) keyEquivalent:@"q"];quit.target=window;[appMenu addItem:quit];
    NSMenuItem *edit=[NSMenuItem new];[menu addItem:edit];edit.submenu=[[NSMenu alloc] initWithTitle:@"編輯"];
    NSArray *labels=@[@"復原",@"重做",@"剪下",@"拷貝",@"貼上",@"全選"];
    NSArray *actions=@[@"undo:",@"redo:",@"cut:",@"copy:",@"paste:",@"selectAll:"];
    NSArray *keys=@[@"z",@"Z",@"x",@"c",@"v",@"a"];
    for(NSUInteger i=0;i<labels.count;i++)[edit.submenu addItemWithTitle:labels[i] action:NSSelectorFromString(actions[i]) keyEquivalent:keys[i]];
    NSApp.mainMenu=menu;
    if(backgroundScreen) {
        NSRect screen=backgroundScreen.visibleFrame,frame=window.frame;
        frame.size.width=MIN(frame.size.width,screen.size.width-40);
        frame.size.height=MIN(frame.size.height,screen.size.height-40);
        frame.origin=NSMakePoint(NSMidX(screen)-frame.size.width/2,NSMidY(screen)-frame.size.height/2);
        [window setFrame:frame display:YES];[window orderFront:nil];
    } else {[window center];[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}
}
void launcher_running(int count){runningCount=count;}
void launcher_snapshot(void *pointer,const char *path) {
    WKWebView *web=findWebView(((__bridge NSWindow *)pointer).contentView);NSString *destination=[NSString stringWithUTF8String:path];
    [web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *image,NSError *error){
        if(error || !image)return;
        NSBitmapImageRep *bitmap=[NSBitmapImageRep imageRepWithData:image.TIFFRepresentation];
        NSData *png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];[png writeToFile:destination atomically:YES];
    }];
}
int launcher_visible(int pid) {
    NSArray *windows=CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly,kCGNullWindowID));
    for(NSDictionary *row in windows){if([row[(id)kCGWindowOwnerPID] intValue]==pid && [row[(id)kCGWindowLayer] intValue]==0)return 1;}
    return 0;
}
void launcher_alert(const char *text){@autoreleasepool{NSAlert *alert=[NSAlert new];alert.messageText=@"Win64 Launcher";alert.informativeText=[NSString stringWithUTF8String:text];[alert runModal];}}
void launcher_reveal(const char *path){[[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]]];}
int launcher_pty_utf8(int fd){struct termios settings;if(tcgetattr(fd,&settings)!=0)return -1;settings.c_iflag|=IUTF8;return tcsetattr(fd,TCSANOW,&settings);}
