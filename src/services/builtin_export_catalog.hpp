#pragma once
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace x64arm::detail {
// 匯出解析與固定映像容量共用同一份名稱目錄，含別名、轉發及稀疏序號。
const std::map<std::string_view,std::set<std::string>>& builtin_export_catalog();
}
