#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::compound {
constexpr std::uint32_t no_node=UINT32_MAX;
constexpr std::size_t default_limit=64U*1024U*1024U;
constexpr std::size_t node_limit=65536;
enum class Kind : std::uint8_t {Storage=1,Stream=2,Root=5};
enum class Failure {InvalidFormat,InvalidTree,InvalidName,Duplicate,Missing,TooLarge};
class Error final : public std::runtime_error {
public:
    Error(Failure value,const char* message):std::runtime_error(message),failure(value){}
    Failure failure;
};
struct Node {
    std::uint32_t parent{no_node};Kind kind{Kind::Storage};std::u16string name;
    std::array<std::uint8_t,16> clsid{};std::uint32_t state_bits{};
    std::uint64_t created{},modified{};std::vector<std::uint8_t> data{};
    bool operator==(const Node&)const=default;
};
// 名稱依 MS-CFB 的 UTF-16 長度及逐 code unit simple uppercase 比較。
bool valid_name(std::u16string_view);
int compare_name(std::u16string_view,std::u16string_view);
struct Document {
    std::uint16_t version{3};std::uint32_t transaction{},next_node{1};
    std::map<std::uint32_t,Node> nodes{{0,Node{no_node,Kind::Root,u"Root Entry"}}};
    std::optional<std::uint32_t> find(std::uint32_t parent,std::u16string_view name)const;
    std::vector<std::uint32_t> children(std::uint32_t parent)const;
    std::uint32_t add(std::uint32_t parent,std::u16string name,Kind);
    void rename(std::uint32_t id,std::u16string name);
    void erase(std::uint32_t id);
    void validate()const;
    bool operator==(const Document&)const=default;
};
bool signature(std::span<const std::uint8_t>)noexcept;
Document parse(std::span<const std::uint8_t>,std::size_t byte_limit=default_limit);
std::vector<std::uint8_t> serialize(const Document&,std::size_t byte_limit=default_limit);
// 保留舊配置，將新一代目錄與串流配置於尾端；可先寫尾端，再以 header 發布。
std::vector<std::uint8_t> serialize_append(const Document&,std::span<const std::uint8_t> previous,std::size_t byte_limit=default_limit);
}
