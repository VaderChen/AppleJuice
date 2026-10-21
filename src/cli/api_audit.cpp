#include "x64arm/api_audit.hpp"
#include "x64arm/pe_loader.hpp"
#include "x64arm/raw_program.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace x64arm {
namespace {
std::string json_string(std::string_view value) {
    std::ostringstream out; out << '"';
    for (const char raw : value) {
        const auto c=static_cast<unsigned char>(raw);
        if(c=='"' || c=='\\')out << '\\' << c;
        else if(c<32)out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"'; return out.str();
}
void resolution(std::ostream& out, const WindowsServices& services, std::string dll, std::string_view symbol) {
    for(auto& c:dll)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
    out << ",\"builtin_module\":" << (services.builtin_module(dll)?"true":"false");
    try {
        const auto address=services.resolve(dll,symbol);
        out << ",\"registered\":true,\"resolved_name\":" << json_string(services.name(address));
    } catch(const std::invalid_argument&) { out << ",\"registered\":false"; }
}
}
void audit_pe(const std::filesystem::path& path, std::ostream& output) {
    const auto bytes=read_raw_file(path);
    const auto parsed=inspect_pe_image(bytes);
    std::string loader_error;
    try { (void)parse_pe_image(bytes,{},parsed.dll); }
    catch(const std::invalid_argument& error) { loader_error=error.what(); }
    const WindowsServices services;
    // 先完成解析才輸出，避免格式錯誤留下半份 JSON。
    output << "{\"schema\":1,\"architecture\":" << json_string(parsed.metadata.architecture==PeArchitecture::X86?"x86":"x64")
           << ",\"dll\":" << (parsed.dll?"true":"false") << ",\"loader_error\":" << json_string(loader_error) << ",\"image_size\":" << parsed.metadata.image_size << ",\"imports\":[";
    bool first=true;
    const auto imports=[&](const auto& list,bool delay) {
        for(const auto& item:list) {
            const auto symbol=item.by_ordinal?"#"+std::to_string(item.ordinal):item.symbol;
            output << (first?"":",") << "{\"dll\":" << json_string(item.dll) << ",\"symbol\":" << json_string(symbol)
                   << ",\"kind\":" << json_string(delay?"delay":"normal") << ",\"iat_rva\":" << item.slot;
            resolution(output,services,item.dll,symbol);output << '}';first=false;
        }
    };
    imports(parsed.imports,false);imports(parsed.delay_imports,true);
    output << "],\"exports\":["; first=true;
    std::multimap<std::uint32_t,std::string_view> names;
    for(const auto& [name,ordinal]:parsed.export_names)names.emplace(ordinal,name);
    auto entries=parsed.export_ordinals;
    for(const auto& [ordinal,target]:parsed.export_forwarders){(void)target;entries.emplace(ordinal,0);}
    for(const auto& [ordinal,rva]:entries) {
        output << (first?"":",") << "{\"ordinal\":" << ordinal << ",\"rva\":" << rva << ",\"names\":[";
        bool first_name=true;const auto [begin,end]=names.equal_range(ordinal);
        for(auto it=begin;it!=end;++it){output << (first_name?"":",") << json_string(it->second);first_name=false;}
        output << ']';
        if(const auto found=parsed.export_forwarders.find(ordinal);found!=parsed.export_forwarders.end()) {
            output << ",\"forwarder\":" << json_string(found->second);
            const auto dot=found->second.find_last_of('.');
            if(dot!=std::string::npos){auto target=found->second.substr(0,dot);if(!target.ends_with(".dll"))target+=".dll";
                output << ",\"target\":{\"dll\":" << json_string(target) << ",\"symbol\":" << json_string(found->second.substr(dot+1));
                resolution(output,services,target,found->second.substr(dot+1));output << '}';}
        }
        output << '}';first=false;
    }
    output << "]}\n";
}
}
