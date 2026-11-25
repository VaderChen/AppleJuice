#include "x64arm/windows_services.hpp"
#include "windows_locale_data.hpp"
#include "crt_mbcs.hpp"
#include "crt_secure_fill_exports.hpp"
#include "crt_library.hpp"
#include "crt_secure_append.h"
#include "crt_bounded_length.hpp"
#include "crt_multibyte_state.hpp"
#include "windows_crt_character.hpp"
#include <unicode/uchar.h>
#include <algorithm>
#include <array>
#include <set>
#include <tuple>

// CRT 多位元組字串（_mbs*／_mbc*／_ismbc*）補充家族。與既有 _mbslen／_mbsnbcpy 等共用 _locale_t 的 mbcinfo：
// 前導／後隨位元組取自客體 _mbctype（_setmbcp 後的實際狀態），單位元組分類與大小寫取自該碼頁字元表；
// UTF-8 依 Windows 視為單位元組碼頁。雙位元組字元的分類與大小寫經碼頁解碼成 Unicode 後判斷，再編回同一碼頁。
// 日文專用函式（平假名／片假名、JIS 等級、JIS↔Shift_JIS、全形／半形）只在碼頁 932 生效，其他碼頁依 Windows 傳回原值或 0。
// _l 版本以最後一個參數的 locale 物件選擇 mbcinfo，其餘共用同一實作。
namespace x64arm {
namespace {
struct ByteClass {std::string_view name;unsigned ctype,type;};
constexpr ByteClass byte_classes[]{
#define CRT_MBC_BYTE(name,ctype,type) {#name,ctype,type},
#include "crt_mbc_byte_operations.inc"
#undef CRT_MBC_BYTE
};
constexpr std::uint32_t einval=22,erange=34,eilseq=42,struncate=80,nlscmperror=0x7fffffffU;
// cp932 半形片假名 0xA1～0xDF（U+FF61～U+FF9F）的全形對應：Unicode 半形／全形相容對應，濁音與半濁音符號採獨立全形符號。
constexpr std::array<char16_t,63> fullwidth_kana{
    0x3002,0x300C,0x300D,0x3001,0x30FB,0x30F2,0x30A1,0x30A3,0x30A5,0x30A7,0x30A9,0x30E3,0x30E5,0x30E7,0x30C3,0x30FC,
    0x30A2,0x30A4,0x30A6,0x30A8,0x30AA,0x30AB,0x30AD,0x30AF,0x30B1,0x30B3,0x30B5,0x30B7,0x30B9,0x30BB,0x30BD,0x30BF,0x30C1,0x30C4,0x30C6,0x30C8,
    0x30CA,0x30CB,0x30CC,0x30CD,0x30CE,0x30CF,0x30D2,0x30D5,0x30D8,0x30DB,0x30DE,0x30DF,0x30E0,0x30E1,0x30E2,0x30E4,0x30E6,0x30E8,0x30E9,0x30EA,
    0x30EB,0x30EC,0x30ED,0x30EF,0x30F3,0x309B,0x309C};
// cp932 單位元組 → 全形雙位元組（_mbbtombc）；0 表示沒有對應。ASCII 引號沿用 JIS X 0208 第一區的右引號。
std::uint32_t japanese_fullwidth(unsigned c){
    char16_t wide=0;
    if(c==0x20)wide=0x3000;else if(c==0x22)return 0x8168;else if(c==0x27)return 0x8166;
    else if(c>=0x21 && c<=0x7e)wide=static_cast<char16_t>(0xff01+(c-0x21));
    else if(c>=0xa1 && c<=0xdf)wide=fullwidth_kana[c-0xa1];
    else return 0;
    const auto encoded=windows_crt_encode(std::u16string(1,wide),932);
    return encoded && encoded->size()==2?(unsigned((*encoded)[0])<<8)|(*encoded)[1]:0;
}
const std::array<std::uint32_t,256>& japanese_fullwidth_table(){
    static const auto table=[]{std::array<std::uint32_t,256> out{};for(unsigned c=0;c<256;++c)out[c]=japanese_fullwidth(c);return out;}();
    return table;
}
// LC_CTYPE 控制嚴格解碼。明定大小時驗證整段輸入，字元數只計至第一個 NUL。
std::optional<std::uint64_t> checked_multibyte_length(const GuestMemory& memory,std::uint64_t text,
        std::uint64_t limit,bool bounded,const CrtLocaleCategory& category){
    if(category.maximum_character_bytes==1)return crt_bounded_length(memory,text,limit,1);
    if(!limit)return std::nullopt; // 多位元組轉換不接受空的明定輸入範圍。
    crt_multibyte::State state{};
    std::uint64_t count=0;
    bool terminated=false;
    for(std::uint64_t offset=0;offset<limit;){
        const auto bytes=memory.readable_prefix(GuestAddress{text+offset},std::min<std::uint64_t>(4096,limit-offset));
        for(std::size_t i=0;i<bytes.size();){
            const auto byte=bytes[i];
            if(byte<128 && crt_multibyte::initial(state)){
                ++i;
                if(!byte){
                    if(!bounded)return count;
                    terminated=true;
                }
            }else{
                const auto decoded=crt_multibyte::decode(state,bytes.subspan(i),category.codepage);
                if(decoded.status==CrtDecodedCharacter::Status::Invalid)return std::nullopt;
                i+=decoded.consumed;
                if(decoded.status==CrtDecodedCharacter::Status::Partial)continue;
            }
            if(!terminated)++count;
        }
        offset+=bytes.size();
    }
    if(!crt_multibyte::initial(state))return std::nullopt;
    return terminated?count:limit;
}
std::optional<std::uint32_t> invoke_crtdll_text(
        GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto finish=[&](std::uint32_t value){
        call.return_to(state,value);
        return std::optional<std::uint32_t>{};
    };
    const auto read=[&](std::uint32_t address){return memory.load(GuestAddress{address},8);};
    const auto first=static_cast<std::uint32_t>(call(0));
    if(name=="_strinc")return finish(first+1);
    if(name=="_strdec")return finish(static_cast<std::uint32_t>(call(1))-1);
    if(name=="_strnextc")return finish(static_cast<std::uint32_t>(read(first)));
    const auto second=static_cast<std::uint32_t>(call(1));
    if(name=="_strninc")return finish(first+second);
    if(name=="_strncnt"){
        std::uint32_t count=0;
        while(count<second && read(first+count))++count;
        return finish(count);
    }
    if(name=="_strspnp"){
        for(auto current=first;const auto value=read(current);++current){
            auto candidate=second;
            auto accepted=read(candidate);
            while(accepted && accepted!=value)accepted=read(++candidate);
            if(!accepted)return finish(current);
        }
        return finish(0);
    }
    throw HostCallFault("CRTDLL 窄字元服務未登錄");
}
}
std::span<const WindowsServiceDescriptor> crt_mbcs_service_descriptors(){
    static const auto table=[]{
        std::vector<WindowsServiceDescriptor> out;
        // {名稱, 參數數, 是否有 _l 版本}
        static constexpr std::tuple<std::string_view,std::string_view,unsigned> entries[]{
            {"_ismbcblank","_ismbcblank_l",1},{"_ismbcalnum","_ismbcalnum_l",1},{"_ismbcalpha","_ismbcalpha_l",1},{"_ismbcdigit","_ismbcdigit_l",1},
            {"_ismbcgraph","_ismbcgraph_l",1},{"_ismbchira","_ismbchira_l",1},{"_ismbckata","_ismbckata_l",1},{"_ismbcl0","_ismbcl0_l",1},
            {"_ismbcl1","_ismbcl1_l",1},{"_ismbcl2","_ismbcl2_l",1},{"_ismbclegal","_ismbclegal_l",1},{"_ismbclower","_ismbclower_l",1},
            {"_ismbcprint","_ismbcprint_l",1},{"_ismbcpunct","_ismbcpunct_l",1},{"_ismbcspace","_ismbcspace_l",1},{"_ismbcsymbol","_ismbcsymbol_l",1},
            {"_ismbcupper","_ismbcupper_l",1},{"_ismbslead","_ismbslead_l",2},{"_ismbstrail","_ismbstrail_l",2},
            {"_mbscmp","_mbscmp_l",2},{"_mbsnbcmp","_mbsnbcmp_l",3},{"_mbscoll","_mbscoll_l",2},{"_mbsicoll","_mbsicoll_l",2},
            {"_mbsnbcoll","_mbsnbcoll_l",3},{"_mbsnbicoll","_mbsnbicoll_l",3},
            {"_mbsncoll","_mbsncoll_l",3},{"_mbsnicoll","_mbsnicoll_l",3},
            {"_mbbtombc","_mbbtombc_l",1},{"_mbbtype","_mbbtype_l",2},{"_mbccpy","_mbccpy_l",2},{"_mbccpy_s","_mbccpy_s_l",4},
            {"_mbcjistojms","_mbcjistojms_l",1},{"_mbcjmstojis","_mbcjmstojis_l",1},{"_mbclen","_mbclen_l",1},{"_mbctohira","_mbctohira_l",1},
            {"_mbctokata","_mbctokata_l",1},{"_mbctolower","_mbctolower_l",1},{"_mbctombb","_mbctombb_l",1},{"_mbctoupper","_mbctoupper_l",1},
            {"_mbsbtype","_mbsbtype_l",2},{"_mbsdec","_mbsdec_l",2},{"_mbsicmp","_mbsicmp_l",2},{"_mbslwr","_mbslwr_l",1},{"_mbslwr_s","_mbslwr_s_l",2},
            {"_mbsnbcat_s","_mbsnbcat_s_l",4},{"_mbsnbcpy_s","_mbsnbcpy_s_l",4},{"_mbsnbicmp","_mbsnbicmp_l",3},{"_mbsnbset","_mbsnbset_l",3},
            {"_mbsncat","_mbsncat_l",3},{"_mbsncmp","_mbsncmp_l",3},{"_mbsncpy","_mbsncpy_l",3},{"_mbsncpy_s","_mbsncpy_s_l",4},
            {"_mbsnicmp","_mbsnicmp_l",3},{"_mbsninc","_mbsninc_l",2},{"_mbsnlen","_mbsnlen_l",2},{"_mbstrlen","_mbstrlen_l",1},{"_mbstrnlen","_mbstrnlen_l",2},{"_mbsnset","_mbsnset_l",3},{"_mbsrev","_mbsrev_l",1},
            {"_mbsset","_mbsset_l",2},{"_mbsstr","_mbsstr_l",2},{"_mbstok","_mbstok_l",2},{"_mbstok_s","_mbstok_s_l",3},{"_mbsupr","_mbsupr_l",1},{"_mbsupr_s","_mbsupr_s_l",2},
        };
#define CRTDLL_TEXT(name,count) out.push_back({"crtdll.dll",#name,count,0,0,0,true});
#include "crtdll_text_services.inc"
#undef CRTDLL_TEXT
        out.push_back({"crt","__p__mbctype",0});
        out.push_back({"crt","__p__mbcasemap",0});
#define CRT_MBC_BYTE(name,ctype,type) out.push_back({"crt",#name,1});out.push_back({"crt",#name "_l",2});
#include "crt_mbc_byte_operations.inc"
#undef CRT_MBC_BYTE
        for (const auto module : secure_crt_libraries) {
            out.push_back({module,"_mbsncat_s",4,0,0,0,true});
            out.push_back({module,"_mbsncat_s_l",5,0,0,0,true});
            for (const auto& entry : crt_secure_set::entries)
                out.push_back({module,entry.name,entry.arguments,0,0,0,true});
        }
        for(const auto& [plain,localized,count]:entries){out.push_back({"crt",plain,count});if(!localized.empty())out.push_back({"crt",localized,count+1});}
        return out;
    }();
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_mbcs(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.family=="crtdll.dll")return invoke_crtdll_text(state,memory,service);
    const WindowsCall arg(state,memory,service);auto name=service.symbol;
    const bool localized=name.ends_with("_l");if(localized)name.remove_suffix(2);
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto set_errno=[&](std::uint32_t code){memory.store(GuestAddress{runtime_.errno_cell},32,code);};
    const auto invalid=[&](std::uint64_t result,std::uint32_t code=einval)->std::optional<std::uint32_t>{
        set_errno(code);
        if(const auto handler=current_invalid_parameter_handler()){start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
        return done(result);
    };
    const auto* fill = crt_secure_set::lookup(service.symbol);
    const bool append = name == "_mbsncat_s" || name == "_mbsnbcat_s";
    if (append || fill) {
        const auto text = arg(0), size = arg(1);
        std::array<std::uint8_t,256> types{};
        const std::uint8_t* lead_table = nullptr;
        if ((append || fill->multibyte) && text && size && (!append || arg(2) || !arg(3))) {
            const auto locale = localized ? arg(service.arguments-1) : 0;
            const auto mbc = locale ? memory.load(GuestAddress{locale+unit},bits)
                                    : memory.load(GuestAddress{threads_.items.at(threads_.current).teb+
                                        (x86_?CRT_MBC_TEB32_VIEW:CRT_MBC_TEB64_VIEW)},bits);
            if (memory.load(GuestAddress{mbc+8},32)) {
                memory.read(GuestAddress{mbc+CRT_MBC_TYPES_OFFSET+1},types);
                lead_table = types.data();
            }
        }
        const auto write = [](void* mem,CrtMbcAddress address,std::uint8_t value) {
            static_cast<GuestMemory*>(mem)->store(GuestAddress{address},8,value);
        };
        CrtAppendResult result{};
        if (append) {
            const auto count = arg(3);
            result = crt_secure_append(&memory,crt_mbc_guest_byte,write,lead_table,text,size,arg(2),count,
                                       name == "_mbsnbcat_s",count == (x86_ ? UINT32_MAX : UINT64_MAX));
        } else {
            const auto count = fill->limit == CrtFillAll ? 0 : arg(3);
            const auto code = static_cast<unsigned>(crt_secure_fill(&memory,crt_mbc_guest_byte,write,lead_table,
                                                    text,size,static_cast<unsigned>(arg(2)),fill->limit,count));
            result = {code,code};
        }
        if (result.error == einval || result.error == erange) {
            if (current_invalid_parameter_handler()) return invalid(result.status,result.error);
            set_errno(result.error);
            report_crt_fast_fail(state,memory,service.symbol);
            return 0xc0000409U;
        }
        if (result.error) set_errno(result.error);
        return done(result.status);
    }
    if(name=="_mbsninc" && (!arg(0) || !arg(1)))return done(arg(0));
    const bool ordinal_compare=name=="_mbscmp" || name=="_mbsncmp" || name=="_mbsnbcmp";
    if(ordinal_compare){
        if(name!="_mbscmp" && !arg(2))return done(0);
        if(!arg(0) || !arg(1)){
            if(current_invalid_parameter_handler())return invalid(nlscmperror);
            set_errno(einval);report_crt_fast_fail(state,memory,service.symbol);return 0xc0000409U;
        }
    }
    const bool checked_length=name=="_mbstrnlen" || name=="_mbstrlen";
    const auto size_error=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;
    const auto length_invalid=[&]()->std::optional<std::uint32_t>{
        if(current_invalid_parameter_handler())return invalid(size_error);
        set_errno(einval);report_crt_fast_fail(state,memory,service.symbol);return 0xc0000409U;
    };
    if(checked_length && (!arg(0) || (name=="_mbstrnlen" && arg(1)>INT32_MAX)))return length_invalid();
    const bool collation=name.starts_with("_mbs") && name.ends_with("coll");
    const CrtLocaleObject* chosen=&current_crt_locale(state,memory);
    if(localized && arg(service.arguments-1)){
        const auto found=runtime_.crt_locales.find(arg(service.arguments-1));if(found==runtime_.crt_locales.end())return checked_length?length_invalid():invalid((collation || ordinal_compare)?nlscmperror:0);chosen=&found->second;
    }
    if(checked_length){
        const bool bounded=name=="_mbstrnlen";
        const auto length=checked_multibyte_length(memory,arg(0),bounded?arg(1):UINT64_MAX,bounded,chosen->categories[2]);
        if(!length){set_errno(eilseq);return done(size_error);}
        return done(*length);
    }
    const auto mbc=crt_mbc_address(memory,*chosen,x86_);
    if(name=="__p__mbctype")return done(mbc+CRT_MBC_TYPES_OFFSET);
    if(name=="__p__mbcasemap")return done(mbc+CRT_MBC_CASE_OFFSET);
    const auto codepage=static_cast<std::uint32_t>(memory.load(GuestAddress{mbc+4},32));const bool multibyte=memory.load(GuestAddress{mbc+8},32)!=0;
    std::array<std::uint8_t,256> types{};
    memory.read(GuestAddress{mbc+CRT_MBC_TYPES_OFFSET+1},types);
    if(ordinal_compare){
        const auto count=name=="_mbscmp"?UINT64_MAX:arg(2);
        return done(static_cast<std::uint32_t>(crt_mbc_compare(&memory,crt_mbc_guest_byte,
            multibyte?types.data():nullptr,arg(0),arg(1),count,name=="_mbsnbcmp")));
    }
    const auto& ctype=windows_crt_character_table(chosen->categories[2]);
    const auto test_byte=[&](unsigned value,const ByteClass& classification){
        return value<256 && crt_mbc_test_byte(value,ctype.masks[value+1],types[value],classification.ctype,classification.type);
    };
    if(const auto found=std::ranges::find(byte_classes,name,&ByteClass::name);found!=std::end(byte_classes))
        return done(test_byte(static_cast<std::uint32_t>(arg(0)),*found));
    const auto page=windows_crt_codepage(codepage);if(!page)return invalid(0);
    const auto& table=windows_crt_character_table(*page);const bool japanese=multibyte && codepage==932;
    const auto lead=[&](unsigned b){return multibyte && (types[b&0xffU]&4U);};
    const auto trail=[&](unsigned b){return multibyte && (types[b&0xffU]&8U);};
    const auto byte=[&](std::uint64_t at){return static_cast<unsigned>(memory.load(GuestAddress{at},8));};
    if(name=="_mbsninc"){
        const auto bytes=crt_mbc_character_bytes(&memory,crt_mbc_guest_byte,multibyte?types.data():nullptr,arg(0),arg(1));
        return done(arg(0)+static_cast<std::uint32_t>(bytes));
    }
    if(name=="_mbsstr"){
        const auto text=arg(0),pattern=arg(1);
        if(pattern && !byte(pattern))return done(text);
        if(!text || !pattern){
            if(current_invalid_parameter_handler())return invalid(0);
            set_errno(einval);report_crt_fast_fail(state,memory,service.symbol);return 0xc0000409U;
        }
        return done(crt_mbc_find_string(&memory,crt_mbc_guest_byte,multibyte?types.data():nullptr,text,pattern));
    }
    if(name=="_mbsnlen"){
        const auto text=arg(0),limit=arg(1);
        if(!multibyte)return done(crt_bounded_length(memory,text,limit,1));
        std::uint64_t count=0,offset=0;
        while(offset<limit){
            const auto first=byte(text+offset++);
            if(!first)return done(count);
            if(lead(first)){
                if(offset==limit)return done(limit);
                if(!byte(text+offset++))return done(count);
            }
            ++count;
        }
        return done(limit);
    }
    const auto put=[&](std::uint64_t at,unsigned value){memory.store(GuestAddress{at},8,value&0xffU);};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    // 字串中位於 at 的字元：{值, 位元組數}；前導位元組後接 NUL 視為字串結束。
    const auto character=[&](std::uint64_t at)->std::pair<unsigned,unsigned>{
        const auto first=byte(at);if(!first)return {0,1};
        if(lead(first)){const auto second=byte(at+1);if(!second)return {0,1};return {(first<<8)|second,2};}
        return {first,1};
    };
    const auto decode=[&](unsigned c)->std::optional<std::uint32_t>{
        if(c<256 || !lead(c>>8) || !trail(c&0xffU))return {};
        const std::array<std::uint8_t,2> pair{static_cast<std::uint8_t>(c>>8),static_cast<std::uint8_t>(c)};
        const auto decoded=windows_crt_decode(pair,codepage);
        if(decoded.status!=CrtDecodedCharacter::Status::Complete || decoded.bytes!=2)return {};return decoded.character;
    };
    const auto encode_double=[&](std::uint32_t value)->std::optional<unsigned>{
        if(value>0xffff)return {};const auto encoded=windows_crt_encode(std::u16string(1,static_cast<char16_t>(value)),codepage);
        if(!encoded || encoded->size()!=2)return {};return (unsigned((*encoded)[0])<<8)|(*encoded)[1];
    };
    const auto convert_case=[&](unsigned c,bool upper)->unsigned{
        if(c<256)return upper?table.upper[c]:table.lower[c];
        const auto value=decode(c);if(!value)return c;
        const auto mapped=static_cast<std::uint32_t>(upper?u_toupper(static_cast<UChar32>(*value)):u_tolower(static_cast<UChar32>(*value)));
        if(mapped==*value)return c;return encode_double(mapped).value_or(c);
    };
    // CRT 分類位元：單位元組取碼頁字元表，雙位元組解碼後與寬字元分類共用 Unicode 規則。
    const auto classify=[&](unsigned c,unsigned mask)->bool{
        if(c<256)return (table.masks[c+1]&mask)!=0;
        const auto value=decode(c);if(!value || *value>0xffff)return false;
        return (windows_crt_unicode_mask(static_cast<char16_t>(*value))&mask)!=0;
    };
    // 前 limit 個位元組中不會切斷雙位元組字元的長度（遇 NUL 停止）。
    const auto whole=[&](std::uint64_t text,std::uint64_t limit){
        std::uint64_t at=0;
        while(at<limit && byte(text+at)){if(lead(byte(text+at))){if(at+1<limit && byte(text+at+1))at+=2;else break;}else ++at;}
        return at;
    };
    // 前 count 個字元占用的位元組數（複製與填充作業）。
    const auto char_bytes=[&](std::uint64_t text,std::uint64_t count){
        std::uint64_t at=0;for(;count && byte(text+at);--count){if(lead(byte(text+at))){if(!byte(text+at+1)){++at;break;}at+=2;}else ++at;}return at;
    };
    const auto length=[&](std::uint64_t text,std::uint64_t limit=UINT64_MAX){std::uint64_t n=0;while(n<limit && byte(text+n))++n;return n;};
    try{
        // ---- 字元分類 ----
        if(name.starts_with("_ismbc")){
            const auto c=u32(0);
            if(c<256){
                const auto found=std::ranges::find_if(byte_classes,[&](const auto& operation){
                    return operation.name.substr(6)==name.substr(6);
                });
                if(found!=std::end(byte_classes))return done(test_byte(c,*found));
            }
            if(name=="_ismbcblank")return done(c<256?test_byte(c,{"",0x40,0}):classify(c,0x40));
            if(name=="_ismbclegal")return done(c>255 && lead(c>>8) && trail(c&0xffU));
            if(name=="_ismbchira")return done(japanese && c>=0x829f && c<=0x82f1);
            if(name=="_ismbckata")return done(japanese && c>=0x8340 && c<=0x8396 && c!=0x837f);
            if(name=="_ismbcsymbol")return done(japanese && c>=0x8141 && c<=0x81ac && c!=0x817f);
            const bool legal=c>255 && lead(c>>8) && trail(c&0xffU);
            if(name=="_ismbcl0")return done(japanese && legal && c>=0x8140 && c<=0x889e);
            if(name=="_ismbcl1")return done(japanese && legal && c>=0x889f && c<=0x9872);
            if(name=="_ismbcl2")return done(japanese && legal && c>=0x989f && c<=0xea9e);
            if(c>255 && !multibyte)return done(0);
            if(name=="_ismbcprint" && c>255)return done(classify(c,0x117) || classify(c,8));
            constexpr std::pair<std::string_view,unsigned> masks[]{{"_ismbcalnum",0x107},{"_ismbcalpha",0x103},{"_ismbcdigit",4},{"_ismbcgraph",0x117},
                {"_ismbclower",2},{"_ismbcprint",0x157},{"_ismbcpunct",0x10},{"_ismbcspace",8},{"_ismbcupper",1}};
            for(const auto& [label,mask]:masks)if(name==label)return done(classify(c,mask));
        }
        if(name=="_ismbslead" || name=="_ismbstrail"){
            // 從字串開頭走到 current，判斷該位置是前導或後隨位元組；是則回 -1。
            const auto text=arg(0),current=arg(1);if(!text || !current)return invalid(0);
            std::uint64_t at=text;
            while(at<current){const auto b=byte(at);if(!b)return done(0);if(lead(b)){if(at+1==current)return done(name=="_ismbstrail"?UINT32_MAX:0U);at+=2;}else ++at;}
            return done(at==current && name=="_ismbslead" && lead(byte(current))?UINT32_MAX:0U);
        }
        if(name=="_mbbtype"){
            // 依前一位元組的型別：前一個是前導（1）時檢查後隨（2）或非法（-1）；否則為前導（1）或單位元組（0）。
            const auto c=u32(0)&0xffU;
            if(static_cast<std::int32_t>(u32(1))==1)return done(trail(c)?2U:UINT32_MAX);
            return done(lead(c)?1U:0U);
        }
        if(name=="_mbsbtype"){
            const auto text=arg(0),count=arg(1);if(!text)return invalid(UINT32_MAX);
            std::uint32_t type=0;
            // 走到 count 之前遇到 NUL 或非法的後隨位元組即回 _MBC_ILLEGAL（-1）。
            for(std::uint64_t i=0;i<=count;++i){const auto b=byte(text+i);if(!b)return done(UINT32_MAX);type=type==1?(trail(b)?2U:UINT32_MAX):lead(b)?1U:0U;if(type==UINT32_MAX)return done(UINT32_MAX);}
            return done(type);
        }
        // ---- 字元轉換 ----
        if(name=="_mbbtombc"){const auto c=u32(0);if(!japanese || c>255)return done(c);const auto mapped=japanese_fullwidth_table()[c];return done(mapped?mapped:c);}
        if(name=="_mbctombb"){
            const auto c=u32(0);if(!japanese || c<256)return done(c);
            const auto& forward=japanese_fullwidth_table();for(unsigned b=0;b<256;++b)if(forward[b]==c)return done(b);
            return done(c);
        }
        if(name=="_mbcjistojms"){
            // JIS X 0208（0x2121～0x7E7E）→ Shift_JIS；非碼頁 932 傳回原值，範圍外傳回 0。
            const auto c=u32(0);if(!japanese)return done(c);
            auto high=c>>8,low=c&0xffU;if(high<0x21 || high>0x7e || low<0x21 || low>0x7e)return done(0);
            if(high&1U)low+=low>=0x60?0x20U:0x1fU;else low+=0x7e;
            high=((high+1)>>1)+(high<=0x5e?0x70U:0xb0U);return done((high<<8)|low);
        }
        if(name=="_mbcjmstojis"){
            const auto c=u32(0);if(!japanese)return done(c);
            auto high=c>>8,low=c&0xffU;if(!lead(high) || !trail(low))return done(0);
            high-=high<=0x9f?0x70U:0xb0U;high<<=1;
            if(low<0x9f){--high;low-=low>0x7f?0x20U:0x1fU;}else low-=0x7e;
            if(high<0x21 || high>0x7e || low<0x21 || low>0x7e)return done(0);
            return done((high<<8)|low);
        }
        if(name=="_mbctohira"){const auto c=u32(0);if(!japanese)return done(c);if(c>=0x8340 && c<=0x837e)return done(c-0xa1);if(c>=0x8380 && c<=0x8393)return done(c-0xa2);return done(c);}
        if(name=="_mbctokata"){const auto c=u32(0);if(!japanese)return done(c);if(c>=0x829f && c<=0x82dd)return done(c+0xa1);if(c>=0x82de && c<=0x82f1)return done(c+0xa2);return done(c);}
        if(name=="_mbctolower" || name=="_mbctoupper")return done(convert_case(u32(0),name=="_mbctoupper"));
        if(name=="_mbclen"){if(!arg(0))return invalid(0);return done(lead(byte(arg(0)))?2U:1U);}
        if(name=="_mbccpy"){
            if(!arg(0) || !arg(1))return invalid(0);
            put(arg(0),byte(arg(1)));if(lead(byte(arg(1))))put(arg(0)+1,byte(arg(1)+1));return done(0);
        }
        if(name=="_mbccpy_s"){
            // (dest, size, int* copied, src)：前導位元組後接 NUL 為 EILSEQ；放不下雙位元組字元為 ERANGE。
            const auto target=arg(0),size=arg(1),copied=arg(2),source=arg(3);
            if(copied)memory.store(GuestAddress{copied},32,0);
            if(!target || !size){return invalid(einval);}
            if(!source){put(target,0);return invalid(einval);}
            const auto first=byte(source);
            if(lead(first)){
                const auto second=byte(source+1);
                if(!second){put(target,0);set_errno(eilseq);return done(eilseq);}
                if(size<2){put(target,0);return invalid(erange,erange);}
                put(target,first);put(target+1,second);if(copied)memory.store(GuestAddress{copied},32,2);return done(0);
            }
            put(target,first);if(copied)memory.store(GuestAddress{copied},32,1);return done(0);
        }
        // ---- 指標移動與長度 ----
        if(name=="_mbsdec"){
            const auto start=arg(0),current=arg(1);if(!start || !current)return invalid(0);if(current<=start)return done(0);
            const auto last=current-1;
            auto cursor=last;
            while(cursor>start && lead(byte(cursor-1)))--cursor;
            return done(last-((last-cursor)&1));
        }
        // ---- 比較 ----
        if(collation){
            if(!arg(0) || !arg(1))return invalid(nlscmperror);
            const bool bytes=name.starts_with("_mbsnb");
            const bool bounded=bytes || name.starts_with("_mbsn");
            const bool fold=name.ends_with("icoll");
            const auto count=bounded?arg(2):UINT64_MAX;
            const auto& order_locale=chosen->categories[1];
            const bool classic=!multibyte && !order_locale.codepage;
            constexpr std::uint64_t max_input=1048576;
            const auto read=[&](std::uint64_t source)->std::optional<std::string>{
                std::string output;
                for(std::uint64_t offset=0,characters=0;offset<max_input;){
                    if((bytes?offset:characters)==count)return output;
                    const auto first=byte(source+offset);
                    if(!first)return output;
                    if(!lead(first)){
                        output.push_back(static_cast<char>(first));++offset;
                    }else{
                        if(bytes && count-offset<2)return output;
                        if(offset+1>=max_input)return {};
                        const auto second=byte(source+offset+1);
                        if(!second)return output;
                        output.push_back(static_cast<char>(first));
                        output.push_back(static_cast<char>(second));
                        offset+=2;
                    }
                    ++characters;
                    if((bytes?offset:characters)==count)return output;
                }
                return {};
            };
            const auto first=read(arg(0)),second=read(arg(1));
            if(!first || !second)return invalid(nlscmperror);
            if(classic){
                const auto key=[&](char value){
                    const auto c=static_cast<unsigned char>(value);
                    return fold && c>='A' && c<='Z'?static_cast<unsigned>(c)+32U:c;
                };
                const auto pair=std::mismatch(first->begin(),first->end(),second->begin(),second->end(),
                    [&](auto left,auto right){return key(left)==key(right);});
                const auto left=pair.first==first->end()?0U:key(*pair.first);
                const auto right=pair.second==second->end()?0U:key(*pair.second);
                return done(left<right?UINT32_MAX:left>right?1U:0U);
            }
            const auto collation_page=multibyte?codepage:order_locale.codepage;
            const auto mbc_locale=static_cast<std::uint32_t>(memory.load(GuestAddress{mbc+12},32));
            const auto locale=multibyte?(mbc_locale?mbc_locale:windows_crt_collation_lcid(collation_page)):order_locale.lcid;
            const auto left=windows_nls_decode(*first,collation_page,true),right=windows_nls_decode(*second,collation_page,true);
            if(!left || !right)return invalid(nlscmperror);
            const auto result=windows_nls_compare(*left,*right,locale,fold?1U:0U);
            return result?done(static_cast<std::uint32_t>(*result)):invalid(nlscmperror);
        }
        if(name=="_mbsicmp" || name=="_mbsnbicmp" || name=="_mbsnicmp"){
            const bool bytes=name=="_mbsnbicmp",bounded=name!="_mbsicmp";
            if(bounded && !arg(2))return done(0);
            if(!arg(0) || !arg(1))return invalid(nlscmperror);
            auto first=arg(0),second=arg(1);std::uint64_t left_first=bounded?arg(2):UINT64_MAX,left_second=left_first;
            // 位元組上限：前導位元組的後隨位元組超出上限時視為字串結束。
            const auto next=[&](std::uint64_t& at,std::uint64_t& left)->unsigned{
                auto value=byte(at);
                if(lead(value)){
                    if(bytes && left<2){value=0;left=0;return 0;}
                    const auto trailing=byte(at+1);value=trailing?(value<<8)|trailing:0;at+=trailing?2:1;left-=bytes?2:1;
                }else{++at;left-=1;}
                return convert_case(value,false);
            };
            while(left_first && left_second){
                const auto a=next(first,left_first),b=next(second,left_second);
                if(a!=b)return done(a<b?UINT32_MAX:1U);
                if(!a)break;
            }
            return done(0);
        }
        // ---- 複製與串接 ----
        if(name=="_mbsncpy" || name=="_mbsncat"){
            // 以字元數計算；_mbsncpy 不足 count 個字元時以 NUL 補足剩下的字元數，_mbsncat 一定以 NUL 結尾。
            const auto target=arg(0),source=arg(1);auto count=arg(2);if(!count)return done(target);if(!target || !source)return invalid(0);
            auto to=target;
            if(name=="_mbsncat"){
                to+=length(target);
                if(multibyte && to>target){std::uint64_t at=target,last=target;while(at<to){last=at;at+=lead(byte(at)) && at+1<to?2:1;}if(lead(byte(last)) && last+1==to)to=last;}
            }
            auto from=source;
            for(;count;--count){
                const auto b=byte(from);
                if(!b)break;
                if(lead(b)){if(!byte(from+1)){break;}put(to,b);put(to+1,byte(from+1));to+=2;from+=2;}
                else {put(to,b);++to;++from;}
            }
            if(name=="_mbsncat")put(to,0);else for(;count;--count,++to)put(to,0);
            return done(target);
        }
        if(name=="_mbsnbcpy_s" || name=="_mbsncpy_s"){
            // (dest, size, src, count)：count＝_TRUNCATE 時截斷並回 STRUNCATE；不切斷雙位元組字元。
            const auto target=arg(0),size=arg(1),source=arg(2),count=arg(3);
            const bool truncate=count==(x86_?UINT32_MAX:UINT64_MAX);
            if(!count && !target && !size)return done(0);
            if(!target || !size)return invalid(einval);
            if(!source){put(target,0);return invalid(einval);}
            const auto wanted=name=="_mbsncpy_s"?char_bytes(source,truncate?UINT64_MAX:count):std::min<std::uint64_t>(truncate?UINT64_MAX:count,length(source));
            auto take=whole(source,std::min<std::uint64_t>(wanted,truncate?size-1:wanted));
            if(!truncate && take+1>size){put(target,0);return invalid(erange,erange);}
            for(std::uint64_t i=0;i<take;++i)put(target+i,byte(source+i));put(target+take,0);
            return done(truncate && take<whole(source,wanted)?struncate:0U);
        }
        // ---- 填充與反轉 ----
        if(name=="_mbsnbset" || name=="_mbsnset" || name=="_mbsset"){
            // 以 c 填入指定長度；雙位元組 c 放不下最後一個位元組時改填空白。
            const auto text=arg(0);if(!text)return invalid(0);const auto c=u32(1);const bool pair=multibyte && c>255;
            const auto total=length(text);
            const auto span=name=="_mbsset"?total:name=="_mbsnbset"?std::min<std::uint64_t>(arg(2),total):char_bytes(text,arg(2));
            for(std::uint64_t at=0;at<span;){if(pair){if(at+1<span){put(text+at,c>>8);put(text+at+1,c);at+=2;}else{put(text+at,' ');++at;}}else{put(text+at,c);++at;}}
            return done(text);
        }
        if(name=="_mbsrev"){
            const auto text=arg(0);if(!text)return invalid(0);
            std::vector<std::vector<std::uint8_t>> characters;
            for(std::uint64_t at=text;byte(at);){const auto b=byte(at);if(lead(b) && byte(at+1)){characters.push_back({static_cast<std::uint8_t>(b),static_cast<std::uint8_t>(byte(at+1))});at+=2;}else{characters.push_back({static_cast<std::uint8_t>(b)});++at;}}
            auto at=text;for(auto it=characters.rbegin();it!=characters.rend();++it)for(const auto b:*it)put(at++,b);
            return done(text);
        }
        // ---- 分詞 ----
        if(name=="_mbstok" || name=="_mbstok_s"){
            const bool secure=name=="_mbstok_s";const auto delimiters=arg(1),context=secure?arg(2):0;
            if(!delimiters || (secure && !context))return invalid(0);
            auto& thread_cursor=threads_.items.at(threads_.current).mbstok_cursor;
            auto cursor=arg(0)?arg(0):secure?memory.load(GuestAddress{context},bits):thread_cursor;
            if(!cursor){if(secure)return invalid(0);return done(0);}
            std::set<unsigned> set;for(std::uint64_t at=delimiters;;){const auto [value,size]=character(at);if(!value)break;set.insert(value);at+=size;}
            const auto store=[&](std::uint64_t value){if(secure)memory.store(GuestAddress{context},bits,value);else thread_cursor=value;};
            auto at=cursor;
            for(;;){const auto [value,size]=character(at);if(!value || !set.contains(value))break;at+=size;}
            if(!character(at).first){store(at);return done(0);}
            const auto token=at;
            for(;;){const auto [value,size]=character(at);if(!value){store(at);break;}if(set.contains(value)){put(at,0);store(at+size);break;}at+=size;}
            return done(token);
        }
        // ---- 大小寫 ----
        if(name=="_mbslwr" || name=="_mbsupr" || name=="_mbslwr_s" || name=="_mbsupr_s"){
            const bool upper=name.starts_with("_mbsupr"),secure=name.ends_with("_s");const auto text=arg(0);
            if(!text){if(secure)return invalid(einval);return done(0);}
            if(secure){const auto size=arg(1);if(!size)return invalid(einval);if(length(text,size)==size){put(text,0);return invalid(einval);}}
            for(std::uint64_t at=text;;){
                const auto [value,size]=character(at);if(!value)break;
                const auto mapped=convert_case(value,upper);
                if(size==2){if(mapped>255){put(at,mapped>>8);put(at+1,mapped);}}else put(at,mapped);
                at+=size;
            }
            return done(secure?0:text);
        }
    }catch(const MemoryFault&){return invalid(0);}
    throw HostCallFault("未知的 CRT 多位元組服務");
}
}
