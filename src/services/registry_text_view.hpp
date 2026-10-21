#pragma once
#include "x64arm/registry.hpp"
#include "x64arm/unicode.hpp"
#include <optional>
namespace x64arm {
inline std::u16string registry_text_fold(std::u16string text){for(auto& c:text)c=unicode_case_unit(c,false);return text;}
// 共用虛擬登錄檔的關聯視圖；只讀取，不替客體安裝預設應用程式。
struct RegistryTextView {
    const RegistryState& registry;
    std::vector<std::u16string> roots;
    std::optional<RegistryView> view{};bool accept_expand{true};
    std::u16string path_key(std::u16string path)const{return registry_text_fold(view?registry_view_path(std::move(path),*view):std::move(path));}
    std::optional<std::u16string> value_at(std::u16string path,std::u16string name={})const{
        const auto key=registry.keys.find(path_key(std::move(path)));if(key==registry.keys.end())return {};
        const auto value=key->second.find(registry_text_fold(std::move(name)));if(value==key->second.end() || (value->second.type!=1 && (!accept_expand || value->second.type!=2)) || value->second.bytes.size()%2)return {};
        std::u16string text;const auto& bytes=value->second.bytes;
        for(std::size_t i=0;i+1<bytes.size();i+=2){const auto c=static_cast<char16_t>(bytes[i]|(bytes[i+1]<<8));if(!c)break;text+=c;}return text;
    }
    std::optional<std::u16string> value(const std::u16string& path,const std::u16string& name={})const{
        for(const auto& root:roots)if(auto found=value_at(root+u"\\"+path,name))return found;return {};
    }
    bool exists(const std::u16string& path)const{
        for(const auto& root:roots)if(registry.keys.contains(path_key(root+u"\\"+path)))return true;return false;
    }
    std::u16string verb(const std::u16string& path,const std::u16string& extra)const{
        if(!extra.empty())return extra;
        if(auto chosen=value(path+u"\\shell");chosen && !chosen->empty()){const auto end=chosen->find_first_of(u" ,;");return chosen->substr(0,end);}
        if(exists(path+u"\\shell\\open"))return u"open";
        for(const auto& root:roots){const auto prefix=path_key(root+u"\\"+path+u"\\shell\\");const auto it=registry.keys.lower_bound(prefix);if(it!=registry.keys.end() && it->first.starts_with(prefix)){auto name=it->first.substr(prefix.size());return name.substr(0,name.find(u'\\'));}}
        return u"open";
    }
};
inline std::u16string registry_user_root(const RegistryState& registry,std::string_view current_sid,std::string_view primary_sid){
    auto user=std::u16string(1,1);
    if(current_sid!=primary_sid){user=std::u16string(1,3)+u"\\"+registry_text_fold(utf8_to_utf16(current_sid));if(!registry.keys.contains(user))user=std::u16string(1,3)+u"\\.default";}
    return user;
}
inline std::vector<std::u16string> class_registry_roots(const RegistryState& registry,std::string_view current_sid,std::string_view primary_sid,bool include_user=true){
    std::vector<std::u16string> roots;
    if(include_user)roots.push_back(registry_user_root(registry,current_sid,primary_sid)+u"\\software\\classes");
    roots.push_back(std::u16string(1,0));roots.push_back(std::u16string(1,2)+u"\\software\\classes");return roots;
}
}
