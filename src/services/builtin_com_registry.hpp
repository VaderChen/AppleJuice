#pragma once
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include <array>
#include <span>
#include <string_view>

namespace x64arm::com_registry {
using Guid=std::array<std::uint8_t,16>;
struct ClassEntry {
    Guid id;
    std::u16string_view name,progid{},version_independent{};
};
struct UpdateResult {std::uint32_t error{};std::vector<std::u16string> changed;};

inline std::u16string format_guid(const Guid& id)
{
    constexpr char16_t digits[]=u"0123456789abcdef";
    constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    std::u16string text=u"{";
    for(unsigned i=0;i<order.size();++i){
        if(i==4 || i==6 || i==8 || i==10)text.push_back(u'-');
        const auto value=id[order[i]];
        text.push_back(digits[value>>4]);
        text.push_back(digits[value&15]);
    }
    text.push_back(u'}');
    return text;
}

inline UpdateResult update(RegistryState& state,const std::filesystem::path& file,RegistryStoreCache& cache,
    RegistryView view,std::span<const ClassEntry> classes,std::u16string_view module,bool remove,std::uint64_t time)
{
    RegistryTransaction transaction(state,file,&cache);
    if(transaction.error)return {0x80070000U|(transaction.error&0xffffU),{}};
    auto next=state;
    UpdateResult result;
    const auto save=[&](const std::u16string& path,std::u16string key,std::u16string_view value){
        for(std::size_t pos=1;;){
            const auto slash=path.find(u'\\',pos);
            next.keys.try_emplace(path.substr(0,slash));
            if(slash==path.npos)break;
            pos=slash+1;
        }
        auto bytes=utf16_bytes(value);
        auto& values=next.keys.at(path);
        if(const auto existing=values.find(key);existing!=values.end())next.bytes-=existing->second.bytes.size();
        else ++next.values;
        next.bytes+=bytes.size();
        values[std::move(key)]={1,std::move(bytes),{}};
        next.modified_times[path]=time;
    };
    const auto erase_key=[&](auto entry){
        for(const auto& [name,value]:entry->second){(void)name;next.bytes-=value.bytes.size();--next.values;}
        next.modified_times.erase(entry->first);
        next.display_names.erase(entry->first);
        next.security_descriptors.erase(entry->first);
        next.classes.erase(entry->first);
        next.volatile_keys.erase(entry->first);
        return next.keys.erase(entry);
    };
    const auto erase_tree=[&](const std::u16string& key){
        const auto prefix=key+u"\\";
        if(auto entry=next.keys.find(key);entry!=next.keys.end())erase_key(entry);
        for(auto child=next.keys.lower_bound(prefix);child!=next.keys.end() && child->first.starts_with(prefix);)
            child=erase_key(child);
    };
    const auto logical_path=[](std::u16string_view tail){
        auto path=std::u16string(1,0)+u"\\";
        for(auto character:tail){
            if(character>=u'A' && character<=u'Z')character+=u'a'-u'A';
            path.push_back(character);
        }
        return path;
    };
    for(const auto& klass:classes){
        const auto guid=format_guid(klass.id);
        const auto logical=logical_path(u"clsid\\"+guid),key=registry_view_path(logical,view);
        result.changed.push_back(logical);
        if(remove)erase_tree(key);
        else{
            const auto server=key+u"\\inprocserver32";
            save(key,u"",klass.name);
            save(server,u"",module);
            save(server,u"threadingmodel",u"Both");
        }
        for(const auto value:{klass.progid,klass.version_independent}){
            if(value.empty())continue;
            const auto path=logical_path(value),id_key=registry_view_path(path,view);
            result.changed.push_back(path);
            if(remove)erase_tree(id_key);
            else{
                save(id_key,u"",klass.name);
                save(id_key+u"\\clsid",u"",guid);
                const bool independent=value==klass.version_independent;
                save(key+(independent?u"\\versionindependentprogid":u"\\progid"),u"",value);
                if(independent && !klass.progid.empty())save(id_key+u"\\curver",u"",klass.progid);
            }
        }
    }
    if(next.keys.size()>4096 || next.values>4096 || next.bytes>16*1024*1024)return {0x8007000eU,{}};
    std::swap(state,next);
    try{
        if(const auto error=transaction.commit()){
            std::swap(state,next);
            return {0x80070000U|(error&0xffffU),{}};
        }
    }catch(...){std::swap(state,next);throw;}
    return result;
}
}
