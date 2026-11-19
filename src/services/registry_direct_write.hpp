#pragma once
// 服務直接讀寫沙箱登錄（呼叫者持有 RegistryTransaction）的共用輔助：機碼路徑以 ASCII 小寫為正規形式，
// 新建機碼保留顯示名稱與修改時間，值數量與位元組統計和其他直接寫入登錄的服務一致。
#include "x64arm/registry.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::registry_direct {
inline std::u16string folded(std::u16string_view text){std::u16string out(text);for(auto& c:out)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return out;}
inline void ensure_key(RegistryState& registry,const std::u16string& display,std::uint64_t now){
    const auto canonical=folded(display);
    for(std::size_t at=0;;){
        const auto next=canonical.find(u'\\',at);const auto prefix=canonical.substr(0,next);
        if(registry.keys.try_emplace(prefix).second){registry.display_names[prefix]=display.substr(0,next);registry.modified_times[prefix]=now;}
        if(next==canonical.npos)break;at=next+1;
    }
}
inline void put_value(RegistryState& registry,const std::u16string& display,std::u16string_view name,std::uint32_t type,std::vector<std::uint8_t> bytes,std::uint64_t now){
    ensure_key(registry,display,now);const auto key=folded(display);auto& values=registry.keys[key];const auto folded_name=folded(name);
    if(const auto old=values.find(folded_name);old!=values.end())registry.bytes-=old->second.bytes.size();else ++registry.values;
    registry.bytes+=bytes.size();values[folded_name]={type,std::move(bytes),std::u16string(name)};registry.modified_times[key]=now;
}
// 刪除機碼與其所有子機碼。
inline void erase_tree(RegistryState& registry,const std::u16string& canonical){
    for(auto it=registry.keys.lower_bound(canonical);it!=registry.keys.end() && it->first.starts_with(canonical);){
        if(it->first.size()!=canonical.size() && it->first[canonical.size()]!=u'\\'){++it;continue;}
        for(const auto& [name,value]:it->second){(void)name;registry.bytes-=value.bytes.size();--registry.values;}
        registry.modified_times.erase(it->first);registry.display_names.erase(it->first);registry.security_descriptors.erase(it->first);it=registry.keys.erase(it);
    }
}
inline const RegistryValue* find_value(const RegistryState& registry,const std::u16string& canonical,std::u16string_view name){
    const auto key=registry.keys.find(canonical);if(key==registry.keys.end())return nullptr;
    const auto value=key->second.find(folded(name));return value==key->second.end()?nullptr:&value->second;
}
inline std::vector<std::uint8_t> string_bytes(std::u16string_view text){
    std::vector<std::uint8_t> out;out.reserve((text.size()+1)*2);
    for(const auto c:text){out.push_back(static_cast<std::uint8_t>(c));out.push_back(static_cast<std::uint8_t>(c>>8));}
    out.push_back(0);out.push_back(0);return out;
}
// REG_MULTI_SZ：各字串以 NUL 結尾，最後再一個 NUL。
inline std::vector<std::uint8_t> multi_string_bytes(const std::vector<std::u16string>& items){
    std::vector<std::uint8_t> out;for(const auto& item:items){const auto bytes=string_bytes(item);out.insert(out.end(),bytes.begin(),bytes.end());}
    out.push_back(0);out.push_back(0);return out;
}
inline std::vector<std::u16string> multi_strings(const RegistryValue* value){
    std::vector<std::u16string> out;if(!value || value->type!=7)return out;std::u16string current;
    for(std::size_t i=0;i+1<value->bytes.size();i+=2){
        const auto c=static_cast<char16_t>(value->bytes[i]|(value->bytes[i+1]<<8));
        if(c){current.push_back(c);continue;}
        if(current.empty())break;out.push_back(std::move(current));current.clear();
    }
    if(!current.empty())out.push_back(std::move(current));
    return out;
}
}
