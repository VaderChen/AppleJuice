#pragma once
#include "x64arm/certificates.hpp"
#include "x64arm/readonly_files.hpp"
#include "certificate_packet.hpp"
#include <algorithm>

namespace x64arm::certificate_detail {
void refresh_store(CertificateState&,std::uint64_t,bool automatic,ReadOnlyFiles&);
struct StoreEntry {
    std::uint64_t identity{},store{},source_identity{},properties{};
    const std::vector<std::uint8_t>* der{};
};
inline std::uint64_t collection_alias(CertificateStore& store,std::uint64_t sibling,std::uint64_t identity){
    const auto key=std::pair{sibling,identity};if(const auto found=store.aliases.find(key);found!=store.aliases.end())return found->second;
    if(store.aliases.size()>=65536 || store.next_identity==UINT64_MAX)throw CertificateError{8};
    const auto value=store.next_identity;store.aliases.emplace(key,value);++store.next_identity;return value;
}
inline std::vector<StoreEntry> store_entries(CertificateState& state,std::uint64_t handle,unsigned depth=0,bool crls=false){
    if(depth>=128)throw CertificateError{87};auto& store=state.stores.at(handle);std::vector<StoreEntry> entries;
    if(!store.collection){const auto& items=crls?store.crls:store.items;const auto& properties=crls?store.crl_property_identities:store.property_identities;
        entries.reserve(items.size());for(const auto& [identity,der]:items)entries.push_back({identity,handle,identity,properties.at(identity),&der});}
    else for(const auto& sibling:store.siblings){auto children=store_entries(state,sibling.store,depth+1,crls);if(children.size()>65536-entries.size())throw CertificateError{8};
        for(auto& child:children){child.identity=collection_alias(store,sibling.identity,child.identity);entries.push_back(child);}}
    return entries;
}
inline bool store_contains(const CertificateState& state,std::uint64_t handle,std::uint64_t wanted){
    if(handle==wanted)return true;for(const auto& sibling:state.stores.at(handle).siblings)if(store_contains(state,sibling.store,wanted))return true;return false;
}
struct StoreTarget {
    std::uint64_t store{};
    std::vector<std::pair<std::uint64_t,std::uint64_t>> path;
};
inline void write_targets(const CertificateState& state,std::uint64_t handle,std::vector<StoreTarget>& targets,std::vector<std::pair<std::uint64_t,std::uint64_t>> path={}){
    const auto& store=state.stores.at(handle);if(store.readonly)return;
    if(!store.collection){targets.push_back({handle,std::move(path)});return;}
    for(const auto& sibling:store.siblings)if(sibling.writable){auto next=path;next.emplace_back(handle,sibling.identity);write_targets(state,sibling.store,targets,std::move(next));}
}
inline std::uint64_t target_identity(CertificateState& state,const StoreTarget& target,std::uint64_t identity){
    for(auto it=target.path.rbegin();it!=target.path.rend();++it)identity=collection_alias(state.stores.at(it->first),it->second,identity);return identity;
}
inline void prune_stores(CertificateState& state){
    bool changed;do {changed=false;for(auto it=state.stores.begin();it!=state.stores.end();){const auto handle=it->first;const auto& store=it->second;
        const auto retained=[&](const auto& contexts){return std::any_of(contexts.begin(),contexts.end(),[&](const auto& item){return item.second.store==handle || item.second.source_store==handle;});};
        const bool active=retained(state.contexts) || retained(state.crl_contexts);
        if(store.references || store.engine_references || store.collection_references || active){++it;continue;}
        for(const auto& sibling:store.siblings)--state.stores.at(sibling.store).collection_references;
        it=state.stores.erase(it);changed=true;
    }}while(changed);
}
}
