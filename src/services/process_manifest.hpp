#pragma once
#include "x64arm/pe_loader.hpp"
#include "x64arm/pe_file_cache.hpp"
#include "x64arm/security_descriptor.hpp"
#include <fstream>
#include <libxml/parser.h>

namespace x64arm::process_manifest {
enum class Level { Invoker, HighestAvailable, Administrator };

// 只解析執行權限宣告；不將 XML 當指令，也不載入 DTD 或外部實體。
inline Level parse(std::span<const std::uint8_t> bytes){
    if(bytes.empty() || bytes.size()>1024U*1024U)throw SecurityDescriptorError{14001};
    struct Document {xmlDocPtr value;~Document(){if(value)xmlFreeDoc(value);}} doc{
        xmlReadMemory(reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),nullptr,nullptr,
                      XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING)};
    if(!doc.value || doc.value->intSubset || doc.value->extSubset)throw SecurityDescriptorError{14001};
    const auto equals=[](const xmlChar* value,const char* text){return value && xmlStrEqual(value,reinterpret_cast<const xmlChar*>(text));};
    const auto element=[&](xmlNodePtr node,const char* name){return node && node->type==XML_ELEMENT_NODE && equals(node->name,name);};
    const auto trust_namespace=[&](xmlNodePtr node){return node->ns && (equals(node->ns->href,"urn:schemas-microsoft-com:asm.v2") || equals(node->ns->href,"urn:schemas-microsoft-com:asm.v3"));};
    const auto root=xmlDocGetRootElement(doc.value);
    if(!element(root,"assembly") || !root->ns || !equals(root->ns->href,"urn:schemas-microsoft-com:asm.v1"))throw SecurityDescriptorError{14001};
    std::optional<Level> result;
    for(auto trust=root->children;trust;trust=trust->next)if(element(trust,"trustInfo") && trust_namespace(trust))
      for(auto security=trust->children;security;security=security->next)if(element(security,"security") && trust_namespace(security))
        for(auto privileges=security->children;privileges;privileges=privileges->next)if(element(privileges,"requestedPrivileges") && trust_namespace(privileges))
          for(auto level=privileges->children;level;level=level->next)if(element(level,"requestedExecutionLevel") && trust_namespace(level)){
            if(result)throw SecurityDescriptorError{14001};
            struct Property {xmlChar* value;~Property(){xmlFree(value);}} value{xmlGetProp(level,BAD_CAST "level")},ui{xmlGetProp(level,BAD_CAST "uiAccess")};
            const auto is=[&](const char* text){return value.value && !xmlStrcasecmp(value.value,reinterpret_cast<const xmlChar*>(text));};
            if(is("asInvoker"))result=Level::Invoker;
            else if(is("highestAvailable"))result=Level::HighestAvailable;
            else if(is("requireAdministrator"))result=Level::Administrator;
            else throw SecurityDescriptorError{14001};
            if(ui.value && xmlStrcasecmp(ui.value,BAD_CAST "false"))throw SecurityDescriptorError{50};
          }
    return result.value_or(Level::Invoker);
}

inline Level executable(const std::filesystem::path& path){
    const auto read=[](const std::filesystem::path& name,std::size_t maximum,bool optional)->std::optional<std::vector<std::uint8_t>>{
        std::ifstream file(name,std::ios::binary|std::ios::ate);
        if(!file){if(optional && errno==ENOENT)return {};throw SecurityDescriptorError{5};}
        const auto size=file.tellg();if(size<0 || static_cast<std::uint64_t>(size)>maximum)throw SecurityDescriptorError{8};
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size())))throw SecurityDescriptorError{30};
        return data;
    };
    PeFileBytes image;
    try{image=read_pe_snapshot(path,512U*1024U*1024U);}
    catch(const std::length_error&){throw SecurityDescriptorError{8};}
    catch(const std::system_error& error){throw SecurityDescriptorError{error.code()==std::errc::permission_denied?5U:30U};}
    const auto embedded=read_pe_resource(*image,u"#24",1);
    if(embedded)return parse(embedded->bytes);
    const auto external=read(path.string()+".manifest",1024U*1024U,true);
    return external?parse(*external):Level::Invoker;
}
}
