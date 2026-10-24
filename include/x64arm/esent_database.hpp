#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::esent {
using Binary=std::vector<std::uint8_t>;
enum class Failure {InvalidHeader,Corrupt,Checksum,Limit,Unsupported,MissingTable,MissingColumn};
struct Error:std::runtime_error {Failure failure;Error(Failure f,const char* message):std::runtime_error(message),failure(f){}};
struct Column {
    std::uint32_t id{},type{},maximum{},flags{},codepage{},offset{};
    std::string name;std::optional<Binary> default_value;bool inherited{};
    bool operator==(const Column&)const=default;
};
struct Table {
    std::uint32_t object{},root{},long_root{},flags{},chunk_max{};
    std::string name,template_name;std::map<std::uint32_t,Column> columns;
    bool operator==(const Table&)const=default;
};
struct Row {
    Binary key;std::size_t offset{},size{};
    bool operator==(const Row&)const=default;
};
struct Database {
    std::shared_ptr<const Binary> bytes;std::uint32_t version{},revision{},page_size{},state{};
    bool backup_header{};std::map<std::string,Table> tables;
    bool operator==(const Database&)const;
};
struct Value {Binary bytes;bool separated{},compressed{};bool operator==(const Value&)const=default;};
struct Cell {bool is_null{true};std::vector<Value> values;bool operator==(const Cell&)const=default;};
constexpr std::size_t default_limit=512ULL*1024*1024;
Database parse(Binary bytes,std::size_t limit=default_limit);
const Table& table(const Database&,std::string_view);
const Column& column(const Table&,std::string_view);
std::vector<Row> rows(const Database&,const Table&);
Cell raw_cell(const Database&,const Table&,const Row&,std::uint32_t column_id);
Binary value(const Database&,const Table&,const Value&,std::size_t limit=default_limit);
Binary decompress(std::span<const std::uint8_t>,std::size_t limit=default_limit);
std::uint32_t header_checksum(std::span<const std::uint8_t>);
std::uint64_t block_checksum(std::span<const std::uint8_t>,std::uint32_t page,bool header);
}
