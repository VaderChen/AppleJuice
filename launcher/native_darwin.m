#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>
#import <CoreText/CoreText.h>
#include <termios.h>
#include <sys/sysctl.h>
#include <dlfcn.h>

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
// 正常啟動固定主螢幕；Smoke 僅使用主螢幕的第二個一般桌面，且不取得焦點。
static NSWindow *primaryWindow;
static BOOL testingWindow;
static BOOL normalStartupWindow;
static NSString *testingSpaceUUID;
static uint64_t primarySpaceID;
static int spaceConnection;
static void (*moveToSpace)(int,CFArrayRef,uint64_t);
static CFArrayRef (*copyWindowSpaces)(int,int,CFArrayRef);
static BOOL verifyPrimarySpace(NSWindow *window) {
    if (!primarySpaceID) return !testingWindow;
    NSArray *ids=@[@(window.windowNumber)];
    NSArray *membership=CFBridgingRelease(copyWindowSpaces(spaceConnection,7,(__bridge CFArrayRef)ids));
    if([membership isEqual:@[@(primarySpaceID)]])return YES;
    moveToSpace(spaceConnection,(__bridge CFArrayRef)ids,primarySpaceID);
    membership=CFBridgingRelease(copyWindowSpaces(spaceConnection,7,(__bridge CFArrayRef)ids));
    return [membership isEqual:@[@(primarySpaceID)]];
}
void *launcher_primary_window(int testing) {
    [NSApplication sharedApplication];
    testingWindow=(testing&1)!=0;normalStartupWindow=!testingWindow || (testing&2)!=0;testingSpaceUUID=nil;primarySpaceID=0;
    NSScreen *screen=NSScreen.screens.firstObject;
    if(!screen)return NULL;
    void *library=dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",RTLD_LAZY);
    int (*mainConnection)(void)=library?dlsym(library,"SLSMainConnectionID"):NULL;
    CFArrayRef (*displaySpaces)(int)=library?dlsym(library,"SLSCopyManagedDisplaySpaces"):NULL;
    moveToSpace=library?dlsym(library,"SLSMoveWindowsToManagedSpace"):NULL;
    copyWindowSpaces=library?dlsym(library,"SLSCopySpacesForWindows"):NULL;
    if(mainConnection && displaySpaces && moveToSpace && copyWindowSpaces) {
        spaceConnection=mainConnection();
        NSArray *topology=CFBridgingRelease(displaySpaces(spaceConnection));
        CFUUIDRef uuid=CGDisplayCreateUUIDFromDisplayID([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
        NSString *displayUUID=uuid?CFBridgingRelease(CFUUIDCreateString(NULL,uuid)):nil;
        if(uuid)CFRelease(uuid);
        for(NSDictionary *display in topology) {
            if(![display[@"Display Identifier"] isEqual:displayUUID] && ![display[@"Display Identifier"] isEqual:@"Main"])continue;
            NSMutableArray *desktops=[NSMutableArray array];
            for(NSDictionary *space in display[@"Spaces"])if([space[@"type"] intValue]==0)[desktops addObject:space];
            if(desktops.count<(testingWindow?2U:1U))return NULL;
            NSDictionary *target=desktops[testingWindow?1:0];
            if(testingWindow)testingSpaceUUID=target[@"uuid"];
            primarySpaceID=[target[@"id64"] unsignedLongLongValue];break;
        }
    }
    if(testingWindow && (!primarySpaceID || !testingSpaceUUID.length))return NULL;
    [NSApp setActivationPolicy:normalStartupWindow?NSApplicationActivationPolicyRegular:NSApplicationActivationPolicyAccessory];
    primaryWindow=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1180,800)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO screen:screen];
    primaryWindow.releasedWhenClosed=NO;
    primaryWindow.restorable=NO;
    // 正常啟動探針沿用目前桌面的真實路徑，只將本程序視窗透明化以免佔用使用者畫面。
    if(testing&2){primaryWindow.alphaValue=0;primaryWindow.ignoresMouseEvents=YES;}
    if(!verifyPrimarySpace(primaryWindow)){[primaryWindow close];primaryWindow=nil;return NULL;}
    fprintf(stderr,"launcher_window_placement testing=%d display=%u space=%llu window=%ld verified=1 no_activate=1\n",testingWindow,[screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue],(unsigned long long)primarySpaceID,(long)primaryWindow.windowNumber);
    return (__bridge void *)primaryWindow;
}
void launcher_close_primary_window(void) {primaryWindow.delegate=nil;[primaryWindow close];primaryWindow=nil;}
char *launcher_window_space_uuid(void) {
    if(testingWindow)return strdup(testingSpaceUUID.UTF8String?:"");
    // 桌面 1 可能沒有 UUID，交由共用平台層依目前拓樸解析主桌面。
    return strdup(primaryWindow?"@primary":"");
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
    // 使用者點擊的 HTTP(S) 連結交給預設瀏覽器；Go bridge 永不提供給外部網頁。
    if(action.navigationType==WKNavigationTypeLinkActivated &&
       ([@"https" isEqualToString:action.request.URL.scheme] || [@"http" isEqualToString:action.request.URL.scheme])) {
        handler(WKNavigationActionPolicyCancel);
        [NSWorkspace.sharedWorkspace openURL:action.request.URL];
        return;
    }
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
    [NSApp setActivationPolicy:(backgroundScreen||!normalStartupWindow)?NSApplicationActivationPolicyAccessory:NSApplicationActivationPolicyRegular];
    window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];window.backgroundColor=[NSColor colorWithWhite:0.97 alpha:1];
    LauncherWindowDelegate *delegate=[LauncherWindowDelegate new];delegate.previous=window.delegate;window.delegate=delegate;
    objc_setAssociatedObject(window,@selector(windowShouldClose:),delegate,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    WKWebView *web=findWebView(window.contentView);
    LauncherNavigationDelegate *navigation=[LauncherNavigationDelegate new];navigation.previous=web.navigationDelegate;web.navigationDelegate=navigation;
    objc_setAssociatedObject(window,@selector(navigationDelegate),navigation,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSMenu *menu=[NSMenu new];NSMenuItem *appItem=[NSMenuItem new];[menu addItem:appItem];NSMenu *appMenu=[NSMenu new];appItem.submenu=appMenu;
    NSMenuItem *quit=[[NSMenuItem alloc] initWithTitle:@"結束 AppleJuice" action:@selector(performClose:) keyEquivalent:@"q"];quit.target=window;[appMenu addItem:quit];
    NSMenuItem *edit=[NSMenuItem new];[menu addItem:edit];edit.submenu=[[NSMenu alloc] initWithTitle:@"編輯"];
    NSArray *labels=@[@"復原",@"重做",@"剪下",@"拷貝",@"貼上",@"全選"];
    NSArray *actions=@[@"undo:",@"redo:",@"cut:",@"copy:",@"paste:",@"selectAll:"];
    NSArray *keys=@[@"z",@"Z",@"x",@"c",@"v",@"a"];
    for(NSUInteger i=0;i<labels.count;i++)[edit.submenu addItemWithTitle:labels[i] action:NSSelectorFromString(actions[i]) keyEquivalent:keys[i]];
    NSApp.mainMenu=menu;
    {
        NSScreen *targetScreen=backgroundScreen?:NSScreen.screens.firstObject;
        NSRect screen=targetScreen.visibleFrame,frame=window.frame;
        frame.size.width=MIN(frame.size.width,screen.size.width-40);
        frame.size.height=MIN(frame.size.height,screen.size.height-40);
        frame.origin=NSMakePoint(NSMidX(screen)-frame.size.width/2,NSMidY(screen)-frame.size.height/2);
        [window setFrame:frame display:NO];
        if(primaryWindow && !verifyPrimarySpace(window)){[window orderOut:nil];return;}
        // 啟動僅呈現視窗；不強制啟用 App，避免 Dock 的歷史狀態切換使用者桌面。
        if(normalStartupWindow)[window orderWindow:NSWindowBelow relativeTo:0];
        else [window orderFront:nil];
        if(primaryWindow && !verifyPrimarySpace(window)){[window orderOut:nil];return;}
        fprintf(stderr,"launcher_window_presented testing=%d normal_startup=%d policy=%ld active=%d no_activate=1\n",testingWindow,normalStartupWindow,(long)NSApp.activationPolicy,NSApp.active);
    }
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
    // 本機測試的客體在桌面 2；從所有桌面核對 WindowServer 視窗及歸屬，不切換桌面。
    NSArray *windows=CFBridgingRelease(CGWindowListCopyWindowInfo(testingWindow?kCGWindowListOptionAll:kCGWindowListOptionOnScreenOnly,kCGNullWindowID));
    for(NSDictionary *row in windows){
        if([row[(id)kCGWindowOwnerPID] intValue]!=pid || [row[(id)kCGWindowLayer] intValue]!=0 || [row[(id)kCGWindowAlpha] doubleValue]<=0)continue;
        CGRect bounds;if(!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)row[(id)kCGWindowBounds],&bounds) || bounds.size.width<=1 || bounds.size.height<=1)continue;
        if(testingWindow){
            if(!primarySpaceID || !copyWindowSpaces)return 0;
            NSArray *ids=@[row[(id)kCGWindowNumber]];
            NSArray *membership=CFBridgingRelease(copyWindowSpaces(spaceConnection,7,(__bridge CFArrayRef)ids));
            if(![membership containsObject:@(primarySpaceID)])continue;
        }
        return 1;
    }
    return 0;
}
void launcher_alert(const char *text){@autoreleasepool{NSAlert *alert=[NSAlert new];alert.messageText=@"AppleJuice";alert.informativeText=[NSString stringWithUTF8String:text];[alert runModal];}}
void launcher_reveal(const char *path){[[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]]];}
int launcher_pty_utf8(int fd){struct termios settings;if(tcgetattr(fd,&settings)!=0)return -1;settings.c_iflag|=IUTF8;return tcsetattr(fd,TCSANOW,&settings);}
// 移到垃圾桶並回傳新位置，讓更新失敗時可以移回原處。
char* launcher_trash(const char *path){@autoreleasepool{NSURL *result=nil;NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];if(![[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:&result error:nil]||!result.path)return NULL;return strdup(result.path.fileSystemRepresentation);}}

// Windows 捷徑沿用系統字碼頁轉換，不啟動外部程式或套用宿主環境變數。
char *launcher_windows_text(const void *bytes, size_t size, uint32_t codepage) {
    CFStringEncoding encoding=CFStringConvertWindowsCodepageToEncoding(codepage);
    if(encoding==kCFStringEncodingInvalidId)return NULL;
    CFStringRef value=CFStringCreateWithBytes(NULL,bytes,size,encoding,false);
    if(!value)return NULL;
    CFIndex length=CFStringGetMaximumSizeForEncoding(CFStringGetLength(value),kCFStringEncodingUTF8)+1;
    char *result=malloc(length);
    if(result && !CFStringGetCString(value,result,length,kCFStringEncodingUTF8)){free(result);result=NULL;}
    CFRelease(value);return result;
}
