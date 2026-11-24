// WinHTTP／WinINet 共用的 URL 與 HTTP 時間輔助函式（宿主端計算，不經網路）：
// - WinHttpCreateUrl、InternetCreateUrlA／W：由 URL_COMPONENTS 組出 URL；字串欄位有長度時取長度、否則以 NUL 結尾，lpszScheme 為 NULL 時用 nScheme
//   （WinINet：FTP 1、GOPHER 2、HTTP 3、HTTPS 4、FILE 5、NEWS 6、MAILTO 7、SOCKS 8；WinHTTP：HTTP 1、HTTPS 2、FTP 3、SOCKS 4；0 為 HTTP），
//   與預設連接埠相同的 nPort 省略；ICU_ESCAPE 編碼路徑與額外資訊中的不安全字元；WinHTTP 的 ICU_REJECT_USERPWD 拒絕帳號密碼。
//   緩衝不足時 ERROR_INSUFFICIENT_BUFFER：WinHTTP 回報所需字元數，WinINet 依文件回報所需位元組數（含 NUL）。
// - WinHttpTimeFromSystemTime、InternetTimeFromSystemTime（A／W 與無尾碼 ANSI 名稱）：RFC 1123 格式（"Sun, 06 Nov 1994 08:49:37 GMT"），星期依日期計算。
// - WinHttpTimeToSystemTime、InternetTimeToSystemTime：接受 RFC 1123、RFC 850 與 asctime 格式（不分大小寫、月份名稱、hh:mm:ss、兩位數年份 <50 為 20xx）。
// - InternetCanonicalizeUrlA／W、InternetCombineUrlA／W：沿用 shlwapi 的 UrlCanonicalize／UrlCombine（加 URL_WININET_COMPATIBILITY）；ICU_NO_ENCODE 與 URL_ESCAPE_UNSAFE 同一位元但意義相反，
//   且依文件一律為 ICU_BROWSER_MODE（不編碼 "?"、"#" 之後）。HRESULT 轉成 BOOL 與 Win32 錯誤碼。
// - Cookie 與隱私設定 API 同屬本服務群組，實作見 windows_http_cookies.cpp。
#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

namespace x64arm {
namespace {
constexpr std::array<const char*,7> week_days{"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
constexpr std::array<const char*,12> months{"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
bool leap(unsigned year){return (year%4==0 && year%100!=0) || year%400==0;}
unsigned month_days(unsigned year,unsigned month){static constexpr unsigned days[]{31,28,31,30,31,30,31,31,30,31,30,31};return month==2 && leap(year)?29:days[month-1];}
// 0＝星期日（Sakamoto 演算法）。
unsigned day_of_week(unsigned year,unsigned month,unsigned day){static constexpr int t[]{0,3,2,5,0,3,5,1,4,6,2,4};if(month<3)--year;return static_cast<unsigned>((year+year/4-year/100+year/400+static_cast<unsigned>(t[month-1])+day)%7);}
struct Time {unsigned year{},month{},day{},hour{},minute{},second{};};
std::optional<std::string> rfc1123(const Time& t){
    if(t.month<1 || t.month>12 || t.day<1 || t.year<1601 || t.year>30827 || t.day>month_days(t.year,t.month) || t.hour>23 || t.minute>59 || t.second>59)return std::nullopt;
    char text[40];std::snprintf(text,sizeof text,"%s, %02u %s %04u %02u:%02u:%02u GMT",week_days[day_of_week(t.year,t.month,t.day)],t.day,months[t.month-1],t.year,t.hour,t.minute,t.second);
    return std::string(text);
}
std::optional<Time> parse_http_time(std::string_view text){
    Time t;bool have_month=false,have_time=false;std::vector<unsigned> numbers;
    std::size_t i=0;const auto alpha=[](char c){return (c>='a' && c<='z') || (c>='A' && c<='Z');};const auto digit=[](char c){return c>='0' && c<='9';};
    while(i<text.size()){
        if(alpha(text[i])){
            std::string word;while(i<text.size() && alpha(text[i]))word.push_back(static_cast<char>(text[i]>='A' && text[i]<='Z'?text[i]+32:text[i])),++i;
            if(word.size()>=3)for(unsigned m=0;m<12;++m){std::string name(months[m]);name[0]=static_cast<char>(name[0]+32);if(word.compare(0,3,name)==0){t.month=m+1;have_month=true;break;}}
            continue;
        }
        if(digit(text[i])){
            unsigned value=0;std::size_t start=i;while(i<text.size() && digit(text[i]) && i-start<6)value=value*10+static_cast<unsigned>(text[i++]-'0');
            if(i<text.size() && text[i]==':' && !have_time){
                // hh:mm[:ss]
                unsigned parts[3]{value,0,0};unsigned count=1;
                while(count<3 && i<text.size() && text[i]==':'){++i;unsigned part=0;std::size_t s=i;while(i<text.size() && digit(text[i]) && i-s<2)part=part*10+static_cast<unsigned>(text[i++]-'0');if(i==s)return std::nullopt;parts[count++]=part;}
                t.hour=parts[0];t.minute=parts[1];t.second=parts[2];have_time=true;continue;
            }
            numbers.push_back(value);continue;
        }
        ++i;
    }
    if(!have_month || numbers.empty())return std::nullopt;
    // 日在 1～31；年份為其他數字（兩位數年份 <50 為 20xx，其餘 19xx）。
    std::optional<unsigned> day,year;
    for(const auto value:numbers){if(!day && value>=1 && value<=31 && (numbers.size()==1 || value!=numbers.back() || numbers.front()==value))day=value;else if(!year)year=value;}
    if(!day || !year)return std::nullopt;
    t.day=*day;t.year=*year<50?*year+2000:*year<100?*year+1900:*year;
    if(t.year<1601 || t.day>month_days(t.year,t.month) || t.hour>23 || t.minute>59 || t.second>59)return std::nullopt;
    return t;
}
std::u16string escape_unsafe(std::u16string_view text){
    std::u16string out;const auto hex="0123456789ABCDEF";
    for(const auto c:text){
        if(c>0x20 && c<0x7f && std::u16string_view(u"\"<>\\^`{|}").find(c)==std::u16string_view::npos){out.push_back(c);continue;}
        const auto bytes=utf16_to_utf8(std::u16string(1,c));for(const char raw:bytes){const auto b=static_cast<unsigned char>(raw);out.push_back(u'%');out.push_back(static_cast<char16_t>(hex[b>>4]));out.push_back(static_cast<char16_t>(hex[b&15]));}
    }
    return out;
}
}
std::span<const WindowsServiceDescriptor> http_helper_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"winhttp.dll","WinHttpCreateUrl",4,0,0,0x2},{"winhttp.dll","WinHttpTimeFromSystemTime",2},{"winhttp.dll","WinHttpTimeToSystemTime",2},
        {"wininet.dll","InternetCreateUrlA",4,0,0,0x2},{"wininet.dll","InternetCreateUrlW",4,0,0,0x2},
        {"wininet.dll","InternetTimeFromSystemTimeA",4,0,0,0xa},{"wininet.dll","InternetTimeFromSystemTimeW",4,0,0,0xa},{"wininet.dll","InternetTimeFromSystemTime",4,0,0,0xa},
        {"wininet.dll","InternetTimeToSystemTimeA",3,0,0,0x4},{"wininet.dll","InternetTimeToSystemTimeW",3,0,0,0x4},{"wininet.dll","InternetTimeToSystemTime",3,0,0,0x4},
        {"wininet.dll","InternetCanonicalizeUrlA",4,0,0,0x8},{"wininet.dll","InternetCanonicalizeUrlW",4,0,0,0x8},{"wininet.dll","InternetCombineUrlA",5,0,0,0x10},{"wininet.dll","InternetCombineUrlW",5,0,0,0x10},
        // Cookie 與隱私設定（windows_http_cookies.cpp）
        {"wininet.dll","InternetGetCookieA",4},{"wininet.dll","InternetGetCookieW",4},{"wininet.dll","InternetGetCookieExA",6,0,0,0x10},{"wininet.dll","InternetGetCookieExW",6,0,0,0x10},
        {"wininet.dll","InternetSetCookieA",3},{"wininet.dll","InternetSetCookieW",3},{"wininet.dll","InternetSetCookieExA",5,0,0,0x8},{"wininet.dll","InternetSetCookieExW",5,0,0,0x8},
        {"wininet.dll","InternetSetPerSiteCookieDecisionA",2,0,0,0x2},{"wininet.dll","InternetSetPerSiteCookieDecisionW",2,0,0,0x2},
        {"wininet.dll","InternetGetPerSiteCookieDecisionA",2},{"wininet.dll","InternetGetPerSiteCookieDecisionW",2},
        {"wininet.dll","InternetEnumPerSiteCookieDecisionA",4,0,0,0x8},{"wininet.dll","InternetEnumPerSiteCookieDecisionW",4,0,0,0x8},{"wininet.dll","InternetClearAllPerSiteCookieDecisions",0},
        {"wininet.dll","PrivacySetZonePreferenceW",4,0,0,0x7},{"wininet.dll","PrivacyGetZonePreferenceW",5,0,0,0x3}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_http_helpers(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol.find("Cookie")!=std::string_view::npos || service.symbol.starts_with("Privacy"))return invoke_http_cookies(state,memory,service);
    const WindowsCall call(state,memory,service);const auto name=service.symbol;const bool inet=service.family=="wininet.dll";
    const bool wide=name.ends_with('W') || !inet;const unsigned unit=x86_?4:8;const std::uint8_t pointer_bits=x86_?32:64;
    const auto finish=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto read=[&](std::uint64_t at,std::uint32_t length=0)->std::u16string{
        if(!at)return {};
        if(wide){if(!length)return read_utf16(memory,GuestAddress{at},65536);std::u16string out;for(std::uint32_t i=0;i<length;++i)out.push_back(static_cast<char16_t>(memory.load(GuestAddress{at+i*2},16)));return out;}
        if(!length)return ansi_to_utf16_lossy(read_ansi(memory,at,65536));std::string out(length,'\0');for(std::uint32_t i=0;i<length;++i)out[i]=static_cast<char>(memory.load(GuestAddress{at+i},8));return utf8_to_utf16_lossy(out);
    };
    const auto encode=[&](std::u16string_view text,bool unicode){std::vector<std::uint8_t> bytes;if(unicode)bytes=utf16_bytes(text);else{const auto utf8=utf16_to_utf8(std::u16string(text));bytes.assign(utf8.begin(),utf8.end());bytes.push_back(0);}return bytes;};
    try{
        if(name.find("TimeFromSystemTime")!=std::string_view::npos){
            const auto system=call(0);const bool winhttp=!inet;const auto output=call(winhttp?1:2);
            if(!system || !output)return fail(87);if(!winhttp && static_cast<std::uint32_t>(call(1)))return fail(87);
            Time t{static_cast<unsigned>(memory.load(GuestAddress{system},16)),static_cast<unsigned>(memory.load(GuestAddress{system+2},16)),static_cast<unsigned>(memory.load(GuestAddress{system+6},16)),
                static_cast<unsigned>(memory.load(GuestAddress{system+8},16)),static_cast<unsigned>(memory.load(GuestAddress{system+10},16)),static_cast<unsigned>(memory.load(GuestAddress{system+12},16))};
            const auto text=rfc1123(t);if(!text)return fail(87);
            const bool unicode=winhttp || name.ends_with('W');const auto bytes=encode(utf8_to_utf16(*text),unicode);
            if(!winhttp && static_cast<std::uint32_t>(call(3))<(unicode?60U:30U))return fail(122); // INTERNET_RFC1123_BUFSIZE 個字元
            memory.write(GuestAddress{output},bytes);return finish(1);
        }
        if(name.find("TimeToSystemTime")!=std::string_view::npos){
            const auto text_at=call(0),system=call(1);if(!text_at || !system)return fail(87);
            const bool unicode=!inet || name.ends_with('W');
            const auto text=unicode?utf16_to_utf8(read_utf16(memory,GuestAddress{text_at},256)):ansi_to_utf8(read_ansi(memory,text_at,256));
            const auto parsed=parse_http_time(text);if(!parsed)return fail(87);
            const std::array<std::uint16_t,8> fields{static_cast<std::uint16_t>(parsed->year),static_cast<std::uint16_t>(parsed->month),static_cast<std::uint16_t>(day_of_week(parsed->year,parsed->month,parsed->day)),
                static_cast<std::uint16_t>(parsed->day),static_cast<std::uint16_t>(parsed->hour),static_cast<std::uint16_t>(parsed->minute),static_cast<std::uint16_t>(parsed->second),0};
            for(unsigned i=0;i<8;++i)memory.store(GuestAddress{system+i*2},16,fields[i]);return finish(1);
        }
        if(name.starts_with("InternetCanonicalizeUrl") || name.starts_with("InternetCombineUrl")){
            const bool combine=name.starts_with("InternetCombineUrl");const auto length=call(combine?3:2);const auto flags=static_cast<std::uint32_t>(call(combine?4:3));
            if(!length || !call(0) || (combine && !call(1)))return fail(87);
            const auto shlwapi_flags=(flags^0x20000000U)|0x02000000U|0x80000000U; // URL_WININET_COMPATIBILITY
            const auto hr=static_cast<std::uint32_t>(combine?nested_service(state,memory,"shlwapi.dll",wide?"UrlCombineW":"UrlCombineA",{call(0),call(1),call(2),length,shlwapi_flags}):
                nested_service(state,memory,"shlwapi.dll",wide?"UrlCanonicalizeW":"UrlCanonicalizeA",{call(0),call(1),length,shlwapi_flags}));
            if(!hr)return finish(1);
            return fail(hr==0x80004003U?122U:hr==0x80070057U?87U:12005U);
        }
        // WinHttpCreateUrl／InternetCreateUrlA／W
        const auto components=call(0);const auto flags=static_cast<std::uint32_t>(call(1));const auto output=call(2),length_at=call(3);
        if(!components || !length_at)return fail(87);
        const unsigned size=x86_?60U:104U;memory.check(GuestAddress{components},size,Access::Read);if(memory.load(GuestAddress{components},32)!=size)return fail(87);
        const std::array<unsigned,6> offsets=x86_?std::array<unsigned,6>{4,16,28,36,44,52}:std::array<unsigned,6>{8,24,40,56,72,88};
        std::array<std::u16string,6> parts;
        for(unsigned i=0;i<6;++i){const auto pointer=memory.load(GuestAddress{components+offsets[i]},pointer_bits);parts[i]=read(pointer,static_cast<std::uint32_t>(memory.load(GuestAddress{components+offsets[i]+unit},32)));}
        auto& [scheme,host,user,password,path,extra]=parts;
        const auto number=static_cast<std::int32_t>(memory.load(GuestAddress{components+(x86_?12U:20U)},32));const auto port=static_cast<unsigned>(memory.load(GuestAddress{components+(x86_?24U:36U)},16));
        if(scheme.empty()){
            static constexpr std::array<std::u16string_view,12> inet_schemes{u"http",u"ftp",u"gopher",u"http",u"https",u"file",u"news",u"mailto",u"socks",u"javascript",u"vbscript",u"res"};
            static constexpr std::array<std::u16string_view,5> winhttp_schemes{u"http",u"http",u"https",u"ftp",u"socks"};
            if(inet && number>=0 && static_cast<std::size_t>(number)<inet_schemes.size())scheme=inet_schemes[static_cast<std::size_t>(number)];
            else if(!inet && number>=0 && static_cast<std::size_t>(number)<winhttp_schemes.size())scheme=winhttp_schemes[static_cast<std::size_t>(number)];
            else return fail(12006);
        }
        auto lower=scheme;for(auto& c:lower)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        if(!inet && (flags&0x4000U) && (!user.empty() || !password.empty()))return fail(12005);
        if(flags&0x80000000U){path=escape_unsafe(path);extra=escape_unsafe(extra);}
        std::u16string url=scheme+u":";
        const bool opaque=lower==u"mailto" || lower==u"news" || lower==u"javascript" || lower==u"vbscript";
        if(!opaque){
            url+=u"//";
            if(!user.empty()){url+=user;if(!password.empty())url+=u":"+password;url+=u"@";}
            url+=host;
            const unsigned fallback=lower==u"http"?80U:lower==u"https"?443U:lower==u"ftp"?21U:lower==u"gopher"?70U:lower==u"socks"?1080U:0U;
            if(port && port!=fallback)url+=u":"+utf8_to_utf16(std::to_string(port));
            if(!host.empty() && !path.empty() && path.front()!=u'/')url+=u"/";
        }
        url+=path+extra;
        const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{length_at},32));
        const auto bytes=encode(url,wide);const auto characters=static_cast<std::uint32_t>(wide?bytes.size()/2:bytes.size());
        if(!output || capacity<characters){memory.store(GuestAddress{length_at},32,inet?static_cast<std::uint32_t>(bytes.size()):characters);return fail(122);}
        memory.write(GuestAddress{output},bytes);memory.store(GuestAddress{length_at},32,characters-1);return finish(1);
    }catch(const MemoryFault&){return fail(998);}
}
}
