#include "x64arm/windows_keyboard.hpp"
#include "x64arm/input.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <unicode/ucnv.h>

namespace x64arm {
// 虛擬鍵盤固定為 en-US；scan code 採 PC Set 1，與 GetKeyboardLayout 的 0409 一致。
std::uint32_t windows_map_virtual_key(std::uint32_t code,std::uint32_t type){
    static const auto keys=[] {
        std::array<std::uint16_t,128> table{};
        const auto row=[&](unsigned start,std::u16string_view values){for(const auto value:values)table[start++]=value;};
        row(2,u"1234567890");row(0x10,u"QWERTYUIOP");row(0x1e,u"ASDFGHJKL");row(0x2c,u"ZXCVBNM");
        const std::array<std::pair<unsigned,unsigned>,41> values{{{1,27},{12,0xbd},{13,0xbb},{14,8},{15,9},{26,0xdb},{27,0xdd},{28,13},{29,0xa2},{39,0xba},{40,0xde},{41,0xc0},{42,0xa0},{43,0xdc},{51,0xbc},{52,0xbe},{53,0xbf},{54,0xa1},{55,0x6a},{56,0xa4},{57,32},{58,20},{69,0x90},{70,0x91},{71,0x24},{72,0x26},{73,0x21},{74,0x6d},{75,0x25},{76,0x0c},{77,0x27},{78,0x6b},{79,0x23},{80,0x28},{81,0x22},{82,0x2d},{83,0x2e},{84,0x2c},{86,0xe2},{87,0x7a},{88,0x7b}}};
        for(const auto& [scan,vk]:values)table[scan]=static_cast<std::uint16_t>(vk);for(unsigned i=0;i<10;++i)table[59+i]=static_cast<std::uint16_t>(0x70+i);return table;
    }();
    static constexpr std::array<std::pair<std::uint32_t,std::uint32_t>,20> extended{{{0xe01c,13},{0xe01d,0xa3},{0xe035,0x6f},{0xe037,0x2c},{0xe038,0xa5},{0xe047,0x24},{0xe048,0x26},{0xe049,0x21},{0xe04b,0x25},{0xe04d,0x27},{0xe04f,0x23},{0xe050,0x28},{0xe051,0x22},{0xe052,0x2d},{0xe053,0x2e},{0xe05b,0x5b},{0xe05c,0x5c},{0xe05d,0x5d},{0xe046,3},{0xe11d,0x13}}};
    if(type==2){if((code>='A' && code<='Z') || (code>='0' && code<='9') || code==8 || code==9 || code==13 || code==27 || code==32)return code;if(code>=0x60 && code<=0x69)return code-0x60+'0';
        switch(code){case 0x6a:return '*';case 0x6b:return '+';case 0x6d:return '-';case 0x6e:return '.';case 0x6f:return '/';case 0xba:return ';';case 0xbb:return '=';case 0xbc:return ',';case 0xbd:return '-';case 0xbe:return '.';case 0xbf:return '/';case 0xc0:return '`';case 0xdb:return '[';case 0xdc:case 0xe2:return '\\';case 0xdd:return ']';case 0xde:return '\'';default:return 0;}}
    if(type==1 || type==3){std::uint32_t vk=code<keys.size()?keys[code]:0;for(const auto& [scan,value]:extended)if(scan==code)vk=value;
        if(type==1){if(vk==0xa0 || vk==0xa1)vk=16;if(vk==0xa2 || vk==0xa3)vk=17;if(vk==0xa4 || vk==0xa5)vk=18;}return vk;}
    if(type==0 || type==4){if(code==16)code=0xa0;if(code==17)code=0xa2;if(code==18)code=0xa4;
        if(code>=0x60 && code<=0x69){constexpr std::array<unsigned,10> scans{0x52,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x48,0x49};return scans[code-0x60];}
        if(code==0x6e)return 0x53;
        if(code!=13)for(const auto& [scan,value]:extended)if(value==code)return type==4?scan:scan&255;
        if(code)for(std::uint32_t scan=1;scan<keys.size();++scan)if(keys[scan]==code)return scan;
    }return 0;
}
std::u16string windows_key_name(std::uint32_t parameter){
    const auto scan=(parameter>>16)&255U;const bool extended=parameter&0x1000000U,ignore=parameter&0x2000000U;
    if(scan==0x45)return extended?u"Num Lock":u"Pause";
    const auto key=windows_map_virtual_key(extended?0xe000U|scan:scan,3);
    if((key>='A' && key<='Z') || (key>='0' && key<='9'))return {static_cast<char16_t>(key)};
    if(ignore && (key==0xa0 || key==0xa1))return u"Shift";
    if(ignore && (key==0xa2 || key==0xa3))return u"Ctrl";
    if(ignore && (key==0xa4 || key==0xa5))return u"Alt";
    if(!key)return {};
    return utf8_to_utf16(input::key_name(scan|(extended?0x80U:0)));
}
namespace {
char16_t alt_character(unsigned number,bool ansi){
    const char byte=static_cast<char>(number&255U);
    if(ansi)return cp1252_to_utf16(std::string_view(&byte,1)).front();
    // 未加前導零的 Alt+numpad 使用 Windows OEM CP437，交由宿主轉碼。
    UErrorCode error=U_ZERO_ERROR;auto* converter=ucnv_open("ibm-437",&error);if(U_FAILURE(error))return u'\ufffd';
    UChar output[2]{};const auto count=ucnv_toUChars(converter,output,2,&byte,1,&error);ucnv_close(converter);
    return U_SUCCESS(error) && count?static_cast<char16_t>(output[0]):u'\ufffd';
}
}
std::u16string windows_key_text(std::uint32_t vk,std::uint32_t scan,const std::array<std::uint8_t,256>& keys,
    std::uint32_t flags,GuiKeyboardTranslation& saved){
    auto next=saved;const bool up=scan&0x8000U;
    const bool shift=(keys[16]|keys[0xa0]|keys[0xa1])&0x80U,control=(keys[17]|keys[0xa2]|keys[0xa3])&0x80U;
    const bool alt=(keys[18]|keys[0xa4]|keys[0xa5])&0x80U;std::u16string result;
    const auto commit=[&]{if(!(flags&4U))saved=next;return result;};
    if(vk==0xe7){result.push_back(static_cast<char16_t>(scan));return commit();}
    if(!(flags&1U) && !control){
        // scan code 決定數字鍵盤，NumLock 關閉時其 VK 可能是導覽鍵。
        const auto base=scan&127U;const bool keypad=!(scan&0x100U) && base>=0x47 && base<=0x52 && base!=0x4a && base!=0x4e;
        unsigned digit=10;constexpr std::array<unsigned,12> digits{7,8,9,10,4,5,6,10,1,2,3,0};
        if(keypad)digit=digits[base-0x47];else if(vk>=0x60 && vk<=0x69)digit=vk-0x60;
        if(alt && digit<10){if(!up){if(!next.alt_digits)next.ansi=digit==0;next.alt_digits=true;next.alt_number=(next.alt_number*10+digit)&65535U;}return commit();}
        if((vk==18 || vk==0xa4 || vk==0xa5) && up && next.alt_digits){result.push_back(alt_character(next.alt_number,next.ansi));next={};return commit();}
    }
    if(up)return commit();
    if(!alt)next={};
    if(control){
        if(vk>='A' && vk<='Z')result.push_back(static_cast<char16_t>(vk-'A'+1));
        else if(vk==8)result.push_back(127);
        else if(vk==13)result.push_back(10);
        else if(vk==0xdb)result.push_back(27);
        else if(vk==0xdc || vk==0xe2)result.push_back(28);
        else if(vk==0xdd)result.push_back(29);
        else if(vk=='6' && shift)result.push_back(30);
        else if(vk==0xbd && shift)result.push_back(31);
        else if(vk==32 || (vk=='2' && shift))result.push_back(0);
        return commit();
    }
    // Windows Alt 不是 macOS Option；一般 Alt+鍵仍產生 WM_SYSCHAR。
    const bool caps=keys[20]&1U;const auto physical=windows_map_virtual_key(vk,4);
    if(vk>='A' && vk<='Z')result=native_us_key_text(physical,shift!=caps,false);
    else if((vk>='0' && vk<='9') || (vk>=0xba && vk<=0xde) || vk==0xe2)result=native_us_key_text(vk==0xe2?0x2b:physical,shift,false);
    else if(vk==8 || vk==9 || vk==13 || vk==27 || vk==32)result.push_back(static_cast<char16_t>(vk));
    else if(vk>=0x60 && vk<=0x69 && !shift)result.push_back(static_cast<char16_t>('0'+vk-0x60));
    else switch(vk){case 0x6a:result=u"*";break;case 0x6b:result=u"+";break;case 0x6d:result=u"-";break;case 0x6e:if(!shift)result=u".";break;case 0x6f:result=u"/";break;default:break;}
    return commit();
}
}
