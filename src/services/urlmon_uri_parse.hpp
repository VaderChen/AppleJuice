#pragma once
// urlmon 的 IUri 模型：把 URI 字串拆成 RFC 3986 的元件並依 Uri_CREATE_* 旗標正規化；IUri、IUriBuilder 與 CoInternetParseIUri／CombineIUri 共用。
// 已知 scheme 的編號、預設連接埠與是否為階層式（有 authority）依 URL_SCHEME；HTTP／HTTPS／FTP／FILE 的反斜線視為斜線。
#include "x64arm/unicode.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace x64arm::uri_model {
enum : std::uint32_t {
    allow_relative=0x1,allow_implicit_wildcard=0x2,allow_implicit_file=0x4,no_fragment=0x8,no_canonicalize=0x10,canonicalize=0x100,
    file_use_dos_path=0x20,no_encode_forbidden=0x8000,
};
struct Scheme {std::u16string_view name;std::uint32_t id,port;bool hierarchical;};
inline constexpr std::array<Scheme,26> schemes{{
    {u"ftp",1,21,true},{u"http",2,80,true},{u"gopher",3,70,true},{u"mailto",4,0,false},{u"news",5,0,false},{u"nntp",6,119,true},{u"telnet",7,23,true},
    {u"wais",8,210,true},{u"file",9,0,true},{u"mk",10,0,false},{u"https",11,443,true},{u"shell",12,0,false},{u"snews",13,563,false},{u"local",14,0,false},
    {u"javascript",15,0,false},{u"vbscript",16,0,false},{u"about",17,0,false},{u"res",18,0,false},{u"ms-shell-rooted",19,0,false},{u"ms-shell-idlist",20,0,false},
    {u"hcp",21,0,false},{u"ms-shell-device",22,0,false},{u"*",23,0,true},{u"search-ms",24,0,false},{u"search",25,0,false},{u"knownfolder",26,0,false}}};
inline const Scheme* find_scheme(std::u16string_view name){for(const auto& s:schemes)if(s.name==name)return &s;return nullptr;}
struct Uri {
    std::u16string raw,scheme,user,password,host,path,query,fragment; // query 含「?」、fragment 含「#」
    std::uint32_t scheme_id{},host_type{},port{},flags{};
    bool relative{},authority{},has_user{},has_password{},has_port{},explicit_port{};
    bool operator==(const Uri&) const = default;
};
inline char16_t lower(char16_t c){return c>=u'A' && c<=u'Z'?static_cast<char16_t>(c+32):c;}
inline std::u16string lowered(std::u16string_view text){std::u16string out(text);for(auto& c:out)c=lower(c);return out;}
inline bool unreserved(char16_t c){return (c>=u'a' && c<=u'z') || (c>=u'A' && c<=u'Z') || (c>=u'0' && c<=u'9') || c==u'-' || c==u'.' || c==u'_' || c==u'~';}
inline int hex(char16_t c){return c>=u'0' && c<=u'9'?c-u'0':c>=u'a' && c<=u'f'?c-u'a'+10:c>=u'A' && c<=u'F'?c-u'A'+10:-1;}
// 百分比編碼：解碼代表非保留字元者、其餘轉大寫；禁止字元（空白、控制字元、"<>`{}|^ 與非 ASCII）以 UTF-8 編碼。
inline std::u16string normalize_escapes(std::u16string_view text,bool decode,bool encode){
    constexpr char16_t digits[]=u"0123456789ABCDEF";std::u16string out;
    for(std::size_t i=0;i<text.size();++i){
        const auto c=text[i];
        if(c==u'%' && i+2<text.size() && hex(text[i+1])>=0 && hex(text[i+2])>=0){
            const auto value=static_cast<char16_t>(hex(text[i+1])*16+hex(text[i+2]));
            if(decode && unreserved(value))out.push_back(value);else{out.push_back(u'%');out.push_back(digits[value>>4]);out.push_back(digits[value&15]);}
            i+=2;continue;
        }
        const bool forbidden=c<=0x20 || c==0x7f || c==u'"' || c==u'<' || c==u'>' || c==u'`' || c==u'{' || c==u'}' || c==u'|' || c==u'^' || c>=0x80;
        if(encode && forbidden){
            std::u16string one(1,c);if(c>=0xd800 && c<=0xdbff && i+1<text.size()){one.push_back(text[i+1]);++i;}
            for(const auto byte:utf16_to_utf8(one)){const auto b=static_cast<unsigned char>(byte);out.push_back(u'%');out.push_back(digits[b>>4]);out.push_back(digits[b&15]);}
        }else out.push_back(c);
    }
    return out;
}
// RFC 3986 5.2.4 去除點區段。
inline std::u16string remove_dots(std::u16string_view input){
    std::u16string in(input),out;
    while(!in.empty()){
        if(in.starts_with(u"../"))in.erase(0,3);else if(in.starts_with(u"./"))in.erase(0,2);
        else if(in.starts_with(u"/./"))in.erase(0,2);else if(in==u"/.")in=u"/";
        else if(in.starts_with(u"/../") || in==u"/.."){in=in.size()==3?std::u16string(u"/"):in.substr(3);const auto at=out.find_last_of(u'/');out.erase(at==out.npos?0:at);}
        else if(in==u"." || in==u".."){in.clear();}
        else{const auto next=in.find(u'/',in[0]==u'/'?1:0);out+=in.substr(0,next);in.erase(0,next==in.npos?in.size():next);}
    }
    return out;
}
inline bool ipv4(std::u16string_view host){
    unsigned parts=0,value=0,count=0;for(const auto c:host){if(c==u'.'){if(!count || ++parts>3)return false;value=count=0;continue;}if(c<u'0' || c>u'9')return false;value=value*10+(c-u'0');if(++count>3 || value>255)return false;}
    return count && parts==3;
}
inline std::optional<Uri> parse(std::u16string_view input,std::uint32_t flags){
    Uri uri;uri.raw=std::u16string(input);uri.flags=flags;
    std::size_t begin=0,end=input.size();while(begin<end && input[begin]<=0x20)++begin;while(end>begin && input[end-1]<=0x20)--end;
    std::u16string text(input.substr(begin,end-begin));if(text.empty())return std::nullopt;
    // 隱含 file：磁碟路徑或 UNC。
    const bool drive=text.size()>=3 && ((text[0]>=u'A' && text[0]<=u'Z') || (text[0]>=u'a' && text[0]<=u'z')) && text[1]==u':' && (text[2]==u'\\' || text[2]==u'/');
    if((flags&allow_implicit_file) && drive){for(auto& c:text)if(c==u'\\')c=u'/';text=u"file:///"+text;}
    else if((flags&allow_implicit_file) && text.starts_with(u"\\\\")){for(auto& c:text)if(c==u'\\')c=u'/';text=u"file:"+text;}
    std::size_t colon=text.find(u':');bool scheme_ok=colon!=text.npos && colon>=2 && ((text[0]>=u'A' && text[0]<=u'Z') || (text[0]>=u'a' && text[0]<=u'z'));
    for(std::size_t i=0;scheme_ok && i<colon;++i){const auto c=text[i];scheme_ok=(c>=u'A' && c<=u'Z') || (c>=u'a' && c<=u'z') || (c>=u'0' && c<=u'9') || c==u'+' || c==u'-' || c==u'.';}
    if(drive && !(flags&allow_implicit_file))return std::nullopt;
    std::u16string rest;
    if(scheme_ok){uri.scheme=lowered(text.substr(0,colon));rest=text.substr(colon+1);}
    else if(flags&allow_relative){uri.relative=true;rest=text;}
    else if(flags&allow_implicit_wildcard){uri.scheme=u"*";rest=u"//"+text;}
    else return std::nullopt;
    const auto* known=uri.relative?nullptr:find_scheme(uri.scheme);uri.scheme_id=uri.relative?0:known?known->id:0;
    const bool slashes=known && (known->id==1 || known->id==2 || known->id==9 || known->id==11);
    if(slashes){std::size_t stop=rest.find_first_of(u"?#");if(stop==rest.npos)stop=rest.size();for(std::size_t i=0;i<stop;++i)if(rest[i]==u'\\')rest[i]=u'/';}
    if(known && known->hierarchical && known->id!=23 && !rest.starts_with(u"//") && !uri.relative)return std::nullopt;
    if(!uri.relative && rest.starts_with(u"//")){
        uri.authority=true;rest.erase(0,2);const auto stop=rest.find_first_of(u"/?#");auto authority=rest.substr(0,stop);rest.erase(0,stop==rest.npos?rest.size():stop);
        if(const auto at=authority.rfind(u'@');at!=authority.npos){
            const auto info=authority.substr(0,at);authority.erase(0,at+1);const auto split=info.find(u':');uri.has_user=true;uri.user=info.substr(0,split);
            if(split!=info.npos){uri.has_password=true;uri.password=info.substr(split+1);}
        }
        std::u16string port;
        if(authority.starts_with(u"[")){const auto close=authority.find(u']');if(close==authority.npos)return std::nullopt;uri.host=lowered(authority.substr(1,close-1));uri.host_type=3;
            if(close+1<authority.size()){if(authority[close+1]!=u':')return std::nullopt;port=authority.substr(close+2);}}
        else{const auto split=authority.rfind(u':');uri.host=authority.substr(0,split);if(split!=authority.npos)port=authority.substr(split+1);
            bool ascii=true;for(const auto c:uri.host)ascii&=c<0x80;uri.host=lowered(uri.host);uri.host_type=uri.host.empty()?0:ipv4(uri.host)?2:ascii?1:4;}
        if(!port.empty()){std::uint64_t value=0;for(const auto c:port){if(c<u'0' || c>u'9')return std::nullopt;value=value*10+(c-u'0');if(value>65535)return std::nullopt;}
            uri.port=static_cast<std::uint32_t>(value);uri.has_port=uri.explicit_port=true;}
        if(uri.host.empty() && known && known->id!=9 && known->id!=23)return std::nullopt;
    }
    if(!uri.has_port && known && known->port){uri.port=known->port;uri.has_port=true;}
    if(!(flags&no_fragment)){if(const auto hash=rest.find(u'#');hash!=rest.npos){uri.fragment=rest.substr(hash);rest.erase(hash);}}
    if(const auto question=rest.find(u'?');question!=rest.npos && (uri.authority || uri.relative || (known && known->hierarchical))){uri.query=rest.substr(question);rest.erase(question);}
    uri.path=rest;
    if(!(flags&no_canonicalize)){
        const bool encode=!(flags&no_encode_forbidden);
        uri.path=normalize_escapes(uri.path,true,encode);uri.query=normalize_escapes(uri.query,false,encode);uri.fragment=normalize_escapes(uri.fragment,false,encode);
        if(uri.authority || (uri.relative && uri.path.starts_with(u"/")))uri.path=remove_dots(uri.path);
        if(uri.authority && uri.path.empty() && known && (known->id==1 || known->id==2 || known->id==9 || known->id==11))uri.path=u"/";
    }
    return uri;
}
inline std::u16string port_text(const Uri& uri){return utf8_to_utf16(std::to_string(uri.port));}
inline bool default_port(const Uri& uri){const auto* s=find_scheme(uri.scheme);return s && s->port && s->port==uri.port;}
inline std::u16string host_text(const Uri& uri){return uri.host_type==3?u"["+uri.host+u"]":uri.host;}
inline std::u16string user_info(const Uri& uri){return uri.has_password?uri.user+u":"+uri.password:uri.user;}
inline std::u16string authority(const Uri& uri,bool with_user){
    std::u16string out;if(with_user && (uri.has_user || uri.has_password))out=user_info(uri)+u"@";out+=host_text(uri);
    if(uri.explicit_port && !(default_port(uri) && !(uri.flags&no_canonicalize)))out+=u":"+port_text(uri);return out;
}
inline std::u16string compose(const Uri& uri,bool with_user,bool with_fragment){
    std::u16string out;if(!uri.relative)out=uri.scheme+u":";if(uri.authority)out+=u"//"+authority(uri,with_user);
    out+=uri.path+uri.query;if(with_fragment)out+=uri.fragment;return out;
}
// 註冊網域：最後兩個標籤（主機只有一或兩個標籤時即主機本身）。
inline std::u16string domain(const Uri& uri){
    if(uri.host_type!=1 && uri.host_type!=4)return {};const auto last=uri.host.rfind(u'.');if(last==uri.host.npos)return {};
    const auto previous=uri.host.rfind(u'.',last-1);return previous==uri.host.npos || last==0?uri.host:uri.host.substr(previous+1);
}
inline std::u16string extension(const Uri& uri){
    const auto slash=uri.path.find_last_of(u'/');const auto segment=slash==uri.path.npos?uri.path:uri.path.substr(slash+1);
    const auto dot=segment.find_last_of(u'.');return dot==segment.npos?std::u16string{}:segment.substr(dot);
}
}
