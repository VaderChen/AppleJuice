#pragma once
#include "x64arm/cpu_features.hpp"
#include "x64arm/processor_information.hpp"
#include "x64arm/registry.hpp"
#include "x64arm/unicode.hpp"
#include <cstdio>

namespace x64arm {
inline void refresh_processor_registry(RegistryState& registry,unsigned count,const ProcessorInformation& cpu){
    const std::u16string root=u"\x0002\\hardware\\description\\system\\centralprocessor";
    // 只重建本 provider 產生的 CPU 鍵，其他硬體／使用者值不回退或整份取代。
    for(auto it=registry.generated_keys.begin();it!=registry.generated_keys.end();){
        if(it->starts_with(root+u"\\")){
            if(const auto found=registry.keys.find(*it);found!=registry.keys.end()){
                for(const auto& [name,value]:found->second){(void)name;registry.bytes-=value.bytes.size();--registry.values;}
                registry.keys.erase(found);
            }
            registry.display_names.erase(*it);registry.volatile_keys.erase(*it);it=registry.generated_keys.erase(it);
        }else ++it;
    }
    const auto key=[&](const std::u16string& path,std::u16string display){
        if(registry.keys.try_emplace(path).second){registry.generated_keys.insert(path);registry.volatile_keys.insert(path);}
        registry.display_names[path]=std::move(display);
    };
    std::u16string path(1,2);
    for(const auto* component:{u"hardware",u"description",u"system",u"centralprocessor"}){path+=u'\\';path+=component;key(path,component);}
    const auto leaf=virtual_cpuid(1,0,count);const auto signature=leaf[0];
    unsigned family=(signature>>8)&15U,model=(signature>>4)&15U;
    if(family==6 || family==15)model|=(signature>>12)&0xf0U;
    if(family==15)family+=(signature>>20)&255U;
    char identifier[96];std::snprintf(identifier,sizeof(identifier),"AMD64 Family %u Model %u Stepping %u",family,model,signature&15U);
    const auto vendor=virtual_cpuid(0,0,count);std::string vendor_text;
    for(unsigned i:{1U,3U,2U})for(unsigned b=0;b<4;++b)vendor_text+=static_cast<char>(vendor[i]>>(8*b));
    for(unsigned index=0;index<count;++index){
        const auto label=utf8_to_utf16(std::to_string(index));const auto child=root+u'\\'+label;key(child,label);
        const auto put=[&](std::u16string name,RegistryValue value){
            auto lower=name;for(auto& c:lower)if(c>=u'A' && c<=u'Z')c+=32;
            auto& values=registry.keys.at(child);const auto found=values.find(lower);
            if(found==values.end())++registry.values;else registry.bytes-=found->second.bytes.size();
            registry.bytes+=value.bytes.size();value.display_name=std::move(name);values[lower]=std::move(value);
        };
        const auto string=[&](const char16_t* name,std::string_view text){put(name,{1,utf16_bytes(utf8_to_utf16(text)),{}});};
        string(u"ProcessorNameString",cpu.name);string(u"Identifier",identifier);string(u"VendorIdentifier",vendor_text);
        // ~MHz 是標稱頻率；沒有主機標稱值時不把最高 DVFS 頻率冒充成 base clock。
        if(cpu.nominal_mhz){std::vector<std::uint8_t> value(4);for(unsigned i=0;i<4;++i)value[i]=static_cast<std::uint8_t>(cpu.nominal_mhz>>(i*8));put(u"~MHz",{4,std::move(value),{}});}
    }
}
}
