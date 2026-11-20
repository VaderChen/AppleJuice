#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "windows_locale_data.hpp"
#include "guest_counted_string.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// shlwapi 字串家族（StrChr／StrRChr／StrCSpn／StrSpn／StrPBrk／StrRStrI、序數比較、複製與串接、StrDup、StrToInt、
// 位元組與時間格式、國際化比較、ChrCmpI、SHStrDup、A／W 互轉、STRRET 取出、IsCharSpace、SHStripMneumonic）。
// 文字模型：W 版以 UTF-16 碼元（Windows 的 WCHAR，16 位元）處理，不使用宿主 32 位元 wchar_t；A 版的 ACP 為 UTF-8，
// 依碼位逐字處理但回傳原始位元組位移，不切斷多位元組字元；不分大小寫比較與 StrCmpI／StrStrI 共用 windows_nls_compare。
namespace x64arm {
namespace {
// 一個「字元」：W 版為一個碼元，A 版為一個 UTF-8 碼位（無效位元組視為單一位元組字元）。
struct ShellCharacter {std::u16string text;std::size_t offset{},size{};};
struct ShellText {
    bool wide{};std::vector<ShellCharacter> characters;std::size_t bytes{};
    std::u16string joined(std::size_t first=0,std::size_t last=SIZE_MAX) const {
        std::u16string out;last=std::min(last,characters.size());for(auto i=first;i<last;++i)out+=characters[i].text;return out;}
};
ShellText shell_text(const GuestMemory& memory,std::uint64_t address,bool wide,std::size_t limit=SIZE_MAX,std::size_t byte_limit=SIZE_MAX){
    ShellText result;result.wide=wide;
    if(wide){
        // 讀到 NUL 或上限為止（N 版的上限內可以沒有 NUL）。
        std::u16string text;const auto cap=std::min({limit,byte_limit/2,std::size_t{1U<<20}});
        for(std::size_t i=0;i<cap;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{address+i*2},16));if(!c)break;text.push_back(c);}
        result.characters.reserve(text.size());
        for(std::size_t i=0;i<text.size();++i)result.characters.push_back({std::u16string(1,text[i]),i*2,2});
        result.bytes=text.size()*2;return result;
    }
    std::string bytes;
    for(std::size_t i=0;i<std::min<std::size_t>(byte_limit,1U<<22);++i){const auto c=static_cast<char>(memory.load(GuestAddress{address+i},8));if(!c)break;bytes.push_back(c);}
    const auto view=std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());
    for(std::size_t at=0;at<view.size() && result.characters.size()<limit;){
        const auto decoded=windows_crt_decode(view.subspan(at),65001);
        const auto size=decoded.status==CrtDecodedCharacter::Status::Complete && decoded.bytes?decoded.bytes:1U;
        auto text=windows_nls_decode(std::string_view(bytes).substr(at,size),65001,false).value_or(std::u16string(1,u'�'));
        result.characters.push_back({std::move(text),at,size});at+=size;
    }
    result.bytes=result.characters.empty()?0:result.characters.back().offset+result.characters.back().size;return result;
}
// 單一字元引數：W 版為 WCHAR；A 版為 WORD（單位元組或 lead＋trail）。
std::u16string shell_character(std::uint64_t value,bool wide){
    if(wide)return std::u16string(1,static_cast<char16_t>(value));
    std::string bytes;if(value>0xff)bytes.push_back(static_cast<char>(value>>8));bytes.push_back(static_cast<char>(value&0xffU));
    return windows_nls_decode(bytes,65001,false).value_or(std::u16string(1,u'�'));
}
bool shell_equal(std::u16string_view left,std::u16string_view right,bool fold){
    if(!fold)return left==right;const auto compared=windows_nls_compare(left,right,0x409,1);return compared && *compared==0;
}
// 三位有效數字的位元組大小（KB、MB…）；truncate 為截斷，否則四捨五入到顯示位數。
std::u16string byte_size_text(std::uint64_t size,bool truncate){
    if(size<1024){auto text=std::to_string(size)+" bytes";return std::u16string(text.begin(),text.end());}
    constexpr const char* units[]{"KB","MB","GB","TB","PB","EB"};
    double value=static_cast<double>(size);unsigned unit=0;value/=1024.0;
    while(value>=1000.0 && unit<5){value/=1024.0;++unit;}
    const int decimals=value<10?2:value<100?1:0;const double scale=decimals==2?100:decimals==1?10:1;
    double shown=truncate?std::floor(value*scale)/scale:std::round(value*scale)/scale;
    if(shown>=1000.0 && unit<5){shown/=1024.0;++unit;}
    char buffer[64];std::snprintf(buffer,sizeof buffer,"%.*f %s",decimals,shown,units[unit]);
    return std::u16string(buffer,buffer+std::strlen(buffer));
}
}
std::span<const WindowsServiceDescriptor> shlwapi_string_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shlwapi.dll","StrChrA",2,0,0,0x2},{"shlwapi.dll","StrChrW",2,0,0,0x2},{"shlwapi.dll","StrChrIA",2,0,0,0x2},{"shlwapi.dll","StrChrIW",2,0,0,0x2},
        {"shlwapi.dll","StrChrNW",3,0,0,0x6},{"shlwapi.dll","StrRChrA",3,0,0,0x4},{"shlwapi.dll","StrRChrW",3,0,0,0x4},
        {"shlwapi.dll","StrRChrIA",3,0,0,0x4},{"shlwapi.dll","StrRChrIW",3,0,0,0x4},
        {"shlwapi.dll","StrCSpnA",2},{"shlwapi.dll","StrCSpnW",2},{"shlwapi.dll","StrCSpnIA",2},{"shlwapi.dll","StrCSpnIW",2},
        {"shlwapi.dll","StrSpnA",2},{"shlwapi.dll","StrSpnW",2},{"shlwapi.dll","StrPBrkA",2},{"shlwapi.dll","StrPBrkW",2},
        {"shell32.dll","StrRStrA",3},{"shell32.dll","StrRStrW",3},
        {"shlwapi.dll","StrRStrIA",3},{"shlwapi.dll","StrRStrIW",3},
        {"shlwapi.dll","StrCmpCA",2},{"shlwapi.dll","StrCmpCW",2},{"shlwapi.dll","StrCmpICA",2},{"shlwapi.dll","StrCmpICW",2},
        {"shlwapi.dll","StrCmpNCA",3,0,0,0x4},{"shlwapi.dll","StrCmpNCW",3,0,0,0x4},{"shlwapi.dll","StrCmpNICA",3,0,0,0x4},{"shlwapi.dll","StrCmpNICW",3,0,0,0x4},
        {"shlwapi.dll","StrCpyW",2},{"shlwapi.dll","StrCpyNW",3,0,0,0x4},{"shlwapi.dll","StrCatW",2},{"shlwapi.dll","StrNCatA",3,0,0,0x4},{"shlwapi.dll","StrNCatW",3,0,0,0x4},
        {"shlwapi.dll","StrCatBuffA",3,0,0,0x4},{"shlwapi.dll","StrCatBuffW",3,0,0,0x4},{"shlwapi.dll","StrCatChainW",4,0,0,0x6},
        {"shlwapi.dll","StrDupA",1},{"shlwapi.dll","StrDupW",1},
        {"shlwapi.dll","StrToIntA",1},{"shlwapi.dll","StrToIntW",1},{"shlwapi.dll","StrToIntExA",3,0,0,0x2},{"shlwapi.dll","StrToIntExW",3,0,0,0x2},
        {"shlwapi.dll","StrToInt64ExA",3,0,0,0x2},{"shlwapi.dll","StrToInt64ExW",3,0,0,0x2},
        {"shlwapi.dll","StrFormatByteSizeA",3,0,0,0x5},{"shlwapi.dll","StrFormatByteSizeW",3,0x1,0,0x4},{"shlwapi.dll","StrFormatByteSize64A",3,0x1,0,0x4},
        {"shlwapi.dll","StrFormatByteSizeEx",4,0x1,0,0xa},{"shlwapi.dll","StrFormatKBSizeA",3,0x1,0,0x4},{"shlwapi.dll","StrFormatKBSizeW",3,0x1,0,0x4},
        {"shlwapi.dll","StrFromTimeIntervalA",4,0,0,0xe},{"shlwapi.dll","StrFromTimeIntervalW",4,0,0,0xe},
        {"shlwapi.dll","StrIsIntlEqualA",4,0,0,0x9},{"shlwapi.dll","StrIsIntlEqualW",4,0,0,0x9},
        {"shlwapi.dll","IntlStrEqWorkerA",4,0,0,0x9},{"shlwapi.dll","IntlStrEqWorkerW",4,0,0,0x9},
        {"shlwapi.dll","ChrCmpIA",2,0,0,0x3},{"shlwapi.dll","ChrCmpIW",2,0,0,0x3},
        {"shlwapi.dll","SHStrDupA",2},{"shlwapi.dll","SHStrDupW",2},
        {"shlwapi.dll","#364",3,0,0,0x4},{"shlwapi.dll","#365",3,0,0,0x4},
        {"shcore.dll","#152",3,0,0,1},{"shcore.dll","#153",3,0,0,1},
        {"shlwapi.dll","#216",4,0,0,9},{"shlwapi.dll","#218",4,0,0,9},
        {"internal-shcore-codec","SHAnsiToUnicode",3,0,0,4},{"internal-shcore-codec","SHUnicodeToAnsi",3,0,0,4},
        {"shlwapi.dll","SHAnsiToUnicode",3,0,0,0x4},{"shlwapi.dll","SHUnicodeToAnsi",3,0,0,0x4},{"shlwapi.dll","SHAnsiToAnsi",3,0,0,0x4},{"shlwapi.dll","SHUnicodeToUnicode",3,0,0,0x4},
        {"shlwapi.dll","StrRetToBufA",4,0,0,0x8},{"shlwapi.dll","StrRetToBufW",4,0,0,0x8},{"shlwapi.dll","StrRetToStrA",3},{"shlwapi.dll","StrRetToStrW",3},{"shlwapi.dll","StrRetToBSTR",3},
        {"internal-shell-char","CharUpperNoDBCSA",1},
        {"internal-shell-char","CharUpperNoDBCSW",1},
        {"internal-shell-char","CharLowerNoDBCSA",1},
        {"internal-shell-char","CharLowerNoDBCSW",1},
        {"shlwapi.dll","IsCharSpaceA",1,0,0,0x1},{"shlwapi.dll","IsCharSpaceW",1,0,0,0x1},
        // kernelbase（由 kernel32 承接）的其他 C1 字元分類：BLANK、CNTRL、DIGIT、PUNCT、XDIGIT。
        {"kernel32.dll","IsCharBlankW",1,0,0,0x1},{"kernel32.dll","IsCharCntrlW",1,0,0,0x1},{"kernel32.dll","IsCharDigitW",1,0,0,0x1},
        {"kernel32.dll","IsCharPunctW",1,0,0,0x1},{"kernel32.dll","IsCharXDigitW",1,0,0,0x1},{"shlwapi.dll","SHStripMneumonicA",1},{"shlwapi.dll","SHStripMneumonicW",1},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_shlwapi_string(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const bool wide=name.ends_with('W') || name=="StrFormatByteSizeEx";
    const unsigned unit=wide?2U:1U;
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto text=[&](std::uint64_t address,std::size_t limit=SIZE_MAX){return shell_text(memory,address,wide,limit);};
    // 寫入 NUL 結尾字串（W：UTF-16；A：UTF-8），最多 capacity 個單位（含 NUL）；回傳寫入的單位數（不含 NUL）。
    const auto write_as=[&](std::uint64_t address,const std::u16string& value,std::size_t capacity,bool as_wide)->std::size_t{
        if(!capacity)return 0;
        if(as_wide){const auto count=std::min(value.size(),capacity-1);auto data=utf16_payload(value.substr(0,count));data.push_back(0);data.push_back(0);memory.write(GuestAddress{address},data);return count;}
        auto bytes=utf16_to_utf8_lossy(value);std::size_t count=std::min(bytes.size(),capacity-1);
        while(count<bytes.size() && count && (static_cast<unsigned char>(bytes[count])&0xc0U)==0x80U)--count; // 不切斷 UTF-8 字元
        std::vector<std::uint8_t> data(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count));data.push_back(0);memory.write(GuestAddress{address},data);return count;
    };
    const auto write=[&](std::uint64_t address,const std::u16string& value,std::size_t capacity){return write_as(address,value,capacity,wide);};
    // CoTaskMemAlloc 配置的 NUL 結尾複本（as_wide：UTF-16，否則 UTF-8）；失敗回 0。
    const auto task_copy=[&](const std::u16string& value,bool as_wide)->std::uint64_t{
        std::vector<std::uint8_t> data;if(as_wide){data=utf16_payload(value);data.push_back(0);data.push_back(0);}else {const auto bytes=utf16_to_utf8_lossy(value);data.assign(bytes.begin(),bytes.end());data.push_back(0);}
        auto block=nested_service(state,memory,"ole32.dll","CoTaskMemAlloc",{data.size()});if(x86_)block&=UINT32_MAX;if(block)memory.write(GuestAddress{block},data);return block;};
    const unsigned pointer_bits=x86_?32U:64U;
    try{
        if(service.family=="internal-shell-char"){
            // NoDBCS 以系統語系就地轉換，接受的字串須能連同 NUL 放入 MAX_PATH。
            const auto address=arg(0);
            if(!address)return done(0);
            unsigned length=0;
            while(length<260 && memory.load(GuestAddress{address+length*unit},wide?16:8))++length;
            if(!length || length==260)return done(0);
            const auto flags=name.starts_with("CharUpper")?0x200U:0x100U;
            const auto mapped=nested_service(state,memory,"kernel32.dll",wide?"LCMapStringW":"LCMapStringA",
                                             {0x800,flags,address,length,address,length});
            return done(mapped?address:0);
        }
        if(name.starts_with("StrChr") || name.starts_with("StrRChr")){
            // StrChr(str, ch)／StrChrNW(str, ch, cchMax)／StrRChr(start, end 可為 NULL, ch)。
            if(!arg(0))return done(0);
            const bool reverse=name.starts_with("StrRChr"),fold=name.find('I')!=name.npos && name!="StrChrNW";
            const auto value=shell_character(arg(reverse?2:1),wide);
            auto source=text(arg(0),name=="StrChrNW"?u32(2):SIZE_MAX);
            std::size_t count=source.characters.size();
            if(reverse && arg(1)){
                if(arg(1)<arg(0))return done(0);
                // 參考實作的 A 版包含 end 指向的字元，W 版則以 end 為上限。
                const auto end=arg(1)-arg(0);
                const auto stop=std::find_if(source.characters.begin(),source.characters.end(),
                    [&](const auto& c){return wide?c.offset>=end:c.offset>end;});
                count=static_cast<std::size_t>(stop-source.characters.begin());
            }
            if(reverse){for(std::size_t i=count;i-->0;)if(shell_equal(source.characters[i].text,value,fold))return done(arg(0)+source.characters[i].offset);}
            else for(std::size_t i=0;i<count;++i)if(shell_equal(source.characters[i].text,value,fold))return done(arg(0)+source.characters[i].offset);
            return done(0);
        }
        if(name.starts_with("StrCSpn") || name.starts_with("StrSpn") || name.starts_with("StrPBrk")){
            // StrCSpn：第一個屬於集合的字元前的長度；StrSpn：開頭全屬於集合的長度；StrPBrk：第一個屬於集合的字元位址。長度以字元（A 版為位元組）計。
            if(!arg(0) || !arg(1))return done(0);
            const bool fold=name.ends_with("IA") || name.ends_with("IW"),span=name.starts_with("StrSpn"),pbrk=name.starts_with("StrPBrk");
            const auto source=text(arg(0)),set=text(arg(1));
            const auto member=[&](const ShellCharacter& c){return std::any_of(set.characters.begin(),set.characters.end(),[&](const auto& s){return shell_equal(c.text,s.text,fold);});};
            for(const auto& c:source.characters)if(member(c)!=span){if(pbrk)return done(arg(0)+c.offset);return done(c.offset/unit);}
            return done(pbrk?0:source.bytes/unit);
        }
        if(name.starts_with("StrRStr")){
            const auto start=arg(0),end=arg(1);
            const bool fold=name.ends_with("IA") || name.ends_with("IW");
            if(!start || !arg(2) || (end && end<=start))return done(0);
            const auto search=text(arg(2));
            if(fold && !search.bytes)return done(0);
            // Last 限制候選起點；已開始的比對仍可跨過 Last。
            const auto limit=end?end-start:UINT64_MAX;
            const auto extra=search.bytes>unit?search.bytes-unit:0;
            const auto byte_limit=limit+std::min<std::uint64_t>(UINT64_MAX-limit,extra);
            const auto source=shell_text(memory,start,wide,SIZE_MAX,byte_limit);
            const auto needle=search.joined();
            const auto count=search.characters.size();
            for(std::size_t i=source.characters.size();i-->0;){
                const auto offset=source.characters[i].offset;
                if(offset>=limit || count>source.characters.size()-i)continue;
                if(shell_equal(source.joined(i,i+count),needle,fold))return done(start+offset);
            }
            return done(0);
        }
        if(name.starts_with("StrCmpC") || name.starts_with("StrCmpIC") || name.starts_with("StrCmpNC") || name.starts_with("StrCmpNIC")){
            const bool fold=name.find("IC")!=name.npos,bounded=name.starts_with("StrCmpN");
            if(!arg(0) || !arg(1))return done(static_cast<std::uint32_t>(arg(0)==arg(1)?0:arg(0)?1:-1));
            const auto limit=bounded?static_cast<std::uint64_t>(std::max(0,static_cast<std::int32_t>(u32(2)))):UINT64_MAX;
            const auto read=[&](std::uint64_t address){
                const auto c=memory.load(GuestAddress{address},wide?16:8);
                return fold && c>='A' && c<='Z'?c+32:c;
            };
            for(std::uint64_t i=0;i<limit;++i){
                const auto left=read(arg(0)+i*unit),right=read(arg(1)+i*unit);
                if(left!=right)return done(left<right?UINT32_MAX:1U);
                if(!left)break;
            }
            return done(0);
        }
        if(name=="StrCpyW" || name=="StrCpyNW" || name=="StrCatW" || name.starts_with("StrNCat") || name.starts_with("StrCatBuff")){
            // StrCpyNW／StrNCat 最多複製 cchMax-1 個字元並附加 NUL；StrCatBuff 的 cchDestBuffSize 為整個目的緩衝區大小。
            if(!arg(0) || !arg(1))return done(arg(0));
            const auto source=text(arg(1));
            std::u16string payload=source.joined();
            if(name=="StrCpyW")return (void)write(arg(0),payload,payload.size()+1),done(arg(0));
            if(name=="StrCpyNW"){const auto cch=static_cast<std::int32_t>(u32(2));if(cch>0)write(arg(0),payload,static_cast<std::size_t>(cch));return done(arg(0));}
            const auto existing=text(arg(0));const auto at=arg(0)+existing.bytes;
            if(name=="StrCatW")return (void)write(at,payload,payload.size()+1),done(arg(0));
            if(name.starts_with("StrNCat")){const auto cch=static_cast<std::int32_t>(u32(2));if(cch>0)write(at,payload,static_cast<std::size_t>(cch));return done(arg(0));}
            const auto total=static_cast<std::int32_t>(u32(2));const auto used=existing.bytes/unit;
            if(total>0 && static_cast<std::size_t>(total)>used+1)write(at,payload,static_cast<std::size_t>(total)-used);
            return done(arg(0));
        }
        if(name=="StrCatChainW"){
            // (Dest, cchDst, ichAt（-1 表示目前結尾）, Src)：回傳串接後 NUL 的索引；一律保持 NUL 結尾。
            const auto capacity=u32(1);if(!arg(0) || !capacity)return done(0);
            std::size_t at=u32(2)==UINT32_MAX?read_utf16(memory,GuestAddress{arg(0)},capacity).size():u32(2);
            if(at>=capacity)at=capacity-1;
            const auto source=arg(3)?read_utf16(memory,GuestAddress{arg(3)},1U<<20):std::u16string{};
            const auto count=std::min(source.size(),static_cast<std::size_t>(capacity)-1-at);
            auto data=utf16_payload(source.substr(0,count));data.push_back(0);data.push_back(0);memory.write(GuestAddress{arg(0)+at*2},data);
            return done(at+count);
        }
        if(name.starts_with("StrDup")){
            // LocalAlloc(LMEM_FIXED) 配置的複本，由 LocalFree 釋放。
            if(!arg(0))return done(0);
            const auto source=text(arg(0));const auto size=source.bytes+unit;
            auto block=nested_service(state,memory,"kernel32.dll","LocalAlloc",{0,size});if(x86_)block&=UINT32_MAX;if(!block)return done(0);
            std::vector<std::uint8_t> data(size);memory.read(GuestAddress{arg(0)},std::span(data).first(source.bytes));memory.write(GuestAddress{block},data);
            return done(block);
        }
        if(name.starts_with("StrToInt")){
            // StrToInt：可選負號與十進位數字；StrToIntEx／StrToInt64Ex 另略過前導空白、接受 +，STIF_SUPPORT_HEX（1）時接受 0x 前綴。
            const bool extended=name.find("Ex")!=name.npos,large=name.starts_with("StrToInt64");
            if(!arg(0) || (extended && !arg(2)))return done(0);
            const auto source=text(arg(0)).joined();std::size_t at=0;
            if(extended)while(at<source.size() && (source[at]==u' ' || source[at]==u'\t'))++at;
            bool negative=false;if(at<source.size() && (source[at]==u'-' || (extended && source[at]==u'+'))){negative=source[at]==u'-';++at;}
            unsigned base=10;
            if(extended && (u32(1)&1U) && at+1<source.size() && source[at]==u'0' && (source[at+1]==u'x' || source[at+1]==u'X')){base=16;at+=2;}
            std::uint64_t value=0;bool any=false;
            for(;at<source.size();++at){const auto c=source[at];unsigned digit=99;
                if(c>=u'0' && c<=u'9')digit=c-u'0';else if(base==16 && c>=u'a' && c<=u'f')digit=c-u'a'+10;else if(base==16 && c>=u'A' && c<=u'F')digit=c-u'A'+10;
                if(digit>=base)break;value=value*base+digit;any=true;}
            const auto result=negative?0-value:value;
            if(!extended)return done(static_cast<std::uint32_t>(result));
            memory.store(GuestAddress{arg(2)},large?64:32,result);return done(any?1U:0U);
        }
        if(name.starts_with("StrFormatByteSize") || name.starts_with("StrFormatKBSize")){
            // StrFormatByteSizeA(DWORD)、W／64A（LONGLONG）、Ex（ULONGLONG, flags, buf, cch → HRESULT）；KB 版向上取整並加千分位。
            const bool ex=name=="StrFormatByteSizeEx",kb=name.starts_with("StrFormatKBSize");
            const auto size=name=="StrFormatByteSizeA"?std::uint64_t{u32(0)}:arg(0);
            const auto buffer=arg(ex?2:1);const auto capacity=u32(ex?3:2);
            if(ex && (u32(1)!=1 && u32(1)!=2))return done(0x80070057U);
            if(!buffer || !capacity)return done(ex?0x80070057U:0U);
            std::u16string value;
            if(kb){
                auto kilobytes=std::to_string((size+1023)/1024);for(auto at=static_cast<std::ptrdiff_t>(kilobytes.size())-3;at>0;at-=3)kilobytes.insert(static_cast<std::size_t>(at),",");
                kilobytes+=" KB";value.assign(kilobytes.begin(),kilobytes.end());
            }else value=byte_size_text(size,!ex || u32(1)==2);
            write(buffer,value,capacity);return done(ex?0U:buffer);
        }
        if(name.starts_with("StrFromTimeInterval")){
            // (Buffer, cch, TimeMS, Digits)：有效位數以顯示欄位的數字串計（例如 1 分 14 秒為 "1"＋"14"），超出部分歸零後輸出 " N hr N min N sec"。
            const auto capacity=u32(1);const auto seconds=std::uint64_t{u32(2)}/1000;const auto digits=static_cast<std::int32_t>(u32(3));
            const std::uint64_t hours=seconds/3600,minutes=seconds/60%60,rest=seconds%60;
            const auto hours_text=hours?std::to_string(hours):std::string{};
            auto pad=[](std::uint64_t v,std::size_t width){auto t=std::to_string(v);if(t.size()<width)t.insert(0,width-t.size(),'0');return t;};
            const auto minutes_text=hours?pad(minutes,2):minutes?std::to_string(minutes):std::string{};
            const auto seconds_text=(hours || minutes)?pad(rest,2):std::to_string(rest);
            auto all=hours_text+minutes_text+seconds_text;
            if(digits>0 && all.size()>static_cast<std::size_t>(digits))for(auto i=static_cast<std::size_t>(digits);i<all.size();++i)all[i]='0';
            const auto field=[&](std::size_t at,std::size_t width){return width?std::stoull(all.substr(at,width)):0ULL;};
            const auto h=field(0,hours_text.size()),m=field(hours_text.size(),minutes_text.size()),sec=field(hours_text.size()+minutes_text.size(),seconds_text.size());
            std::string out;
            if(h)out+=" "+std::to_string(h)+" hr";if(m)out+=" "+std::to_string(m)+" min";if(sec || out.empty())out+=" "+std::to_string(sec)+" sec";
            const std::u16string value(out.begin(),out.end());
            if(!arg(0) || !capacity)return done(value.size());
            return done(write(arg(0),value,capacity));
        }
        if(name.starts_with("StrIsIntlEqual") || name.starts_with("IntlStrEqWorker")){
            // (CaseSensitive, s1, s2, nChar)：前 nChar 個字元（-1 為整段）依語系比較是否相等。
            if(!arg(1) || !arg(2))return done(0);
            const auto limit=static_cast<std::int32_t>(u32(3));const auto count=limit<0?SIZE_MAX:static_cast<std::size_t>(limit);
            const auto left=text(arg(1),count).joined(),right=text(arg(2),count).joined();
            const auto compared=windows_nls_compare(left,right,0x409,(arg(0)&0xffU)?0U:1U);
            return done(compared && *compared==0?1U:0U);
        }
        if(name.starts_with("SHStrDup")){
            // (source, LPWSTR* out)：輸出一律為 UTF-16；A 版依 ACP（UTF-8）轉換。
            if(!arg(1))return done(0x80070057U);memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(pointer_bits),0);if(!arg(0))return done(0x80070057U);
            const auto block=task_copy(text(arg(0)).joined(),true);if(!block)return done(0x8007000eU);
            memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(pointer_bits),block);return done(0);
        }
        if(name=="#364"){
            // ANSI 入口直接複製位元組，截斷也視為可往返。
            (void)nested_service(state,memory,"kernel32.dll","lstrcpynA",{arg(1),arg(0),u32(2)});
            return done(1);
        }
        if(name=="#365"){
            const auto capacity=static_cast<std::int32_t>(u32(2));
            if(!arg(0) || !arg(1) || capacity<=0)return done(0);
            const auto source=read_utf16(memory,GuestAddress{arg(0)},1U<<20);
            const auto written=write_as(arg(1),source,static_cast<std::size_t>(capacity),false);
            // 參考實作使用 MAX_PATH 個 WCHAR（含 NUL）作反向轉換。
            if(source.size()>=260)return done(0);
            const auto encoded=read_ansi(memory,arg(1),written+1);
            return done(utf8_to_utf16_lossy(encoded)==source);
        }
#include "windows_shell_codec.inc"
        if(name.starts_with("StrRetTo")){
            // STRRET：uType 之後是指標對齊的聯集。WSTR（0）取 pOleStr 後以 CoTaskMemFree 釋放；OFFSET（1）為 pidl＋uOffset 的 ANSI；CSTR（2）為內含的 260 位元組 ANSI。
            if(!arg(0))return done(0x80070057U);const unsigned field=x86_?4U:8U;const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));
            std::u16string value;
            if(type==0){const auto pointer=memory.load(GuestAddress{arg(0)+field},static_cast<std::uint8_t>(pointer_bits));
                if(pointer){value=shell_text(memory,pointer,true).joined();(void)nested_service(state,memory,"ole32.dll","CoTaskMemFree",{pointer});memory.store(GuestAddress{arg(0)+field},static_cast<std::uint8_t>(pointer_bits),0);}}
            else if(type==1){if(!arg(1))return done(0x80004005U);value=shell_text(memory,arg(1)+memory.load(GuestAddress{arg(0)+field},32),false).joined();}
            else if(type==2){std::string bytes;for(unsigned i=0;i<260;++i){const auto c=static_cast<char>(memory.load(GuestAddress{arg(0)+field+i},8));if(!c)break;bytes.push_back(c);}value=utf8_to_utf16_lossy(bytes);}
            else return done(0x80070057U);
            if(name.starts_with("StrRetToBuf")){if(!arg(2) || !u32(3))return done(0x80070057U);write(arg(2),value,u32(3));return done(0);}
            if(!arg(2))return done(0x80070057U);
            if(name=="StrRetToBSTR"){
                GuestScratch temporary(*this,memory,state);const auto source=temporary.text(value);if(!source)return done(0x8007000eU);
                auto bstr=nested_service(state,memory,"oleaut32.dll","SysAllocStringLen",{source,value.size()});if(x86_)bstr&=UINT32_MAX;
                memory.store(GuestAddress{arg(2)},static_cast<std::uint8_t>(pointer_bits),bstr);return done(bstr?0U:0x8007000eU);
            }
            const auto block=task_copy(value,wide);memory.store(GuestAddress{arg(2)},static_cast<std::uint8_t>(pointer_bits),block);return done(block?0U:0x8007000eU);
        }
        if(name.starts_with("IsCharSpace") || name=="IsCharBlankW" || name=="IsCharCntrlW" || name=="IsCharDigitW" || name=="IsCharPunctW" || name=="IsCharXDigitW"){
            // GetStringTypeW 的 C1 類別（SPACE 8、BLANK 0x40、CNTRL 0x20、DIGIT 4、PUNCT 0x10、XDIGIT 0x80）；A 版的單一位元組只有 ASCII 是完整字元（ACP＝UTF-8）。
            const auto mask=name.starts_with("IsCharSpace")?0x8U:name=="IsCharBlankW"?0x40U:name=="IsCharCntrlW"?0x20U:name=="IsCharDigitW"?0x4U:name=="IsCharPunctW"?0x10U:0x80U;
            const auto c=wide?static_cast<char16_t>(arg(0)):static_cast<char16_t>(arg(0)&0xffU);if(!wide && c>=0x80)return done(0);
            GuestScratch temporary(*this,memory,state);const auto buffer=temporary.allocate(8);if(!buffer)return done(0);memory.store(GuestAddress{buffer},16,c);
            if(!nested_service(state,memory,"kernel32.dll","GetStringTypeW",{1,buffer,1,buffer+4}))return done(0);
            return done((memory.load(GuestAddress{buffer+4},16)&mask)?1U:0U);
        }
        if(name.starts_with("SHStripMneumonic")){
            const auto bits = static_cast<std::uint8_t>(unit * 8);
            const auto read = [&](std::uint64_t p){ return memory.load(GuestAddress{p}, bits); };
            auto marker = arg(0);
            auto mnemonic = read(marker);
            auto current = mnemonic;
            while(current && current != '&'){
                marker += unit;
                current = read(marker);
            }
            if(current){
                const auto next = read(marker + unit);
                if(next){
                    if(next != '&') mnemonic = next;
                    auto end = marker + unit;
                    while(read(end)) end += unit;
                    for(auto p = marker; p < end; p += unit)
                        memory.store(GuestAddress{p}, bits, read(p + unit));
                }
            }
            return done(mnemonic);
        }
        if(name.starts_with("ChrCmpI")){
            // 相等（不分大小寫）回 0，否則回非零。
            return done(shell_equal(shell_character(arg(0),wide),shell_character(arg(1),wide),true)?0U:1U);
        }
    }catch(const std::bad_alloc&){return done(0);}
    throw HostCallFault("未知的 shlwapi 字串服務");
}
}
