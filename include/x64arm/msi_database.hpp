#pragma once
#include "x64arm/compound_file.hpp"
#include <set>
#include <variant>

namespace x64arm::msi {
// Windows Installer 資料欄位，與宿主指標寬度無關；monostate 代表 MSI NULL。
using Binary = std::vector<std::uint8_t>;
using Cell = std::variant<std::monostate,std::int32_t,std::u16string,Binary>;
struct Error final : std::runtime_error {
    Error(std::uint32_t value,const char* message):std::runtime_error(message),code(value){}
    std::uint32_t code;
};
inline constexpr std::uint16_t valid=0x100,localizable=0x200,string=0x800,nullable=0x1000,key=0x2000,temporary=0x4000;
struct Column {
    std::u16string name;std::uint16_t type{};
    bool binary()const{return (type&~(nullable|temporary|key))==(valid|string);}
    bool text()const{return (type&string) && !binary();}
    bool operator==(const Column&)const=default;
};
struct Row {
    std::vector<Cell> fields;bool temporary{};
    bool operator==(const Row&)const=default;
};
struct Table {
    std::u16string name;std::vector<Column> columns;std::vector<Row> rows;
    bool persistent{true};std::uint32_t holds{};
    bool operator==(const Table&)const=default;
};
struct Database {
    compound::Document storage;
    std::map<std::u16string,Table> tables;
    std::set<std::u16string> binary_streams;
    std::map<std::u16string,std::uint32_t> string_ids;
    std::uint32_t codepage{},default_codepage{1252};
    bool operator==(const Database&)const=default;
};
std::u16string encode_stream_name(std::u16string_view,bool table);
std::u16string decode_stream_name(std::u16string_view);
std::u16string cell_text(const Cell&);
void validate(const Table&);
Table table_catalog(const Database&);
Table column_catalog(const Database&);
Table stream_catalog(const Database&);
std::map<std::u16string,std::uint32_t> string_indices(const Database&);
Database create(std::uint32_t codepage=0,std::uint32_t default_codepage=1252);
Database parse(std::span<const std::uint8_t>,std::uint32_t default_codepage=1252,std::size_t limit=compound::default_limit);
std::vector<std::uint8_t> serialize(const Database&,std::size_t limit=compound::default_limit);
}
