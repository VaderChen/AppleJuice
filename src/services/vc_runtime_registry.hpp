#pragma once
#include "x64arm/registry.hpp"
#include "x64arm/unicode.hpp"
#include "builtin_version.hpp"
#include <array>
#include <string>

namespace x64arm {
// 引擎內建 VC++ 執行階段（msvcp140*.dll、vcruntime140*.dll），依 Windows 的安裝紀錄公開版本，
// 讓檢查 Runtimes 機碼的程式（例如 UE 啟動器）不會要求另外安裝微軟的可再散發套件。
inline void refresh_vc_runtime_registry(RegistryState& registry){
    const auto& version=vc_runtime_version;
    const std::u16string root(1,2);
    const auto key=[&](const std::u16string& path,std::u16string display){
        if(registry.keys.try_emplace(path).second){registry.generated_keys.insert(path);registry.volatile_keys.insert(path);}
        registry.display_names.try_emplace(path,std::move(display));
    };
    const auto put=[&](const std::u16string& leaf,std::u16string name,RegistryValue value){
        auto lower=name;for(auto& c:lower)if(c>=u'A' && c<=u'Z')c+=32;
        auto& values=registry.keys.at(leaf);const auto found=values.find(lower);
        if(found==values.end())++registry.values;else registry.bytes-=found->second.bytes.size();
        registry.bytes+=value.bytes.size();value.display_name=std::move(name);values[lower]=std::move(value);
    };
    const auto dword=[&](const std::u16string& leaf,const char16_t* name,std::uint32_t number){
        std::vector<std::uint8_t> bytes(4);for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(number>>(i*8));
        put(leaf,name,{4,std::move(bytes),{}});
    };
    for(const bool wow64:{false,true})for(const auto* architecture:{u"x64",u"x86"}){
        std::u16string path=root;
        key(path+=u"\\software",u"SOFTWARE");
        if(wow64)key(path+=u"\\wow6432node",u"WOW6432Node");
        key(path+=u"\\microsoft",u"Microsoft");
        key(path+=u"\\visualstudio",u"VisualStudio");
        key(path+=u"\\14.0",u"14.0");
        key(path+=u"\\vc",u"VC");
        key(path+=u"\\runtimes",u"Runtimes");
        key(path+=std::u16string(u"\\")+architecture,architecture);
        dword(path,u"Installed",1);dword(path,u"Major",version[0]);dword(path,u"Minor",version[1]);dword(path,u"Bld",version[2]);dword(path,u"Rbld",version[3]);
        const auto text=utf8_to_utf16("v"+std::to_string(version[0])+"."+std::to_string(version[1])+"."+std::to_string(version[2])+".00");
        put(path,u"Version",{1,utf16_bytes(text),{}});
    }
}
}
