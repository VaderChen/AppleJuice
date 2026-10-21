#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <locale.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <functional>
#include <wctype.h>

namespace x64arm {
std::span<const WindowsServiceDescriptor> userstring_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","wsprintfA",2},{"user32.dll","wsprintfW",2},{"user32.dll","wvsprintfA",3},{"user32.dll","wvsprintfW",3},
        {"user32.dll", "CharUpperA", 1}, {"user32.dll", "CharUpperW", 1},
        {"user32.dll", "CharLowerA", 1}, {"user32.dll", "CharLowerW", 1},
        {"user32.dll", "CharUpperBuffA", 2}, {"user32.dll", "CharUpperBuffW", 2},
        {"user32.dll", "CharLowerBuffA", 2}, {"user32.dll", "CharLowerBuffW", 2},
        {"user32.dll", "CharNextA", 1}, {"user32.dll", "CharNextW", 1}, {"user32.dll", "CharNextExA", 3},
        {"user32.dll", "CharPrevA", 2}, {"user32.dll", "CharPrevW", 2}, {"user32.dll", "CharPrevExA", 4}
    };
    return table;
}
namespace {
// USER32 格式子集不含浮點數及 %n；不將客體 format / va_list 傳入宿主 CRT。
template<class Character>
std::basic_string<Character> user32_format(const GuestMemory& memory,std::basic_string_view<Character> format,bool wide,const std::function<std::uint64_t()>& next,bool& truncated){
    std::basic_string<Character> result;
    const auto append=[&](std::basic_string_view<Character> part){const auto count=std::min<std::size_t>(part.size(),1023-result.size());result.append(part.substr(0,count));truncated|=count<part.size();};
    const auto from_narrow=[&](std::string_view bytes){if constexpr(sizeof(Character)==1)return std::string(bytes);else return utf8_to_utf16_lossy(bytes);};
    const auto from_wide=[&](std::u16string_view text){if constexpr(sizeof(Character)==2)return std::u16string(text);else {std::u16string clean(text);for(std::size_t i=0;i<clean.size();++i){if(clean[i]>=0xd800&&clean[i]<=0xdbff){if(i+1<clean.size()&&clean[i+1]>=0xdc00&&clean[i+1]<=0xdfff)++i;else clean[i]=0xfffd;}else if(clean[i]>=0xdc00&&clean[i]<=0xdfff)clean[i]=0xfffd;}return utf16_to_utf8(clean);}};
    for(std::size_t at=0;at<format.size();){
        if(result.size()==1023){truncated=true;break;}
        if(format[at]!='%'){result.push_back(format[at++]);continue;}++at;
        if(at<format.size()&&format[at]=='%'){result.push_back(format[at++]);continue;}
        bool left=false,prefix=false,zero=false;
        while(at<format.size()&&(format[at]=='-'||format[at]=='#'||format[at]=='0')){left|=format[at]=='-';prefix|=format[at]=='#';zero|=format[at]=='0';++at;}
        const auto decimal=[&](){unsigned value=0;while(at<format.size()&&format[at]>='0'&&format[at]<='9'){value=std::min(2048U,value*10+static_cast<unsigned>(format[at++]-'0'));}return value;};
        unsigned width=decimal();std::optional<unsigned> precision;
        if(at<format.size()&&format[at]=='.'){++at;precision=decimal();}
        Character length=0;bool wide_integer=false;
        if(at<format.size()&&(format[at]=='h'||format[at]=='l'||format[at]=='w'))length=format[at++];
        else if(at<format.size()&&format[at]=='I'){++at;wide_integer=true;if(at+1<format.size()&&format[at]=='6'&&format[at+1]=='4')at+=2;else if(at+1<format.size()&&format[at]=='3'&&format[at+1]=='2'){at+=2;wide_integer=false;}}
        if(at==format.size())break;const auto kind=format[at++];
        const bool string=kind=='s'||kind=='S',character=kind=='c'||kind=='C';const bool number=kind=='d'||kind=='i'||kind=='u'||kind=='x'||kind=='X'||kind=='p';
        if(!string&&!character&&!number){result.push_back(kind);continue;}
        const auto value=next();std::basic_string<Character> field;std::basic_string<Character> lead;
        if(string||character){
            const bool source_wide=length=='l'||length=='w'||(length!='h'&&(wide?(kind=='s'||kind=='c'):(kind=='S'||kind=='C')));
            if(character){if(value || (length!='h'&&length!='l')){if(source_wide)field=from_wide(std::u16string(1,static_cast<char16_t>(value)));else field=from_narrow(std::string(1,static_cast<char>(value)));}}
            else if(!value)field=from_narrow("(null)");
            else if(source_wide){std::u16string text;const auto limit=std::min<std::size_t>(precision.value_or(1024),1024);for(std::size_t i=0;i<limit;++i){if(i>(UINT64_MAX-value)/2)throw HostCallFault("wsprintf UTF-16 地址溢位");const auto ch=static_cast<char16_t>(memory.load(GuestAddress{value+i*2},16));if(!ch)break;text.push_back(ch);}field=from_wide(text);}
            else {std::string text;const auto limit=std::min<std::size_t>(precision.value_or(4096),4096);for(std::size_t i=0;i<limit;++i){if(i>UINT64_MAX-value)throw HostCallFault("wsprintf ANSI 地址溢位");const auto ch=static_cast<char>(memory.load(GuestAddress{value+i},8));if(!ch)break;text.push_back(ch);}field=from_narrow(text);}
            if(string&&precision&&field.size()>*precision)field.resize(*precision);zero=false;
        }else {
            auto magnitude=value;const auto bits=kind=='p'||wide_integer?64U:length=='h'?16U:32U;
            if(bits<64)magnitude&=(UINT64_C(1)<<bits)-1;
            if((kind=='d'||kind=='i')&&(magnitude&(UINT64_C(1)<<(bits-1)))){lead.push_back('-');magnitude=(~magnitude+1)&(bits==64?UINT64_MAX:(UINT64_C(1)<<bits)-1);}
            const bool hex=kind=='x'||kind=='X'||kind=='p';std::array<char,32> digits{};const auto converted=std::to_chars(digits.data(),digits.data()+digits.size(),magnitude,hex?16:10);std::string text(digits.data(),converted.ptr);
            if(kind=='X'||kind=='p')for(auto& ch:text)if(ch>='a'&&ch<='f')ch=static_cast<char>(ch-'a'+'A');field=from_narrow(text);
            if(hex&&prefix){lead.push_back('0');lead.push_back(kind=='x'?'x':'X');}
            if(kind=='p'){width=16;zero=true;}
            if(precision&&field.size()<*precision)field.insert(0,*precision-field.size(),'0');
        }
        width=std::min(width,2048U);const auto occupied=lead.size()+field.size();const auto padding=width>occupied?width-occupied:0;
        if(!left&&!zero)append(std::basic_string<Character>(padding,' '));append(lead);
        if(!left&&zero)append(std::basic_string<Character>(padding,'0'));append(field);
        if(left)append(std::basic_string<Character>(padding,' '));
    }return result;
}
}

namespace {
bool supported_codepage(std::uint16_t codepage) {
    return codepage == 0 || codepage == 1 || codepage == 65001 || codepage == 1252;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_userstring(GuestState& state, GuestMemory& memory,
                                                              const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state, memory, service);
    const auto symbol = service.symbol;
    const bool wide = symbol.ends_with('W');
    std::uint64_t result = 0;
    const auto fail = [&](std::uint32_t error) { last_error_ = error; result = 0; };
    if(symbol=="wsprintfA" || symbol=="wsprintfW" || symbol=="wvsprintfA" || symbol=="wvsprintfW"){
        unsigned index=2;std::uint64_t va=symbol.starts_with("wvs")?arg(2):0;const bool indirect=symbol.starts_with("wvs");
        const auto next=[&](){if(!indirect)return arg(index++);const auto value=memory.load(GuestAddress{va},64);if(va>UINT64_MAX-8)throw HostCallFault("wsprintf va_list 地址溢位");va+=8;return value;};
        bool truncated=false;std::vector<std::uint8_t> output;
        if(wide){const auto format=read_utf16(memory,GuestAddress{arg(1)},65536);const auto text=user32_format<char16_t>(memory,format,true,next,truncated);result=text.size();output=utf16_bytes(text);}
        else {const auto format=read_ansi(memory,arg(1),65536);const auto text=user32_format<char>(memory,format,false,next,truncated);result=text.size();output.assign(text.begin(),text.end());output.push_back(0);}
        memory.write(GuestAddress{arg(0)},output);if(truncated){last_error_=122;result=1024;}
    } else if (symbol.starts_with("CharNext") || symbol.starts_with("CharPrev")) {
        const bool extended = symbol.ends_with("ExA"), previous = symbol.starts_with("CharPrev");
        const auto shift = extended ? 1U : 0U;
        if (extended && (!supported_codepage(static_cast<std::uint16_t>(arg(0))) ||
                         static_cast<std::uint32_t>(arg(previous ? 3 : 2)))) fail(87);
        else if (previous) {
            const auto start = arg(shift), current = arg(shift + 1);
            const auto stride = wide ? 2U : 1U;
            result = current <= start || current - start < stride ? start : current - stride;
        } else {
            const auto pointer = arg(shift);
            const auto stride = wide ? 2U : 1U;
            result = memory.load(GuestAddress{pointer}, wide ? 16 : 8) ? pointer + stride : pointer;
        }
        // CharNextExA／CharPrevExA 採 DBCS lead-byte 規則。現有 ACP/OEM=UTF-8 與1252
        // 都沒有 DBCS lead ranges，故 ANSI 每次一個 byte；不是 UTF-8 scalar iterator。
    } else if (symbol.starts_with("CharUpper") || symbol.starts_with("CharLower")) {
        const bool upper = symbol.starts_with("CharUpper"), buffer = symbol.find("Buff") != std::string_view::npos;
        if (!buffer && (arg(0) >> 16) == 0) {
            const auto character = static_cast<char16_t>(arg(0));
            // ANSI 單字元模式只有一個 ACP byte；UTF-8 高位元組本身不是完整字元。
            result = wide || character < 128 ? unicode_case_unit(character, upper) : character;
        } else {
            std::u16string text;
            const auto count = buffer ? static_cast<std::uint32_t>(arg(1)) : 0U;
            if (buffer && !count) { arg.return_to(state, 0); return std::nullopt; }
            if (buffer && count > output_limit / (wide ? 2U : 1U)) {
                fail(87); arg.return_to(state, result); return std::nullopt;
            }
            std::size_t capacity = 0;
            try {
                if (wide) {
                    if (buffer) {
                        memory.check(GuestAddress{arg(0)}, static_cast<std::size_t>(count) * 2, Access::Read);
                        text.reserve(count);
                        for (std::uint32_t i = 0; i < count; ++i)
                            text.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(0) + i * 2ULL}, 16)));
                    } else text = read_utf16(memory, GuestAddress{arg(0)}, output_limit / 2);
                    capacity = text.size() * 2 + (buffer ? 0 : 2);
                } else {
                    std::string bytes;
                    if (buffer) {
                        bytes.resize(count);
                        memory.read(GuestAddress{arg(0)}, std::span<std::uint8_t>{reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()});
                    } else bytes = read_ansi(memory, arg(0), output_limit);
                    capacity = bytes.size() + (buffer ? 0 : 1);
                    text = utf8_to_utf16(bytes);
                }
                for (auto& character : text) character = unicode_case_unit(character, upper);
                std::vector<std::uint8_t> output;
                if (wide) {
                    output = utf16_bytes(text);
                    if (buffer) output.resize(text.size() * 2);
                } else {
                    const auto encoded = utf16_to_utf8(text);
                    output.assign(encoded.begin(), encoded.end());
                    if (!buffer) output.push_back(0);
                }
                if (output.size() > capacity) fail(122);
                else {
                    memory.check(GuestAddress{arg(0)}, output.size(), Access::Write);
                    memory.write(GuestAddress{arg(0)}, output);
                    result = buffer ? (wide ? text.size() : output.size()) : arg(0);
                }
            } catch (const std::invalid_argument&) { fail(1113); }
        }
    } else throw HostCallFault("未知的 USER32 字元服務");
    arg.return_to(state, result);
    return std::nullopt;
}
}
