#include "x64arm/network_resources.hpp"
#include "x64arm/unicode.hpp"
#import <Foundation/Foundation.h>
#include <NetFS/NetFS.h>
#include <NetFS/NetFSPlugin.h>
#include <CoreFoundation/CFPlugIn.h>
#include <dns_sd.h>
#include <poll.h>
#include <sys/mount.h>
#include <cerrno>
#include <chrono>
#include <set>
#include <utility>

namespace x64arm::network_resources {
namespace {
template<class T> struct Cf {
    T value{};Cf()=default;explicit Cf(T item):value(item){};~Cf(){if(value)CFRelease(value);}
    Cf(const Cf&)=delete;Cf& operator=(const Cf&)=delete;
    Cf(Cf&& other)noexcept:value(std::exchange(other.value,nullptr)){}
};
std::uint32_t error(int code){switch(code){case 0:return 0;case ENOMEM:return 8;case EACCES:case EPERM:return 5;case EAUTH:return 1326;case ENOENT:return 67;case ENETDOWN:case ENETUNREACH:return 1222;case EHOSTUNREACH:case ECONNREFUSED:return 53;case ETIMEDOUT:return 121;case EBUSY:return 2404;case -128:return 1223;default:return 1208;}}
std::u16string text(CFStringRef value){if(!value)return {};const auto length=CFStringGetLength(value);std::u16string result(static_cast<std::size_t>(length),u'\0');CFStringGetCharacters(value,CFRangeMake(0,length),reinterpret_cast<UniChar*>(result.data()));return result;}
Cf<CFStringRef> string(std::u16string_view value){return Cf<CFStringRef>(CFStringCreateWithCharacters(kCFAllocatorDefault,reinterpret_cast<const UniChar*>(value.data()),static_cast<CFIndex>(value.size())));}
Cf<CFMutableDictionaryRef> options(const Credentials& credentials){
    auto result=Cf<CFMutableDictionaryRef>(CFDictionaryCreateMutable(kCFAllocatorDefault,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(result.value,kNetFSNoUserPreferencesKey,kCFBooleanTrue);CFDictionarySetValue(result.value,kNetFSForceNewSessionKey,kCFBooleanTrue);CFDictionarySetValue(result.value,kNetFSAllowLoopbackKey,kCFBooleanTrue);CFDictionarySetValue(result.value,kNAUIOptionKey,kNAUIOptionNoUI);CFDictionarySetValue(result.value,kNetFSUseAuthenticationInfoKey,kCFBooleanFalse);
    CFDictionarySetValue(result.value,kNetFSUseGuestKey,credentials.guest?kCFBooleanTrue:kCFBooleanFalse);const auto user=string(credentials.user),password=string(credentials.password);CFDictionarySetValue(result.value,kNetFSUserNameKey,user.value);CFDictionarySetValue(result.value,kNetFSPasswordKey,password.value);return result;
}
Cf<CFURLRef> url(const Path& path,const Credentials& credentials){
    @autoreleasepool {NSURLComponents* parts=[NSURLComponents new];parts.scheme=@"smb";const auto host=utf16_to_utf8(path.server);parts.host=[NSString stringWithUTF8String:host.c_str()];parts.path=path.share.empty()?@"/":[NSString stringWithUTF8String:utf16_to_utf8(u"/"+path.share).c_str()];
        parts.user=[NSString stringWithUTF8String:utf16_to_utf8(credentials.guest?u"guest":std::u16string_view(credentials.user)).c_str()];parts.password=[NSString stringWithUTF8String:utf16_to_utf8(credentials.password).c_str()];return Cf<CFURLRef>(parts.URL?static_cast<CFURLRef>(CFRetain((__bridge CFURLRef)parts.URL)):nullptr);
    }
}
struct Session {
    Cf<CFPlugInRef> plugin;NetFSMountInterface_V1** interface{};void* session{};std::uint32_t failure{};
    Session(std::u16string_view server,const Credentials& credentials){
        const auto location=string(u"/System/Library/Filesystems/NetFSPlugins/smb.bundle");Cf<CFURLRef> bundle(CFURLCreateWithFileSystemPath(kCFAllocatorDefault,location.value,kCFURLPOSIXPathStyle,true));plugin.value=CFPlugInCreate(kCFAllocatorDefault,bundle.value);if(!plugin.value){failure=1222;return;}
        Cf<CFArrayRef> factories(CFPlugInFindFactoriesForPlugInTypeInPlugIn(kNetFSTypeID,plugin.value));if(!factories.value || !CFArrayGetCount(factories.value)){failure=1222;return;}
        auto* unknown=static_cast<IUnknownVTbl**>(CFPlugInInstanceCreate(kCFAllocatorDefault,static_cast<CFUUIDRef>(const_cast<void*>(CFArrayGetValueAtIndex(factories.value,0))),kNetFSTypeID));if(!unknown){failure=1222;return;}
        void* queried=nullptr;const auto status=(*unknown)->QueryInterface(unknown,CFUUIDGetUUIDBytes(kfs_NetFSMountInterfaceID_V1),&queried);(*unknown)->Release(unknown);if(status || !queried){failure=1222;return;}interface=static_cast<NetFSMountInterface_V1**>(queried);
        auto code=(*interface)->CreateSessionRef(&session);if(code){failure=error(code);return;}const auto endpoint=url(Path{std::u16string(server),{},{}},credentials);if(!endpoint.value){failure=123;return;}const auto configuration=options(credentials);CFDictionaryRef info=nullptr;
        code=(*interface)->GetServerInfo(endpoint.value,session,configuration.value,&info);if(info)CFRelease(info);if(!code){info=nullptr;code=(*interface)->OpenSession(endpoint.value,session,configuration.value,&info);if(info)CFRelease(info);}failure=error(code);
    }
    ~Session(){if(session && interface)(void)(*interface)->CloseSession(session);if(interface)(*interface)->Release(interface);}
};
struct Mounted final:Mount {
    std::filesystem::path path;bool active{true};explicit Mounted(std::filesystem::path value):path(std::move(value)){}
    ~Mounted(){if(active)(void)::unmount(path.c_str(),0);}
    const std::filesystem::path& root()const override{return path;}
    std::uint32_t disconnect(bool force)override{if(!active)return 0;if(::unmount(path.c_str(),force?MNT_FORCE:0)<0)return error(errno);active=false;return 0;}
};
struct Discovery {
    struct Name {std::string value,type,domain;std::uint32_t interface{};};std::vector<Name> names;
    static void found(DNSServiceRef,DNSServiceFlags flags,std::uint32_t index,DNSServiceErrorType status,const char* name,const char* type,const char* domain,void* context){if(!status && (flags&kDNSServiceFlagsAdd))static_cast<Discovery*>(context)->names.push_back({name,type,domain,index});}
    static void resolved(DNSServiceRef,DNSServiceFlags,std::uint32_t,DNSServiceErrorType status,const char*,const char* target,std::uint16_t,std::uint16_t,const unsigned char*,void* context){if(!status && target)*static_cast<std::string*>(context)=target;}
    static void poll(DNSServiceRef service,int milliseconds){pollfd fd{DNSServiceRefSockFD(service),POLLIN,0};const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);while(std::chrono::steady_clock::now()<until){const auto ready=::poll(&fd,1,20);if(ready>0){if(DNSServiceProcessResult(service))break;}else if(ready<0 && errno!=EINTR)break;}}
};
class Native final:public Backend {
public:
    Servers servers()override{
        Discovery discovery;DNSServiceRef service{};const auto status=DNSServiceBrowse(&service,0,0,"_smb._tcp",nullptr,Discovery::found,&discovery);if(status || !service)return {{},1222};Discovery::poll(service,300);DNSServiceRefDeallocate(service);Servers result;std::set<std::string> seen;
        for(const auto& name:discovery.names){std::string host;DNSServiceRef lookup{};if(!DNSServiceResolve(&lookup,0,name.interface,name.value.c_str(),name.type.c_str(),name.domain.c_str(),Discovery::resolved,&host) && lookup){Discovery::poll(lookup,200);DNSServiceRefDeallocate(lookup);}if(!host.empty() && seen.insert(host).second){while(host.ends_with('.'))host.pop_back();result.values.push_back({utf8_to_utf16_lossy(host),utf8_to_utf16_lossy(name.value)});}}return result;
    }
    Shares shares(std::u16string_view server,const Credentials& credentials)override{
        Session session(server,credentials);if(session.failure)return {{},session.failure};const auto configuration=options(credentials);CFDictionaryRef result=nullptr;const auto code=(*session.interface)->EnumerateShares(session.session,configuration.value,&result);Cf<CFDictionaryRef> owned(result);if(code)return {{},error(code)};Shares output;if(!result)return output;
        const auto count=CFDictionaryGetCount(result);std::vector<const void*> keys(static_cast<std::size_t>(count)),values(static_cast<std::size_t>(count));CFDictionaryGetKeysAndValues(result,keys.data(),values.data());
        for(std::size_t n=0;n<keys.size();++n){if(CFGetTypeID(keys[n])!=CFStringGetTypeID() || CFGetTypeID(values[n])!=CFDictionaryGetTypeID())continue;const auto data=static_cast<CFDictionaryRef>(values[n]);Share share;share.name=text(static_cast<CFStringRef>(keys[n]));share.printer=CFDictionaryGetValue(data,kNetFSPrinterShareKey)==kCFBooleanTrue;share.hidden=CFDictionaryGetValue(data,kNetFSIsHiddenKey)==kCFBooleanTrue;output.values.push_back(std::move(share));}return output;
    }
    Connection connect(const Path& path,const Credentials& credentials,const std::filesystem::path& destination,bool readonly)override{
        if(path.share.empty())return {{},67};const auto endpoint=url(path,credentials);if(!endpoint.value)return {{},123};const auto directory=utf8_to_utf16_lossy(destination.string());const auto name=string(directory);Cf<CFURLRef> target(CFURLCreateWithFileSystemPath(kCFAllocatorDefault,name.value,kCFURLPOSIXPathStyle,true));const auto open=options(credentials),mount=options(credentials);
        const std::int32_t flags=MNT_DONTBROWSE|MNT_NOSUID|MNT_NODEV|(readonly?MNT_RDONLY:0);Cf<CFNumberRef> value(CFNumberCreate(kCFAllocatorDefault,kCFNumberSInt32Type,&flags));CFDictionarySetValue(mount.value,kNetFSMountFlagsKey,value.value);CFDictionarySetValue(mount.value,kNetFSMountAtMountDirKey,kCFBooleanTrue);CFDictionarySetValue(mount.value,kNetFSSoftMountKey,kCFBooleanTrue);CFArrayRef mounts=nullptr;
        const auto user=string(credentials.guest?u"guest":std::u16string_view(credentials.user)),password=string(credentials.password);const auto status=NetFSMountURLSync(endpoint.value,target.value,user.value,password.value,open.value,mount.value,&mounts);Cf<CFArrayRef> result(mounts);if(status)return {{},error(status)};if(!mounts || CFArrayGetCount(mounts)!=1)return {{},1208};const auto mounted=text(static_cast<CFStringRef>(CFArrayGetValueAtIndex(mounts,0)));const auto actual=std::filesystem::path(utf16_to_utf8(mounted));if(actual!=destination){(void)::unmount(actual.c_str(),0);return {{},1208};}return {std::make_shared<Mounted>(actual),0};
    }
};
}
std::shared_ptr<Backend> make_native_backend(){return std::make_shared<Native>();}
}
