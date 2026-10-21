#include "x64arm/network_files.hpp"
#include <algorithm>

namespace x64arm {
std::vector<std::shared_ptr<NetworkFileBinding>> NetworkFiles::connections(const std::string& scope)const{std::lock_guard lock(mutex_);const auto found=sessions_.find(scope);return found==sessions_.end()?std::vector<std::shared_ptr<NetworkFileBinding>>{}:found->second;}
NetworkFileLocation NetworkFiles::locate(const std::string& scope,std::u16string_view input)const{
    const auto path=network_resources::canonical(std::u16string(input));std::lock_guard lock(mutex_);const auto session=sessions_.find(scope);const std::vector<std::shared_ptr<NetworkFileBinding>> empty;const auto& values=session==sessions_.end()?empty:session->second;
    if(path.size()>=2 && path[1]==u':'){
        auto letter=path[0];if(letter>=u'a' && letter<=u'z')letter=static_cast<char16_t>(letter-32);
        for(const auto& row:values)if(row->drive && letter==static_cast<char16_t>(row->drive)){
            if(path.size()>2 && path[2]!=u'\\')return {{},{},123,true,{}};return {row,path.size()>3?std::u16string(input.substr(3)):std::u16string{},0,true,lease(row)};
        }
        return {};
    }
    const auto parsed=network_resources::parse_path(input);if(!parsed)return {{},{},path.starts_with(u"\\\\")?123U:0U,path.starts_with(u"\\\\"),{}};
    const auto root=network_resources::canonical(parsed->remote());for(const auto& row:values)if(network_resources::canonical(row->remote)==root)return {row,parsed->system.empty()?std::u16string{}:parsed->system.substr(1),0,true,lease(row)};
    return {{},{},53,true,{}};
}
std::uint32_t NetworkFiles::add(const std::string& scope,std::shared_ptr<NetworkFileBinding> row){
    if(!row || !row->mount || row->mount->root().empty() || !network_resources::parse_path(row->remote))return 87;
    std::lock_guard lock(mutex_);auto& values=sessions_[scope];
    for(const auto& old:values){if(row->drive && row->drive==old->drive)return 85;const auto a=network_resources::parse_path(old->remote),b=network_resources::parse_path(row->remote);if(a && b && network_resources::canonical(a->server)==network_resources::canonical(b->server) && network_resources::canonical(old->user)!=network_resources::canonical(row->user))return 1219;}
    if(values.size()>=256)return 8;values.push_back(std::move(row));return 0;
}
std::uint32_t NetworkFiles::remove(const std::string& scope,std::u16string_view name,bool force){
    const auto key=network_resources::canonical(std::u16string(name));std::lock_guard lock(mutex_);const auto session=sessions_.find(scope);if(session==sessions_.end())return 2250;
    auto& values=session->second;const auto found=std::find_if(values.begin(),values.end(),[&](const auto& row){return network_resources::canonical(row->remote)==key || (row->drive && key==std::u16string(1,static_cast<char16_t>(row->drive+32))+u":");});
    if(found==values.end())return 2250;if(!force && (*found)->leases.load(std::memory_order_acquire))return 2404;
    // 相同 share 的其他本機名稱仍可能使用此掛載；最後一個連線才關閉原生資源。
    const auto mount=(*found)->mount;const bool last=std::count_if(values.begin(),values.end(),[&](const auto& row){return row->mount==mount;})==1;
    if(last){const auto error=mount->disconnect(force);if(error)return error;}values.erase(found);return 0;
}
std::shared_ptr<void> NetworkFiles::lease(std::shared_ptr<NetworkFileBinding> binding){
    struct Lease {std::shared_ptr<NetworkFileBinding> row;explicit Lease(std::shared_ptr<NetworkFileBinding> value):row(std::move(value)){row->leases.fetch_add(1,std::memory_order_acq_rel);}~Lease(){row->leases.fetch_sub(1,std::memory_order_acq_rel);}};
    return std::make_shared<Lease>(std::move(binding));
}
}
