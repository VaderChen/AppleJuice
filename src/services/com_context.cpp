#include "x64arm/com_context.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace x64arm::com_context {
EncodedProperty encode_property(const Property& property,bool win32){
    if(win32 && property.object>UINT32_MAX)throw std::out_of_range("ContextProperty 指標無法表示為 Win32");
    EncodedProperty result;result.size=win32?24:32;std::copy(property.id.begin(),property.id.end(),result.bytes.begin());
    for(unsigned i=0;i<4;++i)result.bytes[16+i]=static_cast<std::uint8_t>(property.flags>>(i*8));
    const unsigned start=win32?20:24,width=win32?4:8;
    for(unsigned i=0;i<width;++i)result.bytes[start+i]=static_cast<std::uint8_t>(property.object>>(i*8));return result;
}
std::optional<Property> decode_property(std::span<const std::uint8_t> bytes,bool win32){
    if(bytes.size()!=(win32?24U:32U))return {};Property result;std::copy_n(bytes.begin(),16,result.id.begin());
    for(unsigned i=0;i<4;++i)result.flags|=static_cast<std::uint32_t>(bytes[16+i])<<(i*8);
    const unsigned start=win32?20:24,width=win32?4:8;
    for(unsigned i=0;i<width;++i)result.object|=static_cast<std::uint64_t>(bytes[start+i])<<(i*8);return result;
}
Enumeration::Enumeration(std::vector<Property> properties):properties_(std::move(properties)){}
std::span<const Property> Enumeration::next(std::size_t count){
    const auto available=std::min(count,properties_.size()-position_);
    const auto result=std::span<const Property>(properties_).subspan(position_,available);
    position_+=available;return result;
}
std::size_t Enumeration::skip(std::size_t count){const auto advanced=std::min(count,properties_.size()-position_);position_+=advanced;return advanced;}
void Enumeration::reset() noexcept{position_=0;}
std::size_t Enumeration::position() const noexcept{return position_;}
const std::vector<Property>& Enumeration::properties() const noexcept{return properties_;}
Status State::create(std::uint64_t owner,std::uint64_t token){
    if(!token)return Status::Invalid;
    if(active_.contains(owner) || contexts_.contains(token))return Status::Occupied;
    if(contexts_.size()>=max_contexts)return Status::Limit;
    const auto inserted=contexts_.emplace(token,Context{owner}).first;
    try{active_.emplace(owner,token);}catch(...){contexts_.erase(inserted);throw;}return Status::Ok;
}
std::optional<std::uint64_t> State::current(std::uint64_t owner) const{const auto found=active_.find(owner);return found==active_.end()?std::nullopt:std::optional{found->second};}
const Context* State::find(std::uint64_t token) const noexcept{const auto found=contexts_.find(token);return found==contexts_.end()?nullptr:&found->second;}
Reference State::add_ref(std::uint64_t token){
    const auto found=contexts_.find(token);if(found==contexts_.end())return {Status::Missing};
    if(found->second.references==std::numeric_limits<std::uint32_t>::max())return {Status::Limit,found->second.references};
    return {Status::Ok,++found->second.references};
}
Reference State::drop(std::map<std::uint64_t,Context>::iterator found){
    auto& context=found->second;
    if(context.references>1){--context.references;return {Status::Ok,context.references};}
    // 先完成所有可能配置，避免 bad_alloc 留下 refs=0 的半完成物件。
    Disposal disposal;disposal.token=found->first;disposal.properties.reserve(context.properties.size());
    for(const auto& [id,property]:context.properties){(void)id;disposal.properties.push_back(property);}
    if(context.active)active_.erase(context.owner);contexts_.erase(found);return {Status::Ok,0,std::move(disposal)};
}
Reference State::release(std::uint64_t token){const auto found=contexts_.find(token);return found==contexts_.end()?Reference{Status::Missing}:drop(found);}
Reference State::retire(std::uint64_t owner){
    const auto active=active_.find(owner);if(active==active_.end())return {Status::Missing};
    const auto found=contexts_.find(active->second);
    auto result=drop(found);if(!result.disposal){found->second.active=false;active_.erase(active);}return result;
}
Change State::set(std::uint64_t token,Property property){
    const auto found=contexts_.find(token);if(found==contexts_.end())return {Status::Missing};
    auto& context=found->second;if(context.frozen)return {Status::Frozen};if(!property.object)return {Status::Invalid};
    const auto previous=context.properties.find(property.id);
    if(previous!=context.properties.end()){auto result=previous->second;previous->second=std::move(property);return {Status::Ok,std::move(result)};}
    if(context.properties.size()>=max_properties)return {Status::Limit};context.properties.emplace(property.id,std::move(property));return {};
}
Change State::remove(std::uint64_t token,const WindowsGuid& id){
    const auto found=contexts_.find(token);if(found==contexts_.end())return {Status::Missing};auto& context=found->second;
    if(context.frozen)return {Status::Frozen};const auto property=context.properties.find(id);if(property==context.properties.end())return {Status::Missing};
    auto previous=property->second;context.properties.erase(property);return {Status::Ok,std::move(previous)};
}
std::optional<Property> State::get(std::uint64_t token,const WindowsGuid& id) const{
    const auto context=find(token);if(!context)return {};const auto found=context->properties.find(id);return found==context->properties.end()?std::nullopt:std::optional{found->second};
}
Status State::freeze(std::uint64_t token){const auto found=contexts_.find(token);if(found==contexts_.end())return Status::Missing;found->second.frozen=true;return Status::Ok;}
std::optional<Enumeration> State::enumerate(std::uint64_t token) const{
    const auto context=find(token);if(!context)return {};std::vector<Property> values;values.reserve(context->properties.size());
    for(const auto& [id,property]:context->properties){(void)id;values.push_back(property);}return Enumeration{std::move(values)};
}
}
