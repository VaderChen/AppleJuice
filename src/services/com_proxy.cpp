#include "x64arm/com_proxy.hpp"

namespace x64arm::com_proxy {
const Entry* Registry::get(std::uint64_t address) const {
    const auto found=entries_.find(address);return found==entries_.end()?nullptr:&found->second;
}
std::uint64_t Registry::find(const Key& key) const {
    const auto found=keys_.find(key);return found==keys_.end()?0:found->second;
}
Result Registry::retire_owned(Origin origin){
    if(!origin.owner || !origin.object)return {Status::Invalid};
    if(next_==UINT64_MAX)return {Status::Limit};
    const auto id=next_;retirements_.emplace(id,Retirement{origin});++next_;
    return {Status::Ok,0,id};
}
Result Registry::adopt(const Key& key,std::uint64_t header){
    if(!key.owner || !key.apartment || !key.original || !header || static_cast<unsigned>(key.kind)>static_cast<unsigned>(Kind::EnumString))return {Status::Invalid};
    if(const auto cached=find(key)){
        auto& entry=entries_.at(cached);
        if(!entry.connected)return {Status::Disconnected};
        if(entry.references==UINT32_MAX)return {Status::Limit};
        // QI／輸出參數已取得一份來源引用；既有快取保留原引用，多出的一份須在 owner 釋放。
        const auto retired=retire_owned({key.owner,key.original});if(retired.status!=Status::Ok)return retired;
        ++entry.references;return {Status::Ok,cached,retired.retirement,entry.references};
    }
    if(entries_.size()>=limit_ || entries_.contains(header))return {Status::Limit};
    // 兩個索引須一起提交，配置失敗不取得呼叫者交來的來源引用。
    const auto inserted=entries_.emplace(header,Entry{key});
    try {keys_.emplace(key,header);}catch(...){entries_.erase(inserted.first);throw;}
    return {Status::Ok,header,0,1};
}
Result Registry::add_ref(std::uint64_t address){
    const auto found=entries_.find(address);if(found==entries_.end())return {Status::Missing};
    auto& entry=found->second;if(entry.references==UINT32_MAX)return {Status::Limit};
    // 斷線後的 IUnknown 仍可維持本地代理的生命週期。
    ++entry.references;return {Status::Ok,address,0,entry.references};
}
Result Registry::dispose(std::map<std::uint64_t,Entry>::iterator found){
    auto& entry=found->second;if(entry.references || entry.pins)return {Status::Busy,found->first,0,entry.references};
    std::uint64_t retirement=0;
    if(entry.origin_owned){const auto retired=retire_owned({entry.key.owner,entry.key.original});if(retired.status!=Status::Ok)return retired;retirement=retired.retirement;}
    const auto header=found->first;keys_.erase(entry.key);entries_.erase(found);
    return {Status::Ok,0,retirement,0,Disposal{header,retirement}};
}
Result Registry::release(std::uint64_t address){
    const auto found=entries_.find(address);if(found==entries_.end())return {Status::Missing};
    auto& entry=found->second;if(!entry.references)return {Status::Invalid};
    --entry.references;
    if(!entry.references && !entry.pins){
        try {const auto result=dispose(found);if(result.status!=Status::Ok)++entry.references;return result;}
        catch(...){++entry.references;throw;}
    }
    return {Status::Ok,address,0,entry.references};
}
Result Registry::pin(std::uint64_t address,std::uint64_t apartment){
    const auto found=entries_.find(address);if(found==entries_.end())return {Status::Missing};
    auto& entry=found->second;if(!entry.connected)return {Status::Disconnected};
    if(entry.key.apartment!=apartment)return {Status::WrongApartment};
    if(!entry.references)return {Status::Invalid};
    if(entry.pins==UINT32_MAX)return {Status::Limit};
    ++entry.pins;return {Status::Ok,address,0,entry.references};
}
Result Registry::unpin(std::uint64_t address){
    const auto found=entries_.find(address);if(found==entries_.end())return {Status::Missing};
    auto& entry=found->second;if(!entry.pins)return {Status::Invalid};
    --entry.pins;
    if(!entry.pins && !entry.references){
        try {const auto result=dispose(found);if(result.status!=Status::Ok)++entry.pins;return result;}
        catch(...){++entry.pins;throw;}
    }
    return {Status::Ok,address,0,entry.references};
}
std::optional<Origin> Registry::start_retirement(std::uint64_t id){
    const auto found=retirements_.find(id);if(found==retirements_.end() || !found->second.held)return {};
    found->second.held=false;found->second.started=true;return found->second.origin;
}
bool Registry::finish_retirement(std::uint64_t id){
    const auto found=retirements_.find(id);
    // 尚未在來源開始 Release 的工作仍持有引用，不能因完成通知而直接丟棄。
    if(found==retirements_.end() || found->second.held)return false;
    retirements_.erase(found);return true;
}
Disconnect Registry::disconnect(std::uint64_t owner){
    Disconnect result;
    // 先配置輸出，再改 ownership；無法配置時不得遺失待 Release 的引用。
    std::size_t count=0;
    for(const auto& [address,entry]:entries_){(void)address;if(entry.key.owner==owner && entry.origin_owned)++count;}
    for(const auto& [id,retirement]:retirements_){(void)id;if(retirement.origin.owner==owner && retirement.held)++count;}
    result.releases.reserve(count);
    for(auto& [address,entry]:entries_){
        (void)address;if(entry.key.owner!=owner)continue;
        entry.connected=false;if(entry.origin_owned){entry.origin_owned=false;result.releases.push_back({owner,entry.key.original});}
    }
    for(auto& [id,retirement]:retirements_){
        (void)id;if(retirement.origin.owner!=owner || !retirement.held)continue;
        retirement.held=false;result.releases.push_back(retirement.origin);
    }
    return result;
}
bool Registry::valid() const {
    if(entries_.size()!=keys_.size() || entries_.size()>limit_)return false;
    for(const auto& [address,entry]:entries_){
        if(!address || (!entry.references && !entry.pins) || !entry.key.owner || !entry.key.apartment || !entry.key.original)return false;
        const auto key=keys_.find(entry.key);if(key==keys_.end() || key->second!=address)return false;
        if(entry.connected!=entry.origin_owned)return false;
    }
    for(const auto& [id,retirement]:retirements_)if(!id || !retirement.origin.owner || !retirement.origin.object || id>=next_ || (retirement.held && retirement.started))return false;
    return true;
}
}
