#pragma once
#include "x64arm/registry.hpp"
#include <array>
#include <optional>
#include <span>

namespace x64arm {struct NetworkConfiguration;}
namespace x64arm::socket_namespace {
inline constexpr std::uint32_t local_namespace=10;
// 由本引擎安裝的本機 TCP/IP provider；不冒用 Windows 的第三方 provider 身分。
inline constexpr std::array<std::uint8_t,16> provider_id{0x09,0xfe,0x25,0xda,0x01,0x8a,0x7f,0x4b,0x94,0xc5,0xd1,0x84,0x38,0x11,0xea,0x61};
inline constexpr std::uint32_t nla_namespace=15;
inline constexpr std::array<std::uint8_t,16> nla_provider_id{0x3a,0x24,0x42,0x66,0xa8,0x3b,0xa6,0x4a,0xba,0xa5,0x2e,0x0b,0xd7,0x1f,0xdd,0x83};
inline constexpr std::array<std::uint8_t,16> nla_class_id{0x15,0xe5,0x37,0x00,0xc9,0xb5,0x43,0x4a,0xba,0xda,0x8b,0x48,0xa8,0x7a,0xd2,0x39};
struct Address {
    std::vector<std::uint8_t> local,remote;
    std::uint32_t type{},protocol{};
    bool operator==(const Address&)const=default;
};
struct Record {
    std::uint32_t namespace_id{local_namespace};
    std::array<std::uint8_t,16> service_class{};
    std::u16string name,context,comment;
    std::optional<std::array<std::uint32_t,2>> version;
    std::optional<std::vector<std::uint8_t>> blob;
    std::vector<Address> addresses;
    bool registered{};
    bool operator==(const Record&)const=default;
};
struct Lookup {
    std::vector<Record> records;
    std::size_t cursor{};
    std::uint32_t flags{};
    bool too_small{};
    bool operator==(const Lookup&)const=default;
};
std::u16string fold(std::u16string_view);
bool valid(const Address&);
std::vector<Record> load(const RegistryState&);
void save(RegistryState&,const std::vector<Record>&);
std::uint32_t change(std::vector<Record>&,Record,std::uint32_t operation,bool multiple);
std::vector<std::uint8_t> information(const Record&,bool wide,bool x86,std::uint64_t base,std::uint32_t flags);
std::vector<std::uint8_t> providers(bool wide,bool x86,std::uint64_t base);
std::vector<Record> nla_records(const NetworkConfiguration&);
}
