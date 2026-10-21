#include "x64arm/windows_services.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
// urlmon 的 IInternetSecurityManager：URL 對應 Internet Explorer 安全區域。
// 本機檔案為「本機電腦」，UNC、不含點的主機名稱與 localhost 為「近端內部網路」，其餘網址為「網際網路」；
// HKCU／HKLM 的 Internet Settings\ZoneMap\Domains 指定優先。
namespace {
constexpr std::uint32_t local_machine=0,intranet=1,internet=3,restricted=4;
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_interface=0x80004002U,no_memory=0x8007000eU;
constexpr std::uint32_t default_action=0x800c0011U,insufficient=0x8007007aU;
std::u16string lower(std::u16string_view text){std::u16string result(text);for(auto& c:result)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return result;}
struct ParsedUrl {std::u16string scheme,host;bool local{},unc{};};
ParsedUrl parse(std::u16string_view url){
    ParsedUrl result;
    if(url.size()>=2 && ((url[0]>=u'A' && url[0]<=u'Z') || (url[0]>=u'a' && url[0]<=u'z')) && url[1]==u':'){result.scheme=u"file";result.local=true;return result;}
    if(url.starts_with(u"\\\\")){result.scheme=u"file";result.unc=true;const auto end=url.find_first_of(u"\\/",2);result.host=lower(url.substr(2,end==url.npos?url.npos:end-2));return result;}
    const auto colon=url.find(u':');if(colon==url.npos || !colon){result.scheme={};return result;}
    result.scheme=lower(url.substr(0,colon));auto rest=url.substr(colon+1);
    if(rest.starts_with(u"//")){
        rest.remove_prefix(2);const auto end=rest.find_first_of(u"/\\?#");auto authority=rest.substr(0,end);
        if(const auto at=authority.rfind(u'@');at!=authority.npos)authority.remove_prefix(at+1);
        if(authority.starts_with(u"[")){const auto close=authority.find(u']');result.host=lower(authority.substr(0,close==authority.npos?authority.npos:close+1));}
        else {const auto port=authority.find(u':');result.host=lower(authority.substr(0,port));}
    }
    if(result.scheme==u"file"){if(result.host.empty() || result.host==u"localhost")result.local=true;else result.unc=true;}
    return result;
}
}
std::span<const WindowsServiceDescriptor> url_security_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"urlmon.dll","CoInternetCreateSecurityManager",3},
        {"internal-url-security","QueryInterface",3},{"internal-url-security","AddRef",1},{"internal-url-security","Release",1},
        {"internal-url-security","SetSecuritySite",2},{"internal-url-security","GetSecuritySite",2},{"internal-url-security","MapUrlToZone",4},
        {"internal-url-security","GetSecurityId",5},{"internal-url-security","ProcessUrlAction",9},{"internal-url-security","QueryCustomPolicy",8},
        {"internal-url-security","SetZoneMapping",4},{"internal-url-security","GetZoneMappings",4}
    };return table;
}
std::uint32_t WindowsServices::url_zone(std::u16string_view url){
    const auto parsed=parse(url);
    if(parsed.local)return local_machine;
    if(parsed.scheme==u"about" || parsed.scheme==u"res" || parsed.scheme==u"mk" || parsed.scheme==u"its" || parsed.scheme==u"ms-its")return local_machine;
    // ZoneMap\Domains：自最長網域往上找，值名稱為 scheme 或 "*"。
    if(!parsed.host.empty() && !parsed.host.starts_with(u"[")){
        if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
        if(!transaction.error){
            initialize_registry_views(runtime_.registry);
            // 網域鍵為最後兩段（example.com），其下的子鍵為其餘前綴（www），另可用 "*" 子鍵涵蓋所有子網域。
            std::u16string domain=parsed.host,sub;
            if(const auto last=parsed.host.rfind(u'.');last!=parsed.host.npos && last>0)
                if(const auto second=parsed.host.rfind(u'.',last-1);second!=parsed.host.npos){domain=parsed.host.substr(second+1);sub=parsed.host.substr(0,second);}
            for(const auto hive:{1U,2U}){
                const auto root=std::u16string(1,static_cast<char16_t>(hive))+u"\\software\\microsoft\\windows\\currentversion\\internet settings\\zonemap\\domains\\"+domain;
                std::vector<std::u16string> paths;if(!sub.empty())paths.push_back(root+u"\\"+sub);paths.push_back(root+u"\\*");paths.push_back(root);
                for(const auto& path:paths){
                    const auto key=runtime_.registry.keys.find(registry_view_path(path,registry_view(0,x86_)));if(key==runtime_.registry.keys.end())continue;
                    for(const auto& name:{parsed.scheme,std::u16string{u"*"}}){
                        const auto value=key->second.find(name);
                        if(value!=key->second.end() && value->second.type==4 && value->second.bytes.size()==4){
                            const auto zone=std::uint32_t{value->second.bytes[0]}|(std::uint32_t{value->second.bytes[1]}<<8)|(std::uint32_t{value->second.bytes[2]}<<16)|(std::uint32_t{value->second.bytes[3]}<<24);
                            if(zone<=restricted)return zone;
                        }
                    }
                }
            }
        }
    }
    if(parsed.unc)return intranet;
    if(parsed.host==u"localhost" || parsed.host==u"127.0.0.1" || parsed.host==u"[::1]")return intranet;
    if(!parsed.host.empty() && parsed.host.find(u'.')==parsed.host.npos && !parsed.host.starts_with(u"["))return intranet; // PlainHostName 規則。
    return internet;
}
std::optional<std::uint32_t> WindowsServices::invoke_url_security(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try{
        if(name=="CoInternetCreateSecurityManager"){
            if(!arg(1))return finish(pointer_error);memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.store(GuestAddress{arg(1)},bits,0);
            if(arg(2))return finish(invalid);
            if(!url_security_table_){
                const auto table=runtime_allocate(memory,{},11*unit);unsigned i=0;
                for(const auto& row:url_security_service_descriptors())if(row.family=="internal-url-security")memory.store(GuestAddress{table+unit*i++},bits,resolve(row.family,row.symbol).value);
                url_security_table_=table;
            }
            if(url_security_managers_.size()>=4096)return finish(no_memory);
            const auto object=allocate_heap(memory,unit);if(!object)return finish(no_memory);memory.store(GuestAddress{object},bits,url_security_table_);
            url_security_managers_.emplace(object,UrlSecurityManager{1,0});memory.store(GuestAddress{arg(1)},bits,object);return finish(0);
        }
        const auto found=url_security_managers_.find(arg(0));if(found==url_security_managers_.end())return finish(invalid);auto& manager=found->second;
        if(name=="QueryInterface"){
            if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return finish(pointer_error);
            std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);
            constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            constexpr std::array<std::uint8_t,16> security{0xee,0xc5,0x9e,0x79,0x41,0xd1,0xd0,0x11,0x8c,0xa2,0x00,0xa0,0xc9,0x0f,0xff,0xc0};
            if(iid!=unknown && iid!=security)return finish(no_interface);
            ++manager.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);
        }
        if(name=="AddRef")return finish(++manager.references);
        if(name=="Release"){
            const auto count=--manager.references;
            if(!count){const auto site=manager.site;url_security_managers_.erase(found);(void)free_heap(memory,arg(0),state);
                if(site){const auto table=memory.load(GuestAddress{site},bits);start_callbacks(state,memory,{{memory.load(GuestAddress{table+2*unit},bits),{site}}},Continuation::Return,0);return std::nullopt;}}
            return finish(count);
        }
        if(name=="SetSecuritySite"){
            // 保存 IInternetSecurityMgrSite 參照；舊的參照釋放。
            const auto previous=manager.site;manager.site=arg(1);std::vector<GuestCallback> callbacks;
            if(arg(1)){const auto table=memory.load(GuestAddress{arg(1)},bits);callbacks.push_back({memory.load(GuestAddress{table+unit},bits),{arg(1)}});}
            if(previous){const auto table=memory.load(GuestAddress{previous},bits);callbacks.push_back({memory.load(GuestAddress{table+2*unit},bits),{previous}});}
            if(callbacks.empty())return finish(0);start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
        }
        if(name=="GetSecuritySite"){
            if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},bits,manager.site);if(!manager.site)return finish(0);
            const auto table=memory.load(GuestAddress{manager.site},bits);start_callbacks(state,memory,{{memory.load(GuestAddress{table+unit},bits),{manager.site}}},Continuation::Return,0);return std::nullopt;
        }
        if(name=="QueryCustomPolicy")return finish(default_action);
        if(name=="SetZoneMapping" || name=="GetZoneMappings")return finish(0x80004001U); // 尚未提供 ZoneMap 寫入與 IEnumString 列舉。
        if(!arg(1))return finish(invalid);
        const auto url=read_utf16(memory,GuestAddress{arg(1)},2084);if(url.empty())return finish(invalid);
        const auto zone=url_zone(url);
        if(name=="MapUrlToZone"){if(!arg(2))return finish(invalid);memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,zone);return finish(0);}
        if(name=="GetSecurityId"){
            // 安全識別：scheme:host 位元組後接 4 bytes 區域。
            if(!arg(3))return finish(invalid);memory.check(GuestAddress{arg(3)},4,Access::Write);
            const auto parsed=parse(url);const auto text=utf16_to_utf8((parsed.scheme.empty()?u"file":parsed.scheme)+u":"+parsed.host);
            std::vector<std::uint8_t> bytes(text.begin(),text.end());for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(zone>>(i*8)));
            const auto capacity=memory.load(GuestAddress{arg(3)},32);memory.store(GuestAddress{arg(3)},32,bytes.size());
            if(capacity<bytes.size() || !arg(2))return finish(insufficient);
            memory.write(GuestAddress{arg(2)},bytes);return finish(0);
        }
        // ProcessUrlAction：各區域的預設政策；允許回 S_OK，需詢問或禁止回 S_FALSE。
        const auto action=static_cast<std::uint32_t>(arg(2));const auto capacity=static_cast<std::uint32_t>(arg(4));
        std::uint32_t policy=0;
        if(zone==restricted)policy=3;
        else if(zone==internet){
            if(action==0x1806 || action==0x1601 || action==0x2100)policy=1;           // 高風險執行、ActiveX 等：詢問
            else if(action>=0x1200 && action<0x1300)policy=3;                           // 未簽章 ActiveX：禁止
        }
        if(arg(3)){if(capacity<4)return finish(invalid);memory.check(GuestAddress{arg(3)},4,Access::Write);memory.store(GuestAddress{arg(3)},32,policy);}
        return finish(policy==0?0U:1U);
    }catch(const MemoryFault&){return finish(pointer_error);}catch(const std::bad_alloc&){return finish(no_memory);}
}
}
