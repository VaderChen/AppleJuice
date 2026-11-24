#include "ansi_codepage.hpp"
#include "windows_locale_data.hpp"
#include "x64arm/unicode.hpp"
#include <array>
#include <atomic>

namespace x64arm {
namespace {
std::atomic<std::uint32_t> ansi_page{65001};
// 前導位元組表與碼頁一起更新；預設 UTF-8 沒有前導位元組。
std::atomic<const std::array<bool,256>*> lead_table{nullptr};
}

std::uint32_t process_ansi_codepage() noexcept {return ansi_page.load(std::memory_order_relaxed);}

void set_process_ansi_codepage(std::uint32_t page) noexcept {
    if(page!=65001){
        try{
            const auto info=windows_nls_codepage(page);
            if(!info)return;
            static std::array<bool,256> tables[4];static std::atomic<unsigned> used{0};
            const auto index=used.fetch_add(1)%4;
            for(unsigned i=0;i<256;++i)tables[index][i]=info->maximum_character_bytes>1 && info->lead[i];
            lead_table.store(&tables[index],std::memory_order_release);
        }catch(...){return;}
    }else lead_table.store(nullptr,std::memory_order_release);
    ansi_page.store(page,std::memory_order_relaxed);
}

std::uint32_t ansi_codepage_for_language(std::uint16_t language) noexcept {
    const unsigned primary=language&0x3ffU,sub=language>>10;
    if(primary==0x04)return sub==0x02 || sub==0x04?936U:sub==0x01 || sub==0x03 || sub==0x05?950U:0U;
    if(primary==0x11)return 932U;
    if(primary==0x12)return 949U;
    return 0;
}

std::uint16_t version_resource_language(std::span<const std::uint8_t> version,std::uint16_t directory_language) noexcept {
    // VarFileInfo 的 "Translation" 值：WORD 語言、WORD 碼頁的陣列；值緊接在 4 位元組對齊的鍵名之後。
    static constexpr char16_t key[]=u"Translation";
    const std::size_t key_bytes=(std::size(key))*2U;
    for(std::size_t at=6;at+key_bytes+4<=version.size();at+=2){
        bool match=true;
        for(std::size_t i=0;i<std::size(key) && match;++i)
            match=version[at+i*2]==static_cast<std::uint8_t>(key[i]&0xff) && version[at+i*2+1]==static_cast<std::uint8_t>(key[i]>>8);
        if(!match)continue;
        const std::size_t value_length=static_cast<std::size_t>(version[at-4])|(static_cast<std::size_t>(version[at-3])<<8U);
        std::size_t value=(at+key_bytes+3)&~std::size_t{3};
        if(value_length<4 || value+4>version.size())break;
        const auto language=static_cast<std::uint16_t>(static_cast<unsigned>(version[value])|(static_cast<unsigned>(version[value+1])<<8U));
        if(language)return language;
        break;
    }
    return directory_language;
}

std::u16string ansi_to_utf16(std::string_view bytes){
    const auto page=process_ansi_codepage();
    if(page==65001)return utf8_to_utf16(bytes);
    if(auto value=windows_nls_decode(bytes,page,false))return std::move(*value);
    return utf8_to_utf16_lossy(bytes);
}
std::u16string ansi_to_utf16_lossy(std::string_view bytes){
    const auto page=process_ansi_codepage();
    if(page==65001)return utf8_to_utf16_lossy(bytes);
    if(auto value=windows_nls_decode(bytes,page,false))return std::move(*value);
    return utf8_to_utf16_lossy(bytes);
}
std::string ansi_to_utf8(std::string_view bytes){
    if(process_ansi_is_utf8())return std::string(bytes);
    return utf16_to_utf8_lossy(ansi_to_utf16(bytes));
}
std::string utf16_to_ansi(std::u16string_view text){
    const auto page=process_ansi_codepage();
    if(page==65001)return utf16_to_utf8(text);
    if(auto value=windows_nls_encode(text,page,false,"?"))return std::string(value->bytes.begin(),value->bytes.end());
    return utf16_to_utf8_lossy(text);
}
std::string utf8_to_ansi(std::string_view text){
    if(process_ansi_is_utf8())return std::string(text);
    return utf16_to_ansi(utf8_to_utf16_lossy(text));
}
std::size_t ansi_prefix_length(std::string_view text,std::size_t maximum) noexcept {
    if(text.size()<=maximum)return text.size();
    if(process_ansi_is_utf8()){
        auto end=maximum;while(end && (static_cast<unsigned char>(text[end])&0xc0U)==0x80U)--end;return end;
    }
    std::size_t at=0;
    while(at<maximum){
        const auto width=ansi_lead_byte(static_cast<std::uint8_t>(text[at])) && at+1<text.size()?2U:1U;
        if(at+width>maximum)break;
        at+=width;
    }
    return at;
}
bool ansi_lead_byte(std::uint8_t byte) noexcept {
    const auto* table=lead_table.load(std::memory_order_acquire);
    return table && (*table)[byte];
}
}
