#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "automation_value.hpp"
#include "windows_locale_data.hpp"
#include "crt_multibyte_state.hpp"
#include "crt_string_conversion.hpp"
#include "crt_time_storage.hpp"
#include "guest_crt_state.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <bit>
#include <chrono>
#include <unicode/udat.h>
#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>
#include <unicode/unum.h>
#include <unicode/udatpg.h>
#include <unicode/uchar.h>
#include <unicode/ucol.h>
#include <unicode/uscript.h>
#include <unicode/ustring.h>
#include <unicode/utrans.h>
#include <unicode/ulocdata.h>
#include "idn_conversion.hpp"
#include "crt_c_locale.hpp"
#include <mutex>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace x64arm {
namespace {
struct Locale {std::u16string name;std::uint32_t id{};bool neutral{};};
std::u16string key(std::u16string value){for(auto& c:value){if(c==u'_')c=u'-';if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);}return value;}
#if defined(__APPLE__)
struct Release {void operator()(const void* value)const{if(value)CFRelease(value);}};
template<class T>using Owned=std::unique_ptr<std::remove_pointer_t<T>,Release>;
std::u16string string(CFStringRef value){if(!value)return {};std::u16string out(static_cast<std::size_t>(CFStringGetLength(value)),0);CFStringGetCharacters(value,CFRangeMake(0,static_cast<CFIndex>(out.size())),reinterpret_cast<UniChar*>(out.data()));std::replace(out.begin(),out.end(),u'_',u'-');return out;}
std::uint32_t locale_code(std::u16string_view name){
    auto identifier=std::u16string(name);std::replace(identifier.begin(),identifier.end(),u'-',u'_');
    Owned<CFStringRef> value(CFStringCreateWithCharacters(nullptr,reinterpret_cast<const UniChar*>(identifier.data()),static_cast<CFIndex>(identifier.size())));
    return CFLocaleGetWindowsLocaleCodeFromLocaleIdentifier(value.get());
}
// Windows 的既有中文地區 LCID 使用 zh-地區；CoreFoundation 帶有額外的 script subtag。
std::u16string windows_name(std::u16string name){
    if(name==u"zh-Hans-CN" || name==u"zh-Hans-SG" || name==u"zh-Hant-TW" || name==u"zh-Hant-HK" || name==u"zh-Hant-MO")name.erase(3,5);
    return name;
}
std::u16string locale_name(std::uint32_t id){Owned<CFStringRef> value(CFLocaleCreateLocaleIdentifierFromWindowsLocaleCode(nullptr,id));return windows_name(string(value.get()));}
const std::map<std::u16string,Locale>& locales(){
    static const auto catalog=[] {
        std::map<std::u16string,Locale> values;
        Owned<CFArrayRef> names(CFLocaleCopyAvailableLocaleIdentifiers());if(!names)throw std::bad_alloc();
        for(CFIndex i=0;i<CFArrayGetCount(names.get());++i){const auto name=static_cast<CFStringRef>(CFArrayGetValueAtIndex(names.get(),i));
            auto text=windows_name(string(name));if(text.empty() || text.size()>=85 || text.find(u'@')!=text.npos)continue;
            Owned<CFDictionaryRef> parts(CFLocaleCreateComponentsFromLocaleIdentifier(nullptr,name));
            const bool neutral=!parts || !CFDictionaryContainsKey(parts.get(),kCFLocaleCountryCode);
            auto id=CFLocaleGetWindowsLocaleCodeFromLocaleIdentifier(name);
            // ICU 有時將未知地區回退到語言 ID；這些 CLDR locale 不能冒用其他地區的 LCID。
            if(!neutral && key(locale_name(id))!=key(text))id=0x1000;
            values.emplace(key(text),Locale{text,id?id:0x1000,neutral});
        }
        values.emplace(u"",Locale{u"",0x7f,false});return values;
    }();return catalog;
}
#else
const std::map<std::u16string,Locale>& locales(){static const std::map<std::u16string,Locale> empty;return empty;}
std::u16string locale_name(std::uint32_t){return {};}
std::uint32_t locale_code(std::u16string_view){return 0;}
#endif
std::optional<Locale> named(std::u16string_view value){const auto found=locales().find(key(std::u16string(value)));if(found==locales().end())return {};return found->second;}
std::optional<Locale> numbered(std::uint32_t id){
    if(id==0x400 || id==0x800)id=0x409;
    if(id==0x7f)return named(u"");
    if(!id || id==0x1000 || id==0xc00 || id==0x1400 || id>0xffff)return {};
    const auto name=locale_name(id);if(name.empty() || locale_code(name)!=id)return {};
    auto value=named(name);if(!value)return {};value->id=id;return value;
}
// ANSI NLS uses the locale code page unless LOCALE_USE_CP_ACP is set.
const char* ansi_encoding(const Locale& locale,bool acp=false){
    if(acp)return "UTF-8";
    const auto id=locale.id&0xffffU,language=id&0x3ffU;
    switch(language){case 0x11:return "windows-932";case 0x12:return "windows-949";case 4:return id==0x404 || id==0xc04 || id==0x1404?"windows-950":"windows-936";
    case 0x19:case 0x22:case 0x23:return "windows-1251";case 8:return "windows-1253";case 0x1f:return "windows-1254";case 0xd:return "windows-1255";case 1:return "windows-1256";case 0x1e:return "windows-874";case 0x2a:return "windows-1258";
    case 5:case 0xe:case 0x15:case 0x18:case 0x1b:case 0x24:return "windows-1250";case 0x25:case 0x26:case 0x27:return "windows-1257";default:return "windows-1252";}
}
std::u16string ansi_input(const GuestMemory& memory,std::uint64_t address,std::int32_t count,bool terminator,const char* encoding,bool strict=false,bool stop_at_null=false){
    std::string bytes;
    if(count<0){bytes=read_ansi(memory,address,1048576);if(terminator)bytes.push_back(0);}
    else {if(count>1048576)throw HostCallFault("ANSI NLS string exceeds limit");bytes.resize(static_cast<std::size_t>(count));memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(bytes.data()),bytes.size()});}
    if(stop_at_null)if(const auto end=bytes.find('\0');end!=bytes.npos)bytes.resize(end);
    UErrorCode error=U_ZERO_ERROR;std::unique_ptr<UConverter,decltype(&ucnv_close)> converter(ucnv_open(encoding,&error),ucnv_close);
    if(U_FAILURE(error))throw HostCallFault("ANSI NLS converter unavailable");
    if(strict){ucnv_setToUCallBack(converter.get(),UCNV_TO_U_CALLBACK_STOP,nullptr,nullptr,nullptr,&error);if(U_FAILURE(error))throw HostCallFault("ANSI NLS strict converter unavailable");}
    error=U_ZERO_ERROR;const auto size=ucnv_toUChars(converter.get(),nullptr,0,bytes.data(),static_cast<std::int32_t>(bytes.size()),&error);
    if(U_FAILURE(error) && error!=U_BUFFER_OVERFLOW_ERROR)throw HostCallFault("ANSI NLS decoding failed");error=U_ZERO_ERROR;
    std::u16string text(static_cast<std::size_t>(size),0);ucnv_toUChars(converter.get(),text.data(),size,bytes.data(),static_cast<std::int32_t>(bytes.size()),&error);
    if(U_FAILURE(error))throw HostCallFault("ANSI NLS decoding failed");return text;
}
std::vector<std::uint8_t> ansi_output(std::u16string_view text,const char* encoding){
    UErrorCode error=U_ZERO_ERROR;std::unique_ptr<UConverter,decltype(&ucnv_close)> converter(ucnv_open(encoding,&error),ucnv_close);
    if(U_FAILURE(error))throw HostCallFault("ANSI NLS converter unavailable");
    const auto size=ucnv_fromUChars(converter.get(),nullptr,0,text.data(),static_cast<std::int32_t>(text.size()),&error);error=U_ZERO_ERROR;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));ucnv_fromUChars(converter.get(),reinterpret_cast<char*>(bytes.data()),size,text.data(),static_cast<std::int32_t>(text.size()),&error);
    if(U_FAILURE(error))throw HostCallFault("ANSI NLS encoding failed");return bytes;
}
std::optional<Locale> specific(Locale value){
    if(!value.neutral)return value;
    // Windows 預設 sublanguage；中文的簡體預設為中國，繁體使用臺灣。
    const auto normalized=key(value.name);
    const auto id=normalized==u"zh" || normalized==u"zh-hans"?0x804U:normalized==u"zh-hant"?0x404U:(value.id&0x3ffU)|0x400U;
    const auto candidate=numbered(id);const auto language=normalized.substr(0,normalized.find(u'-'));
    if(candidate && key(candidate->name).starts_with(language+u"-"))return candidate;
    return {};
}
using NumberFormat=std::unique_ptr<UNumberFormat,decltype(&unum_close)>;
std::u16string number_symbol(UNumberFormat* format,UNumberFormatSymbol symbol){
    UErrorCode error=U_ZERO_ERROR;const auto size=unum_getSymbol(format,symbol,nullptr,0,&error);error=U_ZERO_ERROR;
    std::u16string result(static_cast<std::size_t>(size),0);unum_getSymbol(format,symbol,result.data(),size,&error);if(U_FAILURE(error))throw std::runtime_error("數值格式符號查詢失敗");return result;
}
std::u16string number_text(UNumberFormat* format,std::string_view source){
    UErrorCode error=U_ZERO_ERROR;const auto size=unum_formatDecimal(format,source.data(),static_cast<std::int32_t>(source.size()),nullptr,0,nullptr,&error);error=U_ZERO_ERROR;
    std::u16string result(static_cast<std::size_t>(size),0);unum_formatDecimal(format,source.data(),static_cast<std::int32_t>(source.size()),result.data(),size,nullptr,&error);if(U_FAILURE(error))throw std::runtime_error("數值格式化失敗");return result;
}
std::u16string group_number(std::u16string_view source,unsigned grouping,std::u16string_view separator){
    if(!grouping || separator.empty())return std::u16string(source);const auto sizes=std::to_string(grouping);std::size_t at=source.size(),index=0;std::u16string result;
    while(at){const auto width=static_cast<std::size_t>(sizes[index]-'0'),take=width?std::min(at,width):at;
        if(!result.empty())result=std::u16string(separator)+result;result=std::u16string(source.substr(at-take,take))+result;at-=take;if(index+1<sizes.size())++index;
    }return result;
}
std::u16string decorate_number(std::u16string_view pattern,std::u16string_view number,std::u16string_view symbol,std::u16string_view sign){
    std::u16string out;for(auto c:pattern){if(c==u'n')out+=number;else if(c==u'$')out+=symbol;else if(c==u'-')out+=sign;else out+=c;}return out;
}

using DateFormat=std::unique_ptr<UDateFormat,decltype(&udat_close)>;
std::u16string date_pattern(UDateFormat* format){
    UErrorCode error=U_ZERO_ERROR;const auto size=udat_toPattern(format,false,nullptr,0,&error);error=U_ZERO_ERROR;
    std::u16string out(static_cast<std::size_t>(size),0);udat_toPattern(format,false,out.data(),size,&error);if(U_FAILURE(error))throw std::runtime_error("日期格式查詢失敗");return out;
}
std::u16string date_text(UDateFormat* format,UDate stamp){
    UErrorCode error=U_ZERO_ERROR;const auto size=udat_format(format,stamp,nullptr,0,nullptr,&error);error=U_ZERO_ERROR;
    std::u16string out(static_cast<std::size_t>(size),0);udat_format(format,stamp,out.data(),size,nullptr,&error);if(U_FAILURE(error))throw std::runtime_error("日期格式化失敗");return out;
}
std::u16string quote_pattern(std::u16string_view text){std::u16string result=u"'";for(auto c:text){result+=c;if(c==u'\'')result+=c;}result+=u'\'';return result;}
struct DateToken {char16_t field{};std::u16string text;};
std::vector<DateToken> date_tokens(std::u16string_view pattern){
    std::vector<DateToken> result;bool quoted=false;std::u16string literal;
    const auto flush=[&](){if(!literal.empty()){result.push_back({0,std::move(literal)});literal.clear();}};
    for(std::size_t i=0;i<pattern.size();){const auto c=pattern[i++];if(c==u'\''){
        if(i<pattern.size() && pattern[i]==u'\''){literal+=u'\'';++i;}else quoted=!quoted;continue;}
        if(!quoted && ((c>=u'A' && c<=u'Z') || (c>=u'a' && c<=u'z'))){flush();std::u16string run(1,c);while(i<pattern.size() && pattern[i]==c){run+=c;++i;}result.push_back({c,std::move(run)});}
        else literal+=c;
    }flush();return result;
}
std::u16string render_pattern(const std::vector<DateToken>& tokens){
    std::u16string result,literal;for(const auto& token:tokens){if(!token.field)literal+=token.text;else{if(!literal.empty()){result+=quote_pattern(literal);literal.clear();}result+=token.text;}}
    if(!literal.empty())result+=quote_pattern(literal);return result;
}
void month_pattern_context(std::vector<DateToken>& tokens){
    // 星期 ddd/dddd 已轉成 ICU E；引號內的 d 是 literal，不構成日期數字。
    const bool genitive=std::any_of(tokens.begin(),tokens.end(),[](const DateToken& token){return token.field==u'd' && token.text.size()<=2;});
    for(auto& token:tokens)if((token.field==u'M' || token.field==u'L') && token.text.size()>=3)token.text.assign(token.text.size(),genitive?u'M':u'L');
}
std::u16string time_pattern(std::u16string_view source,unsigned flags,bool windows,std::u16string_view marker){
    auto tokens=date_tokens(source);std::vector<DateToken> out;
    for(std::size_t i=0;i<tokens.size();++i){auto token=tokens[i];const auto c=token.field;
        const bool remove=((flags&1U) && (c==u'm' || c==u's')) || ((flags&2U) && c==u's') || ((flags&4U) && (c==u'a' || c==u't'));
        if(remove){if(!out.empty() && !out.back().field)out.pop_back();else if(i+1<tokens.size() && !tokens[i+1].field)++i;continue;}
        if(c==u'h' || c==u'H'){token.field=(flags&8U)?u'H':c;token.text.assign(std::min<std::size_t>(2,token.text.size()),token.field);}
        else if(windows && c==u't'){token.field=0;token.text=std::u16string(token.text.size()==1?marker.substr(0,1):marker);}
        else if(windows && c && c!=u'm' && c!=u's')token.field=0;
        out.push_back(std::move(token));
    }
    return render_pattern(out);
}
std::u16string windows_date_pattern(std::u16string_view source){
    auto tokens=date_tokens(source);
    for(auto& token:tokens){
        const auto c=token.field;const auto size=token.text.size();
        if(c==u'd')token.text.assign(std::min<std::size_t>(size,4),size>=3?u'E':u'd');
        else if(c==u'M')token.text.assign(std::min<std::size_t>(size,4),u'M');
        else if(c==u'y')token.text.assign(size<=2?2:4,u'y');else if(c==u'g')token.text=u"G";
        else token.field=0;
    }month_pattern_context(tokens);return render_pattern(tokens);
}

constexpr std::uint32_t compare_flags=0x0803103fU;
std::u16string counted_string(const GuestMemory& memory,std::uint64_t pointer,std::int32_t count,bool terminate){
    if(count<0){auto value=read_utf16(memory,GuestAddress{pointer});if(terminate)value.push_back(0);return value;}
    memory.check(GuestAddress{pointer},static_cast<std::uint64_t>(count)*2,Access::Read);
    std::u16string value(static_cast<std::size_t>(count),0);
    for(std::size_t i=0;i<value.size();++i)value[i]=static_cast<char16_t>(memory.load(GuestAddress{pointer+i*2},16));
    return value;
}
// 整數轉換不經宿主 double，避免 UINT64_MAX 附近的十進位精度損失。
std::uint32_t automation_integer(std::string_view decimal,unsigned bits,bool unsigned_type,std::uint64_t& output){
    bool negative=false;std::size_t at=0;if(at<decimal.size() && (decimal[at]=='-' || decimal[at]=='+'))negative=decimal[at++]=='-';
    std::string digits;std::int64_t point=0;bool dot=false;
    for(;at<decimal.size() && decimal[at]!='e' && decimal[at]!='E';++at){const auto c=decimal[at];if(c=='.' && !dot){dot=true;continue;}if(c<'0' || c>'9')return 0x80020005U;digits+=c;if(!dot)++point;}
    if(digits.empty())return 0x80020005U;
    if(at<decimal.size()){
        ++at;bool minus=false;if(at<decimal.size() && (decimal[at]=='-' || decimal[at]=='+'))minus=decimal[at++]=='-';if(at==decimal.size())return 0x80020005U;
        std::int64_t exponent=0;for(;at<decimal.size();++at){if(decimal[at]<'0' || decimal[at]>'9')return 0x80020005U;exponent=std::min<std::int64_t>(100000,exponent*10+decimal[at]-'0');}point+=minus?-exponent:exponent;
    }
    const auto first=digits.find_first_not_of('0');if(first==digits.npos){output=0;return 0;}point-=static_cast<std::int64_t>(first);digits.erase(0,first);
    const auto maximum=unsigned_type?(bits==64?UINT64_MAX:(UINT64_C(1)<<bits)-1):negative?(UINT64_C(1)<<(bits-1)):(UINT64_C(1)<<(bits-1))-1;
    if(point>20)return 0x8002000aU;std::uint64_t value=0;
    for(std::int64_t i=0;i<point;++i){const auto digit=static_cast<std::uint64_t>(static_cast<std::size_t>(i)<digits.size()?digits[static_cast<std::size_t>(i)]-'0':0);if(value>maximum/10 || (value==maximum/10 && digit>maximum%10))return 0x8002000aU;value=value*10+digit;}
    if(point>=0 && static_cast<std::uint64_t>(point)<digits.size()){
        const auto index=static_cast<std::size_t>(point);const char digit=digits[index];const bool tail=digits.find_first_not_of('0',index+1)!=digits.npos;
        if(digit>'5' || (digit=='5' && (tail || (value&1)))){if(value==maximum)return 0x8002000aU;++value;}
    }
    if(unsigned_type && negative && value)return 0x8002000aU;output=negative?UINT64_C(0)-value:value;return 0;
}
std::string icu_locale(const Locale& locale){auto result=utf16_to_utf8(locale.name);std::replace(result.begin(),result.end(),'-','_');return result;}
#include "windows_locale_info.inc"
std::u16string transform(std::u16string value,std::u16string_view rule){
    UErrorCode error=U_ZERO_ERROR;
    std::unique_ptr<UTransliterator,decltype(&utrans_close)> trans(utrans_openU(rule.data(),static_cast<std::int32_t>(rule.size()),UTRANS_FORWARD,nullptr,0,nullptr,&error),utrans_close);
    if(U_FAILURE(error) || !trans)throw std::runtime_error("無法建立 ICU 字串轉換");
    std::int32_t capacity=static_cast<std::int32_t>(value.size()*2+32);
    for(;;){auto output=value;output.resize(static_cast<std::size_t>(capacity));auto length=static_cast<std::int32_t>(value.size()),limit=length;error=U_ZERO_ERROR;
        utrans_transUChars(trans.get(),output.data(),&length,capacity,0,&limit,&error);
        if(error==U_BUFFER_OVERFLOW_ERROR){capacity=std::max(capacity*2,length+1);continue;}
        if(U_FAILURE(error))throw std::runtime_error("ICU 字串轉換失敗");output.resize(static_cast<std::size_t>(length));return output;
    }
}
std::u16string expand_ligatures(std::u16string_view input){
    // Windows NLS 的連字也包含 thorn、sharp s 與帶重音的 AE；單獨 NFKC 不會展開這些字元。
    static constexpr std::pair<char16_t,std::u16string_view> expansions[]{
        {u'\u00c6',u"AE"},{u'\u00de',u"TH"},{u'\u00df',u"ss"},{u'\u00e6',u"ae"},{u'\u00fe',u"th"},
        {u'\u0132',u"IJ"},{u'\u0133',u"ij"},{u'\u0152',u"OE"},{u'\u0153',u"oe"},
        {u'\u01c4',u"D\u017d"},{u'\u01c5',u"D\u017e"},{u'\u01c6',u"d\u017e"},
        {u'\u01c7',u"LJ"},{u'\u01c8',u"Lj"},{u'\u01c9',u"lj"},{u'\u01ca',u"NJ"},{u'\u01cb',u"Nj"},{u'\u01cc',u"nj"},
        {u'\u01e2',u"\u0100\u0112"},{u'\u01e3',u"\u0101\u0113"},{u'\u01f1',u"DZ"},{u'\u01f2',u"Dz"},{u'\u01f3',u"dz"},
        {u'\u01fc',u"\u00c1\u00c9"},{u'\u01fd',u"\u00e1\u00e9"},
        {u'\u05f0',u"\u05d5\u05d5"},{u'\u05f1',u"\u05d5\u05d9"},{u'\u05f2',u"\u05d9\u05d9"},
        {u'\ufb00',u"ff"},{u'\ufb01',u"fi"},{u'\ufb02',u"fl"},{u'\ufb03',u"ffi"},{u'\ufb04',u"ffl"},{u'\ufb05',u"\u017ft"},{u'\ufb06',u"st"}
    };
    std::u16string output;
    for(std::size_t at=0;at<input.size();){
        const auto start=at;UChar32 code=input[at++];
        if(code>=0xd800 && code<=0xdbff && at<input.size() && input[at]>=0xdc00 && input[at]<=0xdfff)code=0x10000+((code-0xd800)<<10)+(input[at++]-0xdc00);
        const auto known=std::find_if(std::begin(expansions),std::end(expansions),[&](const auto& item){return item.first==code;});
        if(known!=std::end(expansions)){output+=known->second;continue;}
        char name[192]{};UErrorCode error=U_ZERO_ERROR;u_charName(code,U_UNICODE_CHAR_NAME,name,sizeof name,&error);
        const auto part=std::u16string(input.substr(start,at-start));
        output+=U_SUCCESS(error) && std::string_view(name).find("LIGATURE")!=std::string_view::npos?transform(part,u"NFKC"):part;
    }
    return output;
}
std::u16string fold_digits(std::u16string_view input){
    std::u16string output;
    for(std::size_t at=0;at<input.size();){
        const auto start=at;UChar32 code=input[at++];
        if(code>=0xd800 && code<=0xdbff && at<input.size() && input[at]>=0xdc00 && input[at]<=0xdfff)code=0x10000+((code-0xd800)<<10)+(input[at++]-0xdc00);
        const auto category=u_charType(code);const auto number=u_getNumericValue(code);
        const bool digit=category==U_DECIMAL_DIGIT_NUMBER || category==U_OTHER_NUMBER || category==U_NON_SPACING_MARK || code==0x3007 || (code>=0x3021 && code<=0x3029);
        if(digit && number>=0 && number<=9 && number==static_cast<int>(number))output.push_back(static_cast<char16_t>(u'0'+static_cast<int>(number)));
        else output+=input.substr(start,at-start);
    }
    return output;
}
std::u16string sort_input(std::u16string text,std::uint32_t flags){
    if(flags&0x20000U)text=transform(std::move(text),u"Fullwidth-Halfwidth");
    if(flags&0x10000U)text=transform(std::move(text),u"Katakana-Hiragana");
    // Windows 比較忽略 Arabic tatweel；不移除其他有語義的字元。
    std::erase(text,u'\u0640');return text;
}
using Collator=std::unique_ptr<UCollator,decltype(&ucol_close)>;
Collator collator(const Locale& locale,std::uint32_t flags){
    UErrorCode error=U_ZERO_ERROR;Collator value(ucol_open(icu_locale(locale).c_str(),&error),ucol_close);
    if(U_FAILURE(error) || !value)return {nullptr,ucol_close};
    const bool ignore_case=flags&0x11U,ignore_accent=flags&0x22U;
    ucol_setStrength(value.get(),ignore_accent?UCOL_PRIMARY:ignore_case?UCOL_SECONDARY:UCOL_TERTIARY);
    ucol_setAttribute(value.get(),UCOL_CASE_LEVEL,ignore_accent&&!ignore_case?UCOL_ON:UCOL_OFF,&error);
    ucol_setAttribute(value.get(),UCOL_NORMALIZATION_MODE,UCOL_ON,&error);
    ucol_setAttribute(value.get(),UCOL_NUMERIC_COLLATION,flags&8U?UCOL_ON:UCOL_OFF,&error);
    ucol_setAttribute(value.get(),UCOL_ALTERNATE_HANDLING,flags&4U?UCOL_SHIFTED:UCOL_NON_IGNORABLE,&error);
    if(flags&4U)ucol_setMaxVariable(value.get(),UCOL_REORDER_CODE_SYMBOL,&error);
    if(U_FAILURE(error))value.reset();return value;
}
std::optional<int> collate_strings(std::u16string left,std::u16string right,const Locale& locale,std::uint32_t flags){
    left=sort_input(std::move(left),flags);right=sort_input(std::move(right),flags);
    const auto order=collator(locale,flags);if(!order)return {};
    const auto result=ucol_strcoll(order.get(),left.data(),static_cast<std::int32_t>(left.size()),right.data(),static_cast<std::int32_t>(right.size()));
    return result==UCOL_LESS?-1:result==UCOL_GREATER?1:0;
}
std::optional<std::vector<std::uint8_t>> collation_key(std::u16string text,const Locale& locale,std::uint32_t flags){
    text=sort_input(std::move(text),flags);const auto order=collator(locale,flags);if(!order)return {};
    const auto size=ucol_getSortKey(order.get(),text.data(),static_cast<std::int32_t>(text.size()),nullptr,0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    ucol_getSortKey(order.get(),text.data(),static_cast<std::int32_t>(text.size()),bytes.data(),size);return bytes;
}
std::u16string map_case(const std::u16string& text,const Locale& locale,std::uint32_t mode){
    const auto name=icu_locale(locale);UErrorCode error=U_ZERO_ERROR;
    const auto apply=[&](char16_t* output,std::int32_t capacity){const auto length=static_cast<std::int32_t>(text.size());
        if(mode==0x100)return u_strToLower(output,capacity,text.data(),length,name.c_str(),&error);
        if(mode==0x200)return u_strToUpper(output,capacity,text.data(),length,name.c_str(),&error);
        return u_strToTitle(output,capacity,text.data(),length,nullptr,name.c_str(),&error);
    };
    const auto required=apply(nullptr,0);if(error!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error))throw std::runtime_error("ICU 大小寫長度查詢失敗");
    std::u16string out(static_cast<std::size_t>(required),0);error=U_ZERO_ERROR;apply(out.data(),required);
    if(U_FAILURE(error))throw std::runtime_error("ICU 大小寫轉換失敗");return out;
}
std::uint16_t character_type(char16_t code,unsigned type){
    const UChar32 c=code;unsigned result=0;const auto category=u_charType(c);
    if(type==1){
        if(u_isupper(c))result|=1;if(u_islower(c))result|=2;if(u_isdigit(c))result|=4;
        if(u_isUWhiteSpace(c))result|=8;if(u_ispunct(c))result|=0x10;if(u_iscntrl(c))result|=0x20;
        if(u_isblank(c))result|=0x40;if(u_isxdigit(c))result|=0x80;if(u_isalpha(c))result|=0x100;if(u_isdefined(c))result|=0x200;
    }else if(type==2){
        switch(u_charDirection(c)){
        case U_LEFT_TO_RIGHT:return 1;case U_RIGHT_TO_LEFT:case U_RIGHT_TO_LEFT_ARABIC:return 2;
        case U_EUROPEAN_NUMBER:return 3;case U_EUROPEAN_NUMBER_SEPARATOR:return 4;case U_EUROPEAN_NUMBER_TERMINATOR:return 5;
        case U_ARABIC_NUMBER:return 6;case U_COMMON_NUMBER_SEPARATOR:return 7;case U_BLOCK_SEPARATOR:return 8;
        case U_SEGMENT_SEPARATOR:return 9;case U_WHITE_SPACE_NEUTRAL:return 10;case U_OTHER_NEUTRAL:return 11;default:return 0;
        }
    }else{
        if(category==U_NON_SPACING_MARK || category==U_ENCLOSING_MARK)result|=1;
        if(u_hasBinaryProperty(c,UCHAR_DIACRITIC))result|=2;
        if(u_getIntPropertyValue(c,UCHAR_INDIC_SYLLABIC_CATEGORY)==U_INSC_VOWEL_DEPENDENT)result|=4;
        if(category>=U_MATH_SYMBOL && category<=U_OTHER_SYMBOL)result|=8;
        UErrorCode error=U_ZERO_ERROR;const auto script=uscript_getScript(c,&error);
        if(script==USCRIPT_KATAKANA)result|=0x10;if(script==USCRIPT_HIRAGANA)result|=0x20;
        const auto width=u_getIntPropertyValue(c,UCHAR_EAST_ASIAN_WIDTH);if(width==U_EA_HALFWIDTH)result|=0x40;if(width==U_EA_FULLWIDTH)result|=0x80;
        if(u_hasBinaryProperty(c,UCHAR_IDEOGRAPHIC))result|=0x100;if(c==0x640)result|=0x200;
        if(c==0x640 || c==0x2d || c==0xaa || c==0xba || c==0x3d)result|=0x400;
        if(c>=0xd800 && c<=0xdbff)result|=0x800;if(c>=0xdc00 && c<=0xdfff)result|=0x1000;
        if(u_isalpha(c))result|=0x8000;
    }return static_cast<std::uint16_t>(result);
}

}
std::optional<CrtLocaleCategory> windows_crt_codepage(std::uint32_t page){
    static std::mutex lock;
    static std::map<std::uint32_t,CrtLocaleCategory> cache;
    std::lock_guard guard(lock);
    if(const auto prior=cache.find(page);prior!=cache.end())return prior->second;
    CrtLocaleCategory value;value.codepage=page;
    if(!page){cache.emplace(page,value);return value;}
    if(page==65000 || page==1200 || page==1201 || page==12000 || page==12001)return {};
    UErrorCode status=U_ZERO_ERROR;
    const auto encoding=page==65001?std::string{"UTF-8"}:page==20127?std::string{"US-ASCII"}:"windows-"+std::to_string(page);
    std::unique_ptr<UConverter,decltype(&ucnv_close)> converter(ucnv_open(encoding.c_str(),&status),ucnv_close);
    if(U_FAILURE(status) || !converter)return {};
    // ICU 的最大值以 UTF-16 code unit 計算；Windows CRT 的 UTF-8 字元可佔四個 bytes。
    value.maximum_character_bytes=page==65001?4U:static_cast<unsigned>(ucnv_getMaxCharSize(converter.get()));
    // CRT UTF-8 的 _ismbblead 不把 UTF-8 起始 byte 當成 DBCS lead。
    if(page!=65001 && value.maximum_character_bytes>1){
        std::array<UBool,256> starters{};ucnv_getStarters(converter.get(),starters.data(),&status);
        if(U_FAILURE(status))return {};
        ucnv_setToUCallBack(converter.get(),UCNV_TO_U_CALLBACK_STOP,nullptr,nullptr,nullptr,&status);
        if(U_FAILURE(status))return {};
        for(unsigned first=0;first<256;++first){
            value.lead[first]=starters[first]!=0;if(!value.lead[first])continue;
            for(unsigned second=1;second<256;++second){
                char input[]{static_cast<char>(first),static_cast<char>(second)};const char* next=input;
                status=U_ZERO_ERROR;ucnv_resetToUnicode(converter.get());
                (void)ucnv_getNextUChar(converter.get(),&next,input+2,&status);
                if(U_SUCCESS(status) && next==input+2)value.trail[second]=true;
            }
        }
    }
    cache.emplace(page,value);return value;
}
std::optional<CrtLocaleCategory> windows_crt_locale(std::u16string_view input){
    if(input==u"C" || input==u"POSIX")return CrtLocaleCategory{};
    std::u16string language(input);std::optional<std::uint32_t> page;
    if(const auto dot=language.rfind(u'.');dot!=language.npos){
        const auto suffix=key(language.substr(dot+1));language.resize(dot);
        if(suffix==u"utf-8" || suffix==u"utf8" || suffix==u"acp" || suffix==u"ocp")page=65001;
        else {
            if(suffix.empty())return {};std::uint32_t number=0;
            for(const auto c:suffix){if(c<u'0' || c>u'9' || number>UINT32_MAX/10U || number*10U>UINT32_MAX-(c-u'0'))return {};number=number*10U+c-u'0';}
            if(!number)return {};page=number;
        }
    }
    auto locale=language.empty()?numbered(0x400):named(language);
    if(!locale){
        // 舊 CRT 的 Language_Country 語法，仍由既有 NLS catalog 解析。
        const auto split=language.find(u'_');const auto lang=key(language.substr(0,split));
        const auto country=split==language.npos?std::u16string{}:key(language.substr(split+1));
        for(const auto& [unused,candidate]:locales()){
            (void)unused;if(candidate.name.empty())continue;
            const auto id=icu_locale(candidate);UErrorCode error=U_ZERO_ERROR;
            std::array<char16_t,128> display_language{},display_country{};
            const auto nl=uloc_getDisplayLanguage(id.c_str(),"en",display_language.data(),static_cast<std::int32_t>(display_language.size()),&error);
            const auto nc=uloc_getDisplayCountry(id.c_str(),"en",display_country.data(),static_cast<std::int32_t>(display_country.size()),&error);
            const auto dash=key(candidate.name).find(u'-');const auto tag=key(candidate.name).substr(0,dash);
            if(U_FAILURE(error) || (lang!=tag && lang!=key(std::u16string(display_language.data(),static_cast<std::size_t>(nl)))))continue;
            const auto region=dash==candidate.name.npos?std::u16string{}:key(candidate.name.substr(dash+1));
            if(!country.empty() && country!=region && country!=key(std::u16string(display_country.data(),static_cast<std::size_t>(nc))))continue;
            const auto resolved=specific(candidate);if(resolved){locale=resolved;break;}
        }
    }
    if(!locale)return {};if(locale->neutral){locale=specific(*locale);if(!locale)return {};}
    if(!page){
        const std::string_view encoding=ansi_encoding(*locale);
        const auto number=encoding.substr(8);std::uint32_t value=0;
        for(const auto c:number)value=value*10U+static_cast<unsigned>(c-'0');page=value;
    }
    auto result=windows_crt_codepage(*page);if(!result)return {};
    result->name=locale->name;result->lcid=locale->id;return result;
}
CrtNumericData windows_crt_numeric(const CrtLocaleCategory& category){
    CrtNumericData result;if(!category.codepage)return result;
    const auto locale=named(category.name);if(!locale)throw std::invalid_argument("CRT 數值 locale 名稱無效");
    const auto decimal=locale_information(*locale,0xe,false),thousands=locale_information(*locale,0xf,false),groups=locale_information(*locale,0x10,false);
    if(!decimal || !thousands || !groups)throw std::runtime_error("CRT 數值 locale 欄位不存在");
    result.decimal=decimal->text;result.thousands=thousands->text;
    // Windows 的「3;2;0」轉為 lconv 的 byte 群組。最後的 0 由字串終止符表示重複。
    std::size_t at=0;while(at<groups->text.size()){
        const auto next=groups->text.find(u';',at);const auto part=std::u16string_view(groups->text).substr(at,next==groups->text.npos?groups->text.size()-at:next-at);
        unsigned value=0;if(part.empty())throw std::runtime_error("CRT 數值分組欄位為空");
        for(const auto digit:part){if(digit<u'0' || digit>u'9' || value>127)throw std::runtime_error("CRT 數值分組欄位無效");value=value*10+digit-u'0';}
        if(value>127)throw std::runtime_error("CRT 數值分組超過 byte 範圍");
        if(!value)break;result.grouping.push_back(static_cast<char>(value));
        if(next==groups->text.npos){result.grouping.push_back(127);break;}at=next+1;
    }return result;
}
CrtTimeData windows_crt_time_data(const CrtLocaleCategory& category){
    CrtTimeData result;
    if(!category.codepage){
        static constexpr std::u16string_view values[]{
            u"Sun",u"Mon",u"Tue",u"Wed",u"Thu",u"Fri",u"Sat",
            u"Sunday",u"Monday",u"Tuesday",u"Wednesday",u"Thursday",u"Friday",u"Saturday",
            u"Jan",u"Feb",u"Mar",u"Apr",u"May",u"Jun",u"Jul",u"Aug",u"Sep",u"Oct",u"Nov",u"Dec",
            u"January",u"February",u"March",u"April",u"May",u"June",u"July",u"August",u"September",u"October",u"November",u"December",
            u"AM",u"PM",u"MM/dd/yy",u"dddd, MMMM dd, yyyy",u"HH:mm:ss",u"en-US"};
        for(unsigned i=0;i<result.fields.size();++i)result.fields[i]=values[i];result.classic=true;return result;
    }
    const auto locale=named(category.name);if(!locale)throw std::invalid_argument("CRT 時間 locale 名稱無效");
    const auto field=[&](unsigned type){const auto value=locale_information(*locale,type,false);if(!value)throw std::invalid_argument("CRT 時間 locale 欄位不存在");return value->text;};
    for(unsigned day=0;day<7;++day){const auto weekday=(day+6)%7;result.fields[day]=field(0x31+weekday);result.fields[7+day]=field(0x2a+weekday);}
    for(unsigned month=0;month<12;++month){result.fields[14+month]=field(0x44+month);result.fields[26+month]=field(0x38+month);}
    result.fields[38]=field(0x28);result.fields[39]=field(0x29);result.fields[40]=field(0x1f);result.fields[41]=field(0x20);result.fields[42]=field(0x1003);result.fields[43]=category.name;
    const auto calendar=locale_information(*locale,0x1009,false);if(!calendar || !calendar->number)throw std::invalid_argument("CRT 時間 calendar 欄位不存在");result.calendar=*calendar->number;return result;
}
std::optional<std::u16string> windows_crt_time_calendar(const CrtTimeData& data,const std::tm& value,unsigned field){
    if(field<40 || field>42)return {};const bool date=field!=42;const auto locale=named(data.fields[43]);if(!locale)return {};
    if(!date && (value.tm_hour<0 || value.tm_hour>23 || value.tm_min<0 || value.tm_min>59 || value.tm_sec<0 || value.tm_sec>60))return {};
    if(date && (value.tm_year< -299 || value.tm_year>8099 || value.tm_mon<0 || value.tm_mon>11 || value.tm_mday<1 || value.tm_mday>31))return {};
    UErrorCode status=U_ZERO_ERROR;
    std::unique_ptr<UCalendar,decltype(&ucal_close)> calendar(ucal_open(u"UTC",3,"en_US@calendar=gregorian",UCAL_GREGORIAN,&status),ucal_close);
    if(U_FAILURE(status) || !calendar)return {};ucal_setAttribute(calendar.get(),UCAL_LENIENT,0);ucal_clear(calendar.get());
    ucal_setDateTime(calendar.get(),date?value.tm_year+1900:2000,date?value.tm_mon:0,date?value.tm_mday:1,date?0:value.tm_hour,date?0:value.tm_min,date?0:value.tm_sec,&status);
    const auto stamp=ucal_getMillis(calendar.get(),&status);if(U_FAILURE(status))return {};
    const auto id=icu_locale(*locale);std::unique_ptr<UDateFormat,decltype(&udat_close)> format(udat_open(UDAT_PATTERN,UDAT_PATTERN,id.c_str(),u"UTC",3,nullptr,0,&status),udat_close);
    if(U_FAILURE(status) || !format)return {};const auto pattern=date?windows_date_pattern(data.fields[field]):time_pattern(data.fields[field],0,true,value.tm_hour<12?data.fields[38]:data.fields[39]);
    udat_applyPattern(format.get(),false,pattern.data(),static_cast<std::int32_t>(pattern.size()));return date_text(format.get(),stamp);
}
CrtDecodedCharacter windows_crt_decode(std::span<const std::uint8_t> bytes,std::uint32_t page){
    if(bytes.empty())return {0,0,CrtDecodedCharacter::Status::Partial};
    if(!page)return {bytes.front(),1,CrtDecodedCharacter::Status::Complete};
    UErrorCode status=U_ZERO_ERROR;const auto encoding=page==65001?std::string{"UTF-8"}:page==20127?std::string{"US-ASCII"}:"windows-"+std::to_string(page);
    std::unique_ptr<UConverter,decltype(&ucnv_close)> converter(ucnv_open(encoding.c_str(),&status),ucnv_close);if(U_FAILURE(status) || !converter)return {};
    ucnv_setToUCallBack(converter.get(),UCNV_TO_U_CALLBACK_STOP,nullptr,nullptr,nullptr,&status);if(U_FAILURE(status))return {};
    const auto first=reinterpret_cast<const char*>(bytes.data());auto next=first;const auto value=ucnv_getNextUChar(converter.get(),&next,first+bytes.size(),&status);
    if(status==U_TRUNCATED_CHAR_FOUND)return {0,bytes.size(),CrtDecodedCharacter::Status::Partial};if(U_FAILURE(status))return {};
    return {static_cast<std::uint32_t>(value),static_cast<std::size_t>(next-first),CrtDecodedCharacter::Status::Complete};
}
std::optional<std::vector<std::uint8_t>> windows_crt_encode(std::u16string_view text,std::uint32_t page){
    if(!page){std::vector<std::uint8_t> bytes;for(const auto c:text){const auto value=crt_c_locale::encode(c);if(!value)return {};bytes.push_back(*value);}return bytes;}
    UErrorCode status=U_ZERO_ERROR;const auto encoding=page==65001?std::string{"UTF-8"}:page==20127?std::string{"US-ASCII"}:"windows-"+std::to_string(page);
    std::unique_ptr<UConverter,decltype(&ucnv_close)> converter(ucnv_open(encoding.c_str(),&status),ucnv_close);if(U_FAILURE(status) || !converter)return {};
    ucnv_setFromUCallBack(converter.get(),UCNV_FROM_U_CALLBACK_STOP,nullptr,nullptr,nullptr,&status);if(U_FAILURE(status))return {};
    const auto size=ucnv_fromUChars(converter.get(),nullptr,0,text.data(),static_cast<std::int32_t>(text.size()),&status);if(U_FAILURE(status) && status!=U_BUFFER_OVERFLOW_ERROR)return {};
    status=U_ZERO_ERROR;std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));ucnv_fromUChars(converter.get(),reinterpret_cast<char*>(bytes.data()),size,text.data(),static_cast<std::int32_t>(text.size()),&status);if(U_FAILURE(status))return {};return bytes;
}
char16_t windows_crt_map_character(char16_t input,const CrtLocaleCategory& locale,bool upper){
    if(!locale.codepage){
        if(upper && input>=u'a' && input<=u'z')return static_cast<char16_t>(input-32);
        if(!upper && input>=u'A' && input<=u'Z')return static_cast<char16_t>(input+32);
        return input;
    }
    auto language=utf16_to_utf8(locale.name);std::replace(language.begin(),language.end(),'-','_');
    UErrorCode status=U_ZERO_ERROR;std::array<char16_t,4> output{};
    const auto count=upper?u_strToUpper(output.data(),4,&input,1,language.c_str(),&status):u_strToLower(output.data(),4,&input,1,language.c_str(),&status);
    return U_SUCCESS(status) && count==1?output[0]:input;
}
const CrtCharacterTable& windows_crt_character_table(const CrtLocaleCategory& locale){
    static std::mutex mutex;static std::map<std::pair<std::u16string,std::uint32_t>,CrtCharacterTable> cache;std::lock_guard lock(mutex);
    const auto key=std::pair{locale.name,locale.codepage};if(const auto found=cache.find(key);found!=cache.end())return found->second;CrtCharacterTable table;
    for(unsigned c=0;c<256;++c){table.lower[c]=table.upper[c]=static_cast<std::uint8_t>(c);
        if(!locale.codepage){table.masks[c+1]=crt_c_locale::character_mask(c);if(c>='A' && c<='Z')table.lower[c]=static_cast<std::uint8_t>(c+32);if(c>='a' && c<='z')table.upper[c]=static_cast<std::uint8_t>(c-32);continue;}
        if(locale.lead[c]){table.masks[c+1]=0x8000;continue;}const std::array<std::uint8_t,1> byte{static_cast<std::uint8_t>(c)};const auto decoded=windows_crt_decode(byte,locale.codepage);if(decoded.status!=CrtDecodedCharacter::Status::Complete)continue;
        const auto value=static_cast<UChar32>(decoded.character);unsigned mask=0;
        if(u_isupper(value))mask|=1;if(u_islower(value))mask|=2;if(u_isdigit(value))mask|=4;if(u_isUWhiteSpace(value))mask|=8;if(u_isgraph(value) && !u_isalnum(value))mask|=16;if(u_iscntrl(value))mask|=32;if(value==' ')mask|=64;if(u_isxdigit(value))mask|=128;if(u_isalpha(value))mask|=0x100;table.masks[c+1]=static_cast<std::uint16_t>(mask);
        if(value>0xffff)continue;for(const bool upper:{false,true}){const auto mapped=windows_crt_map_character(static_cast<char16_t>(value),locale,upper);
            const auto encoded=windows_crt_encode(std::u16string_view(&mapped,1),locale.codepage);if(encoded && encoded->size()==1)(upper?table.upper:table.lower)[c]=encoded->front();}
    }return cache.emplace(key,std::move(table)).first->second;
}
bool windows_locale_name_valid(std::u16string_view name){return named(name).has_value();}
std::optional<std::array<std::uint32_t,3>> windows_locale_digit_substitution(std::uint32_t id){
    const auto locale=numbered(id&~0x80000000U);if(!locale)return {};
    const auto primary=locale->id&0x3ffU;const auto native=locale_information(*locale,0x13,false);
    if(!native || native->text.empty())return {};
    const auto national=native->text.front()==u'0'?9U:native->text.front()==0x660?1U:native->text.front()==0x6f0?0x29U:primary;
    return std::array<std::uint32_t,3>{national,primary,primary==1 || primary==0x29?0U:1U};
}
namespace automation {
std::optional<std::string> parse_number(std::u16string input,std::uint32_t lcid,std::uint32_t& status){
    status=0;const auto locale=numbered(lcid);if(!locale){status=invalid;return {};}
    while(!input.empty() && u_isUWhiteSpace(input.back()))input.pop_back();std::size_t first=0;while(first<input.size() && u_isUWhiteSpace(input[first]))++first;input.erase(0,first);
    const bool parentheses=!input.empty() && input.front()==u'(' && input.back()==u')';
    if(parentheses){input=input.substr(1,input.size()-2);if(input.find_first_of(u"+-()")!=input.npos){status=mismatch;return {};}}
    if(!input.empty() && input.front()==u'+'){input.erase(0,1);if(!input.empty() && (input.front()==u'+' || input.front()==u'-')){status=mismatch;return {};}}
    if(input.empty() || input.size()>32767){status=mismatch;return {};}
    const auto id=icu_locale(*locale);
    for(const auto style:{UNUM_DECIMAL,UNUM_CURRENCY,UNUM_CURRENCY_ACCOUNTING}){
        UErrorCode error=U_ZERO_ERROR;NumberFormat format(unum_open(style,nullptr,0,id.c_str(),nullptr,&error),unum_close);if(U_FAILURE(error) || !format){status=nomemory;return {};}
        std::int32_t position=0;std::vector<char> buffer(input.size()+64);auto length=unum_parseDecimal(format.get(),input.data(),static_cast<std::int32_t>(input.size()),&position,buffer.data(),static_cast<std::int32_t>(buffer.size()),&error);
        if(U_SUCCESS(error) && position==static_cast<std::int32_t>(input.size()) && length>0){std::string decimal(buffer.data(),static_cast<std::size_t>(length));if(decimal.find_first_not_of("0123456789eE.+-")!=decimal.npos){status=mismatch;return {};}if(parentheses)decimal.insert(decimal.begin(),'-');return decimal;}
    }status=mismatch;return {};
}
std::optional<std::u16string> format_number(std::string_view decimal,std::uint32_t lcid){
    const auto locale=numbered(lcid);if(!locale)return {};UErrorCode error=U_ZERO_ERROR;NumberFormat format(unum_open(UNUM_DECIMAL,nullptr,0,icu_locale(*locale).c_str(),nullptr,&error),unum_close);if(U_FAILURE(error) || !format)return {};
    unum_setAttribute(format.get(),UNUM_GROUPING_USED,0);unum_setAttribute(format.get(),UNUM_MAX_FRACTION_DIGITS,340);unum_setAttribute(format.get(),UNUM_MIN_FRACTION_DIGITS,0);
    std::u16string result(512,0);const auto length=unum_formatDecimal(format.get(),decimal.data(),static_cast<std::int32_t>(decimal.size()),result.data(),static_cast<std::int32_t>(result.size()),nullptr,&error);if(U_FAILURE(error) || length<0)return {};result.resize(static_cast<std::size_t>(length));return result;
}
std::uint32_t compare_strings(std::u16string left,std::u16string right,std::uint32_t lcid,std::uint32_t flags){
    if(flags&~0x4004fU)return invalid;const auto locale=numbered(lcid);if(!locale)return invalid;
    const auto mapped=(flags&7U)|((flags&8U)?0x20000U:0)|((flags&0x40U)?0x10000U:0);
    const auto order=collate_strings(std::move(left),std::move(right),*locale,mapped);return order?static_cast<std::uint32_t>(*order+1):nomemory;
}
namespace {
using Date=std::unique_ptr<UDateFormat,decltype(&udat_close)>;
Date date_format(std::uint32_t lcid,std::uint16_t flags,UDateFormatStyle time,UDateFormatStyle date){
    const auto locale=numbered(lcid);if(!locale)return {nullptr,udat_close};auto id=icu_locale(*locale);
    const auto calendars=flags&0x68U;if(calendars && (calendars&(calendars-1)))return {nullptr,udat_close};
    id+=(flags&8U)?"@calendar=islamic":(flags&0x20U)?"@calendar=buddhist":"@calendar=gregorian";
    UErrorCode error=U_ZERO_ERROR;Date result(udat_open(time,date,id.c_str(),u"GMT",3,nullptr,0,&error),udat_close);if(U_FAILURE(error))result.reset();if(result){udat_setLenient(result.get(),false);udat_set2DigitYearStart(result.get(),-1262304000000.0,&error);
        if(!(flags&0x28U)){std::unique_ptr<UCalendar,decltype(&ucal_close)> calendar(ucal_clone(udat_getCalendar(result.get()),&error),ucal_close);if(calendar){ucal_setGregorianChange(calendar.get(),-8640000000000000.0,&error);if(U_SUCCESS(error))udat_setCalendar(result.get(),calendar.get());}}
    }return result;
}
void full_year(UDateFormat* format){
    UErrorCode error=U_ZERO_ERROR;std::u16string pattern(256,0);const auto length=udat_toPattern(format,false,pattern.data(),static_cast<std::int32_t>(pattern.size()),&error);if(U_FAILURE(error) || length<0)return;pattern.resize(static_cast<std::size_t>(length));
    bool quote=false;for(std::size_t i=0;i<pattern.size();++i){if(pattern[i]==u'\'')quote=!quote;else if(!quote && pattern[i]==u'y'){std::size_t end=i+1;while(end<pattern.size() && pattern[end]==u'y')++end;if(end-i<4){pattern.replace(i,end-i,u"yyyy");end=i+4;}i=end-1;}}udat_applyPattern(format,false,pattern.data(),static_cast<std::int32_t>(pattern.size()));
}
}
std::optional<double> parse_date(std::u16string_view input,std::uint32_t lcid,std::uint16_t flags){
    using namespace std::chrono;
    std::u16string text(input);while(!text.empty() && u_isUWhiteSpace(text.back()))text.pop_back();std::size_t first=0;while(first<text.size() && u_isUWhiteSpace(text[first]))++first;text.erase(0,first);if(text.empty() || text.size()>32767)return {};
    for(const auto styles:{std::pair{UDAT_MEDIUM,UDAT_SHORT},std::pair{UDAT_SHORT,UDAT_SHORT},std::pair{UDAT_NONE,UDAT_SHORT},std::pair{UDAT_NONE,UDAT_MEDIUM},std::pair{UDAT_MEDIUM,UDAT_NONE},std::pair{UDAT_SHORT,UDAT_NONE}}){
        const auto format=date_format(lcid,flags,styles.first,styles.second);if(!format)continue;UErrorCode error=U_ZERO_ERROR;std::int32_t position=0;auto milliseconds=udat_parse(format.get(),text.data(),static_cast<std::int32_t>(text.size()),&position,&error);
        if(U_FAILURE(error) || position!=static_cast<std::int32_t>(text.size()))continue;
        const double days=milliseconds/86400000.0+25569.0;const auto whole=std::floor(days);const auto fraction=days-whole;
        const double value=styles.second==UDAT_NONE?fraction:whole+(whole<0?-fraction:fraction);if(time_fields(value))return value;
    }return {};
}
std::optional<std::u16string> format_date(double value,std::uint32_t lcid,std::uint16_t flags){
    using namespace std::chrono;const auto fields=time_fields(value);if(!fields)return {};const auto date=sys_days{year{static_cast<int>((*fields)[0])}/month{(*fields)[1]}/day{(*fields)[3]}};
    const double milliseconds=static_cast<double>(date.time_since_epoch().count())*86400000.0+((*fields)[4]*3600+(*fields)[5]*60+(*fields)[6])*1000.0;
    const auto format=date_format(lcid,flags,value==std::trunc(value)?UDAT_NONE:UDAT_MEDIUM,std::trunc(value)==0 && value!=0?UDAT_NONE:UDAT_SHORT);if(!format)return {};
    full_year(format.get());
    UErrorCode error=U_ZERO_ERROR;std::u16string text(256,0);const auto length=udat_format(format.get(),milliseconds,text.data(),static_cast<std::int32_t>(text.size()),nullptr,&error);if(U_FAILURE(error) || length<0)return {};text.resize(static_cast<std::size_t>(length));return text;
}
}
std::span<const WindowsServiceDescriptor> locale_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"crt","_create_locale",2},{"crt","_wcreate_locale",2},{"crt","_free_locale",1},{"crt","__free_locale",1},{"crt","_get_current_locale",0},{"crt","_configthreadlocale",1},
        {"crt","__x64arm_create_locale_legacy",2},{"crt","__x64arm_wcreate_locale_legacy",2},{"crt","__x64arm_current_locale_legacy",0},
        {"crt","_mbsrchr",2},{"crt","_mbsrchr_l",3},{"crt","_mbschr",2},{"crt","_mbschr_l",3},
        {"crt","isleadbyte",1},{"crt","_isleadbyte_l",2},{"crt","_ismbblead_l",2},{"crt","_ismbbtrail_l",2},
        {"crt","_setmbcp",1},{"crt","_getmbcp",0},
        {"normaliz.dll","IdnToAscii",5},{"normaliz.dll","IdnToUnicode",5},
        {"ntdll.dll","RtlUTF8ToUnicodeN",5},{"ntdll.dll","RtlUnicodeToUTF8N",5},
        {"msvcp140.dll","_Strcoll",5,0,0,0,true},{"msvcp140.dll","_Strxfrm",5,0,0,0,true},
        {"oleaut32.dll","VarI1FromStr",4},{"oleaut32.dll","VarI2FromStr",4},{"oleaut32.dll","VarI4FromStr",4},{"oleaut32.dll","VarI8FromStr",4},
        {"oleaut32.dll","VarUI1FromStr",4},{"oleaut32.dll","VarUI2FromStr",4},{"oleaut32.dll","VarUI4FromStr",4},{"oleaut32.dll","VarUI8FromStr",4},
        {"kernel32.dll","GetNumberFormatA",6},{"kernel32.dll","GetNumberFormatW",6},{"kernel32.dll","GetNumberFormatEx",6},
        {"kernel32.dll","GetCurrencyFormatA",6},{"kernel32.dll","GetCurrencyFormatW",6},{"kernel32.dll","GetCurrencyFormatEx",6},
        {"kernel32.dll","GetDateFormatW",6},{"kernel32.dll","GetDateFormatEx",7},
        {"kernel32.dll","GetTimeFormatW",6},{"kernel32.dll","GetTimeFormatEx",6},
        {"kernel32.dll","ConvertDefaultLocale",1},{"kernel32.dll","CompareStringA",6},{"kernel32.dll","LCMapStringA",6},{"kernel32.dll","GetStringTypeA",5},{"kernel32.dll","GetStringTypeExA",5},{"kernel32.dll","CompareStringW",6},{"kernel32.dll","CompareStringEx",9},{"kernel32.dll","CompareStringOrdinal",5},
        {"kernel32.dll","LCMapStringW",6},{"kernel32.dll","LCMapStringEx",9},
        {"kernel32.dll","FoldStringA",5},{"kernel32.dll","FoldStringW",5},
        {"kernel32.dll","GetStringTypeW",4},{"kernel32.dll","GetStringTypeExW",5},
        {"kernel32.dll","LocaleNameToLCID",2},{"kernel32.dll","LCIDToLocaleName",4},{"kernel32.dll","ResolveLocaleName",3},
        {"kernel32.dll","IsValidLocaleName",1},{"kernel32.dll","IsValidLocale",2},
        {"kernel32.dll","EnumSystemLocalesA",2},{"kernel32.dll","EnumSystemLocalesW",2},{"kernel32.dll","EnumSystemLocalesEx",4}
    };return table;
}
namespace {
// _locale_t 的 locinfo 與 mbcinfo 各自保存碼頁。直接使用既有客體 mbcinfo，
// 讓 explicit locale、目前 locale 快照及執行緒 clone 共用同一份 ABI 狀態。
std::uint64_t crt_mbc_address(GuestMemory& memory,const CrtLocaleObject& object,bool x86){
    return memory.load(GuestAddress{object.storage+(x86?4U:8U)},x86?32:64);
}
void crt_mbc_assign(GuestMemory& memory,std::uint64_t address,const CrtLocaleCategory& category){
    const auto& characters=windows_crt_character_table(category);
    memory.fill(GuestAddress{address},0,544);
    memory.store(GuestAddress{address},32,1);
    memory.store(GuestAddress{address+4},32,category.codepage);
    memory.store(GuestAddress{address+8},32,category.maximum_character_bytes>1 && category.codepage!=65001);
    memory.store(GuestAddress{address+12},32,category.lcid);
    for(unsigned c=0;c<256;++c){
        memory.store(GuestAddress{address+29+c},8,(category.lead[c]?4U:0U)|(category.trail[c]?8U:0U));
        memory.store(GuestAddress{address+285+c},8,characters.lower[c]);
    }
}
void crt_mbc_copy(GuestMemory& memory,const CrtLocaleObject& source,const CrtLocaleObject& destination,bool x86){
    memory.move(GuestAddress{crt_mbc_address(memory,destination,x86)},GuestAddress{crt_mbc_address(memory,source,x86)},544);
}
}
void WindowsServices::sync_crt_thread_state(GuestMemory& memory,std::uint64_t id){
    if(x86_)return;
    const auto& thread=threads_.items.at(id);
    if(!thread.teb)return;
    const auto local=runtime_.crt_thread_locales.find(id);
    const auto locale=local==runtime_.crt_thread_locales.end()?runtime_.crt_current_locale:local->second;
    const auto tables=thread.teb+GUEST_CRT_TABLE_OFFSET;
    std::array<std::uint64_t,6> cells{thread.errno_cell,locale,tables,tables+512,tables+768,0};
    if(locale){
        const auto& object=runtime_.crt_locales.at(locale);
        // create_crt_locale 的內部表格配置；不依 UCRT／舊 MSVCRT 的公開 locinfo 版型。
        const auto ctype=object.storage+16+352+544;
        std::array<std::uint8_t,1024> bytes{};
        memory.read(GuestAddress{ctype+2},bytes);
        memory.write(GuestAddress{tables},bytes);
        cells[5]=object.categories[2].codepage|(std::uint64_t{object.categories[4].codepage}<<32);
    }
    static_assert(sizeof(GuestCrtThreadState)==48 && offsetof(GuestCrtThreadState,ctype_codepage)==40);
    for(std::size_t n=0;n<cells.size();++n)
        memory.store(GuestAddress{thread.teb+GUEST_CRT_STATE_OFFSET+n*8},64,cells[n]);
}
void WindowsServices::sync_crt_thread_states(GuestMemory& memory){
    // 客體執行與服務由同一排程器串行；發布新表後才釋放舊 locale。
    for(const auto& [id,thread]:threads_.items){(void)thread;sync_crt_thread_state(memory,id);}
}
std::uint64_t WindowsServices::create_crt_locale(GuestState& state,GuestMemory& memory,CrtLocaleObject object){
                const unsigned p=x86_?4:8;const auto bits=static_cast<std::uint8_t>(p*8);
                const auto storage=allocate_heap(memory,8192);if(!storage)throw std::bad_alloc{};
                object.storage=storage;
                try{
                    memory.fill(GuestAddress{storage},0,8192);
                    const auto put=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
                    const auto info=storage+2*p,mbc=info+352,ctype=mbc+544,lower=ctype+514,upper=lower+256,conv=(upper+256+p-1)&~std::uint64_t(p-1);
                    auto cursor=conv+18*p+8;
                    const auto save=[&](std::span<const std::uint8_t> bytes){
                        if(bytes.size()>storage+8192-cursor)throw std::bad_alloc();
                        const auto address=cursor;memory.write(GuestAddress{address},bytes);cursor+=bytes.size();return address;
                    };
                    const auto wide_string=[&](std::u16string_view text){return save(utf16_bytes(text));};
                    const auto narrow_string=[&](std::u16string_view text,unsigned page){
                        auto bytes=page?ansi_output(text,page==65001?"UTF-8":("windows-"+std::to_string(page)).c_str()):std::vector<std::uint8_t>{};
                        if(!page){for(const auto c:text)bytes.push_back(static_cast<std::uint8_t>(c));}
                        bytes.push_back(0);return save(bytes);
                    };
                    const auto& ctype_category=object.categories[2];
                    const auto& character_table=windows_crt_character_table(ctype_category);
                    for(unsigned c=0;c<256;++c){
                        memory.store(GuestAddress{ctype+(c+1)*2},16,character_table.masks[c+1]);
                        memory.store(GuestAddress{lower+c},8,character_table.lower[c]);memory.store(GuestAddress{upper+c},8,character_table.upper[c]);
                    }
                    crt_mbc_assign(memory,mbc,ctype_category);
                    const auto field=[&](unsigned category,unsigned type,std::u16string_view fallback){
                        const auto& item=object.categories[category];if(!item.codepage)return std::u16string(fallback);
                        const auto locale=named(item.name);if(!locale)throw std::runtime_error("CRT locale 名稱失效");
                        const auto value=locale_information(*locale,type,false);if(!value)throw std::runtime_error("CRT locale 欄位不存在");return value->text;
                    };
                    std::array<std::u16string,10> fields{
                        field(4,0xe,u"."),field(4,0xf,u""),field(4,0x10,u""),field(3,0x15,u""),field(3,0x14,u""),
                        field(3,0x16,u""),field(3,0x17,u""),field(3,0x18,u""),field(3,0x50,u""),field(3,0x51,u"")};
                    const auto numeric=windows_crt_numeric(object.categories[4]);fields[0]=numeric.decimal;fields[1]=numeric.thousands;
                    fields[2].clear();for(const auto byte:numeric.grouping)fields[2].push_back(static_cast<unsigned char>(byte));
                    for(const auto i:{7U}){
                        std::u16string groups;for(const auto c:fields[i])if(c>=u'1' && c<=u'9')groups.push_back(static_cast<char16_t>(c-u'0'));fields[i]=std::move(groups);
                    }
                    for(unsigned i=0;i<10;++i)put(conv+i*p,narrow_string(fields[i],object.categories[i<3?4:3].codepage));
                    static constexpr unsigned monetary_types[]{0x1a,0x19,0x54,0x55,0x56,0x57,0x52,0x53};
                    for(unsigned i=0;i<8;++i){
                        unsigned value=127;
                        if(object.categories[3].codepage){const auto locale=named(object.categories[3].name);const auto data=locale_information(*locale,monetary_types[i],false);value=data->number.value_or(127);}
                        memory.store(GuestAddress{conv+10*p+i},8,value);
                    }
                    static constexpr unsigned wide_fields[]{0,1,3,4,5,6,8,9};
                    for(unsigned i=0;i<8;++i)put(conv+10*p+8+i*p,wide_string(fields[wide_fields[i]]));
                    auto time=crt_time::encode(windows_crt_time_data(object.categories[5]),object.categories[5].codepage,p);
                    cursor=(cursor+p-1)&~std::uint64_t(p-1);if(cursor>storage+8192 || time.bytes.size()>storage+8192-cursor)throw std::bad_alloc{};
                    const auto time_data=cursor;time.relocate(time_data);(void)save(time.bytes);
                    if(object.legacy){
                        memory.store(GuestAddress{info},32,1);memory.store(GuestAddress{info+4},32,ctype_category.codepage);
                        memory.store(GuestAddress{info+8},32,object.categories[1].codepage);
                        for(unsigned i=0;i<6;++i){
                            const auto& item=object.categories[i];memory.store(GuestAddress{info+12+i*4},32,item.lcid);
                            for(unsigned j=0;j<3;++j)memory.store(GuestAddress{info+36+i*6+j*2},16,j==2?item.codepage:item.lcid);
                            const auto at=info+72+i*4*p;put(at,narrow_string(item.name,item.codepage));put(at+p,wide_string(item.name));
                        }
                        const auto clike=info+72+24*p;memory.store(GuestAddress{clike},32,ctype_category.codepage==0);
                        memory.store(GuestAddress{clike+4},32,ctype_category.maximum_character_bytes);
                        const auto references=clike+8;put(references+3*p,conv);put(references+5*p,ctype);put(references+6*p,ctype+2);put(references+7*p,lower);put(references+8*p,upper);put(references+9*p,time_data);
                    }else{
                        put(info,ctype+2);memory.store(GuestAddress{info+p},32,ctype_category.maximum_character_bytes);memory.store(GuestAddress{info+p+4},32,ctype_category.codepage);
                        const auto public_size=p+8;memory.store(GuestAddress{info+public_size},32,1);
                        memory.store(GuestAddress{info+public_size+4},32,object.categories[1].codepage);memory.store(GuestAddress{info+public_size+8},32,object.categories[5].codepage);
                        const auto categories=(info+public_size+12+p-1)&~std::uint64_t(p-1);
                        for(unsigned i=0;i<6;++i){const auto at=categories+i*4*p;const auto& item=object.categories[i];put(at,narrow_string(item.name,item.codepage));put(at+p,wide_string(item.name));}
                        const auto references=categories+24*p;put(references+3*p,conv);put(references+5*p,ctype);put(references+6*p,lower);put(references+7*p,upper);put(references+8*p,time_data);
                        for(unsigned i=0;i<6;++i){const auto text=memory.load(GuestAddress{categories+i*4*p+p},bits);put(references+(9+i)*p,object.categories[i].codepage?text:0);}
                    }
                    put(storage,info);put(storage+p,mbc);runtime_.crt_locales.emplace(storage,std::move(object));return storage;
                }catch(...){(void)free_heap(memory,storage,state);throw;}
}
CrtLocaleObject& WindowsServices::current_crt_locale(GuestState& state,GuestMemory& memory){
    const auto found=runtime_.crt_thread_locales.find(threads_.current);auto& address=found==runtime_.crt_thread_locales.end()?runtime_.crt_current_locale:found->second;
    if(!address){
        CrtLocaleObject object;object.legacy=true;address=create_crt_locale(state,memory,std::move(object));
        if(const auto page=windows_crt_codepage(runtime_.crt_multibyte_codepage))crt_mbc_assign(memory,crt_mbc_address(memory,runtime_.crt_locales.at(address),x86_),*page);
        sync_crt_thread_states(memory);
    }return runtime_.crt_locales.at(address);
}

std::optional<std::uint32_t> WindowsServices::invoke_locale(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    if(service.family=="crt"){
        const auto returned=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto error=[&](unsigned code,std::uint64_t value=0){memory.store(GuestAddress{runtime_.errno_cell},32,code);return returned(value);};
        const auto invalid=[&](unsigned code,std::uint64_t value=0)->std::optional<std::uint32_t>{
            memory.store(GuestAddress{runtime_.errno_cell},32,code);
            if(current_invalid_parameter_handler()){
                start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,value);
                return std::nullopt;
            }
            return 0xc0000417U;
        };
        if(name=="mbstowcs" || name=="wcstombs" || name=="mbsrtowcs" || name=="wcsrtombs"
           || name=="mbstowcs_s" || name=="wcstombs_s" || name=="mbsrtowcs_s" || name=="wcsrtombs_s"
           || name=="_mbstowcs_l" || name=="_wcstombs_l" || name=="_mbstowcs_s_l" || name=="_wcstombs_s_l"){
            const bool secure=name.find("_s")!=name.npos,restart=name.find("rtow")!=name.npos || name.find("rtomb")!=name.npos;
            const bool narrow=name.find("wcstomb")!=name.npos || name.find("wcsrtomb")!=name.npos;
            const unsigned p=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(p*8);
            const auto size_max=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;
            constexpr std::size_t maximum=1048576;
            const auto destination=arg(secure?1:0),capacity=arg(2);
            const auto source_argument=arg(secure?3:1),count=arg(secure?4:2),return_pointer=secure?arg(0):0;
            const auto state_pointer=restart?arg(secure?5:3):0;
            const auto destination_width=static_cast<std::uint8_t>(narrow?8:16);
            if(return_pointer)memory.store(GuestAddress{return_pointer},bits,0);
            if(secure && destination && capacity)memory.store(GuestAddress{destination},destination_width,0);
            const auto bad_parameter=[&](unsigned code){return invalid(code,secure?code:size_max);};
            if(secure && (destination?!capacity:capacity!=0))return bad_parameter(22);
            // 非安全版本的零長度呼叫不讀取來源，也不變更可續行狀態。
            if(!secure && destination && !count)return returned(0);
            if(!source_argument)return bad_parameter(22);
            const auto source=restart?memory.load(GuestAddress{source_argument},bits):source_argument;
            if(!source)return bad_parameter(22);
            const auto& current=current_crt_locale(state,memory).categories[2];auto category=current;
            if(name.ends_with("_l") && arg(secure?5:3)){
                const auto locale=runtime_.crt_locales.find(arg(secure?5:3));
                if(locale==runtime_.crt_locales.end())return bad_parameter(22);category=locale->second.categories[2];
            }
            crt_multibyte::State conversion{};
            if(restart){
                conversion=narrow?runtime_.crt_implicit_mb_encode:runtime_.crt_implicit_mb_decode;
                if(state_pointer)memory.read(GuestAddress{state_pointer},conversion);
                if(destination){memory.check(GuestAddress{source_argument},p,Access::Write);if(state_pointer)memory.check(GuestAddress{state_pointer},8,Access::Write);}
            }
            const bool truncate=secure && count==size_max;
            const auto requested=!destination?static_cast<std::uint64_t>(maximum):secure?(truncate?capacity-1:std::min(count,capacity)):count;
            const auto limit=static_cast<std::size_t>(std::min<std::uint64_t>(requested,maximum));
            const unsigned source_unit=narrow?2U:1U,destination_unit=narrow?1U:2U;
            const auto source_address=[&](std::size_t index){
                if(index>=maximum || index>(UINT64_MAX-source)/source_unit)throw HostCallFault("CRT 字串轉碼來源超過範圍");
                return GuestAddress{source+index*source_unit};
            };
            const auto output_address=[&](std::size_t index){
                if(index>maximum || index>(UINT64_MAX-destination)/destination_unit)throw HostCallFault("CRT 字串轉碼目的超過範圍");
                return GuestAddress{destination+index*destination_unit};
            };
            crt_multibyte::StringResult converted;
            if(narrow)converted=crt_multibyte::encode_string(conversion,category.codepage,limit,
                [&](std::size_t i){return static_cast<char16_t>(memory.load(source_address(i),16));},
                [&](std::size_t i,std::uint8_t c){if(destination)memory.store(output_address(i),8,c);});
            else converted=crt_multibyte::decode_string(conversion,category.codepage,category.maximum_character_bytes,limit,
                [&](std::size_t i){return static_cast<std::uint8_t>(memory.load(source_address(i),8));},
                [&](std::size_t i,char16_t c){if(destination)memory.store(output_address(i),16,c);});
            if(converted.status==crt_multibyte::StringStatus::Limit && (!destination || requested>maximum))throw HostCallFault("CRT 字串轉碼找不到範圍內的終止字元");
            // _TRUNCATE 預留 NUL；剛好填滿內容但來源已結束仍屬成功。
            if(truncate && converted.status==crt_multibyte::StringStatus::Limit
               && crt_multibyte::initial(converted.state) && !memory.load(source_address(converted.source_units),static_cast<std::uint8_t>(source_unit*8)))
                converted.status=crt_multibyte::StringStatus::Terminated;
            if(restart && destination){
                memory.store(GuestAddress{source_argument},bits,converted.status==crt_multibyte::StringStatus::Terminated?0:source+converted.source_units*source_unit);
                if(state_pointer)memory.write(GuestAddress{state_pointer},converted.state);
                else (narrow?runtime_.crt_implicit_mb_encode:runtime_.crt_implicit_mb_decode)=converted.state;
            }
            if(converted.status==crt_multibyte::StringStatus::Invalid){
                if(destination && secure)memory.store(GuestAddress{destination},destination_width,0);
                if(return_pointer && restart)memory.store(GuestAddress{return_pointer},bits,size_max);
                return error(42,secure?42:size_max);
            }
            if(!secure)return returned(converted.output_units);
            if(destination && converted.output_units>=capacity){memory.store(GuestAddress{destination},destination_width,0);return bad_parameter(34);}
            if(destination)memory.store(output_address(converted.output_units),destination_width,0);
            if(return_pointer)memory.store(GuestAddress{return_pointer},bits,converted.output_units+1);
            return returned(truncate && converted.status==crt_multibyte::StringStatus::Limit?80:0);
        }
        if(name=="btowc" || name=="wctob"){
            const auto& category=current_crt_locale(state,memory).categories[2];const auto value=static_cast<std::uint32_t>(arg(0));
            if(name=="btowc"){if(value>255)return returned(65535);const std::array<std::uint8_t,1> byte{static_cast<std::uint8_t>(value)};
                const auto decoded=windows_crt_decode(byte,category.codepage);return returned(decoded.status==CrtDecodedCharacter::Status::Complete && decoded.character<=65535?decoded.character:65535U);}
            if(value>65535 || value==65535)return returned(UINT32_MAX);const char16_t character=static_cast<char16_t>(value);
            const auto bytes=windows_crt_encode(std::u16string_view(&character,1),category.codepage);return returned(bytes && bytes->size()==1?bytes->front():UINT32_MAX);
        }
        if(name=="mbsinit"){
            if(!arg(0))return returned(1);crt_multibyte::State bytes{};memory.read(GuestAddress{arg(0)},bytes);return returned(crt_multibyte::initial(bytes));
        }
        if(name=="mbrtowc" || name=="mbrlen" || name=="mbtowc" || name=="mblen"){
            const bool restart=name=="mbrtowc" || name=="mbrlen",length=name=="mbrlen" || name=="mblen";
            const auto source=arg(length?0:1),capacity=arg(length?1:2),state_pointer=restart?arg(length?2:3):0;
            const auto size_max=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;crt_multibyte::State conversion=restart?runtime_.crt_implicit_mb_decode:crt_multibyte::State{};
            if(state_pointer){memory.read(GuestAddress{state_pointer},conversion);memory.check(GuestAddress{state_pointer},8,Access::Write);}
            const auto save=[&]{if(restart){if(state_pointer)memory.write(GuestAddress{state_pointer},conversion);else runtime_.crt_implicit_mb_decode=conversion;}};
            if(!source && !restart)return returned(0);
            if(source && !capacity)return returned(restart?size_max-1:UINT32_MAX);
            const auto& category=current_crt_locale(state,memory).categories[2];const auto count=source?std::min<std::uint64_t>(capacity,category.maximum_character_bytes):1;
            std::array<std::uint8_t,8> input{};if(count>input.size())return error(42,restart?size_max:UINT32_MAX);
            crt_multibyte::Result result;auto trial=conversion;
            for(std::uint64_t i=0;i<count;++i){if(source)input[static_cast<std::size_t>(i)]=static_cast<std::uint8_t>(memory.load(GuestAddress{source+i},8));
                trial=conversion;result=crt_multibyte::decode(trial,std::span(input).first(static_cast<std::size_t>(i+1)),category.codepage);
                if(result.status!=CrtDecodedCharacter::Status::Partial)break;
            }conversion=trial;
            if(result.status==CrtDecodedCharacter::Status::Partial){save();if(restart)return returned(size_max-1);return error(42,UINT32_MAX);}
            if(result.status!=CrtDecodedCharacter::Status::Complete || result.character>65535){conversion={};save();return error(42,restart?size_max:UINT32_MAX);}
            if(source && !length && arg(0))memory.store(GuestAddress{arg(0)},16,result.character);save();return returned(result.character?result.consumed:0);
        }
        if(name=="wcrtomb" || name=="wctomb"){
            const bool restart=name=="wcrtomb";const auto size_max=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;
            if(!restart && !arg(0))return returned(0);const auto state_pointer=restart?arg(2):0;
            if(state_pointer)memory.check(GuestAddress{state_pointer},8,Access::Write);
            const char16_t character=arg(0)?static_cast<char16_t>(arg(1)):0;const auto& category=current_crt_locale(state,memory).categories[2];
            const auto bytes=windows_crt_encode(std::u16string_view(&character,1),category.codepage);
            if(!bytes)return error(42,restart?size_max:UINT32_MAX);
            if(arg(0))memory.write(GuestAddress{arg(0)},*bytes);
            if(restart){if(state_pointer)memory.fill(GuestAddress{state_pointer},0,8);else runtime_.crt_implicit_mb_encode={};}return returned(bytes->size());
        }
        if(name=="_configthreadlocale"){
            const auto setting=static_cast<std::uint32_t>(arg(0));const auto found=runtime_.crt_thread_locales.find(threads_.current);const auto previous=found==runtime_.crt_thread_locales.end()?2U:1U;
            if(!setting)return returned(previous);if(setting!=1 && setting!=2)return invalid(22,UINT32_MAX);
            if(setting==1 && found==runtime_.crt_thread_locales.end()){
                const auto original=current_crt_locale(state,memory);auto object=original;object.storage=0;const auto copy=create_crt_locale(state,memory,std::move(object));
                crt_mbc_copy(memory,original,runtime_.crt_locales.at(copy),x86_);
                try{runtime_.crt_thread_locales.emplace(threads_.current,copy);}catch(...){runtime_.crt_locales.erase(copy);(void)free_heap(memory,copy,state);throw;}
                sync_crt_thread_state(memory,threads_.current);
            }else if(setting==2 && found!=runtime_.crt_thread_locales.end()){
                const auto address=found->second;runtime_.crt_thread_locales.erase(found);
                sync_crt_thread_state(memory,threads_.current);
                runtime_.crt_locales.erase(address);(void)free_heap(memory,address,state);
            }return returned(previous);
        }
        if(name=="setlocale" || name=="_wsetlocale" || name=="localeconv" || name=="__lconv_init" || name=="___lc_codepage_func" || name=="___lc_collate_cp_func" || name=="___mb_cur_max_func" || name=="__mb_cur_max_func" || name=="___lc_locale_name_func" || name=="__pctype_func"){
            auto& current=current_crt_locale(state,memory);const unsigned p=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(p*8);
            const auto pointer=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
            if(name=="___lc_codepage_func" || name=="___lc_collate_cp_func")return returned(current.categories[name=="___lc_codepage_func"?2:1].codepage);
            if(name=="___mb_cur_max_func" || name=="__mb_cur_max_func")return returned(current.categories[2].maximum_character_bytes);
            const auto info=pointer(current.storage),references=info+72+24*p+8;
            if(name=="localeconv" || name=="__lconv_init")return returned(name=="__lconv_init"?0:pointer(references+3*p));
            if(name=="__pctype_func")return returned(pointer(references+6*p));
            const auto key=runtime_.crt_thread_locales.contains(threads_.current)?threads_.current:0;auto& query=runtime_.crt_locale_query_buffers[key];
            if(!query){query=allocate_heap(memory,8192);if(!query)return error(12);}
            if(name=="___lc_locale_name_func"){
                const auto names=query+8192-256;for(unsigned i=0;i<6;++i)memory.store(GuestAddress{names+i*p},bits,i && current.categories[i].codepage?pointer(info+72+4*i*p+p):0);return returned(names);
            }
            const auto category=static_cast<unsigned>(arg(0));if(category>5)return invalid(22);
            const bool wide=name=="_wsetlocale";
            if(arg(1)){
                std::u16string text;
                try{text=wide?read_utf16(memory,GuestAddress{arg(1)},4095):utf8_to_utf16(read_ansi(memory,arg(1),4095));}
                catch(const std::invalid_argument&){return returned(0);}
                auto replacement=current;replacement.storage=0;
                // 無效 locale 字串只回傳 NULL；只有 category 的參數錯誤改 errno。
                if(!category && text.starts_with(u"LC_")){
                    static constexpr std::u16string_view names[]{u"",u"LC_COLLATE",u"LC_CTYPE",u"LC_MONETARY",u"LC_NUMERIC",u"LC_TIME"};
                    for(std::size_t start=0;start<text.size();){const auto end=text.find(u';',start);const auto part=text.substr(start,end==text.npos?text.size()-start:end-start);const auto equals=part.find(u'=');
                        if(equals==part.npos)return returned(0);unsigned cat=0;for(unsigned i=1;i<6;++i)if(part.substr(0,equals)==names[i])cat=i;if(!cat)return returned(0);
                        const auto locale=windows_crt_locale(part.substr(equals+1));if(!locale)return returned(0);replacement.categories[cat]=*locale;
                        if(end==text.npos)break;start=end+1;if(start==text.size())return returned(0);
                    }
                }else {const auto locale=windows_crt_locale(text);if(!locale)return returned(0);for(unsigned i=1;i<6;++i)if(!category || category==i)replacement.categories[i]=*locale;}
                const auto saved=create_crt_locale(state,memory,std::move(replacement));auto& active=runtime_.crt_thread_locales.contains(threads_.current)?runtime_.crt_thread_locales.at(threads_.current):runtime_.crt_current_locale;
                // setlocale 只改 locinfo；_setmbcp 才改獨立的 multibyte codepage。
                crt_mbc_copy(memory,current,runtime_.crt_locales.at(saved),x86_);
                const auto old=active;active=saved;
                sync_crt_thread_states(memory);
                runtime_.crt_locales.erase(old);(void)free_heap(memory,old,state);
            }
            const auto& categories=current_crt_locale(state,memory).categories;
            const auto label=[](const CrtLocaleCategory& category){if(!category.codepage)return std::u16string{u"C"};return category.name+u"."+utf8_to_utf16(std::to_string(category.codepage));};
            std::u16string result;bool equal=true;for(unsigned i=2;i<6;++i)equal&=categories[i]==categories[1];
            if(category || equal)result=label(categories[category?category:1]);
            else {static constexpr std::u16string_view names[]{u"",u"LC_COLLATE",u"LC_CTYPE",u"LC_MONETARY",u"LC_NUMERIC",u"LC_TIME"};for(unsigned i=1;i<6;++i){if(i>1)result+=u';';result+=names[i];result+=u'=';result+=label(categories[i]);}}
            const auto narrow=utf16_to_utf8(result);const auto bytes=utf16_bytes(result);if(narrow.size()+1>4096 || bytes.size()>4096-256)return error(22);
            memory.write(GuestAddress{query},{reinterpret_cast<const std::uint8_t*>(narrow.data()),narrow.size()});memory.store(GuestAddress{query+narrow.size()},8,0);memory.write(GuestAddress{query+4096},bytes);return returned(query+(wide?4096:0));
        }
        if(name=="_free_locale" || name=="__free_locale"){
            if(!arg(0))return returned(0);
            const auto found=runtime_.crt_locales.find(arg(0));
            if(found==runtime_.crt_locales.end())throw HostCallFault("CRT locale 物件不存在或已釋放");
            const auto storage=found->second.storage;
            if(!free_heap(memory,storage,state))throw HostCallFault("CRT locale 儲存配置無效");
            runtime_.crt_locales.erase(found);return returned(0);
        }
        const bool current=name=="_get_current_locale" || name=="__x64arm_current_locale_legacy";
        const bool creating=current || name=="_create_locale" || name=="_wcreate_locale" || name=="__x64arm_create_locale_legacy" || name=="__x64arm_wcreate_locale_legacy";
        if(creating){
            const auto category=current?0U:static_cast<unsigned>(arg(0));
            if(category>5 || (!current && !arg(1)))return invalid(22);
            try{
                CrtLocaleObject object;std::optional<CrtLocaleObject> original;
                if(current){original=current_crt_locale(state,memory);object.categories=original->categories;}object.legacy=name.starts_with("__x64arm_");
                if(!current){
                    const bool wide=name.find("wcreate")!=name.npos;
                    const auto text=wide?read_utf16(memory,GuestAddress{arg(1)},255):utf8_to_utf16(read_ansi(memory,arg(1),255));
                    const auto locale=windows_crt_locale(text);if(!locale)return error(22);
                    for(unsigned i=1;i<6;++i)if(!category || category==i)object.categories[i]=*locale;
                }
                const auto created=create_crt_locale(state,memory,std::move(object));
                if(original)crt_mbc_copy(memory,*original,runtime_.crt_locales.at(created),x86_);
                return returned(created);
            }catch(const std::bad_alloc&){return error(12);}catch(const std::invalid_argument&){return error(22);}catch(const std::runtime_error&){return error(22);}
        }
        if(name=="_setmbcp"){
            const auto requested=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(0)));
            const auto page=requested==-4?current_crt_locale(state,memory).categories[2].codepage:requested==-3 || requested==-2?65001U:static_cast<std::uint32_t>(requested);
            const auto settings=windows_crt_codepage(page);if(!settings)return error(22,UINT32_MAX);
            crt_mbc_assign(memory,crt_mbc_address(memory,current_crt_locale(state,memory),x86_),*settings);return returned(0);
        }
        if(name=="_getmbcp"){
            const auto mbc=crt_mbc_address(memory,current_crt_locale(state,memory),x86_);
            return returned(memory.load(GuestAddress{mbc+8},32)?memory.load(GuestAddress{mbc+4},32):0);
        }
        const bool search=name.starts_with("_mbsrchr") || name.starts_with("_mbschr");
        if(search || name=="isleadbyte" || name=="_isleadbyte_l" || name.starts_with("_ismbb")){
            const auto* selected=&current_crt_locale(state,memory);
            if(name.ends_with("_l") && arg(search?2:1)){
                const auto object=runtime_.crt_locales.find(arg(search?2:1));
                if(object==runtime_.crt_locales.end())return invalid(22);selected=&object->second;
            }
            auto locale=selected->categories[2];
            if(search || name.starts_with("_ismbb")){
                const auto address=crt_mbc_address(memory,*selected,x86_);
                const auto current_codepage=windows_crt_codepage(static_cast<std::uint32_t>(memory.load(GuestAddress{address+4},32)));
                if(!current_codepage)return error(22);locale=*current_codepage;
                if(!memory.load(GuestAddress{address+8},32))locale.maximum_character_bytes=1;
            }
            if(!search){
                const auto value=static_cast<std::uint32_t>(arg(0));
                return returned(value<256 && (name.find("trail")!=name.npos?locale.trail[value]:locale.lead[value]));
            }
            if(!arg(0))return invalid(22);
            const bool reverse=name.starts_with("_mbsrchr");
            const auto target=static_cast<std::uint32_t>(arg(1));std::uint64_t match=0;
            const auto sought=locale.maximum_character_bytes==1?target&255U:target;
            for(std::uint64_t offset=0;offset<output_limit;){
                const auto at=arg(0)+offset;if(at<arg(0))throw HostCallFault("CRT 多位元組搜尋位址溢位");
                auto character=static_cast<unsigned>(memory.load(GuestAddress{at},8));unsigned count=1;
                if(character && locale.lead[character]){
                    if(offset+1>=output_limit)return error(42);
                    const auto next=static_cast<unsigned>(memory.load(GuestAddress{at+1},8));
                    if(!next)return returned(reverse?(match?match:at+1):0);
                    character=(character<<8)|next;count=2;
                }
                if(character==sought){match=at;if(!reverse)return returned(match);}
                if(!character)return returned(match);offset+=count;
            }
            throw HostCallFault("CRT 多位元組搜尋找不到終止字元");
        }
        throw HostCallFault("未知 CRT locale 作業");
    }
    if(service.family=="msvcp140.dll"){
        const bool mapping=name=="_Strxfrm";
        const auto returned=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto invalid=[&](unsigned error){if(!mapping)memory.store(GuestAddress{runtime_.errno_cell},32,error);return returned(mapping?(x86_?UINT32_MAX:UINT64_MAX):INT32_MAX);};
        try{
            if(arg(1)<arg(0) || arg(3)<arg(2))return invalid(22);
            const auto first_size=arg(1)-arg(0),second_size=arg(3)-arg(2);
            if(first_size>1048576 || second_size>1048576)return invalid(22);
            // 未指定 _Collvec 時，沿用目前 CRT 的 C locale（code page 0、空名稱）。
            const auto page=arg(4)?static_cast<unsigned>(memory.load(GuestAddress{arg(4)},32)):0U;
            const auto locale_pointer=arg(4)?memory.load(GuestAddress{arg(4)+(x86_?4U:8U)},x86_?32:64):0;
            if(!locale_pointer && (!mapping || !page)){
                std::vector<std::uint8_t> right(static_cast<std::size_t>(second_size));
                if(second_size)memory.read(GuestAddress{arg(2)},right);
                if(mapping){if(second_size<=first_size && second_size)memory.write(GuestAddress{arg(0)},right);return returned(second_size);}
                std::vector<std::uint8_t> left(static_cast<std::size_t>(first_size));
                if(first_size)memory.read(GuestAddress{arg(0)},left);
                const auto cmp=std::lexicographical_compare_three_way(left.begin(),left.end(),right.begin(),right.end());
                return returned(cmp<0?UINT32_MAX:cmp>0?1:0);
            }
            const auto locale=locale_pointer?named(read_utf16(memory,GuestAddress{locale_pointer},255)):numbered(0x400);
            if(!locale)return invalid(22);
            if(mapping && !second_size)return invalid(22);
            const auto encoding=page==0 || page==65001?std::string{"UTF-8"}:page==20127?std::string{"US-ASCII"}:"windows-"+std::to_string(page);
            const auto right=ansi_input(memory,arg(2),static_cast<std::int32_t>(second_size),false,encoding.c_str(),true,true);
            if(!mapping){
                const auto left=ansi_input(memory,arg(0),static_cast<std::int32_t>(first_size),false,encoding.c_str(),true,true);
                const auto order=collate_strings(left,right,*locale,0x1000U);if(!order)return invalid(22);
                return returned(static_cast<std::uint32_t>(*order));
            }
            const auto bytes=collation_key(right,*locale,0);if(!bytes)return invalid(22);
            // MSVC _Strxfrm 的區間 ABI 回報排序鍵大小（含排序鍵終止 byte）。
            if(bytes->size()<=first_size && !bytes->empty())memory.write(GuestAddress{arg(0)},*bytes);
            return returned(bytes->size());
        }catch(const MemoryFault&){return invalid(22);}catch(const std::bad_alloc&){return invalid(12);}
        catch(const std::runtime_error&){return invalid(22);}
    }
    if(name=="RtlUTF8ToUnicodeN" || name=="RtlUnicodeToUTF8N"){
        const bool wide=name=="RtlUTF8ToUnicodeN";
        const auto capacity=static_cast<std::uint32_t>(arg(1)),count=static_cast<std::uint32_t>(arg(4));
        if(!arg(0) && !arg(2))return finish(0xc000000dU);
        if(!arg(3))return finish(0xc00000f2U);
        if(!wide && count%2)return finish(0xc00000f3U);
        if(count>INT32_MAX)return finish(0xc000000dU);
        try{
            if(arg(2))memory.check(GuestAddress{arg(2)},4,Access::Write);
            memory.check(GuestAddress{arg(3)},count,Access::Read);
            std::string bytes;std::u16string units;
            if(wide){bytes.resize(count);memory.read(GuestAddress{arg(3)},std::span(reinterpret_cast<std::uint8_t*>(bytes.data()),bytes.size()));}
            else units=read_utf16_counted(memory,GuestAddress{arg(3)},count/2);
            int32_t required=0,replaced=0;UErrorCode error=U_ZERO_ERROR;
            if(wide)u_strFromUTF8WithSub(nullptr,0,&required,bytes.data(),static_cast<int32_t>(bytes.size()),0xfffd,&replaced,&error);
            else u_strToUTF8WithSub(nullptr,0,&required,units.data(),static_cast<int32_t>(units.size()),0xfffd,&replaced,&error);
            if(U_FAILURE(error) && error!=U_BUFFER_OVERFLOW_ERROR)return finish(0xc000000dU);
            error=U_ZERO_ERROR;
            if(wide){units.resize(static_cast<std::size_t>(required));u_strFromUTF8WithSub(units.data(),required,nullptr,bytes.data(),static_cast<int32_t>(bytes.size()),0xfffd,&replaced,&error);}
            else {bytes.resize(static_cast<std::size_t>(required));u_strToUTF8WithSub(bytes.data(),required,nullptr,units.data(),static_cast<int32_t>(units.size()),0xfffd,&replaced,&error);}
            if(U_FAILURE(error))return finish(0xc000000dU);
            const auto total=wide?units.size()*2:bytes.size();auto written=total;
            if(arg(0)){
                written=std::min<std::size_t>(capacity,total);
                // 截斷僅保留完整 code point，不留下半個 surrogate 或 UTF-8 序列。
                if(wide){written&=~std::size_t{1};if(written && written<total && units[written/2-1]>=0xd800 && units[written/2-1]<=0xdbff)written-=2;}
                else while(written && written<total && (static_cast<unsigned char>(bytes[written])&0xc0U)==0x80U)--written;
                memory.check(GuestAddress{arg(0)},written,Access::Write);
                if(wide)for(std::size_t i=0;i<written/2;++i)memory.store(GuestAddress{arg(0)+i*2},16,units[i]);
                else memory.write(GuestAddress{arg(0)},std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),written));
            }
            if(arg(2))memory.store(GuestAddress{arg(2)},32,written);
            return finish(written<total?0xc0000023U:replaced?0x107U:0);
        }catch(const MemoryFault&){return finish(0xc0000005U);}catch(const std::bad_alloc&){return finish(0xc0000017U);}
    }
    if(name=="IdnToAscii" || name=="IdnToUnicode"){
        const auto flags=static_cast<std::uint32_t>(arg(0));
        const auto count=static_cast<std::int32_t>(arg(2)),capacity=static_cast<std::int32_t>(arg(4));
        if(flags&~3U)return fail(1004);
        if(!arg(1) || !count || count < -1 || capacity<0 || (capacity && !arg(3)) || arg(1)==arg(3))return fail(87);
        try{
            auto source=counted_string(memory,arg(1),count,true);
            const bool terminal=!source.empty() && source.back()==0;
            if(terminal)source.pop_back();
            if(source.empty() || source.find(char16_t{})!=source.npos)return fail(123);
            auto converted=idn_detail::convert(source,name=="IdnToAscii",flags,idn_detail::Profile::windows_idna2003);
            if(!converted)return fail(123);
            auto result=std::move(*converted);
            if(terminal)result.push_back(0);
            if(!capacity)return finish(static_cast<std::uint32_t>(result.size()));
            if(static_cast<std::size_t>(capacity)<result.size())return fail(122);
            memory.check(GuestAddress{arg(3)},result.size()*2,Access::Write);
            for(std::size_t i=0;i<result.size();++i)memory.store(GuestAddress{arg(3)+i*2},16,result[i]);
            return finish(static_cast<std::uint32_t>(result.size()));
        }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(14);}
    }
    if(name=="GetLocaleInfoA" || name=="GetLocaleInfoW" || name=="GetLocaleInfoEx"){
        const bool wide=name!="GetLocaleInfoA",extended=name=="GetLocaleInfoEx";
        const auto flags=static_cast<std::uint32_t>(arg(1)),type=flags&0xffffU;const auto capacity=static_cast<std::int32_t>(arg(3));
        if(capacity<0)return fail(87);if(capacity && !arg(2))return fail(122);
        if(flags&~0xf000ffffU)return fail(1004);
        const bool genitive=flags&0x10000000U;
        if(genitive && !((type>=0x38 && type<=0x4f) || type==0x100e || type==0x100f))return fail(1004);
        try{
            std::optional<Locale> locale;
            if(extended){auto value=arg(0)?read_utf16(memory,GuestAddress{arg(0)},85):std::u16string(u"en-US");
                if(value==u"!x-sys-default-locale")value=u"en-US";locale=named(value);}
            else locale=numbered(static_cast<std::uint32_t>(arg(0)));
            if(!locale)return fail(87);const auto info=locale_information(*locale,type,genitive);if(!info)return fail(1004);
            if(flags&0x20000000U){if(!info->number)return fail(1004);const unsigned required=wide?2:4;
                if(!capacity)return finish(required);if(static_cast<unsigned>(capacity)<required)return fail(122);
                memory.store(GuestAddress{arg(2)},32,*info->number);return finish(required);}
            auto value=info->text;value.push_back(0);const auto bytes=wide?utf16_bytes(info->text):ansi_output(value,"UTF-8");
            const auto required=static_cast<std::uint32_t>(wide?bytes.size()/2:bytes.size());
            if(!capacity)return finish(required);if(static_cast<std::uint32_t>(capacity)<required)return fail(122);
            memory.write(GuestAddress{arg(2)},bytes);return finish(required);
        }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(14);}catch(const std::runtime_error&){return fail(13);}
    }
    if(name=="FoldStringA" || name=="FoldStringW"){
        const auto flags=static_cast<std::uint32_t>(arg(0));const auto count=static_cast<std::int32_t>(arg(2)),capacity=static_cast<std::int32_t>(arg(4));const bool wide=name.ends_with('W');
        if(!arg(1) || !count || capacity<0 || (capacity && !arg(3)) || arg(1)==arg(3))return fail(87);
        if(!flags || flags&~0x20f0U || (flags&0x60U)==0x60U || ((flags&0x2000U) && (flags&0x60U)))return fail(1004);
        try{
            auto text=wide?counted_string(memory,arg(1),count,true):ansi_input(memory,arg(1),count,true,"UTF-8");
            if(flags&0x2000U)text=expand_ligatures(text);
            if(flags&0x10U)text=transform(std::move(text),flags&0x40U?u"NFKD":u"NFKC");
            else if(flags&0x20U)text=transform(std::move(text),u"NFC");
            else if(flags&0x40U)text=transform(std::move(text),u"NFD");
            // 數字摺疊可能把補充平面的一對 UTF-16 縮成一字元；容量仍須符合摺疊前的上限。
            const auto maximum=wide?text.size():ansi_output(text,"UTF-8").size();
            if(flags&0x80U)text=fold_digits(text);
            auto bytes=wide?std::vector<std::uint8_t>{}:ansi_output(text,"UTF-8");const auto actual=wide?text.size():bytes.size();const auto required=flags&0x80U?std::max(maximum,actual):actual;
            if(required>INT32_MAX)return fail(14);if(!capacity)return finish(static_cast<std::uint32_t>(required));
            if(static_cast<std::size_t>(capacity)<required)return fail(122);
            if(wide){bytes.resize(text.size()*2);for(std::size_t i=0;i<text.size();++i){bytes[i*2]=static_cast<std::uint8_t>(text[i]);bytes[i*2+1]=static_cast<std::uint8_t>(text[i]>>8);}}
            memory.write(GuestAddress{arg(3)},bytes);return finish(static_cast<std::uint32_t>(actual));
        }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(14);}catch(const std::runtime_error&){return fail(13);}
    }
    if(name.starts_with("Var") && name.ends_with("FromStr")){
        if(!arg(3) || (static_cast<std::uint32_t>(arg(2))&~0x80000003U))return finish(0x80070057U);
        if(!arg(0))return finish(0x80020005U);const auto locale=numbered(static_cast<std::uint32_t>(arg(1)));if(!locale)return finish(0x80070057U);
        try{
            std::uint32_t parse_status=0;const auto parsed=automation::parse_number(read_utf16(memory,GuestAddress{arg(0)},32767),static_cast<std::uint32_t>(arg(1)),parse_status);if(!parsed)return finish(parse_status);const auto& decimal=*parsed;
            const bool unsigned_type=name.starts_with("VarUI");const auto bytes=static_cast<unsigned>(name[unsigned_type?5:4]-'0');std::uint64_t value=0;
            const auto status=automation_integer(decimal,bytes*8,unsigned_type,value);if(status)return finish(status);memory.store(GuestAddress{arg(3)},static_cast<std::uint8_t>(bytes*8),value);return finish(0);
        }catch(const std::invalid_argument&){return finish(0x80020005U);}catch(const std::bad_alloc&){return finish(0x8007000eU);}
    }
    if(locales().empty())return fail(50);
    const auto requested=[&](){return !arg(0) || arg(0)==(x86_?UINT32_MAX:UINT64_MAX)?std::u16string(u"en-US"):read_utf16(memory,GuestAddress{arg(0)},85);};
    if(name.starts_with("GetNumberFormat") || name.starts_with("GetCurrencyFormat")){
        const bool currency=name.starts_with("GetCurrency"),extended=name.ends_with("Ex"),wide=!name.ends_with('A');
        const auto locale=extended?named(requested()):numbered(static_cast<std::uint32_t>(arg(0)));if(!locale)return fail(87);
        const auto capacity=static_cast<std::int32_t>(arg(5));const auto flags=static_cast<std::uint32_t>(arg(1));
        if(!arg(2) || capacity<0 || (capacity && (!arg(4) || arg(4)==arg(2))))return fail(87);
        if(flags&~0xc0000000U || (arg(3) && flags))return fail(1004);
        const auto read=[&](std::uint64_t address){return wide?read_utf16(memory,GuestAddress{address}):utf8_to_utf16_lossy(read_ansi(memory,address));};
        const auto input=read(arg(2));std::string decimal;bool point=false,digit=false;
        for(std::size_t i=0;i<input.size();++i){const auto c=input[i];if(c>=u'0' && c<=u'9')digit=true;else if(c==u'.' && !point)point=true;else if(c!=u'-' || i)return fail(87);decimal+=static_cast<char>(c);}
        if(!digit)return fail(87);
        UErrorCode error=U_ZERO_ERROR;const auto id=icu_locale(*locale);NumberFormat format(unum_open(currency?UNUM_CURRENCY_ACCOUNTING:UNUM_DECIMAL,nullptr,0,id.c_str(),nullptr,&error),unum_close);if(U_FAILURE(error) || !format)return fail(87);
        unum_setAttribute(format.get(),UNUM_ROUNDING_MODE,UNUM_ROUND_HALFUP);
        std::u16string result;
        if(!arg(3)){
            if(!currency){unum_setAttribute(format.get(),UNUM_MIN_FRACTION_DIGITS,2);unum_setAttribute(format.get(),UNUM_MAX_FRACTION_DIGITS,2);}
            result=number_text(format.get(),decimal);
        }else{
            const unsigned pointer=x86_?12U:16U,unit=x86_?4U:8U,order=pointer+unit*2;const auto base=arg(3);
            memory.check(GuestAddress{base},currency?(x86_?32U:48U):(x86_?24U:40U),Access::Read);
            const auto load=[&](unsigned at){return static_cast<unsigned>(memory.load(GuestAddress{base+at},32));};
            const auto digits=load(0),zero=load(4),grouping=load(8),negative=load(order),positive=currency?load(order+4):0;
            if(digits>9 || zero>1 || negative>(currency?15U:4U) || positive>3)return fail(87);
            const auto groups=std::to_string(grouping);if(grouping && groups.find('0')!=std::string::npos && groups.find('0')!=groups.size()-1)return fail(87);
            const auto decimal_ptr=memory.load(GuestAddress{base+pointer},static_cast<std::uint8_t>(unit*8)),thousands_ptr=memory.load(GuestAddress{base+pointer+unit},static_cast<std::uint8_t>(unit*8));
            if(!decimal_ptr || !thousands_ptr)return fail(87);const auto separator=read(decimal_ptr),thousands=read(thousands_ptr);
            std::u16string symbol;if(currency){const auto address=memory.load(GuestAddress{base+order+8},static_cast<std::uint8_t>(unit*8));if(!address)return fail(87);symbol=read(address);}
            if(separator.size()>4 || thousands.size()>4 || symbol.size()>13 || (digits && separator.empty()))return fail(87);
            const auto invalid_separator=[](std::u16string_view value){return std::any_of(value.begin(),value.end(),[](char16_t c){return c>=u'0' && c<=u'9';});};
            if(invalid_separator(separator) || invalid_separator(thousands))return fail(87);
            const auto sign=number_symbol(format.get(),UNUM_MINUS_SIGN_SYMBOL);const bool minus=decimal.front()=='-';if(minus)decimal.erase(0,1);
            error=U_ZERO_ERROR;NumberFormat magnitude(unum_open(UNUM_DECIMAL,nullptr,0,"en_US_POSIX",nullptr,&error),unum_close);if(U_FAILURE(error) || !magnitude)return fail(87);
            unum_setAttribute(magnitude.get(),UNUM_GROUPING_USED,0);unum_setAttribute(magnitude.get(),UNUM_MIN_FRACTION_DIGITS,static_cast<std::int32_t>(digits));unum_setAttribute(magnitude.get(),UNUM_MAX_FRACTION_DIGITS,static_cast<std::int32_t>(digits));unum_setAttribute(magnitude.get(),UNUM_ROUNDING_MODE,UNUM_ROUND_HALFUP);
            const auto formatted=number_text(magnitude.get(),decimal);const auto dot=formatted.find(u'.');auto integral=formatted.substr(0,dot);
            if(!zero && integral==u"0" && digits)integral.clear();result=group_number(integral,grouping,thousands);if(dot!=formatted.npos)result+=separator+formatted.substr(dot+1);
            static constexpr std::array<std::u16string_view,5> numbers{u"(n)",u"-n",u"- n",u"n-",u"n -"};
            static constexpr std::array<std::u16string_view,4> positives{u"$n",u"n$",u"$ n",u"n $"};
            static constexpr std::array<std::u16string_view,16> negatives{u"($n)",u"-$n",u"$-n",u"$n-",u"(n$)",u"-n$",u"n-$",u"n$-",u"-n $",u"-$ n",u"n $-",u"$ n-",u"$ -n",u"n- $",u"($ n)",u"(n $)"};
            result=decorate_number(currency?(minus?negatives[negative]:positives[positive]):minus?numbers[negative]:u"n",result,symbol,sign);
        }
        const auto narrow=wide?std::string{}:utf16_to_utf8(result);const auto required=static_cast<std::uint32_t>((wide?result.size():narrow.size())+1);
        if(!capacity)return finish(required);if(static_cast<std::uint32_t>(capacity)<required)return fail(122);
        if(wide)memory.write(GuestAddress{arg(4)},utf16_bytes(result));else memory.write(GuestAddress{arg(4)},std::span(reinterpret_cast<const std::uint8_t*>(narrow.c_str()),narrow.size()+1));return finish(required);
    }
    if(name=="GetDateFormatW" || name=="GetDateFormatEx" || name=="GetTimeFormatW" || name=="GetTimeFormatEx"){
        const bool date=name.starts_with("GetDate"),extended=name.ends_with("Ex");
        auto locale=extended?named(requested()):numbered(static_cast<std::uint32_t>(arg(0)));if(!locale)return fail(87);
        if(date && extended && arg(6))return fail(87);
        const auto flags=static_cast<std::uint32_t>(arg(1)),options=flags&0xffU;const auto capacity=static_cast<std::int32_t>(arg(5));
        if(capacity<0 || (capacity && !arg(4)))return fail(87);
        if(flags&~(date?0xc00000ffU:0xc000000fU))return fail(1004);
        if(date){const auto style=options&0x8bU;if(std::popcount(style)>1 || std::popcount(options&0x70U)>1 || (arg(3) && options))return fail(1004);
            if(options&~0x8bU)return fail(50); // alternate calendar／layout markers 留待同家族擴充。
        }
        UDate stamp=static_cast<double>(runtime_.system_filetime()/10000)-11644473600000.0;
        UErrorCode error=U_ZERO_ERROR;
        if(arg(2)){
            memory.check(GuestAddress{arg(2)},16,Access::Read);std::array<int,8> fields{};for(unsigned i=0;i<8;++i)fields[i]=static_cast<int>(memory.load(GuestAddress{arg(2)+i*2},16));
            if(date){if(fields[0]<1601)return fail(87);fields[4]=fields[5]=fields[6]=fields[7]=0;}
            else{fields[0]=2000;fields[1]=1;fields[3]=1;fields[7]=0;}
            std::unique_ptr<UCalendar,decltype(&ucal_close)> calendar(ucal_open(u"UTC",3,"en_US@calendar=gregorian",UCAL_GREGORIAN,&error),ucal_close);
            if(U_FAILURE(error) || !calendar)return fail(8);ucal_setAttribute(calendar.get(),UCAL_LENIENT,0);ucal_clear(calendar.get());
            ucal_setDateTime(calendar.get(),fields[0],fields[1]-1,fields[3],fields[4],fields[5],fields[6],&error);stamp=ucal_getMillis(calendar.get(),&error);if(U_FAILURE(error))return fail(87);
        }
        const auto id=icu_locale(*locale);DateFormat format(udat_open(date?UDAT_NONE:UDAT_MEDIUM,date?(options&2U?UDAT_FULL:UDAT_SHORT):UDAT_NONE,id.c_str(),u"UTC",3,nullptr,0,&error),udat_close);
        if(U_FAILURE(error) || !format)return fail(87);
        std::u16string pattern;
        if(date){
            if(arg(3))pattern=windows_date_pattern(read_utf16(memory,GuestAddress{arg(3)}));
            else if(options&(8U|0x80U)){
                std::unique_ptr<UDateTimePatternGenerator,decltype(&udatpg_close)> generator(udatpg_open(id.c_str(),&error),udatpg_close);if(U_FAILURE(error) || !generator)return fail(87);
                const auto skeleton=options&8U?u"yMMMM":u"MMMMd";const auto size=udatpg_getBestPattern(generator.get(),skeleton,5,nullptr,0,&error);error=U_ZERO_ERROR;
                pattern.resize(static_cast<std::size_t>(size));udatpg_getBestPattern(generator.get(),skeleton,5,pattern.data(),size,&error);if(U_FAILURE(error))return fail(87);
            }
        }else{
            std::u16string marker;DateFormat ampm(udat_open(UDAT_PATTERN,UDAT_PATTERN,id.c_str(),u"UTC",3,u"a",1,&error),udat_close);if(U_FAILURE(error) || !ampm)return fail(87);marker=date_text(ampm.get(),stamp);
            pattern=time_pattern(arg(3)?read_utf16(memory,GuestAddress{arg(3)}):date_pattern(format.get()),options,arg(3)!=0,marker);
        }
        if(date && !arg(3)){
            if(pattern.empty())pattern=date_pattern(format.get());
            auto tokens=date_tokens(pattern);month_pattern_context(tokens);pattern=render_pattern(tokens);
        }
        if(arg(3) || !pattern.empty())udat_applyPattern(format.get(),false,pattern.data(),static_cast<std::int32_t>(pattern.size()));
        const auto value=date_text(format.get(),stamp);const auto required=static_cast<std::uint32_t>(value.size()+1);
        if(!capacity)return finish(required);if(static_cast<std::uint32_t>(capacity)<required)return fail(122);memory.write(GuestAddress{arg(4)},utf16_bytes(value));return finish(required);
    }
    if(name=="GetStringTypeW" || name=="GetStringTypeExW" || name=="GetStringTypeA" || name=="GetStringTypeExA"){
        const unsigned offset=name=="GetStringTypeW"?0U:1U,type=static_cast<unsigned>(arg(offset));
        const auto source=arg(offset+1),output=arg(offset+3);const auto length=static_cast<std::int32_t>(arg(offset+2));
        if(type!=1 && type!=2 && type!=4)return fail(1004);
        if(!source || !output || source==output || !length)return fail(87);
        const auto locale=numbered(static_cast<std::uint32_t>(arg(0)));if(name.ends_with('A') && !locale)return fail(87);
        const auto text=name.ends_with('A')?ansi_input(memory,source,length,true,ansi_encoding(*locale)):counted_string(memory,source,length,true);memory.check(GuestAddress{output},text.size()*2,Access::Write);
        for(std::size_t i=0;i<text.size();++i)memory.store(GuestAddress{output+i*2},16,character_type(text[i],type));return finish(1);
    }
    if(name=="ConvertDefaultLocale"){
        const auto id=static_cast<std::uint32_t>(arg(0));
        if(id==0x400 || id==0x800 || id==0)return finish(0x409);
        const auto locale=numbered(id);if(!locale || !locale->neutral)return finish(id);
        const auto converted=specific(*locale);return finish(converted?converted->id:id);
    }
    if(name=="CompareStringOrdinal"){
        if(!arg(0) || !arg(2) || arg(4)>1)return fail(87);
        auto left=counted_string(memory,arg(0),static_cast<std::int32_t>(arg(1)),false),right=counted_string(memory,arg(2),static_cast<std::int32_t>(arg(3)),false);
        if(arg(4)){for(auto& c:left)c=static_cast<char16_t>(u_toupper(c));for(auto& c:right)c=static_cast<char16_t>(u_toupper(c));}
        const auto result=left.compare(right);return finish(result<0?1:result>0?3:2);
    }
    if(name=="CompareStringW" || name=="CompareStringA" || name=="CompareStringEx" || name=="LCMapStringA" || name=="LCMapStringW" || name=="LCMapStringEx"){
        const bool extended=name.ends_with("Ex"),mapping=name.starts_with("LCMap");
        const auto locale=extended?named(requested()):numbered(static_cast<std::uint32_t>(arg(0)));
        if(!locale)return fail(87);if(extended && (arg(7) || arg(8)))return fail(87);if(extended && arg(6))return fail(50);
        const bool ansi=name.ends_with('A');const auto raw_flags=static_cast<std::uint32_t>(arg(1));const auto flags=ansi?raw_flags&~0x40000000U:raw_flags;
        const auto input=[&](std::uint64_t address,std::int32_t length,bool terminal){return ansi?ansi_input(memory,address,length,terminal,ansi_encoding(*locale,(raw_flags&0x40000000U)!=0)):counted_string(memory,address,length,terminal);};
        if(!arg(2))return fail(87);
        if(!mapping){
            if(flags&~compare_flags)return fail(1004);if(!arg(4))return fail(87);
            const auto order=collate_strings(input(arg(2),static_cast<std::int32_t>(arg(3)),false),input(arg(4),static_cast<std::int32_t>(arg(5)),false),*locale,flags);
            if(!order)return fail(87);return finish(static_cast<std::uint32_t>(*order+2));
        }
        const auto capacity=static_cast<std::int32_t>(arg(5)),length=static_cast<std::int32_t>(arg(3));const bool sortkey=flags&0x400U;
        if(capacity<0 || !length || (capacity && !arg(4)))return fail(87);
        if(flags&(0x20000000U|0x40000U))return fail(50); // Windows 的 sort handle／hash 尚無共同後端契約。
        if(sortkey){if(flags&~(compare_flags|0xc00U))return fail(1004);}
        else{
            constexpr unsigned allowed=0x07f00b06U;
            if(!flags || (flags&~allowed) || ((flags&0x300000U)==0x300000U) || ((flags&0xc00000U)==0xc00000U) || ((flags&0x6000000U)==0x6000000U)
                || ((flags&0x1000000U) && !(flags&0x300U)) || ((flags&6U) && (flags&~0x806U)))return fail(1004);
        }
        if(capacity && arg(2)==arg(4) && (sortkey || !(flags&0x300U) || (flags&~0x1000300U)))return fail(87);
        auto text=input(arg(2),length,!sortkey);std::vector<std::uint8_t> bytes;std::uint32_t required=0;
        if(sortkey){
            auto sorted=collation_key(std::move(text),*locale,flags);if(!sorted)return fail(87);
            bytes=std::move(*sorted);required=static_cast<std::uint32_t>(bytes.size());
        }else{
            if(flags&0x800000U)text=transform(std::move(text),u"Halfwidth-Fullwidth; NFC");
            if(flags&0x400000U)text=transform(std::move(text),u"Fullwidth-Halfwidth");
            if(flags&0x100000U)text=transform(std::move(text),u"Katakana-Hiragana");
            if(flags&0x200000U)text=transform(std::move(text),u"Hiragana-Katakana");
            if(flags&0x2000000U)text=transform(std::move(text),u"Traditional-Simplified");
            if(flags&0x4000000U)text=transform(std::move(text),u"Simplified-Traditional");
            if(flags&0x300U)text=map_case(text,flags&0x1000000U?*locale:Locale{u"",0x7f,false},flags&0x300U);
            if(flags&2U)text=transform(std::move(text),u"NFD; [:Nonspacing Mark:] Remove; NFC");
            if(flags&4U)text=transform(std::move(text),u"[[:Punctuation:][:Symbol:]] Remove");
            if(ansi){bytes=ansi_output(text,ansi_encoding(*locale,(raw_flags&0x40000000U)!=0));required=static_cast<std::uint32_t>(bytes.size());}
            else {required=static_cast<std::uint32_t>(text.size());bytes=utf16_bytes(text);bytes.resize(text.size()*2);}
        }
        if(flags&0x800U)for(std::size_t i=0;i+1<bytes.size();i+=2)std::swap(bytes[i],bytes[i+1]);
        if(!capacity)return finish(required);if(static_cast<std::uint32_t>(capacity)<required)return fail(122);
        memory.write(GuestAddress{arg(4)},bytes);return finish(required);
    }
    if(name=="IsValidLocaleName")return finish(named(requested()).has_value());
    if(name=="IsValidLocale"){
        if(arg(1)!=1 && arg(1)!=2)return fail(1004);return finish(numbered(static_cast<std::uint32_t>(arg(0))).has_value());
    }
    if(name=="LocaleNameToLCID"){
        if(arg(1)&~UINT64_C(0x08000000))return fail(1004);
        auto locale=named(requested());if(!locale)return fail(87);
        if(!(arg(1)&0x08000000))locale=specific(*locale);
        return locale?finish(locale->id):fail(87);
    }
    if(name=="LCIDToLocaleName" || name=="ResolveLocaleName"){
        std::optional<Locale> locale;std::uint64_t output=0;std::int32_t capacity=0;
        if(name=="LCIDToLocaleName"){
            if(arg(3)&~UINT64_C(0x08000000))return fail(1004);
            locale=numbered(static_cast<std::uint32_t>(arg(0)));if(locale && !(arg(3)&0x08000000))locale=specific(*locale);output=arg(1);capacity=static_cast<std::int32_t>(arg(2));
        }else{
            auto value=requested();locale=named(value);
            while(!locale){const auto pos=value.find_last_of(u'-');if(pos==value.npos)break;value.resize(pos);locale=named(value);}
            if(locale && locale->neutral)locale=specific(*locale);output=arg(1);capacity=static_cast<std::int32_t>(arg(2));
        }
        if(!locale || capacity<0 || (!output && capacity))return fail(87);const auto required=static_cast<std::uint32_t>(locale->name.size()+1);
        if(!capacity)return finish(required);if(static_cast<std::uint32_t>(capacity)<required)return fail(122);
        memory.write(GuestAddress{output},utf16_bytes(locale->name));return finish(required);
    }
    if(name.starts_with("EnumSystemLocales")){
        const bool extended=name.ends_with("Ex"),wide=extended || name.ends_with('W');const auto flags=static_cast<std::uint32_t>(arg(1));
        if(!arg(0) || (extended && arg(3)))return fail(87);
        if((extended && (flags&~0x7fU)) || (!extended && flags!=1 && flags!=2))return fail(1004);
        memory.check(GuestAddress{arg(0)},1,Access::Execute);
        std::vector<std::pair<std::vector<std::uint8_t>,unsigned>> items;std::set<std::uint32_t> ids;
        for(const auto& [key,locale]:locales()){
            (void)key;if(locale.name.empty())continue;const unsigned kind=(locale.id==0x1000?4U:1U)|(locale.neutral?0x10U:0x20U);
            if(extended){if(flags && !(flags&kind))continue;items.emplace_back(utf16_bytes(locale.name),kind);}
            else {if(locale.neutral || locale.id==0x1000 || !ids.insert(locale.id).second)continue;char hex[9]{};std::snprintf(hex,sizeof hex,"%08x",locale.id);items.emplace_back(wide?utf16_bytes(utf8_to_utf16(hex)):std::vector<std::uint8_t>(hex,hex+9),0);}
        }
        std::size_t size=0;for(const auto& item:items)size+=item.first.size();if(!size)return finish(1);
        const auto buffer=allocate_heap(memory,size);if(!buffer)return fail(8);std::vector<GuestCallback> callbacks;auto at=buffer;
        try{for(const auto& [bytes,kind]:items){memory.write(GuestAddress{at},bytes);callbacks.push_back({arg(0),extended?std::vector<std::uint64_t>{at,kind,arg(2)}:std::vector<std::uint64_t>{at}});at+=bytes.size();}start_callbacks(state,memory,std::move(callbacks),Continuation::LocaleEnumeration,buffer);}catch(...){(void)free_heap(memory,buffer,state);throw;}
        return std::nullopt;
    }
    return fail(50);
}
}
