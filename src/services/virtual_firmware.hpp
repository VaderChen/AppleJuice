#pragma once
#include "x64arm/processor_information.hpp"
#include "x64arm/cpu_features.hpp"
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace x64arm::virtual_firmware {
// 客體平台採傳統 BIOS 描述，不公開宿主 UEFI 或韌體服務。
enum class Type : std::uint32_t { Bios=1, Uefi=2 };
inline constexpr Type type=Type::Bios;
inline constexpr std::string_view manufacturer="x64arm",product="Windows compatibility environment",board="Virtual platform",version="1",bios="Virtual firmware",processor="x64arm virtual AMD64 processor";
inline void put(std::vector<std::uint8_t>& bytes,unsigned at,std::uint64_t value,unsigned width=2){
    for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));
}
// SMBIOS 3.6（DMTF DSP0134）：描述實際的客體 CPU／記憶體額度，不讀取宿主韌體。
inline std::vector<std::uint8_t> smbios(unsigned processors,std::uint64_t memory,std::uint64_t identity,const ProcessorInformation& cpu={}){
    std::vector<std::uint8_t> table;
    const auto append=[&](unsigned type,unsigned length,unsigned handle,std::initializer_list<std::string_view> strings,auto fill){
        std::vector<std::uint8_t> entry(length);entry[0]=static_cast<std::uint8_t>(type);entry[1]=static_cast<std::uint8_t>(length);put(entry,2,handle);
        fill(entry);table.insert(table.end(),entry.begin(),entry.end());
        for(const auto text:strings){table.insert(table.end(),text.begin(),text.end());table.push_back(0);}
        if(strings.size()==0)table.push_back(0);table.push_back(0);
    };
    append(0,0x1a,0,{manufacturer,bios,"01/01/2026"},[](auto& e){e[4]=1;e[5]=2;e[8]=3;put(e,10,8,8);e[19]=type==Type::Uefi?8:0;e[20]=1;e[22]=e[23]=0xff;});
    append(1,0x1b,1,{manufacturer,product,version},[&](auto& e){
        e[4]=1;e[5]=2;e[6]=3;put(e,8,identity,8);put(e,16,identity^UINT64_C(0x78363461726d0001),8);
        e[15]=static_cast<std::uint8_t>((e[15]&0x0f)|0x40);e[16]=static_cast<std::uint8_t>((e[16]&0x3f)|0x80);e[24]=2;
    });
    append(2,0x0f,2,{manufacturer,board,version},[](auto& e){e[4]=1;e[5]=2;e[6]=3;put(e,11,6);e[13]=0x0a;});
    append(3,0x15,6,{manufacturer,version},[](auto& e){e[4]=1;e[5]=2;e[6]=2;e[9]=e[10]=e[11]=2;e[12]=2;});
    append(4,0x30,3,{cpu.socket,cpu.manufacturer,cpu.name},[&](auto& e){
        e[4]=1;e[5]=3;e[6]=2;e[7]=2;e[16]=3;
        const auto signature=virtual_cpuid(1,0,processors);put(e,8,signature[0],4);put(e,12,signature[3],4);
        put(e,20,std::min(cpu.maximum_mhz,65535U));put(e,22,std::min(cpu.nominal_mhz,65535U));e[24]=0x41;e[25]=2;put(e,26,0xffff);put(e,28,0xffff);put(e,30,0xffff);
        e[35]=e[36]=e[37]=static_cast<std::uint8_t>(processors>=255?255:processors);put(e,38,4);
        for(unsigned offset:{42U,44U,46U})put(e,offset,std::min(processors,65535U));
    });
    append(16,0x17,4,{},[&](auto& e){e[4]=3;e[5]=3;e[6]=2;const auto kb=memory/1024;put(e,7,kb>=0x80000000U?0x80000000U:kb,4);put(e,11,0xfffe);put(e,13,1);if(kb>=0x80000000U)put(e,15,memory,8);});
    append(17,0x28,5,{"Virtual memory","Virtual Bank 0",manufacturer},[&](auto& e){
        put(e,4,4);put(e,6,0xfffe);put(e,8,0xffff);put(e,10,0xffff);
        const auto mb=memory/(1024*1024);put(e,12,mb>=0x7fff?0x7fff:mb);e[14]=2;e[16]=1;e[17]=2;e[18]=2;put(e,19,4);e[23]=3;
        if(mb>=0x7fff)put(e,28,std::min<std::uint64_t>(mb,0x7fffffff),4);
    });
    append(127,4,0xffff,{},[](auto&){});
    std::vector<std::uint8_t> result{0,3,6,0,0,0,0,0};put(result,4,table.size(),4);result.insert(result.end(),table.begin(),table.end());return result;
}
inline bool acpi_valid(std::span<const std::uint8_t> bytes){
    if(bytes.size()<36 || bytes.size()>1024*1024)return false;
    const auto size=std::uint32_t{bytes[4]}|(std::uint32_t{bytes[5]}<<8)|(std::uint32_t{bytes[6]}<<16)|(std::uint32_t{bytes[7]}<<24);
    unsigned sum=0;for(const auto b:bytes)sum+=b;return size==bytes.size() && !(sum&255U);
}
}
