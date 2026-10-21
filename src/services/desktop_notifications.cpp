#include "x64arm/desktop_notifications.hpp"
#include <algorithm>
namespace x64arm::desktop {
Result Icons::apply(std::uint32_t action,const Change& change){
    const auto& input=change.value;
    if(action>4 || input.flags&~255U)return {Status::Invalid,0};
    auto found=std::find_if(values_.begin(),values_.end(),[&](const auto& row){const auto& value=row.second;
        return input.guid?value.guid==input.guid:!value.guid && value.owner==input.owner && value.id==input.id;});
    if(action==0 && found!=values_.end())return {Status::Exists,found->first};
    if(action!=0 && found==values_.end())return {Status::Missing,0};
    if(action==2){const auto token=found->first;if(focused_==token)focused_=0;values_.erase(found);return {Status::Ok,token};}
    if(action==3){if(found->second.state&1)return {Status::Invalid,found->first};focused_=found->first;return {Status::Ok,found->first};}
    if(action==4){if(input.version!=0 && input.version!=3 && input.version!=4)return {Status::Invalid,found->first};found->second.version=input.version;return {Status::Ok,found->first};}
    if((input.flags&8) && ((input.state|change.state_mask)&~3U))return {Status::Invalid,0};
    if((input.flags&16) && ((input.info_flags&~0xbfU) || (input.info_flags&15)>4))return {Status::Invalid,0};
    if(action==0 && values_.size()>=1024)return {Status::Limit,0};
    Icon value=action==0?Icon{}:found->second;
    if(action==0){value.owner=input.owner;value.id=input.id;value.guid=input.guid;value.token=next_token_;value.version=0;}
    if(input.flags&1)value.callback=input.callback;
    if(input.flags&2)value.image=input.image;
    if(input.flags&4)value.tip=input.tip;
    if(input.flags&8)value.state=(value.state&~change.state_mask)|(input.state&change.state_mask);
    if(input.flags&16){value.info=input.info;value.title=input.title;value.info_flags=input.info_flags;value.balloon=input.balloon;value.timeout=std::clamp(input.timeout,10000U,30000U);++value.serial;}
    value.flags=(value.flags&~0xc0U)|(input.flags&0xc0U);
    const auto token=value.token;
    if(action==0){values_.emplace(token,std::move(value));++next_token_;}else found->second=std::move(value);
    return {Status::Ok,token};
}
std::optional<Message> Icons::event(const Event& input)const{
    const auto found=values_.find(input.token);Callback receiver;
    if(found==values_.end()){if(input.code!=0x403 || !input.retired)return {};receiver=*input.retired;}
    else{const auto& icon=found->second;receiver={icon.owner,icon.id,icon.callback,icon.version};
        if((icon.state&1) && ((input.code>=0x200 && input.code<=0x209) || input.code==0x7b || input.code==0x400 || input.code==0x401 || input.code==0x406 || input.code==0x407))return {};
    }
    if(!receiver.message)return {};
    Message result;result.owner=receiver.owner;result.message=receiver.message;
    if(receiver.version==4){result.wparam=static_cast<std::uint16_t>(input.point[0])|(std::uint64_t{static_cast<std::uint16_t>(input.point[1])}<<16);
        result.lparam=(input.code&0xffffU)|(std::uint64_t{receiver.id&0xffffU}<<16);}
    else{result.wparam=receiver.id;result.lparam=input.code;}return result;
}
void Icons::erase_owner(std::uint64_t owner){std::erase_if(values_,[&](const auto& row){return row.second.owner==owner;});if(!values_.contains(focused_))focused_=0;}
Status Attention::toggle(std::uint64_t window,bool invert,std::uint32_t flags){
    if(!window || !(flags&3) || flags&~3U)return Status::Invalid;
    const auto found=values_.find(window);
    if(!invert || (found!=values_.end() && found->second.phase)){values_.erase(window);return Status::Ok;}
    if(found==values_.end() && values_.size()>=4096)return Status::Limit;
    values_.insert_or_assign(window,Flash{flags,0,0,UINT64_MAX,true});return Status::Ok;
}
Status Attention::set(std::uint64_t window,std::uint32_t flags,std::uint32_t count,std::uint32_t timeout,std::uint32_t caret,std::uint64_t now){
    if(!window || flags&~15U || ((flags&8) && !(flags&4)))return Status::Invalid;
    if(!(flags&3) || (!(flags&4) && !count)){values_.erase(window);return Status::Ok;}
    if(!values_.contains(window) && values_.size()>=4096)return Status::Limit;
    const auto interval=std::uint64_t{timeout?timeout:std::max(caret,1U)}*10000;
    const auto due=now>UINT64_MAX-interval?UINT64_MAX:now+interval;
    values_.insert_or_assign(window,Flash{flags,count,interval,due,true});return Status::Ok;
}
bool Attention::caption(std::uint64_t window,bool active)const{
    const auto it=values_.find(window);return active!=(it!=values_.end() && (it->second.flags&1) && it->second.phase);
}
bool Attention::tick(std::uint64_t now,std::uint64_t foreground){
    bool changed=false;
    for(auto it=values_.begin();it!=values_.end();){
        auto& value=it->second;
        if((value.flags&8) && foreground==it->first){it=values_.erase(it);changed=true;continue;}
        if(value.interval && value.deadline<=now && value.deadline!=UINT64_MAX){
            const auto steps=1+(now-value.deadline)/value.interval;
            if(!(value.flags&4)){
                const auto off=steps/2+((steps&1) && value.phase?1U:0U);
                if(off>=value.remaining){it=values_.erase(it);changed=true;continue;}
                value.remaining-=static_cast<std::uint32_t>(off);
            }
            if(steps&1)value.phase=!value.phase;
            const auto periods=steps>UINT64_MAX/value.interval?UINT64_MAX:steps*value.interval;
            value.deadline=value.deadline>UINT64_MAX-periods?UINT64_MAX:value.deadline+periods;changed=true;
        }++it;
    }return changed;
}
std::uint64_t Attention::deadline()const{std::uint64_t next=UINT64_MAX;for(const auto& [id,value]:values_){(void)id;next=std::min(next,value.deadline);}return next;}
}
