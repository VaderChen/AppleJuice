#include "x64arm/com_error.hpp"
#include <algorithm>
namespace x64arm::com_error {
Reporting reporting(std::uint32_t flags,bool debugger,bool com_initialized){
    return {bool((flags&force_exceptions) || (debugger && !(flags&suppress_exceptions))),bool(com_initialized && (flags&use_set_error_info) && !(flags&suppress_set_error_info))};
}
std::u16string reported_message(std::uint32_t hresult,std::optional<std::u16string_view> message,std::u16string_view fallback){
    if(!(hresult&0x80000000U))return {};
    const auto text=message.value_or(fallback);const auto nul=text.find(u'\0');
    return std::u16string{text.substr(0,std::min<std::size_t>(maximum_message_units,nul==text.npos?text.size():nul))};
}
Status State::create(std::uint64_t token,Object object){
    if(!token || objects_.contains(token))return Status::Invalid;
    if(objects_.size()>=4096)return Status::Limit;
    object.references=1;objects_.emplace(token,std::move(object));return Status::Ok;
}
Object* State::find(std::uint64_t token){const auto it=objects_.find(token);return it==objects_.end()?nullptr:&it->second;}
const Object* State::find(std::uint64_t token) const {const auto it=objects_.find(token);return it==objects_.end()?nullptr:&it->second;}
Reference State::add_ref(std::uint64_t token){
    auto* object=find(token);if(!object)return {Status::Missing,0,false};
    if(object->references==UINT32_MAX)return {Status::Limit,UINT32_MAX,false};
    return {Status::Ok,++object->references,false};
}
Reference State::release(std::uint64_t token){
    auto* object=find(token);if(!object)return {Status::Missing,0,false};
    if(!object->references)return {Status::Invalid,0,false};
    const auto count=--object->references;
    if(!count)objects_.erase(token);
    return {Status::Ok,count,!count};
}
std::uint64_t State::current(std::uint64_t thread) const {const auto it=channels_.find(thread);return it==channels_.end()?0:it->second;}
std::uint64_t State::replace(std::uint64_t thread,std::uint64_t pointer){
    const auto old=current(thread);
    if(pointer)channels_.insert_or_assign(thread,pointer);else channels_.erase(thread);
    return old;
}
std::uint64_t State::take(std::uint64_t thread){return replace(thread,0);}
Status State::set_flags(std::uint32_t flags){if(flags&~15U)return Status::Invalid;flags_=flags;return Status::Ok;}
}
