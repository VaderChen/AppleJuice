#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <locale.h>
#include <wctype.h>
#include "x64arm/host_services.hpp"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#if defined(__APPLE__)
#include <CoreFoundation/CFString.h>
#endif

namespace x64arm {
namespace {
struct CaseLocale {
#if defined(_WIN32)
    _locale_t value{};
    CaseLocale() : value(_create_locale(LC_CTYPE, ".UTF-8")) {
        if (!value) throw HostCallFault("宿主無法建立固定 Unicode 大小寫 locale");
    }
    ~CaseLocale() { _free_locale(value); }
#else
    locale_t value{};
    CaseLocale() {
        for (const auto name : {"C.UTF-8", "en_US.UTF-8"}) {
            value = newlocale(LC_CTYPE_MASK, name, nullptr);
            if (value) break;
        }
        if (!value) throw HostCallFault("宿主無法建立固定 Unicode 大小寫 locale");
    }
    ~CaseLocale() { freelocale(value); }
#endif
};
}
char16_t unicode_case_unit(char16_t character, bool upper) {
    if(character<128){
        if(upper)return character>=u'a' && character<=u'z'?static_cast<char16_t>(character-32):character;
        return character>=u'A' && character<=u'Z'?static_cast<char16_t>(character+32):character;
    }
    // Windows W 介面以 UTF-16 code unit 運作，保留 surrogate；不作會擴長的完整 case folding。
    if (character >= 0xd800 && character <= 0xdfff) return character;
    static const CaseLocale locale;
#if defined(_WIN32)
    const auto converted = upper ? _towupper_l(character, locale.value) : _towlower_l(character, locale.value);
#else
    const auto converted = upper ? towupper_l(character, locale.value) : towlower_l(character, locale.value);
#endif
    return converted <= 0xffff ? static_cast<char16_t>(converted) : character;
}
std::optional<std::u16string> normalize_utf16(std::u16string_view text,bool composed) {
    // 大多數 API 路徑是 ASCII，不需建立平台 Unicode 物件。
    if(std::all_of(text.begin(),text.end(),[](char16_t c){return c<128;}))return std::u16string(text);
#if defined(__APPLE__)
    struct OwnedString {CFMutableStringRef value{CFStringCreateMutable(kCFAllocatorDefault,0)};~OwnedString(){if(value)CFRelease(value);}} value;
    if(!value.value)throw std::bad_alloc();
    CFStringAppendCharacters(value.value,reinterpret_cast<const UniChar*>(text.data()),static_cast<CFIndex>(text.size()));
    CFStringNormalize(value.value,composed?kCFStringNormalizationFormC:kCFStringNormalizationFormD);
    const auto count=CFStringGetLength(value.value);std::u16string result(static_cast<std::size_t>(count),0);
    CFStringGetCharacters(value.value,CFRangeMake(0,count),reinterpret_cast<UniChar*>(result.data()));return result;
#else
    // 非 Apple backend 尚無 canonical normalization provider，不能假裝完成。
    (void)composed;return std::nullopt;
#endif
}
namespace {
constexpr std::array<char16_t,32> cp1252_high{0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
}
std::u16string cp1252_to_utf16(std::string_view text){std::u16string output;output.reserve(text.size());for(const auto character:text){const auto byte=static_cast<unsigned char>(character);output.push_back(byte>=0x80 && byte<0xa0?cp1252_high[byte-0x80]:byte);}return output;}
std::string utf16_to_cp1252(std::u16string_view text,char replacement,bool& used_default){
    std::string output;output.reserve(text.size());used_default=false;
    for(std::size_t i=0;i<text.size();++i){const auto c=text[i];if(c<0x80 || (c>=0xa0 && c<=0xff))output.push_back(static_cast<char>(c));
        else {unsigned index=0;while(index<32 && cp1252_high[index]!=c)++index;if(index<32)output.push_back(static_cast<char>(0x80+index));else {output.push_back(replacement);used_default=true;if(c>=0xd800 && c<=0xdbff && i+1<text.size() && text[i+1]>=0xdc00 && text[i+1]<=0xdfff)++i;}}
    }return output;
}
namespace {
// 共用驗證器不配置記憶體、不拋例外；嚴格／寬鬆模式只在錯誤處理上不同。
const char* decode_utf8(std::string_view text,std::size_t& i,std::uint32_t& value) {
    const auto first=static_cast<std::uint8_t>(text[i++]);
    std::uint32_t minimum=0;unsigned continuation=0;
    if(first<128)value=first;
    else if(first>=0xc2 && first<=0xdf){value=first&31U;continuation=1;minimum=0x80;}
    else if(first>=0xe0 && first<=0xef){value=first&15U;continuation=2;minimum=0x800;}
    else if(first>=0xf0 && first<=0xf4){value=first&7U;continuation=3;minimum=0x10000;}
    else return "UTF-8 起始 byte 不合法";
    for(unsigned n=0;n<continuation;++n){
        if(i==text.size())return "UTF-8 序列截斷";
        const auto byte=static_cast<std::uint8_t>(text[i++]);
        if((byte&0xc0U)!=0x80)return "UTF-8 continuation byte 不合法";
        value=(value<<6)|(byte&63U);
    }
    if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return "UTF-8 code point 不合法";
    return nullptr;
}
template<bool Lossy> std::u16string convert_utf8(std::string_view text) {
    std::u16string result;
    // 小型多位元組字串可能仍適用 SSO；不因輸入的 byte 數強迫配置 heap。
    if(text.size()>32)result.reserve(text.size());
    for(std::size_t i=0;i<text.size();){
        if(static_cast<std::uint8_t>(text[i])<128){
#if defined(__aarch64__)
            // 僅讀取 view 內完整的 16 bytes，ASCII 的 NUL 也照長度保留。
            while(text.size()-i>=16){
                const auto bytes=vld1q_u8(reinterpret_cast<const std::uint8_t*>(text.data()+i));
                if(vmaxvq_u8(bytes)>=128)break;
                const auto low=vmovl_u8(vget_low_u8(bytes)),high=vmovl_high_u8(bytes);
                const auto offset=result.size();result.resize(offset+16);
                std::memcpy(result.data()+offset,&low,sizeof(low));std::memcpy(result.data()+offset+8,&high,sizeof(high));i+=16;
            }
#endif
            while(i<text.size() && static_cast<std::uint8_t>(text[i])<128)result.push_back(static_cast<char16_t>(text[i++]));
            continue;
        }
        const auto start=i;std::uint32_t value=0;
        if(const auto* error=decode_utf8(text,i,value)){
            if constexpr(!Lossy)throw std::invalid_argument(error);
            else {result.push_back(u'\ufffd');i=start+1;continue;} // 保留既有每個無效 byte 的替代語義。
        }
        if(value<=0xffff)result.push_back(static_cast<char16_t>(value));
        else {value-=0x10000;result.push_back(static_cast<char16_t>(0xd800U|(value>>10)));result.push_back(static_cast<char16_t>(0xdc00U|(value&1023U)));}
    }
    return result;
}
}
std::u16string utf8_to_utf16(std::string_view text){return convert_utf8<false>(text);}
std::u16string utf8_to_utf16_lossy(std::string_view text){return convert_utf8<true>(text);}
std::string utf16_to_utf8(std::u16string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if(text[i]<128){
#if defined(__aarch64__)
            if constexpr(std::endian::native==std::endian::little)while(text.size()-i>=16){
                const auto* source=reinterpret_cast<const std::uint8_t*>(text.data()+i);
                const auto low=vreinterpretq_u16_u8(vld1q_u8(source)),high=vreinterpretq_u16_u8(vld1q_u8(source+16));
                if(vmaxvq_u16(vmaxq_u16(low,high))>=128)break;
                const auto bytes=vcombine_u8(vmovn_u16(low),vmovn_u16(high));
                const auto offset=result.size();result.resize(offset+16);
                vst1q_u8(reinterpret_cast<std::uint8_t*>(result.data()+offset),bytes);i+=16;
            }
#endif
            while(i<text.size() && text[i]<128)result.push_back(static_cast<char>(text[i++]));
            continue;
        }
        std::uint32_t value = text[i++];
        if (value >= 0xd800 && value <= 0xdbff) {
            if (i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) throw std::invalid_argument("UTF-16 缺少 low surrogate");
            value = 0x10000U + ((value - 0xd800U) << 10) + (text[i++] - 0xdc00U);
        } else if (value >= 0xdc00 && value <= 0xdfff) throw std::invalid_argument("UTF-16 孤立的 low surrogate");
        if (value < 0x80) result.push_back(static_cast<char>(value));
        else if (value < 0x800) {
            result.push_back(static_cast<char>(0xc0U | (value >> 6))); result.push_back(static_cast<char>(0x80U | (value & 63U)));
        } else if (value < 0x10000) {
            result.push_back(static_cast<char>(0xe0U | (value >> 12))); result.push_back(static_cast<char>(0x80U | ((value >> 6) & 63U))); result.push_back(static_cast<char>(0x80U | (value & 63U)));
        } else {
            result.push_back(static_cast<char>(0xf0U | (value >> 18))); result.push_back(static_cast<char>(0x80U | ((value >> 12) & 63U)));
            result.push_back(static_cast<char>(0x80U | ((value >> 6) & 63U))); result.push_back(static_cast<char>(0x80U | (value & 63U)));
        }
    }
    return result;
}
std::u16string read_utf16_counted(const GuestMemory& memory,GuestAddress address,std::size_t count) {
    std::u16string result;
    if(!count)return result;
    if(count<=8){
        // 短資料以一次範圍讀取處理跨映射；失敗時重走逐字流程，保留 16-bit 故障契約。
        std::array<std::uint8_t,16> bytes{};bool readable=true;
        try{memory.read(address,std::span(bytes).first(count*2));}catch(const MemoryFault&){readable=false;}
        if(readable){
            result.resize(count);
            if constexpr(std::endian::native==std::endian::little)std::memcpy(result.data(),bytes.data(),count*2);
            else for(std::size_t n=0;n<count;++n)result[n]=static_cast<char16_t>(bytes[n*2]|(static_cast<unsigned>(bytes[n*2+1])<<8));
            return result;
        }
        for(std::size_t n=0;n<count;++n)result.push_back(static_cast<char16_t>(memory.load(GuestAddress{address.value+n*2},16)));
        return result;
    }
    for(std::size_t i=0;i<count;){
        const GuestAddress at{address.value+i*2};
        std::span<const std::uint8_t> bytes;
        try{bytes=memory.readable_prefix(at,std::min(count-i,SIZE_MAX/2)*2);}catch(const MemoryFault&){} // 由 scalar 路徑產生原本的 16-bit 故障。
        if(bytes.size()<2){
            result.push_back(static_cast<char16_t>(memory.load(at,16)));++i;continue;
        }
        // 跨映射仍逐段批次解碼；只有橫跨兩個映射的單一字元需要 scalar load。
        const auto units=bytes.size()/2;result.resize(i+units);
        if constexpr(std::endian::native==std::endian::little)std::memcpy(result.data()+i,bytes.data(),units*2);
        else for(std::size_t n=0;n<units;++n)result[i+n]=static_cast<char16_t>(bytes[n*2]|(static_cast<unsigned>(bytes[n*2+1])<<8));
        i+=units;
    }
    return result;
}
std::u16string read_utf16(const GuestMemory& memory, GuestAddress address, std::size_t limit) {
    std::u16string result;
    for (std::size_t i = 0; i <= limit;) {
        if (i > (UINT64_MAX - address.value) / 2) throw MemoryFault(address, Access::Read, 2, "UTF-16 地址溢位");
        const GuestAddress at{address.value+i*2};std::span<const std::uint8_t> bytes;
        try{bytes=memory.readable_prefix(at,(std::min<std::size_t>(limit-i,1023)+1)*2);}
        catch(const MemoryFault&){(void)memory.load(at,16);throw;} // 保留原有 16-bit 故障範圍。
        if(bytes.size()<2){
            const auto value=static_cast<char16_t>(memory.load(at,16)); // 字元跨映射時由共用 scalar 路徑處理。
            if(!value)return result;
            if(i==limit)throw std::invalid_argument("UTF-16 字串超過長度上限");
            result.push_back(value);++i;continue;
        }
        const auto units=bytes.size()/2;std::size_t count=0;
        while(count<units && (bytes[count*2] || bytes[count*2+1]))++count;
        if(count>limit-i)throw std::invalid_argument("UTF-16 字串超過長度上限");
        result.resize(i+count);
        if constexpr(std::endian::native==std::endian::little){if(count)std::memcpy(result.data()+i,bytes.data(),count*2);}
        else for(std::size_t n=0;n<count;++n)result[i+n]=static_cast<char16_t>(bytes[n*2]|(static_cast<unsigned>(bytes[n*2+1])<<8));
        if(count<units)return result;
        i+=count;
    }
    throw std::logic_error("無法到達的 UTF-16 分支");
}
std::vector<std::uint8_t> utf16_bytes(std::u16string_view text) {
    std::vector<std::uint8_t> bytes((text.size() + 1) * 2);
    if constexpr(std::endian::native==std::endian::little){if(!text.empty())std::memcpy(bytes.data(),text.data(),text.size()*2);}
    else for (std::size_t i = 0; i < text.size(); ++i) { bytes[i * 2] = static_cast<std::uint8_t>(text[i]); bytes[i * 2 + 1] = static_cast<std::uint8_t>(text[i] >> 8); }
    return bytes;
}
}
