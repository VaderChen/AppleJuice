#pragma once
#include "x64arm/memory.hpp"
#include "x64arm/windows_abi.hpp"
#include <array>
#include <bit>
#include <vector>

namespace x64arm::msvc_detail {
// EXCEPTION_RECORD 與 MSVC ThrowInfo／CatchableType 共用解碼；所有位址都是客體位址。
struct ExceptionRecord {
    std::uint32_t code{},flags{};
    std::vector<std::uint64_t> parameters;
    bool cpp()const{return code==0xe06d7363U && parameters.size()>=3 && parameters[2] &&
        (parameters[0]==0x19930520U || parameters[0]==0x19930521U || parameters[0]==0x19930522U || parameters[0]==0x1994000U);}
};
inline ExceptionRecord exception_record(const GuestMemory& memory,std::uint64_t at,bool x86){
    const auto count=memory.load(GuestAddress{at+(x86?16U:24U)},32);if(count>15)throw HostCallFault("MSVC 例外參數數目無效");
    ExceptionRecord record;record.code=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));record.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32))|1U;
    for(unsigned i=0;i<count;++i)record.parameters.push_back(memory.load(GuestAddress{at+(x86?20U:32U)+i*(x86?4U:8U)},x86?32:64));return record;
}
inline void write_exception_record(GuestMemory& memory,std::uint64_t at,bool x86,const ExceptionRecord& record){
    memory.write(GuestAddress{at},std::vector<std::uint8_t>(x86?80U:152U));memory.store(GuestAddress{at},32,record.code);memory.store(GuestAddress{at+4},32,record.flags);
    memory.store(GuestAddress{at+(x86?16U:24U)},32,record.parameters.size());
    for(unsigned i=0;i<record.parameters.size();++i)memory.store(GuestAddress{at+(x86?20U:32U)+i*(x86?4U:8U)},x86?32:64,record.parameters[i]);
}
inline std::uint64_t offset(std::uint64_t base,std::int64_t delta){
    const auto magnitude=delta<0?static_cast<std::uint64_t>(-(delta+1))+1:static_cast<std::uint64_t>(delta);
    if(delta<0?magnitude>base:magnitude>UINT64_MAX-base)throw HostCallFault("MSVC 例外位址溢位");return delta<0?base-magnitude:base+magnitude;
}
struct ExceptionType {
    std::uint64_t size{},source{},copy{},destroy{};
    std::uint32_t properties{};
    bool virtual_base()const{return (properties&4U)!=0;}
    bool interface_handle()const{return (properties&8U)!=0;}
};
inline ExceptionType exception_type(const GuestMemory& memory,const ExceptionRecord& record,bool x86){
    if(!record.cpp())throw HostCallFault("MSVC 例外型別資料不適用");
    const auto base=x86?0:record.parameters.size()>3?record.parameters[3]:0;
    const auto word=[&](std::uint64_t at){return static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));};
    const auto reference=[&](std::uint64_t at){return offset(base,word(at));};
    const auto info=record.parameters[2];memory.check(GuestAddress{info},16,Access::Read);
    const auto array=reference(info+12);const auto count=word(array);if(!count || count>4096)throw HostCallFault("MSVC catchable type 清單無效");
    memory.check(GuestAddress{array+4},static_cast<std::size_t>(count)*4,Access::Read);const auto type=reference(array+4);memory.check(GuestAddress{type},28,Access::Read);
    ExceptionType result;result.properties=word(type);result.size=word(type+20);result.source=record.parameters[1];
    if(!result.size || result.size>memory.limit() || !result.source)throw HostCallFault("MSVC 例外物件大小或來源無效");
    const auto member=std::bit_cast<std::int32_t>(word(type+8)),vbptr=std::bit_cast<std::int32_t>(word(type+12)),vbase=std::bit_cast<std::int32_t>(word(type+16));
    if(!(result.properties&1U)){
        if(vbptr>=0){const auto cell=offset(result.source,vbptr),table=memory.load(GuestAddress{cell},x86?32:64);result.source=offset(cell,std::bit_cast<std::int32_t>(word(offset(table,vbase))));}
        else if(vbptr!=-1)throw HostCallFault("MSVC 例外 PMD 無效");result.source=offset(result.source,member);
        if(word(type+24))result.copy=reference(type+24);
    }
    if(word(info+4))result.destroy=reference(info+4);memory.check(GuestAddress{result.source},static_cast<std::size_t>(result.size),Access::Read);
    if(result.copy)memory.check(GuestAddress{result.copy},1,Access::Execute);if(result.destroy)memory.check(GuestAddress{result.destroy},1,Access::Execute);return result;
}
}
