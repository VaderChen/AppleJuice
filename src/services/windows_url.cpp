#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <unicode/uchar.h>

namespace x64arm {
namespace {
constexpr std::uint32_t url_invalid=0x80070057U,url_pointer=0x80004003U;
constexpr std::uint32_t escape_spaces_only=0x04000000U,dont_escape_extra=0x02000000U,escape_percent=0x00001000U,escape_segment=0x00002000U;
constexpr std::uint32_t escape_utf8=0x00040000U,escape_ascii_component=0x00080000U,unescape_inplace=0x00100000U;
enum : unsigned {escape_slash=1,escape_question=2,escape_hash=4,stop_hash=8,stop_question=16,backslash_as_slash=32,collapse_slashes=64};
// ParseURL 的 scheme 判定：冒號前至少兩個英數或 + - . 字元。回傳 scheme 長度（0＝不是 URL）。
std::size_t url_scheme(std::u16string_view url,bool unicode=false){
    std::size_t at=0;
    while(at<url.size()){const auto c=url[at];if(!((c>=u'a' && c<=u'z') || (c>=u'A' && c<=u'Z') || (c>=u'0' && c<=u'9') || c==u'-' || c==u'+' || c==u'.' || (unicode && u_isalnum(c))))break;++at;}
    return at<url.size() && url[at]==u':' && at>1?at:0;
}
bool alnum(char16_t c){return (c>=u'a' && c<=u'z') || (c>=u'A' && c<=u'Z') || (c>=u'0' && c<=u'9');}
bool needs_escape(char16_t c,std::uint32_t flags,unsigned rules){
    if(flags&escape_spaces_only)return c==u' ';
    if((flags&escape_percent) && c==u'%')return true;
    if((flags&escape_utf8) && c>=0x80)return true;
    if(flags&escape_ascii_component)return c<0x80 && !(alnum(c) || c==u'-' || c==u'.' || c==u'_' || c==u'~');
    if(c<=31 || (c>=127 && c<=255))return true;
    if(alnum(c))return false;
    switch(c){
    case u' ':case u'<':case u'>':case u'"':case u'{':case u'}':case u'|':case u'\\':case u'^':case u']':case u'[':case u'`':case u'&':return true;
    case u'/':return rules&escape_slash;
    case u'?':return rules&escape_question;
    case u'#':return rules&escape_hash;
    default:return false;
    }
}
std::u16string url_escape(std::u16string_view url,std::uint32_t flags){
    constexpr std::u16string_view hex=u"0123456789ABCDEF";
    // SPACES_ONLY 排除其他控制；否則一律視為 DONT_ESCAPE_EXTRA_INFO（與 Windows 相同）。
    if(flags&escape_spaces_only)flags&=~(dont_escape_extra|escape_percent|escape_segment);else flags|=dont_escape_extra;
    unsigned rules=0;const auto scheme_length=url_scheme(url);
    if(flags&escape_segment)rules=escape_question|escape_hash|escape_slash;
    else {
        rules=stop_hash|stop_question;
        std::u16string scheme(url.substr(0,scheme_length));for(auto& c:scheme)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        const auto suffix=scheme_length?url.substr(scheme_length+1):std::u16string_view{};
        const auto leading=suffix.empty()?u'\0':suffix.front();
        if(!scheme_length){}
        else if(scheme==u"file"){rules|=backslash_as_slash|collapse_slashes|escape_hash;rules&=~stop_hash;}
        else if(scheme==u"http" || scheme==u"https"){rules|=backslash_as_slash;if(leading!=u'/' && leading!=u'\\')rules|=escape_slash;}
        else if(scheme==u"mailto"){rules|=escape_slash|escape_question|escape_hash;rules&=~(stop_question|stop_hash);}
        else if(leading!=u'/')rules|=escape_slash;
    }
    std::u16string out;bool stop=false;
    const auto percent=[&](std::uint8_t byte){out+=u'%';out+=hex[byte>>4];out+=hex[byte&15];};
    for(std::size_t at=0;at<url.size();){
        auto c=url[at];
        if((rules&collapse_slashes) && at==scheme_length+1){
            std::size_t slashes=0;while(at<url.size() && (url[at]==u'/' || url[at]==u'\\')){++slashes;++at;}
            if(slashes==2 && url.size()-at>=9){
                std::u16string host(url.substr(at,9));for(auto& h:host)if(h>=u'A' && h<=u'Z')h=static_cast<char16_t>(h+32);
                if(host==u"localhost"){if(at+9<url.size() && (url[at+9]==u'/' || url[at+9]==u'\\'))at+=10;slashes=3;}
            }
            if(slashes==1 || slashes==3)out+=u"///";else if(slashes)out+=u"//";
            if(slashes)continue;
        }
        if(c==u'#' && (rules&stop_hash))stop=true;
        if(c==u'?' && (rules&stop_question))stop=true;
        if(c==u'\\' && (rules&backslash_as_slash) && !stop)c=u'/';
        if(!stop && needs_escape(c,flags,rules)){
            if(flags&(escape_utf8|escape_ascii_component) && c>=0x80){
                // 成對代理以一個碼點編碼；單獨的代理字元以 U+FFFD 取代。
                std::size_t used=1;std::u16string unit(1,c);
                if(c>=0xd800 && c<=0xdbff && at+1<url.size() && url[at+1]>=0xdc00 && url[at+1]<=0xdfff){unit+=url[at+1];used=2;}
                else if(c>=0xd800 && c<=0xdfff)unit=u"\ufffd";
                for(const auto byte:utf16_to_utf8(unit))percent(static_cast<std::uint8_t>(byte));
                at+=used;continue;
            }
            percent(static_cast<std::uint8_t>(c));
        }else out+=c;
        ++at;
    }
    return out;
}
std::u16string url_unescape(std::u16string_view url,std::uint32_t flags);
std::u16string lower(std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;}
// URL_SCHEME 列舉值（ParseURL 的 nScheme）；未知 scheme 為 URL_SCHEME_UNKNOWN（0）。
std::uint32_t scheme_number(std::u16string_view scheme){
    static constexpr std::u16string_view names[]{u"ftp",u"http",u"gopher",u"mailto",u"news",u"nntp",u"telnet",u"wais",u"file",u"mk",u"https",u"shell",u"snews",u"local",u"javascript",u"vbscript",u"about",u"res",u"ms-shell-rooted",u"ms-shell-idlist",u"hcp",u"ms-shell-device",u"*",u"search-ms",u"search",u"knownfolder"};
    const auto key=lower(std::u16string(scheme));for(std::size_t i=0;i<std::size(names);++i)if(key==names[i])return static_cast<std::uint32_t>(i+1);return 0;
}
// 階層式 URL 的組成：scheme、權限（使用者、密碼、主機、連接埠）、路徑、查詢與片段；opaque 表示 "scheme:" 後不是 "//"。
struct UrlParts {std::u16string scheme,user,password,host,port,path,query,fragment;bool authority{},opaque{},has_user{},has_password{},has_port{},has_query{},has_fragment{};};
std::optional<UrlParts> url_parts(std::u16string_view url){
    const auto scheme_length=url_scheme(url);if(!scheme_length)return std::nullopt;UrlParts parts;parts.scheme=std::u16string(url.substr(0,scheme_length));
    auto rest=url.substr(scheme_length+1);
    if(const auto hash=rest.find(u'#');hash!=rest.npos){parts.fragment=std::u16string(rest.substr(hash+1));parts.has_fragment=true;rest=rest.substr(0,hash);}
    if(const auto question=rest.find(u'?');question!=rest.npos){parts.query=std::u16string(rest.substr(question+1));parts.has_query=true;rest=rest.substr(0,question);}
    if(rest.size()>=2 && (rest[0]==u'/' || rest[0]==u'\\') && (rest[1]==u'/' || rest[1]==u'\\')){
        parts.authority=true;rest=rest.substr(2);auto end=rest.find_first_of(u"/\\");if(end==rest.npos)end=rest.size();
        auto authority=rest.substr(0,end);parts.path=std::u16string(rest.substr(end));
        if(const auto at=authority.rfind(u'@');at!=authority.npos){auto credentials=authority.substr(0,at);authority=authority.substr(at+1);parts.has_user=true;
            if(const auto colon=credentials.find(u':');colon!=credentials.npos){parts.user=std::u16string(credentials.substr(0,colon));parts.password=std::u16string(credentials.substr(colon+1));parts.has_password=true;}
            else parts.user=std::u16string(credentials);}
        if(const auto colon=authority.rfind(u':');colon!=authority.npos && authority.find(u']',colon)==authority.npos){parts.port=std::u16string(authority.substr(colon+1));parts.has_port=true;authority=authority.substr(0,colon);}
        parts.host=std::u16string(authority);
    }else {parts.path=std::u16string(rest);parts.opaque=rest.empty() || (rest[0]!=u'/' && rest[0]!=u'\\');}
    return parts;
}
// 路徑的 "." 與 ".." 段（RFC 3986 remove_dot_segments）；file URL 的磁碟代號不會被 ".." 越過。
std::u16string remove_dots(std::u16string_view path,bool drive_root){
    std::vector<std::u16string> segments;const bool absolute=!path.empty() && path.front()==u'/';std::size_t floor=0;bool trailing=false;
    for(std::size_t at=absolute?1:0;;){
        auto end=path.find(u'/',at);const bool last=end==path.npos;if(last)end=path.size();const auto segment=path.substr(at,end-at);
        if(segment==u"."){trailing=last;}
        else if(segment==u".."){if(segments.size()>floor)segments.pop_back();trailing=last;}
        else if(segment.empty() && last){trailing=true;}
        else {segments.emplace_back(segment);trailing=false;if(drive_root && segments.size()==1 && segment.size()==2 && segment[1]==u':')floor=1;}
        if(last)break;at=end+1;}
    std::u16string out=absolute?u"/":u"";for(std::size_t i=0;i<segments.size();++i){if(i)out+=u'/';out+=segments[i];}
    if(trailing && !segments.empty())out+=u'/';return out;
}
std::u16string assemble(const UrlParts& p){
    std::u16string out=p.scheme+u":";
    if(p.authority){out+=u"//";if(p.has_user){out+=p.user;if(p.has_password){out+=u':';out+=p.password;}out+=u'@';}out+=p.host;if(p.has_port){out+=u':';out+=p.port;}}
    out+=p.path;if(p.has_query){out+=u'?';out+=p.query;}if(p.has_fragment){out+=u'#';out+=p.fragment;}return out;
}
// DOS 路徑：磁碟代號開頭或 UNC。
bool dos_path(std::u16string_view text){const bool letter=!text.empty() && ((text[0]>=u'a' && text[0]<=u'z') || (text[0]>=u'A' && text[0]<=u'Z'));return (text.size()>=2 && letter && text[1]==u':') || (text.size()>=2 && text[0]==u'\\' && text[1]==u'\\');}
// UrlCreateFromPath：C:\a b → file:///C:/a%20b，\\server\share → file://server/share；'%'、'#'、空白等以百分比編碼。
std::u16string url_from_path(std::u16string_view path){
    std::u16string body;const bool unc=path.size()>=2 && path[0]==u'\\' && path[1]==u'\\';
    for(const auto c:unc?path.substr(2):path)body+=c==u'\\'?u'/':c;
    std::u16string escaped;constexpr std::u16string_view hex=u"0123456789ABCDEF";
    for(const auto c:body){if(c==u' ' || c==u'%' || c==u'#' || c<=31 || c==u'"' || c==u'<' || c==u'>' || c==u'^' || c==u'`' || c==u'{' || c==u'}' || c==u'|' || (c>=127 && c<=255)){escaped+=u'%';escaped+=hex[(c>>4)&15];escaped+=hex[c&15];}else escaped+=c;}
    return unc?u"file://"+escaped:u"file:///"+escaped;
}
// PathCreateFromUrl：file URL 轉回 DOS 路徑並解除百分比編碼；不是 file URL 時回 nullopt。
std::optional<std::u16string> path_from_url(std::u16string_view url){
    if(lower(std::u16string(url.substr(0,std::min<std::size_t>(5,url.size()))))!=u"file:")return std::nullopt;
    auto rest=std::u16string(url.substr(5));std::size_t slashes=0;while(slashes<rest.size() && (rest[slashes]==u'/' || rest[slashes]==u'\\'))++slashes;
    std::u16string body=rest.substr(slashes);
    if(slashes==2 && lower(body.substr(0,std::min<std::size_t>(10,body.size())))==u"localhost/"){body=body.substr(10);slashes=3;}
    const bool drive=body.size()>=2 && (body[1]==u':' || body[1]==u'|') && alnum(body[0]);
    std::u16string path;if(drive){path=body;path[1]=u':';}else if(slashes==2 || slashes>=4)path=u"\\\\"+body;else path=(slashes?u"\\":u"")+body;
    for(auto& c:path)if(c==u'/')c=u'\\';if(const auto hash=path.find(u'#');hash!=path.npos && !drive)path.resize(hash);
    std::u16string out;for(std::size_t i=0;i<path.size();++i){const auto v=[](char16_t c)->int{if(c>=u'0' && c<=u'9')return c-u'0';if(c>=u'a' && c<=u'f')return c-u'a'+10;if(c>=u'A' && c<=u'F')return c-u'A'+10;return -1;};
        if(path[i]==u'%' && i+2<path.size() && v(path[i+1])>=0 && v(path[i+2])>=0){out+=static_cast<char16_t>(v(path[i+1])*16+v(path[i+2]));i+=2;}else out+=path[i];}
    return out;
}
// UrlCanonicalize：可選先解除編碼；scheme 轉小寫，階層式 URL 的 '\' 改成 '/' 並移除 "."／".."（URL_DONT_SIMPLIFY 除外），最後依旗標編碼。
// URL_WININET_COMPATIBILITY：WinINet 的 InternetCanonicalizeUrl／InternetCombineUrl 只在 URL_ESCAPE_UNSAFE（即未指定 ICU_NO_ENCODE）時才編碼，預設不再單獨編碼空白。
constexpr std::uint32_t canon_unescape=0x10000000U,canon_escape_unsafe=0x20000000U,canon_dont_simplify=0x08000000U,canon_dos_path=0x00200000U,canon_wininet=0x80000000U;
std::optional<std::u16string> apply_scheme(const std::u16string& url,std::uint32_t flags){
    if(url_parts(url) && !(flags&8U) && !dos_path(url))return {};
    if((flags&4U) && dos_path(url))return url_from_path(url);
    if(flags&2U){
        const auto host=lower(url);
        if(host.starts_with(u"www."))return u"http://"+url;
        if(host.starts_with(u"ftp."))return u"ftp://"+url;
    }
    if(flags&1U)return u"http://"+url;
    return {};
}
std::u16string url_canonicalize(std::u16string text,std::uint32_t flags){
    if((flags&canon_dos_path) && dos_path(text))text=url_from_path(text);
    if(flags&canon_unescape)text=url_unescape(text,0);
    auto parts=url_parts(text);if(!parts){
        // 沒有 scheme：只整理斜線與點段。
        std::replace(text.begin(),text.end(),u'\\',u'/');return (flags&canon_dont_simplify)?text:remove_dots(text,false);}
    parts->scheme=lower(parts->scheme);
    if(!parts->opaque){std::replace(parts->path.begin(),parts->path.end(),u'\\',u'/');
        if(parts->scheme!=u"file")parts->host=lower(parts->host);
        if(!(flags&canon_dont_simplify))parts->path=remove_dots(parts->path,parts->scheme==u"file");
        if(parts->authority && parts->path.empty() && parts->scheme!=u"file")parts->path=u"/";}
    auto out=assemble(*parts);
    if(flags&canon_unescape)return out;
    if((flags&canon_wininet) && !(flags&(canon_escape_unsafe|escape_spaces_only)))return out;
    if(flags&escape_spaces_only || !(flags&canon_escape_unsafe)){if(parts->opaque)return out;std::u16string spaced;for(const auto c:out){if(c==u' ')spaced+=u"%20";else spaced+=c;}return spaced;}
    return url_escape(out,flags&~(canon_unescape|canon_escape_unsafe|canon_dont_simplify|canon_dos_path|canon_wininet));
}
// UrlCombine（RFC 3986 參照解析）後再正規化。
std::u16string url_combine(std::u16string_view base,std::u16string_view relative,std::uint32_t flags){
    if(url_scheme(relative))return url_canonicalize(std::u16string(relative),flags);
    auto parts=url_parts(base);if(!parts || parts->opaque)return url_canonicalize(std::u16string(relative),flags);
    std::u16string ref(relative);std::replace(ref.begin(),ref.end(),u'\\',u'/');
    if(ref.starts_with(u"//"))return url_canonicalize(parts->scheme+u":"+ref,flags);
    std::u16string fragment;bool has_fragment=false;if(const auto hash=ref.find(u'#');hash!=ref.npos){fragment=ref.substr(hash+1);has_fragment=true;ref.resize(hash);}
    std::u16string query;bool has_query=false;if(const auto question=ref.find(u'?');question!=ref.npos){query=ref.substr(question+1);has_query=true;ref.resize(question);}
    if(ref.empty()){if(has_query){parts->query=query;parts->has_query=true;}}
    else {std::replace(parts->path.begin(),parts->path.end(),u'\\',u'/');
        if(ref.front()==u'/')parts->path=ref;else {const auto slash=parts->path.rfind(u'/');parts->path=(slash==parts->path.npos?(parts->authority?u"/":u""):parts->path.substr(0,slash+1))+ref;}
        parts->query=query;parts->has_query=has_query;}
    parts->fragment=fragment;parts->has_fragment=has_fragment;return url_canonicalize(assemble(*parts),flags);
}
std::u16string url_unescape(std::u16string_view url,std::uint32_t flags){
    const auto value=[](char16_t c)->int{if(c>=u'0' && c<=u'9')return c-u'0';if(c>=u'a' && c<=u'f')return c-u'a'+10;if(c>=u'A' && c<=u'F')return c-u'A'+10;return -1;};
    std::u16string out;bool stop=false;std::string pending;
    const auto flush=[&]{if(pending.empty())return;out+=utf8_to_utf16_lossy(pending);pending.clear();};
    for(std::size_t at=0;at<url.size();++at){
        const auto c=url[at];
        if((flags&dont_escape_extra) && (c==u'#' || c==u'?'))stop=true;
        if(!stop && c==u'%' && at+2<url.size() && value(url[at+1])>=0 && value(url[at+2])>=0){
            const auto byte=static_cast<char16_t>(value(url[at+1])*16+value(url[at+2]));at+=2;
            if(flags&escape_utf8){pending.push_back(static_cast<char>(byte));continue;}
            out+=byte;continue;
        }
        flush();out+=c;
    }
    flush();return out;
}
}
std::span<const WindowsServiceDescriptor> url_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shlwapi.dll","UrlEscapeA",4},{"shlwapi.dll","UrlEscapeW",4},{"shlwapi.dll","UrlUnescapeA",4},{"shlwapi.dll","UrlUnescapeW",4},
        {"shdocvw.dll","#169",4},{"shdocvw.dll","#170",4},
        {"shlwapi.dll","ParseURLA",2},{"shlwapi.dll","ParseURLW",2},{"shlwapi.dll","UrlGetPartA",5},{"shlwapi.dll","UrlGetPartW",5},{"shlwapi.dll","UrlIsA",2},{"shlwapi.dll","UrlIsW",2},
        {"shlwapi.dll","UrlIsOpaqueA",1},{"shlwapi.dll","UrlIsOpaqueW",1},{"shlwapi.dll","UrlIsNoHistoryA",1},{"shlwapi.dll","UrlIsNoHistoryW",1},
        {"shlwapi.dll","UrlCanonicalizeA",4},{"shlwapi.dll","UrlCanonicalizeW",4},{"shlwapi.dll","UrlCombineA",5},{"shlwapi.dll","UrlCombineW",5},
        {"shlwapi.dll","UrlCompareA",3},{"shlwapi.dll","UrlCompareW",3},{"shlwapi.dll","UrlCreateFromPathA",4},{"shlwapi.dll","UrlCreateFromPathW",4},
        {"shlwapi.dll","PathCreateFromUrlA",4},{"shlwapi.dll","PathCreateFromUrlW",4},{"shlwapi.dll","PathCreateFromUrlAlloc",3},
        {"shlwapi.dll","UrlGetLocationA",1},{"shlwapi.dll","UrlGetLocationW",1},{"shlwapi.dll","UrlApplySchemeA",4},{"shlwapi.dll","UrlApplySchemeW",4},
    };return table;
}
// URL 家族的字串輸出共用 output：容量以字元計並含 NUL。
// A 版本的位元組逐一對應 0～255 的碼元處理，0x80 以上以原位元組轉義（不換碼頁）；UTF-8 選項只用於 W 版本。
std::optional<std::uint32_t> WindowsServices::invoke_url(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool outside=service.family=="shdocvw.dll";
    const bool wide=name.ends_with('W') || (outside && name=="#170");
    const auto done=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto flags=service.arguments>=4?static_cast<std::uint32_t>(arg(3)):0U;const bool escape=name.starts_with("UrlEscape");
    const auto read=[&](std::uint64_t address){
        if(wide)return read_utf16(memory,GuestAddress{address},32768);
        std::u16string text;for(const auto c:read_ansi(memory,address,32768))text.push_back(static_cast<unsigned char>(c));return text;
    };
    const auto write=[&](std::uint64_t address,std::u16string_view text){
        for(std::size_t i=0;i<text.size();++i){if(wide)memory.store(GuestAddress{address+i*2},16,text[i]);else memory.store(GuestAddress{address+i},8,text[i]&0xffU);}
        if(wide)memory.store(GuestAddress{address+text.size()*2},16,0);else memory.store(GuestAddress{address+text.size()},8,0);
    };
    // 輸出慣例同 UrlEscape：容量含 NUL；成功時寫回不含 NUL 的長度，空間不足回 E_POINTER 並寫回所需長度（含 NUL）。
    const auto output=[&](std::uint64_t buffer,std::uint64_t count,const std::u16string& text,std::uint32_t success=0)->std::optional<std::uint32_t>{
        if(!buffer || !count)return done(url_invalid);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{count},32));
        if(text.size()>=capacity){memory.store(GuestAddress{count},32,text.size()+1);return done(url_pointer);}write(buffer,text);memory.store(GuestAddress{count},32,text.size());return done(success);};
    // URLIS：URL 0、OPAQUE 1、NOHISTORY 2、FILEURL 3、APPLIABLE 4、DIRECTORY 5、HASQUERY 6。
    const auto url_is=[&](const std::u16string& url,std::uint32_t kind)->bool{
        const auto parts=url_parts(url);const auto scheme=parts?lower(parts->scheme):std::u16string{};
        switch(kind){
        case 0:return parts.has_value();
        case 1:return parts && (parts->opaque || scheme==u"mailto" || scheme==u"javascript" || scheme==u"vbscript" || scheme==u"about" || scheme==u"news");
        case 2:return parts && (scheme==u"mailto" || scheme==u"shell" || scheme==u"javascript" || scheme==u"vbscript" || scheme==u"about");
        case 3:return scheme==u"file";
        case 4:return parts.has_value() || dos_path(url);
        case 5:return !url.empty() && (url.back()==u'/' || url.back()==u'\\');
        case 6:return url.find(u'?')!=url.npos;
        default:return false;}};
    try{
        if(outside){
            if(!arg(0) || !arg(2))return done(0);
            const auto source=read(arg(0));
            auto applied=apply_scheme(source,3);
            const auto text=url_canonicalize(applied?std::move(*applied):source,escape_spaces_only);
            const auto capacity=memory.load(GuestAddress{arg(2)},32);
            const auto needed=static_cast<std::uint32_t>(text.size()+1);
            if(capacity<needed){memory.store(GuestAddress{arg(2)},32,needed);return done(0);}
            if(arg(1))write(arg(1),text);
            memory.store(GuestAddress{arg(2)},32,needed-1);
            return done(arg(1)?(wide?1U:needed):0U);
        }
        if(name.starts_with("ParseURL")){
            // PARSEDURL：cbSize、pszProtocol、cchProtocol、pszSuffix、cchSuffix、nScheme；指標指向輸入字串內部。
            const unsigned unit=x86_?4U:8U,size=x86_?24U:40U,scheme_at=x86_?20U:36U;if(!arg(1))return done(url_invalid);if(memory.load(GuestAddress{arg(1)},32)!=size)return done(url_invalid);
            if(!arg(0))return done(0x80041001U);const auto url=read(arg(0));const auto length=url_scheme(url,wide);
            if(!length){memory.store(GuestAddress{arg(1)+unit},x86_?32:64,0);return done(0x80041001U);}
            const auto at=[&](std::size_t offset){return arg(0)+offset*(wide?2U:1U);};
            memory.store(GuestAddress{arg(1)+unit},x86_?32:64,at(0));memory.store(GuestAddress{arg(1)+unit*2},32,length);
            memory.store(GuestAddress{arg(1)+unit*3},x86_?32:64,at(length+1));memory.store(GuestAddress{arg(1)+unit*4},32,url.size()-length-1);
            memory.store(GuestAddress{arg(1)+scheme_at},32,scheme_number(std::u16string_view(url).substr(0,length)));return done(0);
        }
        if(name.starts_with("UrlGetPart")){
            // (url, out, count*, part, flags)：SCHEME 1、HOSTNAME 2、USERNAME 3、PASSWORD 4、PORT 5、QUERY 6；KEEPSCHEME（1）時主機名稱前加 "scheme:"。缺少的部分回 S_FALSE 與空字串。
            if(!arg(0) || !arg(1) || !arg(2))return done(url_invalid);const auto parts=url_parts(read(arg(0)));const auto part=static_cast<std::uint32_t>(arg(3));
            if(!parts || part<1 || part>6)return done(url_invalid);
            if(part!=1 && part!=6 && !parts->authority)return done(0x80004005U);
            std::u16string value;bool present=true;
            switch(part){case 1:value=parts->scheme;break;case 2:value=parts->host;present=!value.empty();break;case 3:value=parts->user;present=parts->has_user;break;
                case 4:value=parts->password;present=parts->has_password;break;case 5:value=parts->port;present=parts->has_port;break;default:value=parts->query;present=parts->has_query;break;}
            if(!present)value.clear();if(present && part==2 && (static_cast<std::uint32_t>(arg(4))&1U))value=parts->scheme+u":"+value;
            return output(arg(1),arg(2),value,present?0U:1U);
        }
        if(name.starts_with("UrlIs")){
            if(!arg(0))return done(0);const auto url=read(arg(0));
            const auto kind=name.starts_with("UrlIsOpaque")?1U:name.starts_with("UrlIsNoHistory")?2U:static_cast<std::uint32_t>(arg(1));return done(url_is(url,kind)?1U:0U);
        }
        if(name.starts_with("UrlCanonicalize")){if(!arg(0))return done(url_invalid);return output(arg(1),arg(2),url_canonicalize(read(arg(0)),flags));}
        if(name.starts_with("UrlCombine")){if(!arg(0) || !arg(1))return done(url_invalid);return output(arg(2),arg(3),url_combine(read(arg(0)),read(arg(1)),static_cast<std::uint32_t>(arg(4))));}
        if(name.starts_with("UrlCompare")){
            // 區分大小寫；fIgnoreSlash 時忽略結尾的 '/'。
            if(!arg(0) || !arg(1))return done(0);auto a=read(arg(0)),b=read(arg(1));
            if(static_cast<std::uint32_t>(arg(2))){if(!a.empty() && a.back()==u'/')a.pop_back();if(!b.empty() && b.back()==u'/')b.pop_back();}
            return done(static_cast<std::uint32_t>(a<b?-1:a>b?1:0));
        }
        if(name.starts_with("UrlCreateFromPath")){
            // 已是 URL 時原樣複製並回 S_FALSE。
            if(!arg(0))return done(url_invalid);const auto path=read(arg(0));if(url_parts(path) && !dos_path(path))return output(arg(1),arg(2),path,1);
            if(!dos_path(path))return done(url_invalid);return output(arg(1),arg(2),url_from_path(path));
        }
        if(name=="PathCreateFromUrlAlloc"){
            // (url, path**, flags)：以 LocalAlloc 配置結果。
            if(!arg(0) || !arg(1))return done(url_invalid);memory.store(GuestAddress{arg(1)},x86_?32:64,0);const auto path=path_from_url(read_utf16(memory,GuestAddress{arg(0)},32768));if(!path)return done(url_invalid);
            const auto buffer=nested_service(state,memory,"kernel32.dll","LocalAlloc",{0x40,(path->size()+1)*2});if(!buffer)return done(0x8007000eU);
            for(std::size_t i=0;i<path->size();++i)memory.store(GuestAddress{buffer+i*2},16,(*path)[i]);memory.store(GuestAddress{arg(1)},x86_?32:64,buffer);return done(0);
        }
        if(name.starts_with("PathCreateFromUrl")){if(!arg(0))return done(url_invalid);const auto path=path_from_url(read(arg(0)));if(!path)return done(url_invalid);return output(arg(1),arg(2),*path);}
        if(name.starts_with("UrlGetLocation")){
            // 回傳輸入字串中 '#' 的位置；opaque URL 與沒有片段時回 NULL。
            if(!arg(0))return done(0);const auto url=read(arg(0));const auto parts=url_parts(url);if(!parts || parts->opaque || !parts->has_fragment)return done(0);
            arg.return_to(state,arg(0)+url.find(u'#')*(wide?2U:1U));return std::nullopt;
        }
        if(name.starts_with("UrlApplyScheme")){
            if(!arg(0))return done(url_invalid);
            const auto applied=apply_scheme(read(arg(0)),flags);
            if(!applied)return done(1);
            return output(arg(1),arg(2),*applied);
        }
        if(escape){
            if(!arg(0) || !arg(1) || !arg(2) || !memory.load(GuestAddress{arg(2)},32))return done(url_invalid);
            if(!wide && (flags&escape_utf8))return done(0x80004001U);
            return output(arg(1),arg(2),url_escape(read(arg(0)),flags));
        }
        if(!arg(0))return done(url_invalid);
        const bool inplace=flags&unescape_inplace;
        if(!inplace && (!arg(1) || !arg(2)))return done(url_invalid);
        const auto result=url_unescape(read(arg(0)),wide?flags:flags&~escape_utf8);
        if(inplace){write(arg(0),result);return done(0);}
        return output(arg(1),arg(2),result);
    }catch(const MemoryFault&){return done(outside?0U:url_invalid);}
}
}
