#pragma once
#include "x64arm/msi_database.hpp"

namespace x64arm::msi {
// Query 不保存宿主指標；重新綁定目前 schema，避免關閉／修改資料表後使用舊欄位。
struct Query {
    std::u16string sql;
    std::vector<Column> columns;
    std::size_t parameters{};
    bool writes{};
    bool operator==(const Query&)const=default;
};
struct Result {
    std::vector<Column> columns;
    std::vector<Row> rows;
    bool operator==(const Result&)const=default;
};
Query prepare(const Database&,std::u16string_view);
Result execute(Database&,const Query&,std::span<const Cell> parameters={});
std::u16string column_definition(const Column&);
std::optional<std::int32_t> cell_integer(const Cell&);
}
