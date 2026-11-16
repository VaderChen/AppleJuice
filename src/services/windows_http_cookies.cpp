// WinINet Cookie（RFC 6265）與隱私設定，宿主端實作：
// - InternetSetCookieA／W、InternetSetCookieExA／W：解析 Set-Cookie 字串（Expires／Max-Age／Domain／Path／Secure／HttpOnly）；名稱非 NULL 時組成「名稱=資料」，
//   沒有 "=" 時為無名稱 Cookie。沒有到期時間為 session Cookie（只存在本程序）；有到期時間存入沙盒登錄；到期時間已過即刪除同名 Cookie。
//   Domain 須與 URL 主機相符，且除主機本身外必須含內嵌點；IP 主機只接受完全相同。HttpOnly Cookie 只能以 InternetSetCookieEx 加 INTERNET_COOKIE_HTTPONLY
//   設定或覆寫，否則 ERROR_INVALID_OPERATION。InternetSetCookieEx 回傳 InternetCookieState：COOKIE_STATE_ACCEPT（1），被隱私設定拒絕為 COOKIE_STATE_REJECT（5）；
//   InternetSetCookie 被拒絕時回傳 FALSE。P3P 標頭不評估（現行 Windows 已不支援 P3P）。每個網域最多 50 個 Cookie，超過時移除最舊者。
// - InternetGetCookieA／W、InternetGetCookieExA／W：依主機、路徑與 Secure（只限 https）篩選，路徑較長者在前、同長度依建立先後；InternetGetCookie 忽略名稱，
//   Ex 版依名稱篩選；HttpOnly 只在 Ex 加 INTERNET_COOKIE_HTTPONLY 時傳回。lpdwSize 輸入為字元數，成功時為複製的字元數（不含 NUL），
//   資料為 NULL 或 ERROR_INSUFFICIENT_BUFFER 時為所需位元組數（含 NUL）。沒有 Cookie 為 ERROR_NO_MORE_ITEMS；非 http／https URL 為 ERROR_INVALID_PARAMETER。
// - WinINet 請求（InternetOpenUrl、HttpSendRequest(Ex)）未設 INTERNET_FLAG_NO_COOKIES 時，送出符合的 Cookie（含 HttpOnly，與應用程式自訂的 Cookie 標頭合併），
//   並保存最終回應的 Set-Cookie（快取命中除外）。
// - 各站台決定：InternetSetPerSiteCookieDecisionA／W（COOKIE_STATE_ACCEPT／REJECT，UNKNOWN 清除）、InternetGetPerSiteCookieDecisionA／W（完全相同主機，
//   沒有時 ERROR_NOT_FOUND）、InternetEnumPerSiteCookieDecisionA／W（依主機名稱排序，超出為 ERROR_NO_MORE_ITEMS）、InternetClearAllPerSiteCookieDecisions。
//   決定套用到主機與其子網域，優先於區域範本。
// - PrivacySetZonePreferenceW／PrivacyGetZonePreferenceW：區域 0～4、第一方／第三方各自的範本（0～5、CUSTOM 100、ADVANCED 101，預設 MEDIUM）與偏好字串
//   （只有 CUSTOM／ADVANCED 保存）；以回傳值表示 Win32 錯誤碼。PRIVACY_TEMPLATE_NO_COOKIES 封鎖網際網路區域的 Cookie 讀寫，其他範本接受。
#include "x64arm/windows_services.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/url_cache.hpp"
#include <algorithm>
#include <array>
#include <curl/curl.h>

namespace x64arm {
namespace {
struct CookieError {std::uint32_t code;};
constexpr std::uint32_t accept=1,reject=5,no_more_items=259;
std::string lower(std::string text){for(auto& c:text)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return text;}
std::string_view trim(std::string_view text){const auto first=text.find_first_not_of(" \t");if(first==text.npos)return {};const auto last=text.find_last_not_of(" \t");return text.substr(first,last-first+1);}
struct CookieUrl {std::string host,path;bool secure{};};
// 只接受 http／https；主機小寫並去除帳號與連接埠，路徑不含查詢字串與片段。
std::optional<CookieUrl> cookie_url(std::string_view url){
    const auto colon=url.find("://");if(colon==url.npos)return std::nullopt;const auto scheme=lower(std::string(url.substr(0,colon)));
    if(scheme!="http" && scheme!="https")return std::nullopt;
    CookieUrl result;result.secure=scheme=="https";
    const auto rest=url.substr(colon+3);const auto end=rest.find_first_of("/?#");auto authority=rest.substr(0,end);
    const auto at=authority.rfind('@');if(at!=authority.npos)authority=authority.substr(at+1);
    if(authority.starts_with('[')){const auto close=authority.find(']');if(close==authority.npos)return std::nullopt;authority=authority.substr(0,close+1);}
    else{const auto port=authority.find(':');if(port!=authority.npos)authority=authority.substr(0,port);}
    result.host=lower(std::string(authority));if(result.host.empty() || result.host.size()>255)return std::nullopt;
    if(end==rest.npos || rest[end]!='/')result.path="/";
    else{const auto stop=rest.find_first_of("?#",end);result.path=std::string(stop==rest.npos?rest.substr(end):rest.substr(end,stop-end));}
    return result;
}
bool ip_host(std::string_view host){return host.starts_with('[') || host.find_first_not_of("0123456789.")==host.npos;}
bool domain_match(std::string_view host,std::string_view domain){return host==domain || (!ip_host(host) && host.size()>domain.size() && host.ends_with(domain) && host[host.size()-domain.size()-1]=='.');}
bool path_match(std::string_view request,std::string_view cookie){return request==cookie || (request.starts_with(cookie) && (cookie.ends_with('/') || request[cookie.size()]=='/'));}
std::string default_path(std::string_view path){if(path.empty() || path.front()!='/')return "/";const auto last=path.rfind('/');return last==0?"/":std::string(path.substr(0,last));}
bool control(std::string_view text){return std::any_of(text.begin(),text.end(),[](char c){const auto b=static_cast<unsigned char>(c);return (b<0x20 && b!='\t') || b==0x7f;});}
bool expired(const HttpCookie& c,std::uint64_t now){return c.expires && c.expires<=now;}
std::optional<HttpCookie> parse_cookie(std::string_view text,const CookieUrl& url,std::uint64_t now){
    if(control(text))return std::nullopt;
    const auto semicolon=text.find(';');const auto pair=text.substr(0,semicolon);HttpCookie c;const auto equals=pair.find('=');
    if(equals==pair.npos)c.value=std::string(trim(pair));else{c.name=std::string(trim(pair.substr(0,equals)));c.value=std::string(trim(pair.substr(equals+1)));}
    if((c.name.empty() && c.value.empty()) || c.name.size()+c.value.size()>4096)return std::nullopt;
    std::optional<std::uint64_t> max_age,expires;std::string domain;bool have_path=false;
    for(auto rest=semicolon==text.npos?std::string_view{}:text.substr(semicolon+1);!rest.empty();){
        const auto next=rest.find(';');const auto attribute=rest.substr(0,next);rest=next==rest.npos?std::string_view{}:rest.substr(next+1);
        const auto eq=attribute.find('=');const auto key=lower(std::string(trim(attribute.substr(0,eq))));const auto value=eq==attribute.npos?std::string_view{}:trim(attribute.substr(eq+1));
        if(key=="expires"){
            const auto time=::curl_getdate(std::string(value).c_str(),nullptr);
            // time_t 轉 FILETIME；1601 年以前取最早時間（仍為已過期），過遠的未來限制在 30000 年內。
            if(time!=-1){const auto seconds=std::clamp<std::int64_t>(static_cast<std::int64_t>(time)+INT64_C(11644473600),0,INT64_C(894000000000));expires=std::max<std::uint64_t>(1,static_cast<std::uint64_t>(seconds)*10000000);}
        }else if(key=="max-age"){
            const bool negative=value.starts_with('-');const auto digits=negative?value.substr(1):value;
            if(digits.empty() || digits.find_first_not_of("0123456789")!=digits.npos)continue;
            std::uint64_t seconds=0;for(const char d:digits){if(seconds>UINT64_C(100000000000))break;seconds=seconds*10+static_cast<unsigned>(d-'0');}
            max_age=negative || !seconds?1:std::min<std::uint64_t>(now+seconds*10000000,UINT64_C(0x7fffffffffffffff));
        }else if(key=="domain"){std::string d(value);if(d.starts_with('.'))d.erase(0,1);if(!d.empty())domain=lower(std::move(d));}
        else if(key=="path"){if(!value.empty() && value.front()=='/'){c.path=std::string(value);have_path=true;}}
        else if(key=="secure")c.secure=true;
        else if(key=="httponly")c.http_only=true;
    }
    if(max_age)c.expires=*max_age;else if(expires)c.expires=*expires;
    if(!domain.empty()){
        if(!domain_match(url.host,domain) || (domain!=url.host && domain.find('.')==domain.npos))return std::nullopt;
        c.domain=std::move(domain);
    }else{c.domain=url.host;c.host_only=true;}
    if(!have_path)c.path=default_path(url.path);
    if(c.path.size()>1024)return std::nullopt;
    c.created=now;return c;
}
std::string join(const std::vector<HttpCookie>& cookies){std::string out;for(const auto& c:cookies){if(!out.empty())out+="; ";if(!c.name.empty())out+=c.name+"=";out+=c.value;}return out;}
// 各站台決定（主機與上層網域，最具體者優先）優先；否則網際網路區域範本 PRIVACY_TEMPLATE_NO_COOKIES 封鎖。
std::uint32_t policy(const RegistryState& registry,std::string_view host,bool third_party){
    const auto decisions=url_cache::load_cookie_decisions(registry);
    for(std::string_view h=host;!h.empty();){
        const auto found=decisions.find(std::string(h));if(found!=decisions.end())return found->second;
        const auto dot=h.find('.');if(dot==h.npos || ip_host(host))break;h=h.substr(dot+1);
    }
    const auto zones=url_cache::load_privacy(registry);const auto found=zones.find(3U<<1|(third_party?1U:0U));
    return found!=zones.end() && found->second.template_id==0?reject:accept;
}
std::string site(std::string text){
    if(text.starts_with('.'))text.erase(0,1);text=lower(std::move(text));
    if(text.empty() || text.size()>255 || std::any_of(text.begin(),text.end(),[](char c){const auto b=static_cast<unsigned char>(c);return b<0x80 && !((b>='a' && b<='z') || (b>='0' && b<='9') || b=='-' || b=='.' || b=='_');}))return {};
    return text;
}
// 寫入交易：失敗時只還原 Cookie 機碼與容量計數，不複製整個登錄。
struct CookieStore {
    RegistryState& state;RegistryTransaction native;std::optional<std::map<std::u16string,RegistryValue>> before;std::uint64_t bytes{},values{};bool saved{};
    CookieStore(RegistryState& s,const std::filesystem::path& path,RegistryStoreCache* cache):state(s),native(s,path,cache){
        if(native.error)throw CookieError{native.error};const auto found=s.keys.find(url_cache::cookie_index);if(found!=s.keys.end())before=found->second;bytes=s.bytes;values=s.values;
    }
    ~CookieStore(){if(saved)return;if(before)state.keys[url_cache::cookie_index]=std::move(*before);else state.keys.erase(url_cache::cookie_index);state.bytes=bytes;state.values=values;}
    CookieStore(const CookieStore&)=delete;CookieStore& operator=(const CookieStore&)=delete;
    void commit(){if(const auto error=native.commit())throw CookieError{error};saved=true;}
};
}
std::vector<HttpCookie> WindowsServices::http_cookies(std::string_view url,bool http_only,bool third_party){
    const auto target=cookie_url(url);if(!target)throw CookieError{87};
    RegistryTransaction view(runtime_.registry,registry_file_,&registry_store_cache_,true);if(view.error)throw CookieError{view.error};
    if(policy(runtime_.registry,target->host,third_party)==reject)return {};
    const auto now=url_cache::now();auto& session=runtime_.http.session_cookies;std::erase_if(session,[&](const auto& c){return expired(c,now);});
    const auto persistent=url_cache::load_cookies(runtime_.registry);std::vector<HttpCookie> result;
    for(const auto* list:std::array<const std::vector<HttpCookie>*,2>{&session,&persistent})for(const auto& c:*list){
        if(expired(c,now) || (c.host_only?target->host!=c.domain:!domain_match(target->host,c.domain)) || !path_match(target->path,c.path) || (c.secure && !target->secure) || (c.http_only && !http_only))continue;
        result.push_back(c);
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.path.size()!=b.path.size()?a.path.size()>b.path.size():a.created<b.created;});
    return result;
}
std::uint32_t WindowsServices::store_http_cookie(std::string_view url,std::string_view text,bool http_only,bool third_party){
    const auto target=cookie_url(url);if(!target)throw CookieError{87};
    const auto now=url_cache::now();auto cookie=parse_cookie(text,*target,now);if(!cookie)throw CookieError{87};
    if(cookie->http_only && !http_only)throw CookieError{4317};
    CookieStore store(runtime_.registry,registry_file_,&registry_store_cache_);
    if(policy(runtime_.registry,target->host,third_party)==reject)return reject;
    auto persistent=url_cache::load_cookies(runtime_.registry);const auto original=persistent;auto session=runtime_.http.session_cookies;
    const auto same=[&](const HttpCookie& c){return c.name==cookie->name && c.domain==cookie->domain && c.path==cookie->path;};
    for(const auto* list:{&session,&persistent}){const auto found=std::find_if(list->begin(),list->end(),same);if(found!=list->end()){if(found->http_only && !http_only)throw CookieError{4317};cookie->created=found->created;}}
    for(auto* list:{&session,&persistent})std::erase_if(*list,[&](const auto& c){return same(c) || expired(c,now);});
    if(!expired(*cookie,now)){
        // 每個網域最多 50 個 Cookie，持久 Cookie 總數最多 3000 個；超過時移除最舊者。
        const auto evict=[&](auto&& selected){
            HttpCookie* oldest=nullptr;std::vector<HttpCookie>* owner=nullptr;
            for(auto* list:{&session,&persistent})for(auto& c:*list)if(selected(c) && (!oldest || c.created<oldest->created)){oldest=&c;owner=list;}
            if(oldest)owner->erase(owner->begin()+(oldest-owner->data()));
        };
        const auto in_domain=[&](const HttpCookie& c){return c.domain==cookie->domain;};
        while(std::count_if(session.begin(),session.end(),in_domain)+std::count_if(persistent.begin(),persistent.end(),in_domain)>=50)evict(in_domain);
        if(cookie->expires){while(persistent.size()>=3000)evict([&](const HttpCookie& c){return c.expires!=0;});persistent.push_back(*cookie);}
        else session.push_back(*cookie);
    }
    if(persistent!=original){url_cache::store_cookies(runtime_.registry,persistent);store.commit();}
    runtime_.http.session_cookies=std::move(session);
    return accept;
}
void WindowsServices::add_http_cookies(HttpInput& input){
    try{
        const auto text=join(http_cookies(input.url,true,false));if(text.empty())return;
        const auto found=std::find_if(input.headers.begin(),input.headers.end(),[](const auto& field){return lower(field.first)=="cookie";});
        if(found==input.headers.end())input.headers.emplace_back("Cookie",text);else{if(!found->second.empty())found->second+="; ";found->second+=text;}
    }catch(const CookieError&){}catch(const std::invalid_argument&){}catch(const std::length_error&){}catch(const std::bad_alloc&){}
}
void WindowsServices::remember_http_cookies(HttpHandle& request){
    if(!request.inet || request.cache_hit || request.response.error || (request.cache_flags&0x00080000U))return; // INTERNET_FLAG_NO_COOKIES
    const auto& url=request.response.effective_url.empty()?request.input.url:request.response.effective_url;
    for(const auto& [name,value]:request.response.headers){
        if(lower(name)!="set-cookie")continue;
        try{(void)store_http_cookie(url,value,true,false);}catch(const CookieError&){}catch(const std::invalid_argument&){}catch(const std::length_error&){}catch(const std::bad_alloc&){}
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_http_cookies(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const bool privacy=name.starts_with("Privacy");
    const auto finish=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    // Privacy* 以回傳值表示錯誤碼；其他函式回傳 FALSE 並設定 last error。
    const auto fail=[&](std::uint32_t error){if(privacy)return finish(error);last_error_=error;return finish(0);};
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(call(index));};
    const auto read=[&](std::uint64_t at)->std::string{if(!at)return {};return wide?utf16_to_utf8_lossy(read_utf16(memory,GuestAddress{at},65536)):read_ansi(memory,at,65536);};
    const auto encode=[&](std::string_view text){std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(utf8_to_utf16_lossy(text));else{bytes.assign(text.begin(),text.end());bytes.push_back(0);}return bytes;};
    try{
        if(name.starts_with("InternetGetCookie")){
            const bool ex=name.starts_with("InternetGetCookieEx");const auto data=call(2),size_at=call(3);const auto flags=ex?d(4):0U;
            if(!call(0) || !size_at || (ex && call(5)))return fail(87);
            memory.check(GuestAddress{size_at},4,Access::Read);memory.check(GuestAddress{size_at},4,Access::Write);
            auto cookies=http_cookies(read(call(0)),(flags&0x2000U)!=0,(flags&0x10U)!=0); // INTERNET_COOKIE_HTTPONLY／INTERNET_COOKIE_THIRD_PARTY
            if(ex && call(1)){const auto wanted=read(call(1));std::erase_if(cookies,[&](const auto& c){return c.name!=wanted;});}
            const auto text=join(cookies);if(text.empty())return fail(no_more_items);
            const auto bytes=encode(text);const auto characters=static_cast<std::uint32_t>(wide?bytes.size()/2:bytes.size());
            if(!data){memory.store(GuestAddress{size_at},32,bytes.size());return finish(1);}
            if(memory.load(GuestAddress{size_at},32)<characters){memory.store(GuestAddress{size_at},32,bytes.size());return fail(122);}
            memory.write(GuestAddress{data},bytes);memory.store(GuestAddress{size_at},32,characters-1);return finish(1);
        }
        if(name.starts_with("InternetSetCookie")){
            const bool ex=name.starts_with("InternetSetCookieEx");const auto flags=ex?d(3):0U;if(!call(0) || !call(2))return fail(87);
            auto text=read(call(2));if(call(1))text=read(call(1))+"="+text;
            const auto decision=store_http_cookie(read(call(0)),text,(flags&0x2000U)!=0,(flags&0x10U)!=0);
            if(!ex && decision!=accept)return fail(4317);
            return finish(ex?decision:1);
        }
        if(name=="InternetClearAllPerSiteCookieDecisions"){CookieStore store(runtime_.registry,registry_file_,&registry_store_cache_);url_cache::store_cookie_decisions(runtime_.registry,{});store.commit();return finish(1);}
        if(name.starts_with("InternetSetPerSiteCookieDecision")){
            const auto host=site(read(call(0)));const auto decision=d(1);if(host.empty() || (decision && decision!=accept && decision!=reject))return fail(87);
            CookieStore store(runtime_.registry,registry_file_,&registry_store_cache_);auto decisions=url_cache::load_cookie_decisions(runtime_.registry);
            if(decision)decisions[host]=decision;else decisions.erase(host);if(decisions.size()>4096)return fail(8);
            url_cache::store_cookie_decisions(runtime_.registry,decisions);store.commit();return finish(1);
        }
        if(name.starts_with("InternetGetPerSiteCookieDecision")){
            const auto host=site(read(call(0)));if(host.empty() || !call(1))return fail(87);memory.check(GuestAddress{call(1)},4,Access::Write);
            RegistryTransaction view(runtime_.registry,registry_file_,&registry_store_cache_,true);if(view.error)return fail(view.error);
            const auto decisions=url_cache::load_cookie_decisions(runtime_.registry);const auto found=decisions.find(host);if(found==decisions.end())return fail(1168);
            memory.store(GuestAddress{call(1)},32,found->second);return finish(1);
        }
        if(name.starts_with("InternetEnumPerSiteCookieDecision")){
            const auto out=call(0),size_at=call(1),decision_at=call(2);const auto index=d(3);if(!out || !size_at || !decision_at)return fail(87);
            memory.check(GuestAddress{size_at},4,Access::Read);memory.check(GuestAddress{size_at},4,Access::Write);memory.check(GuestAddress{decision_at},4,Access::Write);
            RegistryTransaction view(runtime_.registry,registry_file_,&registry_store_cache_,true);if(view.error)return fail(view.error);
            const auto decisions=url_cache::load_cookie_decisions(runtime_.registry);if(index>=decisions.size())return fail(no_more_items);
            auto entry=decisions.begin();std::advance(entry,index);const auto bytes=encode(entry->first);const auto characters=static_cast<std::uint32_t>(wide?bytes.size()/2:bytes.size());
            if(memory.load(GuestAddress{size_at},32)<characters){memory.store(GuestAddress{size_at},32,characters);return fail(122);}
            memory.write(GuestAddress{out},bytes);memory.store(GuestAddress{size_at},32,characters-1);memory.store(GuestAddress{decision_at},32,entry->second);return finish(1);
        }
        if(name=="PrivacySetZonePreferenceW"){
            const auto zone=d(0),type=d(1),id=d(2);if(zone>4 || type>1 || (id>5 && id!=100 && id!=101))return fail(87);
            url_cache::PrivacyPreference preference{id,{}};if(id>=100 && call(3))preference.preference=read_utf16(memory,GuestAddress{call(3)},32768);
            CookieStore store(runtime_.registry,registry_file_,&registry_store_cache_);auto zones=url_cache::load_privacy(runtime_.registry);
            zones[zone<<1|type]=std::move(preference);url_cache::store_privacy(runtime_.registry,zones);store.commit();return finish(0);
        }
        if(name=="PrivacyGetZonePreferenceW"){
            const auto zone=d(0),type=d(1);if(zone>4 || type>1 || (call(3) && !call(4)))return fail(87);
            RegistryTransaction view(runtime_.registry,registry_file_,&registry_store_cache_,true);if(view.error)return fail(view.error);
            const auto zones=url_cache::load_privacy(runtime_.registry);const auto found=zones.find(zone<<1|type);
            const auto preference=found==zones.end()?url_cache::PrivacyPreference{}:found->second;
            if(call(2))memory.store(GuestAddress{call(2)},32,preference.template_id);
            if(call(4)){
                memory.check(GuestAddress{call(4)},4,Access::Read);memory.check(GuestAddress{call(4)},4,Access::Write);
                const auto needed=static_cast<std::uint32_t>(preference.preference.size()+1);const auto capacity=memory.load(GuestAddress{call(4)},32);memory.store(GuestAddress{call(4)},32,needed);
                if(call(3)){if(capacity<needed)return fail(234);memory.write(GuestAddress{call(3)},utf16_bytes(preference.preference));} // ERROR_MORE_DATA
            }
            return finish(0);
        }
        return fail(120);
    }catch(const CookieError& error){return fail(error.code);}
    catch(const MemoryFault&){return fail(998);}catch(const std::invalid_argument&){return fail(13);}catch(const std::length_error&){return fail(8);}catch(const std::bad_alloc&){return fail(8);}
}
}
