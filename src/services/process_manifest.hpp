#pragma once
#include "image_file_metadata.hpp"
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

// 應用程式清單 compatibility/application/supportedOS 宣告的最高 Windows 版本：100＝Windows 10／11、81＝Windows 8.1、0＝未宣告。
// 未宣告 8.1／10 的程式在 Windows 8.1 之後由 GetVersion(Ex)／VerifyVersionInfo 看到 6.2.9200（Windows 8）。清單格式不合法時視為未宣告。
inline unsigned supported_os(std::span<const std::uint8_t> bytes){
    if(bytes.empty() || bytes.size()>1024U*1024U)return 0;
    struct Document {xmlDocPtr value;~Document(){if(value)xmlFreeDoc(value);}} doc{
        xmlReadMemory(reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),nullptr,nullptr,
                      XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING)};
    if(!doc.value || doc.value->intSubset || doc.value->extSubset)return 0;
    const auto equals=[](const xmlChar* value,const char* text){return value && xmlStrEqual(value,reinterpret_cast<const xmlChar*>(text));};
    const auto element=[&](xmlNodePtr node,const char* name){return node && node->type==XML_ELEMENT_NODE && equals(node->name,name);};
    const auto compatibility=[&](xmlNodePtr node){return node->ns && equals(node->ns->href,"urn:schemas-microsoft-com:compatibility.v1");};
    const auto root=xmlDocGetRootElement(doc.value);
    if(!element(root,"assembly"))return 0;
    unsigned result=0;
    for(auto group=root->children;group;group=group->next)if(element(group,"compatibility") && compatibility(group))
      for(auto application=group->children;application;application=application->next)if(element(application,"application") && compatibility(application))
        for(auto os=application->children;os;os=os->next)if(element(os,"supportedOS") && compatibility(os)){
            struct Property {xmlChar* value;~Property(){xmlFree(value);}} id{xmlGetProp(os,BAD_CAST "Id")};
            const auto is=[&](const char* text){return id.value && !xmlStrcasecmp(id.value,reinterpret_cast<const xmlChar*>(text));};
            if(is("{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"))result=std::max(result,100U);
            else if(is("{1f676c76-80e1-4239-95bb-83d0f6d0da78}"))result=std::max(result,81U);
        }
    return result;
}

// 應用程式清單 windowsSettings/activeCodePage：UTF-8 回傳 65001；Legacy、未宣告或格式不合法回傳 0（沿用預設判定）。
inline std::uint32_t active_codepage(std::span<const std::uint8_t> bytes){
    if(bytes.empty() || bytes.size()>1024U*1024U)return 0;
    struct Document {xmlDocPtr value;~Document(){if(value)xmlFreeDoc(value);}} doc{
        xmlReadMemory(reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),nullptr,nullptr,
                      XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING)};
    if(!doc.value || doc.value->intSubset || doc.value->extSubset)return 0;
    const auto root=xmlDocGetRootElement(doc.value);
    if(!root || root->type!=XML_ELEMENT_NODE || !xmlStrEqual(root->name,BAD_CAST "assembly"))return 0;
    // assembly/application/windowsSettings/activeCodePage；windowsSettings 的命名空間依 Windows 版本不同，只比對元素名稱。
    for(auto application=root->children;application;application=application->next)
      if(application->type==XML_ELEMENT_NODE && xmlStrEqual(application->name,BAD_CAST "application"))
        for(auto settings=application->children;settings;settings=settings->next)
          if(settings->type==XML_ELEMENT_NODE && xmlStrEqual(settings->name,BAD_CAST "windowsSettings"))
            for(auto page=settings->children;page;page=page->next)
              if(page->type==XML_ELEMENT_NODE && xmlStrEqual(page->name,BAD_CAST "activeCodePage")){
                  struct Content {xmlChar* value;~Content(){xmlFree(value);}} text{xmlNodeGetContent(page)};
                  if(!text.value)return 0;
                  std::string value(reinterpret_cast<const char*>(text.value));
                  while(!value.empty() && (value.back()==' ' || value.back()=='\n' || value.back()=='\r' || value.back()=='\t'))value.pop_back();
                  while(!value.empty() && (value.front()==' ' || value.front()=='\n' || value.front()=='\r' || value.front()=='\t'))value.erase(0,1);
                  return !xmlStrcasecmp(BAD_CAST value.c_str(),BAD_CAST "UTF-8")?65001U:0U;
              }
    return 0;
}

struct Inspection {
    Level level{};std::uint16_t subsystem{},flags{},machine{};
    std::uint64_t file_bytes{},read_bytes{},read_calls{};
};
inline Inspection inspect(const HostFile& source,const std::filesystem::path& path){
    const auto read=[](const std::filesystem::path& name,std::size_t maximum,bool optional)->std::optional<std::vector<std::uint8_t>>{
        std::ifstream file(name,std::ios::binary|std::ios::ate);
        if(!file){if(optional && errno==ENOENT)return {};throw SecurityDescriptorError{5};}
        const auto size=file.tellg();if(size<0 || static_cast<std::uint64_t>(size)>maximum)throw SecurityDescriptorError{8};
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size())))throw SecurityDescriptorError{30};
        return data;
    };
    image_file::Reader reader(source);image_file::PeMetadata image(reader);
    const auto embedded=image.resource(24,1,1024U*1024U);
    Level level=Level::Invoker;
    if(embedded)level=parse(*embedded);
    else if(const auto external=read(path.string()+".manifest",1024U*1024U,true))level=parse(*external);
    reader.finish();
    return {level,image.header().subsystem,image.header().flags,image.header().machine,reader.size(),reader.read_bytes(),reader.read_calls()};
}
inline Level executable(const std::filesystem::path& path){
    // 初始 EXE 已由呼叫端選定；仍沿用同一個來源能力與描述元流程。
    ReadOnlyFiles files;const auto guest=files.map_executable(path);const auto source=files.open_library_reader(guest);
    if(source.error)throw SecurityDescriptorError{source.error};return inspect(*source.file,source.source).level;
}
}
