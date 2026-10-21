#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace x64arm::hlsl_linkage {
constexpr std::uint32_t tag=0x534c4358; // XCLS：引擎產生之動態 linkage 中繼資料。
struct Type {std::string name;std::uint32_t id{},size{};};
struct Instance {std::string name,type;std::uint32_t buffer{},offset{},count{1};};
struct Interface {std::string name,type;std::uint32_t slot{},count{1};std::vector<std::uint32_t> types;bool parameter{};};
struct Metadata {
    bool direct_named{true};
    std::vector<Type> types;
    std::vector<Instance> instances;
    std::vector<Interface> interfaces;
    std::vector<std::uint8_t> encode()const{
        std::vector<std::uint8_t> out;
        const auto word=[&](std::uint32_t value){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(value>>(8*i)));};
        const auto string=[&](const std::string& text){if(text.size()>4096)throw std::invalid_argument("linkage 名稱過長");word(static_cast<std::uint32_t>(text.size()));out.insert(out.end(),text.begin(),text.end());};
        word(direct_named?3U:2U);word(static_cast<std::uint32_t>(types.size()));word(static_cast<std::uint32_t>(instances.size()));word(static_cast<std::uint32_t>(interfaces.size()));
        for(const auto& t:types){string(t.name);word(t.id);word(t.size);}
        for(const auto& i:instances){string(i.name);string(i.type);word(i.buffer);word(i.offset);word(i.count);}
        for(const auto& i:interfaces){string(i.name);string(i.type);word(i.slot);word(i.count);word(static_cast<std::uint32_t>(i.types.size()));for(auto type:i.types)word(type);word(i.parameter?1U:0U);}
        return out;
    }
    static Metadata decode(std::span<const std::uint8_t> data){
        std::size_t cursor=0;
        const auto word=[&](){if(cursor>data.size() || data.size()-cursor<4)throw std::invalid_argument("linkage 中繼資料越界");std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t{data[cursor++]}<<(8*i);return v;};
        const auto string=[&](){const auto size=word();if(size>4096 || size>data.size()-cursor)throw std::invalid_argument("linkage 名稱越界");std::string text(reinterpret_cast<const char*>(data.data()+cursor),size);cursor+=size;return text;};
        const auto version=word();if(version!=1 && version!=2 && version!=3)throw std::invalid_argument("linkage 中繼資料版本不符");const auto nt=word(),nv=word(),ni=word();
        if(nt>65536 || nv>65536 || ni>256)throw std::invalid_argument("linkage 中繼資料數量越界");Metadata result;result.direct_named=version>=3;
        for(unsigned n=0;n<nt;++n){Type t;t.name=string();t.id=word();t.size=word();if(!t.id || t.size>61440)throw std::invalid_argument("linkage 型別不合法");result.types.push_back(std::move(t));}
        for(unsigned n=0;n<nv;++n){Instance i;i.name=string();i.type=string();i.buffer=word();i.offset=word();i.count=word();if(i.buffer>=14 || i.offset>=65536 || !i.count)throw std::invalid_argument("linkage instance 不合法");result.instances.push_back(std::move(i));}
        for(unsigned n=0;n<ni;++n){Interface i;i.name=string();i.type=string();i.slot=word();i.count=word();const auto count=word();if(count>nt)throw std::invalid_argument("linkage interface 型別數量越界");for(unsigned j=0;j<count;++j)i.types.push_back(word());if(version>=2)i.parameter=word()!=0;if(i.slot>=256 || !i.count || i.count>256-i.slot)throw std::invalid_argument("linkage interface 不合法");result.interfaces.push_back(std::move(i));}
        if(cursor!=data.size())throw std::invalid_argument("linkage 中繼資料含額外資料");return result;
    }
};
}
