#include "x64arm/windows_keyboard.hpp"
#include "input_catalogue_cache.hpp"
#include <map>
#include <filesystem>
#include <Carbon/Carbon.h>
#include <algorithm>

namespace x64arm {
namespace {
std::vector<NativeInputMethod> collect_input_methods(bool include_filenames){
    const auto string=[](CFStringRef value){std::u16string text;if(value){text.resize(static_cast<std::size_t>(CFStringGetLength(value)));CFStringGetCharacters(value,CFRangeMake(0,static_cast<CFIndex>(text.size())),reinterpret_cast<UniChar*>(text.data()));}return text;};
    std::map<std::u16string,std::u16string> bundle_files;
    const auto bundle_file=[&](CFURLRef url){auto bundle=CFBundleCreate(nullptr,url);if(!bundle)return std::u16string{};auto executable=CFBundleCopyExecutableURL(bundle);std::u16string filename;
        if(executable){auto name=CFURLCopyLastPathComponent(executable);filename=string(name);if(name)CFRelease(name);CFRelease(executable);}
        const auto identifier=string(CFBundleGetIdentifier(bundle));if(!identifier.empty() && !filename.empty())bundle_files[identifier]=filename;CFRelease(bundle);return filename;};
    if(include_filenames){
    std::vector<std::filesystem::path> directories{"/System/Library/Input Methods","/Library/Input Methods"};
    if(const auto home=std::getenv("HOME"))directories.emplace_back(std::filesystem::path(home)/"Library/Input Methods");
    for(const auto& directory:directories){std::error_code error;for(std::filesystem::directory_iterator it(directory,error),end;!error && it!=end;it.increment(error))if(it->path().extension()==".app"){
        const auto path=it->path().string();auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(path.data()),static_cast<CFIndex>(path.size()),true);if(url){(void)bundle_file(url);CFRelease(url);}
        std::error_code nested;for(std::filesystem::directory_iterator plugin(it->path()/"Contents/PlugIns",nested),last;!nested && plugin!=last;plugin.increment(nested))if(plugin->path().extension()==".appex"){
            const auto extension=plugin->path().string();auto extension_url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(extension.data()),static_cast<CFIndex>(extension.size()),true);if(extension_url){(void)bundle_file(extension_url);CFRelease(extension_url);}}
    }}
    }
    // 同一輸入法的各模式共用 bundle；每次列舉只解析一次（包含無結果）。
    std::map<std::u16string,std::u16string> resolved_files;
    auto sources=TISCreateInputSourceList(nullptr,true);auto current=TISCopyCurrentKeyboardInputSource();
    const auto current_id=current?string(static_cast<CFStringRef>(TISGetInputSourceProperty(current,kTISPropertyInputSourceID))):std::u16string{};
    std::vector<NativeInputMethod> result;
    if(sources)for(CFIndex i=0;i<CFArrayGetCount(sources);++i){
        auto source=static_cast<TISInputSourceRef>(const_cast<void*>(CFArrayGetValueAtIndex(sources,i)));
        auto type=static_cast<CFStringRef>(TISGetInputSourceProperty(source,kTISPropertyInputSourceType));
        if(!type || (!CFEqual(type,kTISTypeKeyboardInputMode) && !CFEqual(type,kTISTypeKeyboardInputMethodWithoutModes) &&
                     !CFEqual(type,kTISTypeKeyboardInputMethodModeEnabled)))continue;
        auto languages=static_cast<CFArrayRef>(TISGetInputSourceProperty(source,kTISPropertyInputSourceLanguages));
        if(!languages || !CFArrayGetCount(languages))continue;
        const auto language=string(static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages,0)));
        const auto lang=language.starts_with(u"ja")?0x411U:language.starts_with(u"ko")?0x412U:language.starts_with(u"zh-Hant")?0x404U:language.starts_with(u"zh-Hans") || language==u"zh"?0x804U:0U;
        if(!lang)continue;
        NativeInputMethod item;item.layout=lang;item.identifier=string(static_cast<CFStringRef>(TISGetInputSourceProperty(source,kTISPropertyInputSourceID)));
        item.current=item.identifier==current_id;item.description=string(static_cast<CFStringRef>(TISGetInputSourceProperty(source,kTISPropertyLocalizedName)));
        if(include_filenames){
            auto bundle_id=static_cast<CFStringRef>(TISGetInputSourceProperty(source,kTISPropertyBundleID));
            if(bundle_id){
                const auto identifier=string(bundle_id);auto [found,inserted]=resolved_files.try_emplace(identifier);
                if(inserted){
                    auto urls=LSCopyApplicationURLsForBundleIdentifier(bundle_id,nullptr);
                    if(urls && CFArrayGetCount(urls))found->second=bundle_file(static_cast<CFURLRef>(CFArrayGetValueAtIndex(urls,0)));
                    if(urls)CFRelease(urls);
                    if(found->second.empty())if(const auto fallback=bundle_files.find(identifier);fallback!=bundle_files.end())found->second=fallback->second;
                }
                item.filename=found->second;
            }
        }
        result.push_back(std::move(item));
    }
    if(current)CFRelease(current);if(sources)CFRelease(sources);
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.identifier<b.identifier;});
    std::map<unsigned,unsigned> ordinals;
    for(auto& item:result){const auto ordinal=++ordinals[item.layout];item.layout|=(0xe000U+ordinal)<<16;}
    return result;
}
struct InputCatalogue {
    detail::InputCatalogueCache cache;
    InputCatalogue(){
        const auto changed=[](CFNotificationCenterRef,void* observer,CFStringRef,const void*,CFDictionaryRef){
            static_cast<InputCatalogue*>(observer)->cache.invalidate();
        };
        for(const auto name:{kTISNotifyEnabledKeyboardInputSourcesChanged,kTISNotifySelectedKeyboardInputSourceChanged})
            CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(),this,changed,name,nullptr,CFNotificationSuspensionBehaviorDeliverImmediately);
    }
    ~InputCatalogue(){CFNotificationCenterRemoveEveryObserver(CFNotificationCenterGetDistributedCenter(),this);}
    auto snapshot(){return cache.get([]{
        auto methods=collect_input_methods(false);
        for(auto& method:methods)method.current=false;
        return methods;
    },detail::InputCatalogueCache::Clock::now());}
};
InputCatalogue& input_catalogue(){static InputCatalogue value;return value;}
}
std::vector<NativeInputMethod> native_input_methods(bool include_filenames){
    if(include_filenames)return collect_input_methods(true);
    auto result=*input_catalogue().snapshot();
    // 使用者切換輸入法立即可見，不等待快照期限或 notification 交付。
    if(auto current=TISCopyCurrentKeyboardInputSource()){
        auto identifier=static_cast<CFStringRef>(TISGetInputSourceProperty(current,kTISPropertyInputSourceID));
        std::u16string text;
        if(identifier){text.resize(static_cast<std::size_t>(CFStringGetLength(identifier)));CFStringGetCharacters(identifier,CFRangeMake(0,static_cast<CFIndex>(text.size())),reinterpret_cast<UniChar*>(text.data()));}
        for(auto& method:result)method.current=method.identifier==text;
        CFRelease(current);
    }
    return result;
}
bool native_input_layout_supported(std::uint32_t layout){
    const auto snapshot=input_catalogue().snapshot();
    return std::any_of(snapshot->begin(),snapshot->end(),[&](const auto& method){return method.layout==layout;});
}
std::u16string native_us_key_text(std::uint32_t scan,bool shift,bool caps,bool* native){
    if(native)*native=false;
    // 固定的 Windows 0409 對應 U.S. 配置；不擅自切換使用者的 macOS 輸入法。
    static const auto layout=[]()->CFDataRef{
        const void* key=kTISPropertyInputSourceID;const void* value=CFSTR("com.apple.keylayout.US");
        const auto filter=CFDictionaryCreate(nullptr,&key,&value,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        const auto sources=TISCreateInputSourceList(filter,true);CFRelease(filter);CFDataRef data=nullptr;
        if(sources && CFArrayGetCount(sources)){
            const auto source=static_cast<TISInputSourceRef>(const_cast<void*>(CFArrayGetValueAtIndex(sources,0)));
            data=static_cast<CFDataRef>(TISGetInputSourceProperty(source,kTISPropertyUnicodeKeyLayoutData));if(data)CFRetain(data);
        }
        if(sources)CFRelease(sources);return data;
    }();
    constexpr std::array<unsigned,50> codes{
        0x1e,0x1f,0x20,0x21,0x23,0x22,0x2c,0x2d,0x2e,0x2f,0x56,0x30,0x10,0x11,0x12,0x13,
        0x15,0x14,2,3,4,5,7,6,0x0d,0x0a,8,0x0c,9,0x0b,0x1b,0x18,
        0x16,0x1a,0x17,0x19,0x1c,0x26,0x24,0x28,0x25,0x27,0x2b,0x33,0x35,0x31,0x32,0x34,0x0f,0x39};
    auto found=std::find(codes.begin(),codes.end(),scan);unsigned mac=found==codes.end()?UINT_MAX:static_cast<unsigned>(found-codes.begin());
    if(scan==0x29)mac=50;
    if(!layout || mac==UINT_MAX)return {};
    UInt32 dead{};UniChar units[8]{};UniCharCount count{};
    const auto modifiers=((shift?shiftKey:0)|(caps?alphaLock:0))>>8;
    const auto status=UCKeyTranslate(reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layout)),static_cast<UInt16>(mac),
        kUCKeyActionDown,static_cast<UInt32>(modifiers),40,kUCKeyTranslateNoDeadKeysMask,&dead,8,&count,units);
    if(status!=noErr)return {};if(native)*native=true;
    return std::u16string(reinterpret_cast<const char16_t*>(units),count);
}
}
