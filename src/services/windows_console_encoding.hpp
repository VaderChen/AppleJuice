#pragma once
#include "windows_locale_data.hpp"
#include "x64arm/native_console.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm {
class GuestMemory;
// 主控台螢幕資訊（GetConsoleScreenBufferInfo 與 Ex 版本共用）：最大視窗以原生視窗所在螢幕或終端目前大小計。
std::array<std::uint16_t,2> console_maximum_window(const ConsoleBuffer&);
void console_write_screen_info(GuestMemory&,std::uint64_t address,const ConsoleBuffer&);
// 執行檔名稱與別名來源不分大小寫比較；命令歷程依執行檔名稱分組保存。
bool console_same_name(std::u16string_view,std::u16string_view);
void console_remember_command(ConsoleBuffer&,std::u16string_view exe,std::u16string_view command);
// 主控台 A 介面沿用 NLS 碼頁；宿主終端與畫面只接收 UTF-8。
inline std::string console_encode(std::u16string_view text,std::uint32_t page){
    const auto result=windows_nls_encode(text,page,false,"?");
    if(!result)throw std::invalid_argument("主控台碼頁無法編碼");
    return {result->bytes.begin(),result->bytes.end()};
}
inline std::u16string console_decode(std::string_view text,std::uint32_t page){
    const auto result=windows_nls_decode(text,page,false);
    if(!result)throw std::invalid_argument("主控台碼頁無法解碼");
    return *result;
}
inline void console_append_scalar(std::u16string& text,std::uint32_t value){
    if(value>0xffff){value-=0x10000;text+=static_cast<char16_t>(0xd800+(value>>10));text+=static_cast<char16_t>(0xdc00+(value&1023));}
    else text+=static_cast<char16_t>(value);
}
inline std::string console_output_text(ConsoleBuffer& screen,std::string_view bytes){
    auto& pending=screen.output_codepage_pending;pending+=bytes;std::u16string text;
    std::size_t position=0;
    while(position<pending.size()){
        const auto decoded=windows_crt_decode(std::span(reinterpret_cast<const std::uint8_t*>(pending.data()+position),pending.size()-position),screen.output_codepage);
        if(decoded.status==CrtDecodedCharacter::Status::Partial)break;
        if(decoded.status==CrtDecodedCharacter::Status::Invalid){text+=u'\ufffd';++position;}
        else {console_append_scalar(text,decoded.character);position+=decoded.bytes;}
    }
    pending.erase(0,position);return utf16_to_utf8(text);
}
// WriteConsoleInputA 的多 byte 字元可跨呼叫；已接受的記錄仍留在同一輸入佇列。
inline void console_decode_input(ConsoleBuffer& buffer,std::span<const ConsoleRecord> incoming){
    auto& pending=buffer.ansi_input_records;pending.insert(pending.end(),incoming.begin(),incoming.end());
    while(!pending.empty()){
        auto record=pending.front();if(console_field(record,0,2)!=1){buffer.records.push_back(record);pending.pop_front();continue;}
        std::array<std::uint8_t,8> bytes{};std::size_t available=0;
        for(const auto& item:pending){
            if(available==bytes.size() || console_field(item,0,2)!=1 || console_field(item,4)!=console_field(record,4))break;
            bytes[available++]=static_cast<std::uint8_t>(console_field(item,14,1));
        }
        const auto decoded=windows_crt_decode(std::span(bytes).first(available),buffer.input_codepage);
        if(decoded.status==CrtDecodedCharacter::Status::Partial && available==pending.size())break;
        const bool valid=decoded.status==CrtDecodedCharacter::Status::Complete;
        std::u16string text;console_append_scalar(text,valid?decoded.character:0xfffd);
        for(const auto c:text){set_console_field(record,14,c,2);buffer.records.push_back(record);}
        for(std::size_t i=0;i<(valid?decoded.bytes:1);++i)pending.pop_front();
    }
}
}
