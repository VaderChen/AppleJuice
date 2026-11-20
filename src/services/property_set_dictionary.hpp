#pragma once
#include "property_set_stream.hpp"
#include "windows_locale_data.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm::property_set {
using Names = std::map<std::uint32_t,std::u16string>;
inline unsigned dictionary_page(const Section& section) {
    const auto it=section.properties.find(1);
    if(it==section.properties.end())return 1252;
    if(read(it->second,0)!=2)throw Invalid{};
    return read(it->second,4,2);
}
inline Names decode_names(const Section& section) {
    Names names;
    const auto it=section.properties.find(0);
    if(it==section.properties.end())return names;
    const auto page=dictionary_page(section);
    const std::span<const std::uint8_t> data(it->second);
    const auto count=read(data,0);
    size_t pos=4;
    for(std::uint32_t i=0;i<count;++i){
        const auto id=read(data,pos);
        const size_t units=read(data,pos+4);
        pos+=8;
        const size_t width=page==1200?2:1;
        if(!units || units>(data.size()-pos)/width)throw Invalid{};
        const size_t length=units*width;
        if(read(data,pos+length-width,static_cast<unsigned>(width)))throw Invalid{};
        std::u16string name;
        if(page==1200){
            for(size_t j=0;j+2<length;j+=2)name.push_back(static_cast<char16_t>(read(data,pos+j,2)));
        }else{
            const auto decoded=windows_nls_decode({reinterpret_cast<const char*>(data.data()+pos),length-1},page,true);
            if(!decoded)throw Invalid{};
            name=*decoded;
        }
        if(name.find(u'\0')!=name.npos || !names.emplace(id,std::move(name)).second)throw Invalid{};
        pos+=length;
        if(page==1200)pos=(pos+3)&~size_t{3};
        if(pos>data.size())throw Invalid{};
    }
    return names;
}
inline std::optional<Bytes> encode_names(const Names& names,unsigned page,std::uint16_t& version) {
    Bytes data;
    auto dword=[&](size_t value){
        if(value>UINT32_MAX)throw Invalid{};
        for(unsigned i=0;i<4;++i)data.push_back(static_cast<std::uint8_t>(value>>(8*i)));
    };
    dword(names.size());
    for(const auto& [id,name]:names){
        dword(id);
        if(page==1200){
            dword(name.size()+1);
            auto bytes=utf16_bytes(name);
            data.insert(data.end(),bytes.begin(),bytes.end());
            data.resize((data.size()+3)&~size_t{3});
            if(name.size()>=256)version=1;
        }else{
            auto bytes=windows_crt_encode(name,page);
            if(!bytes)return {};
            dword(bytes->size()+1);
            data.insert(data.end(),bytes->begin(),bytes->end());
            data.push_back(0);
            if(bytes->size()>=256)version=1;
        }
    }
    data.resize((data.size()+3)&~size_t{3});
    return data;
}
inline auto find_name(const Names& names,std::u16string_view key,bool sensitive) {
    return std::find_if(names.begin(),names.end(),[&](const auto& entry){
        const auto& text=entry.second;
        return text.size()==key.size() && std::equal(text.begin(),text.end(),key.begin(),[&](auto a,auto b){
            return a==b || (!sensitive && unicode_case_unit(a,true)==unicode_case_unit(b,true));
        });
    });
}
}
