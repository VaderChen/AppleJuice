#pragma once
#include "x64arm/registry.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/process_identity.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <chrono>

// 客體 SCM 設定與狀態放在同一份持久登錄交易中；不建立或修改 launchd 工作。
namespace x64arm::guest_service {
using Values=std::map<std::u16string,RegistryValue>;
inline std::u16string fold(std::u16string_view text){std::u16string value(text);for(auto& c:value)c=unicode_case_unit(c,false);return value;}
inline const std::u16string root=std::u16string(1,2)+u"\\system\\currentcontrolset\\services";
inline std::u16string path(std::u16string_view name){return root+u"\\"+fold(name);}
inline std::uint64_t now(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline bool valid_name(std::u16string_view name){return !name.empty() && name.size()<=256 && name.find_first_of(u"/\\")==name.npos;}
inline std::uint64_t number(const Values& values,std::u16string_view key,std::uint64_t fallback=0){
    const auto it=values.find(std::u16string(key));if(it==values.end())return fallback;
    const auto& value=it->second;if((value.type!=4 && value.type!=11) || value.bytes.size()!=(value.type==4?4U:8U))throw SecurityDescriptorError{13};
    std::uint64_t result=0;for(unsigned i=0;i<value.bytes.size();++i)result|=std::uint64_t{value.bytes[i]}<<(i*8);return result;
}
inline void put_number(Values& values,std::u16string_view key,std::uint64_t value,bool wide=false){
    RegistryValue result;result.type=wide?11U:4U;result.bytes.resize(wide?8:4);for(unsigned i=0;i<result.bytes.size();++i)result.bytes[i]=static_cast<std::uint8_t>(value>>(i*8));values[std::u16string(key)]=std::move(result);
}
inline std::u16string text(const Values& values,std::u16string_view key,std::u16string fallback={}){
    const auto it=values.find(std::u16string(key));if(it==values.end())return fallback;const auto& value=it->second;
    if((value.type!=1 && value.type!=2) || value.bytes.size()<2 || value.bytes.size()%2 || value.bytes[value.bytes.size()-1] || value.bytes[value.bytes.size()-2])throw SecurityDescriptorError{13};
    std::u16string result;for(std::size_t i=0;i+2<value.bytes.size();i+=2){const auto c=static_cast<char16_t>(value.bytes[i]|(unsigned{value.bytes[i+1]}<<8));if(!c)throw SecurityDescriptorError{13};result.push_back(c);}return result;
}
inline void put_text(Values& values,std::u16string_view key,std::u16string_view value,std::uint32_t type=1){values[std::u16string(key)]={type,utf16_bytes(value),{}};}
inline security::Bytes bytes(const Values& values,std::u16string_view key){const auto it=values.find(std::u16string(key));return it==values.end()?security::Bytes{}:it->second.bytes;}
inline bool installed(const Values& values){return values.contains(u"imagepath") && values.contains(u"x64arm_instance");}
inline ProcessIdentity identity(const Values& values){return {static_cast<std::uint32_t>(number(values,u"x64arm_pid")),number(values,u"x64arm_birth")};}
inline std::array<std::uint32_t,9> status(const Values& values){
    std::array<std::uint32_t,9> words{static_cast<std::uint32_t>(number(values,u"type",0x10)),1,0,0,0,0,0,0,0};
    const auto stored=bytes(values,u"x64arm_status");if(!stored.empty()){
        if(stored.size()!=28)throw SecurityDescriptorError{13};for(unsigned i=0;i<7;++i){words[i]=0;for(unsigned j=0;j<4;++j)words[i]|=std::uint32_t{stored[i*4+j]}<<(j*8);}
    }
    const auto owner=identity(values);
    if(words[1]!=1 && (!owner || !process_identity::query(owner.pid).live(owner))){words[1]=1;words[2]=0;words[3]=1067;words[4]=words[5]=words[6]=0;}
    if(words[1]!=1)words[7]=owner.pid;return words;
}
inline void put_status(Values& values,std::span<const std::uint32_t> words){
    if(words.size()<7)throw SecurityDescriptorError{87};security::Bytes data(28);for(unsigned i=0;i<7;++i)for(unsigned j=0;j<4;++j)data[i*4+j]=static_cast<std::uint8_t>(words[i]>>(j*8));values[u"x64arm_status"]={3,std::move(data),{}};
}
struct Reference {ProcessIdentity owner;std::uint64_t handle{};};
inline std::vector<Reference> references(const Values& values){
    const auto data=bytes(values,u"x64arm_handles");if(data.size()%20)throw SecurityDescriptorError{13};std::vector<Reference> result;
    for(std::size_t at=0;at<data.size();at+=20){const auto read=[&](unsigned offset,unsigned width){std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t{data[at+offset+i]}<<(i*8);return value;};Reference item{{static_cast<std::uint32_t>(read(0,4)),read(4,8)},read(12,8)};if(process_identity::query(item.owner.pid).live(item.owner))result.push_back(item);}return result;
}
inline void put_references(Values& values,const std::vector<Reference>& refs){
    security::Bytes data;for(const auto& ref:refs){const auto append=[&](std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)data.push_back(static_cast<std::uint8_t>(value>>(i*8)));};append(ref.owner.pid,4);append(ref.owner.birth,8);append(ref.handle,8);}values[u"x64arm_handles"]={3,std::move(data),{}};
}
inline void create_path(RegistryState& registry,const std::u16string& key){for(std::size_t at=1;at<key.size();++at)if(key[at]==u'\\')registry.keys.try_emplace(key.substr(0,at));registry.keys.try_emplace(key);}
inline void erase(RegistryState& registry,const std::u16string& key){const auto prefix=key+u"\\";std::erase_if(registry.keys,[&](const auto& item){return item.first==key || item.first.starts_with(prefix);});std::erase_if(registry.security_descriptors,[&](const auto& item){return item.first==key || item.first.starts_with(prefix);});std::erase_if(registry.display_names,[&](const auto& item){return item.first==key || item.first.starts_with(prefix);});std::erase_if(registry.modified_times,[&](const auto& item){return item.first==key || item.first.starts_with(prefix);});}
inline std::uint32_t commit(RegistryState& registry,RegistryTransaction& transaction){
    registry.values=registry.bytes=0;for(const auto& [key,values]:registry.keys){(void)key;registry.values+=values.size();for(const auto& [name,value]:values){(void)name;registry.bytes+=value.bytes.size();}}
    return transaction.commit();
}
inline constexpr std::array<std::uint32_t,4> mapping{0x2008dU,0x20002U,0x20170U,0xf01ffU};
}
