#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace x64arm::automation {
constexpr std::uint32_t invalid=0x80070057U, mismatch=0x80020005U,
    overflow=0x8002000aU, badtype=0x80020008U, nomemory=0x8007000eU;
// 固定 Windows VARIANT 配置；不能使用宿主的 long、wchar_t 或 COM 指標。
using Value=std::array<std::uint8_t,24>;
std::uint64_t get(const Value&,unsigned offset,unsigned bytes);
void put(Value&,unsigned offset,unsigned bytes,std::uint64_t);
bool valid_type(std::uint16_t,bool references=true);
std::uint32_t number_convert(const Value&,std::string_view decimal,std::uint16_t target,Value&);
std::uint32_t number_compare(const Value&,const Value&,std::string_view left={},std::string_view right={});
std::string number_string(const Value&);
std::optional<std::string> parse_number(std::u16string,std::uint32_t lcid,std::uint32_t& status);
std::optional<std::u16string> format_number(std::string_view,std::uint32_t lcid);
std::uint32_t compare_strings(std::u16string,std::u16string,std::uint32_t lcid,std::uint32_t flags);
std::optional<double> parse_date(std::u16string_view,std::uint32_t lcid,std::uint16_t flags);
std::optional<std::u16string> format_date(double,std::uint32_t lcid,std::uint16_t flags);
std::optional<std::array<unsigned,8>> time_fields(double);
std::optional<double> date_value(std::array<unsigned,8>);
std::u16string bool_string(bool,std::uint32_t lcid,bool local);
}
