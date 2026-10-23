#include "x64arm/windows_services.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::array<std::uint32_t,4> key_mapping{0x20019U,0x20006U,0x20019U,0xf003fU};
std::u16string fold(std::u16string value){for(auto& c:value)c=unicode_case_unit(c,false);return value;}
std::uint32_t key_flags(std::uint32_t flags,bool setting){
    if(!flags || flags&~(setting?0xa000001fU:0x1fU) || (flags&0xa0000000U)==0xa0000000U)throw SecurityDescriptorError{87};
    if(flags&8U)throw SecurityDescriptorError{1314};
    return setting?((flags&3U?0x80000U:0U)|(flags&0xa0000004U?0x40000U:0U)|(flags&16U?0x80000U:0U)):0x20000U;
}
}
const std::vector<std::uint8_t>& WindowsServices::registry_security_descriptor(const std::u16string& path){
    auto& descriptors=runtime_.registry.security_descriptors;
    if(const auto found=descriptors.find(path);found!=descriptors.end())return found->second;
    initialize_tokens();const auto& primary=runtime_.token_objects.at(1);security::SecurityDescriptor sd;
    const auto slash=path.find_last_of(u'\\');
    if(slash!=path.npos){sd=security::inherit_descriptor({},security::parse_relative_descriptor(registry_security_descriptor(path.substr(0,slash))),primary.owner,primary.primary_group,true);}
    else{sd.data[0]=primary.owner;sd.data[1]=primary.primary_group;}
    if(!(sd.control&4U)){
        sd.control|=4;const auto acl=security::parse_acl(primary.default_dacl);std::vector<security::Bytes> entries;
        for(std::size_t i=0;i<acl.offsets.size();++i){const auto at=acl.offsets[i],end=i+1<acl.offsets.size()?acl.offsets[i+1]:acl.used;auto ace=security::basic_ace(std::span{acl.bytes}.subspan(at,end-at));ace.flags|=2U;entries.push_back(security::encode_ace(ace));}
        sd.data[3]=security::encode_acl(entries,acl.bytes[0]);
    }
    // 舊版客體登錄資料由原始 primary 身分持有，不能按每次開啟者重新製造 ACL。
    return descriptors.emplace(path,security::relative_descriptor(sd)).first->second;
}
std::vector<std::uint8_t> WindowsServices::registry_creation_descriptor(const std::vector<std::uint8_t>& parent,security::SecurityDescriptor child){
    initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
    return object_security_descriptor(security::inherit_descriptor(std::move(child),security::parse_relative_descriptor(parent),token.owner,token.primary_group,true),true,true);
}
std::uint32_t WindowsServices::registry_security_access(const std::u16string& path,std::uint32_t desired){
    return object_security_access(registry_security_descriptor(path),desired&~0x300U,key_mapping);
}
std::pair<std::u16string,std::uint32_t> WindowsServices::registry_security_handle(std::uint64_t value,std::uint32_t required){
    const auto root=static_cast<std::uint32_t>(value);auto& registry=runtime_.registry;
    if((value==root || value==(UINT64_C(0xffffffff00000000)|root)) && root>=0x80000000U && root<=0x80000006U && root!=0x80000004U){
        initialize_tokens();std::u16string path(1,static_cast<char16_t>(root&255));
        if(root==0x80000001U){
            const bool cache=!runtime_.registry_cache_disabled && !runtime_.registry_user_cache_disabled;
            const auto found=runtime_.registry_predefined_cache.find(root);
            if(cache && found!=runtime_.registry_predefined_cache.end())path=found->second;
            else if(runtime_.token_objects.at(effective_token_id()).user.sid!=runtime_.token_objects.at(1).user.sid){
                path=std::u16string(1,3)+u"\\"+fold(utf8_to_utf16(token_user_sid_string(effective_token_id())));
                if(!registry.keys.contains(path))path=std::u16string(1,3)+u"\\.default";
            }
            if(cache)runtime_.registry_predefined_cache[root]=path;
        }
        registry.keys.try_emplace(path);const auto granted=registry_security_access(path,required);return {path,granted};
    }
    const auto found=registry.handles.find(value);if(found==registry.handles.end())throw SecurityDescriptorError{6};
    if(!registry.keys.contains(found->second.path))throw SecurityDescriptorError{1018};
    if((found->second.access&required)!=required)throw SecurityDescriptorError{5};return {found->second.path,found->second.access};
}
std::optional<std::uint32_t> WindowsServices::invoke_registry_security(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    RegistryIoScope io([&](const auto& work){await_registry_io(state,memory,work);});
    using namespace security;
    const WindowsCall call(state,memory,service);const auto api=service.symbol;
    const bool reg=api=="RegGetKeySecurity" || api=="RegSetKeySecurity",setting=api.starts_with("Set") || api=="RegSetKeySecurity";
    const bool named=api.starts_with("GetNamed") || api.starts_with("SetNamed"),wide=api.ends_with('W');
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    std::uint32_t result=0;
    try{
        if(!registry_file_.empty() && !runtime_.network.external_io)throw SecurityDescriptorError{50};
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,!setting);if(transaction.error)throw SecurityDescriptorError{transaction.error};
        const auto original=setting?runtime_.registry:RegistryState{};const auto flags=static_cast<std::uint32_t>(call(reg?1:2));
        const auto needed=key_flags(flags,setting);std::u16string path;
        if(named){
            if(!call(0))throw SecurityDescriptorError{87};
            auto name=fold(wide?read_utf16(memory,GuestAddress{call(0)}):utf8_to_utf16(read_ansi(memory,call(0),32768)));
            const auto slash=name.find(u'\\');const auto prefix=name.substr(0,slash);
            const std::array<std::u16string_view,7> roots{u"classes_root",u"current_user",u"machine",u"users",u"",u"current_config",u""};
            const auto found=std::find(roots.begin(),roots.end(),prefix);if(found==roots.end() || prefix.empty())throw SecurityDescriptorError{123};
            path=registry_security_handle(0x80000000U+static_cast<std::uint32_t>(found-roots.begin()),0).first;
            if(slash!=name.npos)path+=name.substr(slash);
            const auto kind=static_cast<std::uint32_t>(call(1));const auto view=registry_view(kind==12?0x200U:kind==13?0x100U:0U,x86_);
            path=registry_view_path(path,view);if(!runtime_.registry.keys.contains(path))throw SecurityDescriptorError{2};
            (void)registry_security_access(path,needed);
        }else path=registry_security_handle(call(0),needed).first;
        auto sd=parse_relative_descriptor(registry_security_descriptor(path));
        if(setting){
            SecurityDescriptor next;
            if(reg){if(!call(2))throw SecurityDescriptorError{87};next=read_descriptor(memory,call(2),x86_,true);if((flags&4U) && !(next.control&4U))throw SecurityDescriptorError{1338};}
            else{if(flags&1U)next.data[0]=read_sid(memory,call(3));if(flags&2U)next.data[1]=read_sid(memory,call(4));if(flags&4U){next.control|=4;if(call(5))next.data[3]=read_acl(memory,call(5)).bytes;}if(flags&16U){next.control|=16;if(call(6))next.data[2]=read_acl(memory,call(6)).bytes;}}
            if((flags&1U && next.data[0].empty()) || (flags&2U && next.data[1].empty()))throw SecurityDescriptorError{1337};
            if(flags&1U){sd.data[0]=next.data[0];sd.control&=static_cast<std::uint16_t>(~1U);}if(flags&2U){sd.data[1]=next.data[1];sd.control&=static_cast<std::uint16_t>(~2U);}
            if(flags&4U){sd.control|=4;sd.data[3]=next.data[3];}if(flags&16U){sd.control|=16;sd.data[2]=next.data[2];}
            if(flags&0x80000000U)sd.control|=0x1000U;if(flags&0x20000000U)sd.control&=static_cast<std::uint16_t>(~0x1000U);
            auto bytes=object_security_descriptor(std::move(sd),(flags&1U)!=0);std::map<std::u16string,Bytes> changes;changes.emplace(path,std::move(bytes));
            // SecurityInfo 更新繼承樹；RegSetKeySecurity 保留其低階、僅目前物件的契約。
            if(!reg && (flags&0xa0000014U)){
                const auto prefix=path+u"\\";
                for(auto item=runtime_.registry.keys.lower_bound(prefix);item!=runtime_.registry.keys.end() && item->first.starts_with(prefix);++item){
                    auto child=parse_relative_descriptor(registry_security_descriptor(item->first));const auto parent=item->first.substr(0,item->first.find_last_of(u'\\'));
                    const auto parent_sd=parse_relative_descriptor(changes.contains(parent)?changes.at(parent):registry_security_descriptor(parent));
                    auto inherited=inherit_descriptor(child,parent_sd,child.data[0],child.data[1],true);
                    if(!(flags&0xa0000004U)){inherited.data[3]=child.data[3];inherited.control=static_cast<std::uint16_t>((inherited.control&~0x140cU)|(child.control&0x140cU));}
                    if(!(flags&16U)){inherited.data[2]=child.data[2];inherited.control=static_cast<std::uint16_t>((inherited.control&~0x2830U)|(child.control&0x2830U));}
                    changes.emplace(item->first,relative_descriptor(inherited));
                }
            }
            for(auto& [key,descriptor]:changes)runtime_.registry.security_descriptors[key]=std::move(descriptor);
            if(const auto error=commit_registry_transaction(transaction)){runtime_.registry=original;throw SecurityDescriptorError{error};}
            for(const auto& [key,descriptor]:changes){(void)descriptor;poll_registry_notifications(false,key,8);}
        }else{
            if(!(flags&1U))sd.data[0].clear();if(!(flags&2U))sd.data[1].clear();
            if(!(flags&16U)){sd.data[2].clear();sd.control&=static_cast<std::uint16_t>(~0x30U);}
            if(!(flags&4U)){sd.data[3].clear();sd.control&=static_cast<std::uint16_t>(~12U);}
            const auto bytes=relative_descriptor(sd);
            if(reg){
                if(!call(3))throw SecurityDescriptorError{87};memory.check(GuestAddress{call(3)},4,Access::Write);const auto capacity=memory.load(GuestAddress{call(3)},32);
                if(call(2) && capacity>=bytes.size())memory.check(GuestAddress{call(2)},bytes.size(),Access::Write);
                memory.store(GuestAddress{call(3)},32,bytes.size());if(!call(2) || capacity<bytes.size())throw SecurityDescriptorError{122};memory.write(GuestAddress{call(2)},bytes);
            }else{
                if(!call(7))throw SecurityDescriptorError{87};for(unsigned i=3;i<=7;++i)if(call(i))memory.check(GuestAddress{call(i)},unit,Access::Write);
                const auto output=allocate_heap(memory,bytes.size());if(!output)throw SecurityDescriptorError{8};memory.write(GuestAddress{output},bytes);runtime_.local_allocations.insert(output);
                for(unsigned i=0;i<4;++i){const auto argument=i==2?6U:i==3?5U:i+3;if(!call(argument))continue;std::uint32_t at{};for(unsigned j=0;j<4;++j)at|=std::uint32_t{bytes[4+i*4+j]}<<(j*8);memory.store(GuestAddress{call(argument)},bits,at?output+at:0);}
                memory.store(GuestAddress{call(7)},bits,output);
            }
        }
    }catch(const SecurityDescriptorError& error){result=error.code;}catch(const MemoryFault&){result=998;}catch(const std::bad_alloc&){result=8;}
    call.return_to(state,result);return std::nullopt;
}
}
