#include "x64arm/debug_events.hpp"
#include <algorithm>
#include <limits>
#include <type_traits>

namespace x64arm::debugger {
namespace {
void require(bool value,std::uint32_t code=87){if(!value)throw Error{code};}
}
std::vector<std::uint8_t> encode(const Event& event,ExecutionMode mode){
    require(event.pid && event.tid && !event.payload.valueless_by_exception());
    const bool small=mode==ExecutionMode::X86;const std::size_t base=small?12:16,unit=small?4:8;
    std::vector<std::uint8_t> bytes(small?96:176);
    const auto put=[&](std::size_t at,unsigned count,std::uint64_t value){for(unsigned i=0;i<count;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));};
    const auto pointer=[&](std::size_t at,std::uint64_t value){require(!small || value<=UINT32_MAX,299);put(base+at,static_cast<unsigned>(unit),value);};
    const auto value=[&](std::size_t at,std::uint32_t number){put(base+at,4,number);};
    put(0,4,event.payload.index()+1);put(4,4,event.pid);put(8,4,event.tid);
    std::visit([&](const auto& data){using T=std::decay_t<decltype(data)>;
        if constexpr(std::is_same_v<T,Exception>){
            require(data.parameters.size()<=15);value(0,data.code);value(4,data.flags);pointer(8,data.record);pointer(8+unit,data.address);
            value(8+unit*2,static_cast<std::uint32_t>(data.parameters.size()));const auto parameters=small?20U:32U;
            for(std::size_t i=0;i<data.parameters.size();++i)pointer(parameters+i*unit,data.parameters[i]);value(small?80:152,data.first_chance);
        }else if constexpr(std::is_same_v<T,CreateThread>){pointer(0,data.thread);pointer(unit,data.teb);pointer(unit*2,data.start);
        }else if constexpr(std::is_same_v<T,CreateProcess>){
            pointer(0,data.file);pointer(unit,data.process);pointer(unit*2,data.thread);pointer(unit*3,data.base);value(unit*4,data.debug_offset);value(unit*4+4,data.debug_size);
            pointer(unit*4+8,data.teb);pointer(unit*5+8,data.start);pointer(unit*6+8,data.image_name);put(base+unit*7+8,2,data.unicode);
        }else if constexpr(std::is_same_v<T,ExitThread> || std::is_same_v<T,ExitProcess>){value(0,data.code);
        }else if constexpr(std::is_same_v<T,LoadDll>){pointer(0,data.file);pointer(unit,data.base);value(unit*2,data.debug_offset);value(unit*2+4,data.debug_size);pointer(unit*2+8,data.image_name);put(base+unit*3+8,2,data.unicode);
        }else if constexpr(std::is_same_v<T,UnloadDll>){pointer(0,data.base);
        }else if constexpr(std::is_same_v<T,DebugString>){pointer(0,data.address);put(base+unit,2,data.unicode);put(base+unit+2,2,data.length);
        }else if constexpr(std::is_same_v<T,Rip>){value(0,data.error);value(4,data.type);}
    },event.payload);return bytes;
}
Event decode(std::span<const std::uint8_t> bytes,ExecutionMode mode){
    const bool small=mode==ExecutionMode::X86;const std::size_t base=small?12:16,unit=small?4:8;
    require(bytes.size()==(small?96U:176U),13);
    const auto get=[&](std::size_t at,std::size_t count){require(at<=bytes.size() && count<=bytes.size()-at,13);std::uint64_t value=0;for(std::size_t i=0;i<count;++i)value|=std::uint64_t{bytes[at+i]}<<(i*8);return value;};
    const auto pointer=[&](std::size_t at){return get(base+at,unit);};
    const auto value=[&](std::size_t at){return static_cast<std::uint32_t>(get(base+at,4));};
    Event event{static_cast<std::uint32_t>(get(4,4)),static_cast<std::uint32_t>(get(8,4)),ExitProcess{}};
    require(event.pid && event.tid,13);
    switch(get(0,4)){
    case 1:{Exception data;data.code=value(0);data.flags=value(4);data.record=pointer(8);data.address=pointer(8+unit);const auto count=value(8+unit*2);require(count<=15,13);
        for(std::size_t i=0;i<count;++i)data.parameters.push_back(pointer((small?20U:32U)+i*unit));data.first_chance=value(small?80:152)!=0;event.payload=std::move(data);break;}
    case 2:event.payload=CreateThread{pointer(0),pointer(unit),pointer(unit*2)};break;
    case 3:{CreateProcess data;data.file=pointer(0);data.process=pointer(unit);data.thread=pointer(unit*2);data.base=pointer(unit*3);data.debug_offset=value(unit*4);data.debug_size=value(unit*4+4);
        data.teb=pointer(unit*4+8);data.start=pointer(unit*5+8);data.image_name=pointer(unit*6+8);data.unicode=get(base+unit*7+8,2)!=0;event.payload=data;break;}
    case 4:event.payload=ExitThread{value(0)};break;
    case 5:event.payload=ExitProcess{value(0)};break;
    case 6:{LoadDll data;data.file=pointer(0);data.base=pointer(unit);data.debug_offset=value(unit*2);data.debug_size=value(unit*2+4);data.image_name=pointer(unit*2+8);data.unicode=get(base+unit*3+8,2)!=0;event.payload=data;break;}
    case 7:event.payload=UnloadDll{pointer(0)};break;
    case 8:event.payload=DebugString{pointer(0),static_cast<std::uint16_t>(get(base+unit+2,2)),get(base+unit,2)!=0};break;
    case 9:event.payload=Rip{value(0),value(4)};break;
    default:throw Error{13};
    }
    // 這是內部 stream 的固定格式；拒絕非 canonical padding／布林值，不能把損毀封包交給 guest。
    const auto canonical=encode(event,mode);require(std::equal(bytes.begin(),bytes.end(),canonical.begin()),13);return event;
}
void EventQueue::check_owner(std::uint64_t owner)const{require(owner && owner==owner_,5);}
bool EventQueue::attached(std::uint32_t pid,std::uint64_t birth)const{const auto found=sessions_.find(pid);return found!=sessions_.end() && found->second==birth;}
void EventQueue::attach(std::uint32_t pid,std::uint64_t birth,std::uint64_t owner){
    require(pid && birth && owner);require(!sessions_.contains(pid) && (!owner_ || owner==owner_),5);require(sessions_.size()<64,8);
    sessions_.emplace(pid,birth);owner_=owner;
}
void EventQueue::detach(std::uint32_t pid,std::uint64_t birth,std::uint64_t owner){
    check_owner(owner);require(attached(pid,birth),87);sessions_.erase(pid);delivered_.erase(pid);std::erase_if(pending_,[&](const auto& item){return item.event.pid==pid;});if(sessions_.empty())owner_=0;
}
std::uint64_t EventQueue::push(std::uint32_t pid,std::uint64_t birth,Event event){
    require(attached(pid,birth) && event.pid==pid && event.tid);require(pending_.size()+delivered_.size()<4096 && next_!=UINT64_MAX,8);
    (void)encode(event,ExecutionMode::X64);const auto sequence=next_;pending_.push_back({sequence,birth,std::move(event)});++next_;return sequence;
}
const Envelope* EventQueue::offer(std::uint64_t owner)const{
    check_owner(owner);const auto found=std::find_if(pending_.begin(),pending_.end(),[&](const auto& item){return !delivered_.contains(item.event.pid);});return found==pending_.end()?nullptr:&*found;
}
const Envelope& EventQueue::deliver(std::uint64_t owner,std::uint64_t sequence){
    const auto candidate=offer(owner);require(candidate && candidate->sequence==sequence);const auto pid=candidate->event.pid;
    const auto [found,inserted]=delivered_.emplace(pid,*candidate);require(inserted);
    std::erase_if(pending_,[&](const auto& item){return item.sequence==sequence;});return found->second;
}
Envelope EventQueue::continue_event(std::uint64_t owner,std::uint32_t pid,std::uint32_t tid,std::uint32_t status){
    check_owner(owner);require(status==handled || status==not_handled || status==reply_later);require(status!=reply_later,50);
    const auto found=delivered_.find(pid);require(found!=delivered_.end() && found->second.event.tid==tid);
    auto result=found->second;delivered_.erase(found);return result;
}
}
