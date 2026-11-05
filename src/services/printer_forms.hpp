#pragma once
#include "x64arm/registry.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_printing.hpp"
#include <algorithm>
#include <array>
#include <optional>

namespace x64arm::printer_forms {
inline const std::u16string path=u"\x0002\\system\\currentcontrolset\\control\\print\\forms";
struct Form {
    std::uint32_t flags{},paper{};
    std::u16string name,mui;
    std::string keyword;
    std::array<std::int32_t,6> geometry{}; // SIZEL，接著 RECTL；千分之一毫米。
    std::uint32_t resource{};
    std::map<std::uint16_t,std::u16string> languages;
    bool operator==(const Form&)const=default;
};
inline std::u16string fold(std::u16string name){for(auto& c:name)c=unicode_case_unit(c,false);return name;}
inline bool valid_name(std::u16string_view name){return !name.empty() && name.size()<=31;}
inline bool valid_geometry(const Form& f){
    const auto& g=f.geometry;
    return g[0]>0 && g[1]>0 && g[2]>=0 && g[3]>=0 && g[4]>=g[2] && g[5]>=g[3] && g[4]<=g[0] && g[5]<=g[1];
}
inline std::vector<Form> builtins(){
    Form letter;letter.flags=1;letter.paper=1;letter.name=u"Letter";letter.geometry={215900,279400,0,0,215900,279400};
    Form a4;a4.flags=1;a4.paper=9;a4.name=u"A4";a4.geometry={210000,297000,0,0,210000,297000};
    return {std::move(letter),std::move(a4)};
}
// spooler 自有的版本化值；沿用登錄交易的鎖、配額、原子提交及 sandbox 隔離。
inline std::vector<std::uint8_t> encode(const Form& f){
    std::vector<std::uint8_t> out;
    const auto number=[&](std::uint32_t v){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(v>>(i*8)));};
    const auto text=[&](std::u16string_view v){number(static_cast<std::uint32_t>(v.size()));for(auto c:v){out.push_back(static_cast<std::uint8_t>(c));out.push_back(static_cast<std::uint8_t>(c>>8));}};
    number(0x31464a41);number(f.flags);number(f.paper);for(auto v:f.geometry)number(static_cast<std::uint32_t>(v));
    number(f.resource);text(f.name);text(f.mui);number(static_cast<std::uint32_t>(f.keyword.size()));out.insert(out.end(),f.keyword.begin(),f.keyword.end());
    number(static_cast<std::uint32_t>(f.languages.size()));for(const auto& [lang,name]:f.languages){number(lang);text(name);}return out;
}
inline std::optional<Form> decode(const RegistryValue& value){
    if(value.type!=3 || value.bytes.size()>1024*1024)return {};
    const auto& bytes=value.bytes;std::size_t at=0;bool valid=true;
    const auto number=[&](){if(bytes.size()-at<4){valid=false;return 0U;}std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t{bytes[at++]}<<(i*8);return v;};
    const auto text=[&](){std::u16string out;const auto count=number();if(count>32767 || count>(bytes.size()-at)/2){valid=false;return out;}
        for(unsigned i=0;i<count;++i){const auto c=static_cast<char16_t>(bytes[at]|(unsigned{bytes[at+1]}<<8));at+=2;if(!c)valid=false;out.push_back(c);}return out;};
    if(number()!=0x31464a41)return {};
    Form f;f.flags=number();f.paper=number();for(auto& v:f.geometry)v=static_cast<std::int32_t>(number());f.resource=number();f.name=text();f.mui=text();
    const auto count=number();if(count>63 || count>bytes.size()-at)return {};
    f.keyword.assign(reinterpret_cast<const char*>(bytes.data()+at),count);at+=count;
    const auto languages=number();if(languages>4096)return {};
    for(unsigned i=0;i<languages && valid;++i){const auto lang=number();auto name=text();if(lang>65535 || name.empty() || !f.languages.emplace(static_cast<std::uint16_t>(lang),std::move(name)).second)valid=false;}
    if(!valid || at!=bytes.size() || (f.flags!=0 && f.flags!=2) || f.paper<256 || f.paper>32767 || !valid_name(f.name) || !valid_geometry(f) || f.keyword.find('\0')!=f.keyword.npos)return {};
    return f;
}
inline std::vector<Form> all(const RegistryState& registry){
    auto forms=builtins();const auto key=registry.keys.find(path);if(key==registry.keys.end())return forms;
    for(const auto& [name,value]:key->second){
        auto f=decode(value);if(!f || fold(f->name)!=name)continue;
        if(std::any_of(forms.begin(),forms.end(),[&](const auto& p){return p.paper==f->paper || fold(p.name)==name || (!f->keyword.empty() && p.keyword==f->keyword);}))continue;
        forms.push_back(std::move(*f));
    }
    return forms;
}
inline std::array<std::uint64_t,3> revisions(const RegistryState& registry){
    std::array<std::uint64_t,3> out{};const auto key=registry.keys.find(path);if(key==registry.keys.end())return out;
    const auto value=key->second.find(u"");if(value==key->second.end() || value->second.type!=3 || value->second.bytes.size()!=24)return out;
    for(unsigned i=0;i<24;++i)out[i/8]|=std::uint64_t{value->second.bytes[i]}<<((i%8)*8);return out;
}
inline std::uint32_t pixels(std::uint32_t microns){return static_cast<std::uint32_t>((std::uint64_t{microns}*96+12700)/25400);}
inline bool printable(const Form& f){
    const auto w=static_cast<std::uint32_t>(f.geometry[0]),h=static_cast<std::uint32_t>(f.geometry[1]);
    return w<=3276700 && h<=3276700 && pixels(w) && pixels(h) && std::uint64_t{pixels(w)}*pixels(h)<=32U*1024U*1024U;
}
inline PrinterSettings settings(const Form& f){
    PrinterSettings s;s.paper=f.paper;s.form=f.name;s.paper_width=static_cast<unsigned>(f.geometry[0]);s.paper_height=static_cast<unsigned>(f.geometry[1]);
    s.width=pixels(s.paper_width);s.height=pixels(s.paper_height);return s;
}
inline unsigned changes(const std::vector<Form>& before,const std::vector<Form>& after){
    unsigned result=0;
    for(const auto& old:before){const auto found=std::find_if(after.begin(),after.end(),[&](const auto& f){return fold(f.name)==fold(old.name);});
        if(found==after.end())result|=0x40000;else if(*found!=old)result|=0x20000;}
    for(const auto& f:after)if(std::none_of(before.begin(),before.end(),[&](const auto& old){return fold(f.name)==fold(old.name);}))result|=0x10000;
    return result;
}
}
