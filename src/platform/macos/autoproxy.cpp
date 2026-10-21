#include "x64arm/autoproxy.hpp"
#include <CFNetwork/CFNetwork.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStoreCopyDHCPInfo.h>
#include <netdb.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace x64arm {
namespace {
template<class T> struct Owned {
    T value{};
    ~Owned(){if(value)CFRelease(value);}
};
std::string string(CFStringRef value){
    if(!value)return {};
    std::vector<char> bytes(static_cast<std::size_t>(CFStringGetMaximumSizeForEncoding(CFStringGetLength(value),kCFStringEncodingUTF8)+1));
    return CFStringGetCString(value,bytes.data(),static_cast<CFIndex>(bytes.size()),kCFStringEncodingUTF8)?std::string(bytes.data()):std::string{};
}
std::string javascript_string(const std::string& value){
    constexpr char digits[]="0123456789abcdef";std::string quoted="\"";
    for(std::size_t i=0;i<value.size();++i){const auto byte=static_cast<unsigned char>(value[i]);
        if(byte=='"' || byte=='\\'){quoted+='\\';quoted+=static_cast<char>(byte);}
        else if(byte<32){quoted+="\\u00";quoted+=digits[byte>>4];quoted+=digits[byte&15];}
        else if(byte==0xe2 && i+2<value.size() && static_cast<unsigned char>(value[i+1])==0x80 && (static_cast<unsigned char>(value[i+2])==0xa8 || static_cast<unsigned char>(value[i+2])==0xa9)){quoted+=static_cast<unsigned char>(value[i+2])==0xa8?"\\u2028":"\\u2029";i+=2;}
        else quoted+=static_cast<char>(byte);
    }
    quoted+='"';return quoted;
}
bool valid_url(const std::string& text){
    if(text.find_first_of("\r\n")!=std::string::npos)return false;
    Owned<CFURLRef> url{CFURLCreateWithBytes(nullptr,reinterpret_cast<const UInt8*>(text.data()),static_cast<CFIndex>(text.size()),kCFStringEncodingUTF8,nullptr)};
    if(!url.value)return false;
    Owned<CFStringRef> scheme{CFURLCopyScheme(url.value)},host{CFURLCopyHostName(url.value)},user{CFURLCopyUserName(url.value)};
    return scheme.value && host.value && !string(host.value).empty() && !user.value && (CFStringCompare(scheme.value,CFSTR("http"),kCFCompareCaseInsensitive)==kCFCompareEqualTo || CFStringCompare(scheme.value,CFSTR("https"),kCFCompareCaseInsensitive)==kCFCompareEqualTo);
}
std::string discover(std::uint32_t flags){
    if(flags&1){
        // 先查宿主主要介面真正取得的 DHCP option 252，不將手動 PAC 設定當成 WPAD。
        Owned<CFDictionaryRef> info{SCDynamicStoreCopyDHCPInfo(nullptr,nullptr)};
        if(info.value)if(const auto data=DHCPInfoGetOptionData(info.value,252)){
            std::string url(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),static_cast<std::size_t>(CFDataGetLength(data)));
            while(!url.empty() && (url.back()==0 || url.back()=='\r' || url.back()=='\n' || url.back()==' '))url.pop_back();
            if(valid_url(url))return url;
        }
    }
    if(flags&2){
        // 使用宿主 resolver 的本機搜尋網域；只有實際解析出 WPAD 位址才交付 URL。
        addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;addrinfo* addresses=nullptr;
        const auto result=::getaddrinfo("wpad",nullptr,&hints,&addresses);
        if(addresses)::freeaddrinfo(addresses);if(!result)return "http://wpad/wpad.dat";
    }
    return {};
}
struct State {std::atomic<bool> done{},cancel{};AutoProxyResult result;};
class Task final:public NativeAutoProxy {
    std::shared_ptr<State> state_;
public:
    explicit Task(std::shared_ptr<State> state):state_(std::move(state)){}
    ~Task()override{state_->cancel.store(true);}
    bool done()const override{return state_->done.load(std::memory_order_acquire);}
    AutoProxyResult take()override{if(!done())throw std::logic_error("自動代理查詢尚未完成");return std::move(state_->result);}
    void cancel()noexcept override{state_->cancel.store(true);}
};
void evaluate(State& state,AutoProxyInput input){
    if(input.detect_flags)state.result.script_url=discover(input.detect_flags);
    if(state.result.script_url.empty())state.result.script_url=input.script_url;
    if(state.result.script_url.empty()){state.result.error=12180;return;}
    if(input.discovery_only)return;
    if(!valid_url(state.result.script_url)){state.result.error=12006;return;}
    if(!valid_url(input.target_url)){state.result.error=12005;return;}
    input.http.url=state.result.script_url;input.http.method="GET";
    const auto request=start_native_http(std::move(input.http));if(!request){state.result.error=50;return;}
    while(!request->done()){if(state.cancel.load()){request->cancel();state.result.error=12017;return;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    auto response=request->take();
    if(response.error || response.status!=200){state.result.error=response.status==401 || response.status==407?12015:12167;return;}
    if(state.cancel.load()){state.result.error=12017;return;}
    if(input.flags&8){const auto begin=input.target_url.find("://");if(begin!=input.target_url.npos){const auto end=input.target_url.find_first_of("/?#",begin+3);for(auto i=begin+3;i<(end==input.target_url.npos?input.target_url.size():end);++i)if(input.target_url[i]>='A' && input.target_url[i]<='Z')input.target_url[i]+=32;}}
    Owned<CFURLRef> target{CFURLCreateWithBytes(nullptr,reinterpret_cast<const UInt8*>(input.target_url.data()),static_cast<CFIndex>(input.target_url.size()),kCFStringEncodingUTF8,nullptr)};
    if(!target.value){state.result.error=12005;return;}
    Owned<CFStringRef> target_host{CFURLCopyHostName(target.value)};
    std::string source(response.body.begin(),response.body.end());
    // CFNetwork 會省略 HTTPS URL 路徑；WinHTTP 的 PAC 則須收到客體交付的完整 URL。
    source+="\n;(function(){var original=FindProxyForURL;FindProxyForURL=function(){return original("+javascript_string(input.target_url)+","+javascript_string(string(target_host.value))+");};})();\n";
    Owned<CFStringRef> script{CFStringCreateWithBytes(nullptr,reinterpret_cast<const UInt8*>(source.data()),static_cast<CFIndex>(source.size()),kCFStringEncodingUTF8,false)};
    if(!script.value || !target.value){state.result.error=12166;return;}
    CFErrorRef error=nullptr;
    // CFNetwork 依目標 URL 將 PROXY 標成 HTTP／HTTPS；WinHTTP 的 scheme 則代表
    // PAC 回傳的代理協定。PAC 仍收到上方包裝保留的完整原始 URL 與 host，
    // 只以 HTTP URL 解析回傳清單，避免把 HTTP CONNECT 代理誤標為 HTTPS。
    const auto parser_url="http"+input.target_url.substr(input.target_url.find(':'));
    Owned<CFURLRef> parser_target{CFURLCreateWithBytes(nullptr,reinterpret_cast<const UInt8*>(parser_url.data()),static_cast<CFIndex>(parser_url.size()),kCFStringEncodingUTF8,nullptr)};
    if(!parser_target.value){state.result.error=12005;return;}
    Owned<CFArrayRef> proxies{CFNetworkCopyProxiesForAutoConfigurationScript(script.value,parser_target.value,&error)};Owned<CFErrorRef> owned_error{error};
    if(!proxies.value){state.result.error=12166;return;}
    bool legacy_finished=false;
    if(CFArrayGetCount(proxies.value)>8192){state.result.error=8;return;}
    for(CFIndex i=0;i<CFArrayGetCount(proxies.value);++i){
        const auto proxy=static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(proxies.value,i));
        const auto type=static_cast<CFStringRef>(CFDictionaryGetValue(proxy,kCFProxyTypeKey));
        if(type && CFEqual(type,kCFProxyTypeNone)){state.result.entries.push_back({});legacy_finished=true;continue;}
        if(!type || (!CFEqual(type,kCFProxyTypeHTTP) && !CFEqual(type,kCFProxyTypeHTTPS) && !CFEqual(type,kCFProxyTypeSOCKS)))continue;
        const auto host=string(static_cast<CFStringRef>(CFDictionaryGetValue(proxy,kCFProxyHostNameKey)));
        const auto port=static_cast<CFNumberRef>(CFDictionaryGetValue(proxy,kCFProxyPortNumberKey));int number=0;
        if(host.empty() || !port || !CFNumberGetValue(port,kCFNumberIntType,&number) || number<1 || number>65535)continue;
        const auto scheme=CFEqual(type,kCFProxyTypeHTTPS)?2U:CFEqual(type,kCFProxyTypeSOCKS)?4U:1U;
        state.result.entries.push_back({host,scheme,static_cast<std::uint16_t>(number),true,false});
        if(!legacy_finished && scheme!=4){if(!state.result.proxy.empty())state.result.proxy+=';';state.result.proxy+=(host.find(':')!=host.npos?"["+host+"]":host)+":"+std::to_string(number);}
    }
    if(state.result.entries.empty())state.result.error=12178;
}
}
std::shared_ptr<NativeAutoProxy> start_native_autoproxy(AutoProxyInput input){
    auto state=std::make_shared<State>();auto task=std::make_shared<Task>(state);
    std::thread([state,input=std::move(input)]()mutable{
        try{evaluate(*state,std::move(input));}catch(...){state->result.error=8;}
        state->done.store(true,std::memory_order_release);
    }).detach();return task;
}
}
