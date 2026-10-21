#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::hid {
inline constexpr std::uint32_t success=0x00110000,invalid_preparsed=0xc0110001,
    invalid_type=0xc0110002,invalid_length=0xc0110003,usage_not_found=0xc0110004,
    value_out_of_range=0xc0110005,bad_log_phy_values=0xc0110006,buffer_too_small=0xc0110007,
    null_value=0x80110001,
    incompatible_id=0xc011000a,not_value_array=0xc011000b,report_not_found=0xc0110010;
struct Error:std::runtime_error {
    std::uint32_t status;
    explicit Error(std::uint32_t value):std::runtime_error("HID 報告或預解析資料無效"),status(value){}
};
struct Usage {std::uint16_t page{},value{};bool operator==(const Usage&)const=default;};
struct Field {
    unsigned type{},offset{},width{},count{},link{},index{};
    std::uint8_t report{};std::uint32_t flags{},unit{},unit_exponent{};
    std::int32_t minimum{},maximum{},physical_minimum{},physical_maximum{};
    Usage link_usage{};bool range{},button{};
    std::vector<Usage> usages;
    bool variable()const{return flags&2U;}
    bool absolute()const{return !(flags&4U);}
    unsigned data_index(unsigned element)const;
    unsigned value_count(unsigned element)const;
    unsigned indices()const;
};
struct Layout {
    Usage usage{};std::uint16_t links{};
    std::vector<Field> fields;
    std::array<std::array<unsigned,256>,3> report_bytes{};
    std::array<unsigned,3> data_indices{};
    bool numbered{};
    unsigned bytes(unsigned type)const;
    std::vector<std::array<std::uint8_t,72>> caps(unsigned type,bool buttons)const;
    std::array<std::uint8_t,64> caps()const;
    void validate(unsigned type,std::span<const std::uint8_t> report)const;
    std::uint32_t value(const Field&,unsigned,std::span<const std::uint8_t>)const;
    std::int32_t scaled_value(const Field&,unsigned,std::span<const std::uint8_t>)const;
    void value(const Field&,unsigned,std::uint32_t,std::span<std::uint8_t>)const;
    const Field* find(unsigned type,Usage usage,unsigned link,std::uint8_t report,bool button,unsigned* element=nullptr)const;
};
Layout parse(std::span<const std::uint8_t> descriptor,Usage top={});
// HIDP_PREPARSED_DATA 為 opaque；所有本引擎 HID 入口共用版本化、可複製的資料。
std::vector<std::uint8_t> preparsed(std::span<const std::uint8_t> descriptor,Usage top={});
Layout decode(std::span<const std::uint8_t> data);
std::uint32_t preparsed_size(std::span<const std::uint8_t> header);
}
