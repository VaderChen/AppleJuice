#pragma once
#include "x64arm/readonly_files.hpp"
#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace x64arm::directory_overlay {
inline char fold(char c) {return c>='A' && c<='Z'?static_cast<char>(c+32):c;}
inline bool same_name(std::string_view a,std::string_view b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](char x,char y){return fold(x)==fold(y);});
}
inline std::string folded(std::string_view name) {
    std::string result(name);for(auto& c:result)c=fold(c);return result;
}
// 只補入不存在的名稱，保留目的地順序、既有重名項目及第一個來源的中繼資料。
inline void append_missing(std::vector<DirectoryEntry>& entries,std::span<const DirectoryEntry> extra) {
    if(extra.empty())return;
    // 小集合直接比較，避免為有界的少量名稱配置雜湊表。
    if(entries.size()<=16 && extra.size()<=16-entries.size()){
        for(const auto& item:extra)if(std::none_of(entries.begin(),entries.end(),[&](const auto& old){return same_name(old.name,item.name);}))entries.push_back(item);
        return;
    }
    std::unordered_set<std::string> names;
    names.reserve(entries.size()+extra.size());
    for(const auto& item:entries)names.insert(folded(item.name));
    for(const auto& item:extra)if(names.insert(folded(item.name)).second)entries.push_back(item);
}
}
