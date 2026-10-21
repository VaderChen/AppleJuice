#include "x64arm/native_shell.hpp"
#include "x64arm/unicode.hpp"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreServices/CoreServices.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <climits>
#include <set>

namespace x64arm::shell {
namespace {
NSString* text(std::string_view value){return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];}
NSURL* target(const std::shared_ptr<HostFile>& file,std::string_view uri){
    if(!file){NSURL* url=[NSURL URLWithString:text(uri)];return url && !url.fileURL && url.scheme.length?url:nil;}
    const auto fd=file->duplicate_descriptor();if(fd<0)return nil;
    char path[PATH_MAX]{};const auto result=fcntl(fd,F_GETPATH,path);close(fd);if(result<0)return nil;
    return [NSURL fileURLWithPath:text(path)];
}
std::uint32_t error(NSError* value){
    if([value.domain isEqualToString:NSCocoaErrorDomain]){
        if(value.code==NSUserCancelledError)return 1223;
        if(value.code==NSFileNoSuchFileError || value.code==NSFileReadNoSuchFileError)return 2;
        if(value.code==NSFileReadNoPermissionError || value.code==NSFileWriteNoPermissionError)return 5;
    }
    return 1155; // 沒有可用的關聯／Launch Services 拒絕啟動。
}
}
std::uint32_t reveal(std::span<const std::shared_ptr<HostFile>> files,std::uint32_t flags){
    @autoreleasepool {
        // Finder 沒有公開的桌面選取或檔名編輯介面；多選的 EDIT 依 Win32 契約忽略。
        if((flags&2U) || ((flags&1U) && files.size()==1))return 50;
        if(files.empty() || files.size()>4096)return 87;
        NSMutableArray<NSURL*>* urls=[NSMutableArray arrayWithCapacity:files.size()];
        for(const auto& file:files){NSURL* url=target(file,{});if(!url)return 2;[urls addObject:url];}
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:urls];return 0;
    }
}
std::vector<Application> applications(const std::shared_ptr<HostFile>& file,std::string_view uri){
    @autoreleasepool {
        NSURL* url=target(file,uri);if(!url)return {};
        std::vector<Application> result;std::set<std::string> seen;
        NSArray<NSURL*>* candidates=nil;
        if(@available(macOS 12.0,*))candidates=[NSWorkspace.sharedWorkspace URLsForApplicationsToOpenURL:url];
        else candidates=CFBridgingRelease(LSCopyApplicationURLsForURL((__bridge CFURLRef)url,kLSRolesAll));
        for(NSURL* app in candidates){
            NSString* path=app.path;if(!path || !path.UTF8String)continue;
            std::string identifier(path.UTF8String);if(!seen.insert(identifier).second)continue;
            NSString* label=[NSFileManager.defaultManager displayNameAtPath:path];
            result.push_back({std::move(identifier),utf8_to_utf16_lossy(label.UTF8String?label.UTF8String:"")});
            if(result.size()==256)break;
        }
        return result;
    }
}
std::uint32_t open(const std::shared_ptr<HostFile>& file,std::string_view uri,const Application& application){
    @autoreleasepool {
        NSURL* url=target(file,uri);if(!url)return 2;
        // 啟動前重新確認候選，避免以未登錄的宿主路徑代替使用者選擇。
        const auto available=applications(file,uri);
        if(std::none_of(available.begin(),available.end(),[&](const auto& app){return app.identifier==application.identifier;}))return 1155;
        NSError* failure=nil;
        NSRunningApplication* running=[NSWorkspace.sharedWorkspace openURLs:@[url]
            withApplicationAtURL:[NSURL fileURLWithPath:text(application.identifier)]
            options:NSWorkspaceLaunchDefault configuration:@{} error:&failure];
        return running?0:error(failure);
    }
}
std::optional<Application> default_application(const std::shared_ptr<HostFile>& file,std::string_view uri){
    @autoreleasepool {
        NSURL* url=target(file,uri);if(!url)return {};
        NSURL* app=[NSWorkspace.sharedWorkspace URLForApplicationToOpenURL:url];if(!app || !app.path.UTF8String)return {};
        NSString* label=[NSFileManager.defaultManager displayNameAtPath:app.path];
        return Application{std::string(app.path.UTF8String),utf8_to_utf16_lossy(label.UTF8String?label.UTF8String:"")};
    }
}
std::optional<Dock> dock(){
    @autoreleasepool {
        NSScreen* screen=NSScreen.mainScreen;if(!screen)return {};
        const auto full=screen.frame,visible=screen.visibleFrame;
        CFPropertyListRef raw=CFPreferencesCopyAppValue(CFSTR("orientation"),CFSTR("com.apple.dock"));
        NSString* orientation=raw && CFGetTypeID(raw)==CFStringGetTypeID()?(__bridge NSString*)raw:@"bottom";
        const std::uint32_t edge=[orientation isEqualToString:@"left"]?0U:[orientation isEqualToString:@"right"]?2U:3U;
        if(raw)CFRelease(raw);
        const bool hidden=CFPreferencesGetAppBooleanValue(CFSTR("autohide"),CFSTR("com.apple.dock"),nullptr);
        Dock result;result.edge=edge;result.state=2U|(hidden?1U:0U);
        CGRect dockBounds{};bool found=false;
        CFArrayRef windows=CGWindowListCopyWindowInfo(kCGWindowListOptionAll,kCGNullWindowID);
        if(windows){
            for(NSDictionary* window in (__bridge NSArray*)windows){
                if(![window[(__bridge NSString*)kCGWindowOwnerName] isEqualToString:@"Dock"] ||
                   ![window[(__bridge NSString*)kCGWindowName] isEqualToString:@"Dock"])continue;
                NSDictionary* bounds=window[(__bridge NSString*)kCGWindowBounds];CGRect box{};
                if(bounds && CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)bounds,&box) && box.size.width>0 && box.size.height>0){dockBounds=box;found=true;break;}
            }
            CFRelease(windows);
        }
        if(found){result.rectangle={static_cast<std::int32_t>(dockBounds.origin.x-full.origin.x),static_cast<std::int32_t>(dockBounds.origin.y),static_cast<std::int32_t>(CGRectGetMaxX(dockBounds)-full.origin.x),static_cast<std::int32_t>(CGRectGetMaxY(dockBounds))};return result;}
        // 無視窗名稱權限時，以 NSScreen 的實際保留區推得可見 Dock 邊界。
        const auto width=static_cast<std::int32_t>(full.size.width),height=static_cast<std::int32_t>(full.size.height);
        if(!hidden && edge==0 && visible.origin.x>full.origin.x)result.rectangle={0,0,static_cast<std::int32_t>(visible.origin.x-full.origin.x),height};
        else if(!hidden && edge==2 && NSMaxX(visible)<NSMaxX(full))result.rectangle={static_cast<std::int32_t>(NSMaxX(visible)-full.origin.x),0,width,height};
        else if(!hidden && edge==3 && visible.origin.y>full.origin.y)result.rectangle={0,static_cast<std::int32_t>(NSMaxY(full)-visible.origin.y),width,height};
        else return result; // 狀態仍可查詢，不臆造隱藏 Dock 的矩形。
        return result;
    }
}
}
