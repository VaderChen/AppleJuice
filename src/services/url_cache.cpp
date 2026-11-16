#include "x64arm/url_cache.hpp"
#include "x64arm/unicode.hpp"
#include <chrono>
#include <set>
#include <stdexcept>
namespace x64arm::url_cache {
namespace {
constexpr std::size_t limit=1024U*1024U;
void number(std::vector<std::uint8_t>& bytes,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
void text(std::vector<std::uint8_t>& bytes,std::u16string_view value){if(value.size()>32768)throw std::length_error("快取字串過長");number(bytes,value.size(),4);for(auto c:value)number(bytes,c,2);}
struct Reader {
    std::span<const std::uint8_t> bytes;std::size_t at{};
    std::uint64_t number(unsigned width){if(width>bytes.size()-at)throw std::invalid_argument("快取索引截斷");std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t{bytes[at++]}<<(8*i);return value;}
    std::u16string text(){const auto size=number(4);if(size>32768 || size>(bytes.size()-at)/2)throw std::invalid_argument("快取索引字串無效");std::u16string value;for(std::uint64_t i=0;i<size;++i)value.push_back(static_cast<char16_t>(number(2)));return value;}
    std::string raw(){const auto size=number(4);if(size>32768 || size>bytes.size()-at)throw std::invalid_argument("快取索引字串無效");std::string value(reinterpret_cast<const char*>(bytes.data()+at),static_cast<std::size_t>(size));at+=static_cast<std::size_t>(size);return value;}
};
void raw(std::vector<std::uint8_t>& bytes,std::string_view value){if(value.size()>32768)throw std::length_error("快取字串過長");number(bytes,value.size(),4);bytes.insert(bytes.end(),value.begin(),value.end());}
// Cookie 機碼中的單一值；空內容即刪除。容量與數量計入整個登錄的上限。
void put_value(RegistryState& state,const std::u16string& key_path,std::u16string_view name,std::vector<std::uint8_t> bytes,bool erase){
    auto& values=state.keys[key_path];const auto found=values.find(std::u16string(name));const auto previous=found==values.end()?0:found->second.bytes.size();
    if(erase){if(found!=values.end()){state.bytes-=previous;--state.values;values.erase(found);}}
    else{if(bytes.size()>limit || state.bytes-previous+bytes.size()>16U*1024U*1024U || (found==values.end() && state.values>=4096))throw std::length_error("Cookie 儲存容量不足");state.bytes=state.bytes-previous+bytes.size();if(found==values.end())++state.values;values[std::u16string(name)]={3,std::move(bytes),{}};}
    state.modified_times[key_path]=now();
}
const RegistryValue* get_value(const RegistryState& state,const std::u16string& key_path,std::u16string_view name){const auto key=state.keys.find(key_path);if(key==state.keys.end())return nullptr;const auto found=key->second.find(std::u16string(name));if(found==key->second.end())return nullptr;if(found->second.type!=3)throw std::invalid_argument("Cookie 儲存型別無效");return &found->second;}
constexpr std::uint64_t metadata_magic=UINT64_C(0x3141544d43435834),cookie_magic=UINT64_C(0x31494b4f4f433458),decision_magic=UINT64_C(0x3143454453435834),privacy_magic=UINT64_C(0x3156495250435834);
}
std::uint64_t now(){return UINT64_C(116444736000000000)+static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()/100);}
std::u16string key(std::u16string_view url){if(url.empty() || url.size()>4096)throw std::invalid_argument("快取 URL 無效");constexpr char16_t digits[]=u"0123456789abcdef";std::u16string name;for(auto c:url)for(int shift=12;shift>=0;shift-=4)name.push_back(digits[(c>>shift)&15]);return name;}
std::vector<std::uint8_t> encode(const Entry& e){
    if(e.headers.size()>limit || e.leases.size()>1024)throw std::length_error("快取索引超過上限");std::vector<std::uint8_t> bytes;number(bytes,UINT64_C(0x3148434341523458),8);text(bytes,e.url);text(bytes,e.file);text(bytes,e.redirect);text(bytes,e.extension);
    for(auto value:{e.size,e.modified,e.expires,e.accessed,e.synced})number(bytes,value,8);for(auto value:{e.type,e.hits,e.exemption})number(bytes,value,4);
    number(bytes,(e.header_wide?1U:0U)|(e.deleting?2U:0U),4);number(bytes,e.headers.size(),4);bytes.insert(bytes.end(),e.headers.begin(),e.headers.end());number(bytes,e.leases.size(),4);for(const auto& lease:e.leases){number(bytes,lease.id,8);number(bytes,lease.until,8);number(bytes,lease.pid,4);}return bytes;
}
Entry decode(std::span<const std::uint8_t> bytes){
    Reader r{bytes};if(r.number(8)!=UINT64_C(0x3148434341523458))throw std::invalid_argument("未知快取索引");Entry e;e.url=r.text();e.file=r.text();e.redirect=r.text();e.extension=r.text();e.size=r.number(8);e.modified=r.number(8);e.expires=r.number(8);e.accessed=r.number(8);e.synced=r.number(8);e.type=static_cast<std::uint32_t>(r.number(4));e.hits=static_cast<std::uint32_t>(r.number(4));e.exemption=static_cast<std::uint32_t>(r.number(4));const auto flags=r.number(4);if(flags&~3U)throw std::invalid_argument("快取索引旗標無效");e.header_wide=(flags&1)!=0;e.deleting=(flags&2)!=0;
    const auto size=r.number(4);if(size>limit || size>bytes.size()-r.at || (e.header_wide && size%2))throw std::invalid_argument("快取 header 無效");e.headers.assign(bytes.begin()+static_cast<std::ptrdiff_t>(r.at),bytes.begin()+static_cast<std::ptrdiff_t>(r.at+size));r.at+=static_cast<std::size_t>(size);
    const auto count=r.number(4);if(count>1024)throw std::invalid_argument("快取引用過多");for(std::uint64_t i=0;i<count;++i){Lease l;l.id=r.number(8);l.until=r.number(8);l.pid=static_cast<std::uint32_t>(r.number(4));e.leases.push_back(l);}if(r.at!=bytes.size() || e.url.empty())throw std::invalid_argument("快取索引有未解析資料");return e;
}
std::optional<Entry> get(const RegistryState& state,std::u16string_view url,bool redirects){
    const auto index_at=state.keys.find(index);if(index_at==state.keys.end())return {};std::set<std::u16string> seen;std::u16string target(url);
    for(unsigned depth=0;depth<16;++depth){if(!seen.insert(target).second)throw std::invalid_argument("快取轉址循環");const auto found=index_at->second.find(key(target));if(found==index_at->second.end())return {};if(found->second.type!=3)throw std::invalid_argument("快取索引型別無效");auto e=decode(found->second.bytes);if(e.url!=target)throw std::invalid_argument("快取索引名稱不符");if(redirects && !e.redirect.empty()){target=e.redirect;continue;}return e;}throw std::invalid_argument("快取轉址過深");
}
void put(RegistryState& state,const Entry& e){auto bytes=encode(e);auto& values=state.keys[index];const auto name=key(e.url);const auto found=values.find(name);const auto previous=found==values.end()?0:found->second.bytes.size();if(state.bytes-previous+bytes.size()>16U*1024U*1024U || (found==values.end() && state.values>=4096))throw std::length_error("快取索引容量不足");state.bytes=state.bytes-previous+bytes.size();if(found==values.end())++state.values;values[name]={3,std::move(bytes),{}};state.modified_times[index]=now();}
void erase(RegistryState& state,std::u16string_view url){const auto found=state.keys.find(index);if(found==state.keys.end())return;const auto value=found->second.find(key(url));if(value!=found->second.end()){state.bytes-=value->second.bytes.size();--state.values;found->second.erase(value);state.modified_times[index]=now();}}
std::vector<std::uint8_t> information(const Entry& e,bool wide,bool x86,std::uint64_t base){
    const unsigned unit=x86?4:8,width=wide?2:1,fixed=x86?80:112;std::vector<std::uint8_t> bytes(fixed);const auto set=[&](unsigned at,std::uint64_t value,unsigned n=4){for(unsigned i=0;i<n;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
    const auto append=[&](unsigned cell,std::span<const std::uint8_t> data,unsigned zeros){if(data.empty())return;while(bytes.size()%width)bytes.push_back(0);set(cell,base+bytes.size(),unit);bytes.insert(bytes.end(),data.begin(),data.end());bytes.insert(bytes.end(),zeros,0);};
    const auto add_text=[&](unsigned cell,std::u16string_view value){if(value.empty())return;std::vector<std::uint8_t> data;if(wide)data=utf16_bytes(value);else{auto s=utf16_to_utf8(value);data.assign(s.begin(),s.end());data.push_back(0);}append(cell,data,0);};
    set(0,fixed);add_text(x86?4:8,e.url);add_text(x86?8:16,e.file);const unsigned type=x86?12:24;set(type,e.type);set(type+4,e.leases.size());set(type+8,e.hits);set(type+12,e.size);set(type+16,e.size>>32);const unsigned times=x86?32:44;set(times,e.modified,8);set(times+8,e.expires,8);set(times+16,e.accessed,8);set(times+24,e.synced,8);
    auto header=e.headers;if(wide!=e.header_wide){header.clear();if(wide)for(auto c:e.headers){header.push_back(c);header.push_back(0);}else{std::u16string text;for(std::size_t i=0;i<e.headers.size();i+=2)text.push_back(static_cast<char16_t>(e.headers[i]|static_cast<unsigned>(e.headers[i+1])<<8));const auto s=utf16_to_utf8(text);header.assign(s.begin(),s.end());}}
    append(x86?64:80,header,width);set(x86?68:88,header.size()/width);add_text(x86?72:96,e.extension);set(x86?76:104,e.exemption);return bytes;
}
std::vector<HttpCookie> load_cookies(const RegistryState& state){
    std::vector<HttpCookie> cookies;const auto* value=get_value(state,cookie_index,u"cookies");if(!value)return cookies;Reader r{value->bytes};if(r.number(8)!=cookie_magic)throw std::invalid_argument("未知 Cookie 儲存");
    const auto count=r.number(4);if(count>4096)throw std::invalid_argument("Cookie 過多");
    for(std::uint64_t i=0;i<count;++i){HttpCookie c;c.name=r.raw();c.value=r.raw();c.domain=r.raw();c.path=r.raw();c.expires=r.number(8);c.created=r.number(8);const auto flags=r.number(4);c.host_only=flags&1;c.secure=flags&2;c.http_only=flags&4;cookies.push_back(std::move(c));}
    return cookies;
}
void store_cookies(RegistryState& state,const std::vector<HttpCookie>& cookies){
    std::vector<std::uint8_t> bytes;number(bytes,cookie_magic,8);number(bytes,cookies.size(),4);
    for(const auto& c:cookies){raw(bytes,c.name);raw(bytes,c.value);raw(bytes,c.domain);raw(bytes,c.path);number(bytes,c.expires,8);number(bytes,c.created,8);number(bytes,(c.host_only?1U:0U)|(c.secure?2U:0U)|(c.http_only?4U:0U),4);}
    put_value(state,cookie_index,u"cookies",std::move(bytes),cookies.empty());
}
std::map<std::string,std::uint32_t> load_cookie_decisions(const RegistryState& state){
    std::map<std::string,std::uint32_t> decisions;const auto* value=get_value(state,cookie_index,u"decisions");if(!value)return decisions;Reader r{value->bytes};if(r.number(8)!=decision_magic)throw std::invalid_argument("未知 Cookie 決定");
    const auto count=r.number(4);if(count>4096)throw std::invalid_argument("Cookie 決定過多");for(std::uint64_t i=0;i<count;++i){auto host=r.raw();decisions[std::move(host)]=static_cast<std::uint32_t>(r.number(4));}
    return decisions;
}
void store_cookie_decisions(RegistryState& state,const std::map<std::string,std::uint32_t>& decisions){
    std::vector<std::uint8_t> bytes;number(bytes,decision_magic,8);number(bytes,decisions.size(),4);for(const auto& [host,decision]:decisions){raw(bytes,host);number(bytes,decision,4);}
    put_value(state,cookie_index,u"decisions",std::move(bytes),decisions.empty());
}
std::map<std::uint32_t,PrivacyPreference> load_privacy(const RegistryState& state){
    std::map<std::uint32_t,PrivacyPreference> zones;const auto* value=get_value(state,cookie_index,u"privacy");if(!value)return zones;Reader r{value->bytes};if(r.number(8)!=privacy_magic)throw std::invalid_argument("未知隱私設定");
    const auto count=r.number(4);if(count>64)throw std::invalid_argument("隱私設定過多");for(std::uint64_t i=0;i<count;++i){const auto key=static_cast<std::uint32_t>(r.number(4));PrivacyPreference p;p.template_id=static_cast<std::uint32_t>(r.number(4));p.preference=r.text();zones[key]=std::move(p);}
    return zones;
}
void store_privacy(RegistryState& state,const std::map<std::uint32_t,PrivacyPreference>& zones){
    std::vector<std::uint8_t> bytes;number(bytes,privacy_magic,8);number(bytes,zones.size(),4);for(const auto& [key,p]:zones){number(bytes,key,4);number(bytes,p.template_id,4);text(bytes,p.preference);}
    put_value(state,cookie_index,u"privacy",std::move(bytes),zones.empty());
}
Metadata load_metadata(const RegistryState& state){
    Metadata m;const auto* value=get_value(state,metadata_index,u"state");if(!value)return m;Reader r{value->bytes};if(r.number(8)!=metadata_magic)throw std::invalid_argument("未知快取中繼資料");
    m.next_group=r.number(8);m.quota=static_cast<std::uint32_t>(r.number(4));m.sync_mode=static_cast<std::uint32_t>(r.number(4));for(auto& item:m.header)item=static_cast<std::uint32_t>(r.number(4));
    const auto groups=r.number(4);if(groups>4096)throw std::invalid_argument("快取群組過多");
    for(std::uint64_t i=0;i<groups;++i){Group g;g.id=r.number(8);g.flags=static_cast<std::uint32_t>(r.number(4));g.type=static_cast<std::uint32_t>(r.number(4));g.quota=static_cast<std::uint32_t>(r.number(4));for(auto& item:g.owner)item=static_cast<std::uint32_t>(r.number(4));g.name=r.text();
        const auto urls=r.number(4);if(urls>65536)throw std::invalid_argument("快取群組成員過多");for(std::uint64_t k=0;k<urls;++k)g.urls.push_back(r.text());m.groups.push_back(std::move(g));}
    const auto containers=r.number(4);if(containers>256)throw std::invalid_argument("快取容器過多");
    for(std::uint64_t i=0;i<containers;++i){Container c;c.name=r.text();c.prefix=r.text();c.path=r.text();c.limit=static_cast<std::uint32_t>(r.number(4));c.type=static_cast<std::uint32_t>(r.number(4));c.options=static_cast<std::uint32_t>(r.number(4));m.containers.push_back(std::move(c));}
    if(r.at!=value->bytes.size())throw std::invalid_argument("快取中繼資料多餘位元組");return m;
}
void store_metadata(RegistryState& state,const Metadata& m){
    std::vector<std::uint8_t> bytes;number(bytes,metadata_magic,8);number(bytes,m.next_group,8);number(bytes,m.quota,4);number(bytes,m.sync_mode,4);for(const auto item:m.header)number(bytes,item,4);
    number(bytes,m.groups.size(),4);for(const auto& g:m.groups){number(bytes,g.id,8);number(bytes,g.flags,4);number(bytes,g.type,4);number(bytes,g.quota,4);for(const auto item:g.owner)number(bytes,item,4);text(bytes,g.name);number(bytes,g.urls.size(),4);for(const auto& url:g.urls)text(bytes,url);}
    number(bytes,m.containers.size(),4);for(const auto& c:m.containers){text(bytes,c.name);text(bytes,c.prefix);text(bytes,c.path);number(bytes,c.limit,4);number(bytes,c.type,4);number(bytes,c.options,4);}
    put_value(state,metadata_index,u"state",std::move(bytes),false);
}
}
