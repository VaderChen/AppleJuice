#pragma once
#include "x64arm/windows_runtime.hpp"
#include "x64arm/windows_abi.hpp"
#include "x64arm/memory.hpp"
#include <algorithm>
#include <bit>
#include <map>

namespace x64arm::msvc_eh {
// FH3／FH4 只在磁碟格式上不同；展開、型別比對與 catch 物件共用這些資料。
struct CatchEntry {
    std::uint32_t flags{};std::uint64_t type{},handler{};
    std::int64_t object_offset{};std::int32_t parent_offset{};
    std::array<std::uint64_t,2> continuations{};unsigned continuation_count{};
};
struct TryEntry {std::int32_t low{},high{},catch_high{};std::vector<CatchEntry> handlers;};
struct UnwindEntry {
    enum class Kind : std::uint8_t {None,Object,ObjectPointer,Funclet};
    std::int32_t next{-1};std::uint64_t action{};Kind kind{Kind::None};std::uint32_t object{};
};
struct FunctionInfo {
    std::uint64_t address{};std::int32_t help{};std::uint32_t flags{},parent_offset{};
    bool compressed{},catch_funclet{};
    std::vector<UnwindEntry> unwind;
    std::vector<TryEntry> tries;std::vector<std::pair<std::uint32_t,std::int32_t>> ips;
};
inline std::uint64_t metadata_address(const GuestMemory& memory,const RuntimeImage& image,std::uint64_t offset,std::size_t size){
    if(offset>image.size || size>image.size-offset || offset>UINT64_MAX-image.base)throw HostCallFault("MSVC EH metadata 超出所屬映像");
    const auto at=image.base+offset;memory.check(GuestAddress{at},size,Access::Read);return at;
}
class PackedReader {
public:
    PackedReader(const GuestMemory& memory,const RuntimeImage& image,std::uint64_t address):memory_(memory),image_(image),at_(address){
        if(address<image.base)throw HostCallFault("FH4 metadata 位址不在映像內");
        (void)metadata_address(memory,image,address-image.base,1);
    }
    std::uint64_t position()const noexcept{return at_;}
    std::uint8_t byte(){(void)metadata_address(memory_,image_,at_-image_.base,1);const auto value=static_cast<std::uint8_t>(memory_.load(GuestAddress{at_},8));++at_;return value;}
    std::uint32_t fixed(){(void)metadata_address(memory_,image_,at_-image_.base,4);const auto value=static_cast<std::uint32_t>(memory_.load(GuestAddress{at_},32));at_+=4;return value;}
    std::uint32_t number(){
        const auto first=byte();unsigned length=1;while(length<5 && (first&(1U<<(length-1))))++length;
        if(length==5)return fixed();
        std::uint32_t value=first;for(unsigned i=1;i<length;++i)value|=std::uint32_t(byte())<<(8*i);
        return value>>length;
    }
    std::uint32_t count(std::uint32_t limit){const auto value=number();if(value>limit)throw HostCallFault("FH4 metadata 數量過大");return value;}
private:
    const GuestMemory& memory_;const RuntimeImage& image_;std::uint64_t at_;
};
inline FunctionInfo function_info4(const GuestMemory& memory,const RuntimeImage& image,std::uint64_t address,std::uint32_t function_begin){
    FunctionInfo result;result.address=address;result.compressed=true;PackedReader input(memory,image,address);
    const auto header=input.byte();if(header&0x80U)throw HostCallFault("FH4 FuncInfo 保留位無效");
    result.catch_funclet=(header&1U)!=0;result.flags=((header&0x20U)?1U:0U)|((header&0x40U)?4U:0U);
    if(header&4U)(void)input.number(); // BBT metadata 不改變 C++ 例外語義。
    const auto unwind=(header&8U)?input.fixed():0U,tries=(header&16U)?input.fixed():0U;
    auto ips=input.fixed();if(result.catch_funclet)result.parent_offset=input.number();
    if(header&2U){
        PackedReader segments(memory,image,metadata_address(memory,image,ips,1));const auto count=segments.count(65536);ips=0;
        for(std::uint32_t i=0;i<count;++i){const auto begin=segments.fixed(),table=segments.fixed();if(begin>=image.size)throw HostCallFault("FH4 分段函式 RVA 無效");
            if(begin==function_begin){if(ips)throw HostCallFault("FH4 分段 IP table 重複");ips=table;}}
    }
    if(unwind){
        PackedReader table(memory,image,metadata_address(memory,image,unwind,1));const auto count=table.count(65536);const auto first=table.position();
        std::map<std::uint64_t,std::int32_t> offsets;
        for(std::uint32_t i=0;i<count;++i){
            const auto position=table.position();const auto encoded=table.number(),back=encoded>>2;
            if(!back || back>position-image.base)throw HostCallFault("FH4 unwind offset 不向外層推進");
            const auto destination=position-back;UnwindEntry entry;entry.kind=static_cast<UnwindEntry::Kind>(encoded&3U);
            if(destination>=first){const auto found=offsets.find(destination);if(found==offsets.end())throw HostCallFault("FH4 unwind offset 未指向先前項目");entry.next=found->second;}
            if(entry.kind!=UnwindEntry::Kind::None){const auto action=table.fixed();if(action)entry.action=metadata_address(memory,image,action,1);}
            if(entry.kind==UnwindEntry::Kind::Object || entry.kind==UnwindEntry::Kind::ObjectPointer)entry.object=table.number();
            offsets.emplace(position,static_cast<std::int32_t>(i));result.unwind.push_back(entry);
        }
    }
    std::int32_t largest=-1;
    if(tries){
        PackedReader table(memory,image,metadata_address(memory,image,tries,1));const auto count=table.count(4096);
        for(std::uint32_t i=0;i<count;++i){TryEntry block;block.low=static_cast<std::int32_t>(table.count(65535));block.high=static_cast<std::int32_t>(table.count(65535));block.catch_high=static_cast<std::int32_t>(table.count(65535));
            if(block.high<block.low || block.catch_high<block.high)throw HostCallFault("FH4 try state 範圍無效");largest=std::max(largest,block.catch_high);
            const auto handlers=table.fixed();PackedReader list(memory,image,metadata_address(memory,image,handlers,1));const auto handler_count=list.count(4096);
            for(std::uint32_t j=0;j<handler_count;++j){CatchEntry caught;const auto flags=list.byte();const auto continuations=static_cast<unsigned>((flags>>4)&3U);
                if((flags&0xc0U) || continuations==3)throw HostCallFault("FH4 catch header 保留位無效");
                if(flags&1U)caught.flags=list.number();
                if(flags&2U){const auto type=list.fixed();if(type)caught.type=metadata_address(memory,image,type,17);}
                if(flags&4U)caught.object_offset=list.number();
                caught.handler=metadata_address(memory,image,list.fixed(),1);caught.continuation_count=continuations;
                for(unsigned k=0;k<continuations;++k){const std::uint64_t target=(flags&8U)?list.fixed():std::uint64_t(function_begin)+list.number();caught.continuations[k]=metadata_address(memory,image,target,1);}
                block.handlers.push_back(caught);
            }
            result.tries.push_back(std::move(block));
        }
    }
    if(ips){
        PackedReader table(memory,image,metadata_address(memory,image,ips,1));const auto count=table.count(65536);std::uint64_t pc=function_begin;
        for(std::uint32_t i=0;i<count;++i){pc+=table.number();const auto encoded=table.count(65536);const auto level=static_cast<std::int32_t>(encoded)-1;
            if(pc>=image.size)throw HostCallFault("FH4 IP-state RVA 超出映像");result.ips.emplace_back(static_cast<std::uint32_t>(pc),level);largest=std::max(largest,level);}
    }
    // 沒有任何 cleanup 時，FH4 可省略 unwind map；try／IP state 仍然有效。
    if(!unwind){for(std::int32_t state=0;state<=largest;++state)result.unwind.push_back({state-1,0,UnwindEntry::Kind::None,0});}
    else if(largest>=static_cast<std::int32_t>(result.unwind.size()))throw HostCallFault("FH4 state 超出 unwind map");
    return result;
}
}
