#pragma once
#include "x64arm/bluetooth.hpp"
#include <algorithm>
#include <stdexcept>

namespace x64arm::bluetooth::wire {
struct Invalid:std::runtime_error {Invalid():std::runtime_error("Bluetooth ABI 資料範圍無效"){};};
inline std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t at,unsigned size){
    if(size>8 || at>bytes.size() || size>bytes.size()-at)throw Invalid{};
    std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{bytes[at+i]}<<(i*8);return value;
}
inline void put(std::span<std::uint8_t> bytes,std::size_t at,unsigned size,std::uint64_t value){
    if(size>8 || at>bytes.size() || size>bytes.size()-at)throw Invalid{};
    for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));
}
inline Uuid uuid(std::span<const std::uint8_t> bytes){
    if(bytes.size()<20)throw Invalid{};if(bytes[0])return Uuid::short_uuid(static_cast<std::uint16_t>(get(bytes,4,2)));
    Uuid result;std::copy_n(bytes.begin()+4,16,result.bytes.begin());return result;
}
inline void uuid(std::span<std::uint8_t> output,const Uuid& value){
    if(output.size()<20)throw Invalid{};std::fill_n(output.begin(),20,0);
    if(const auto small=value.short_value()){output[0]=1;put(output,4,2,*small);}else std::copy(value.bytes.begin(),value.bytes.end(),output.begin()+4);
}
inline Bytes service(const Service& value){Bytes result(24);uuid(result,value.uuid);put(result,20,2,value.handle);return result;}
inline Service service(std::span<const std::uint8_t> value){if(value.size()<24)throw Invalid{};return {uuid(value),static_cast<std::uint16_t>(get(value,20,2))};}
inline Bytes characteristic(const Characteristic& value){
    Bytes result(36);put(result,0,2,value.service);uuid(std::span(result).subspan(4),value.uuid);put(result,24,2,value.handle);put(result,26,2,value.value_handle);
    for(unsigned i=0;i<8;++i)result[28+i]=(value.properties>>i)&1U;return result;
}
inline Characteristic characteristic(std::span<const std::uint8_t> value){
    if(value.size()<36)throw Invalid{};Characteristic result{static_cast<std::uint16_t>(get(value,0,2)),uuid(value.subspan(4)),static_cast<std::uint16_t>(get(value,24,2)),static_cast<std::uint16_t>(get(value,26,2)),0};
    for(unsigned i=0;i<8;++i)if(value[28+i])result.properties|=static_cast<std::uint8_t>(1U<<i);return result;
}
inline Bytes descriptor(const Descriptor& value){Bytes result(32);put(result,0,2,value.service);put(result,2,2,value.characteristic);put(result,4,4,value.type);uuid(std::span(result).subspan(8),value.uuid);put(result,28,2,value.handle);return result;}
inline Descriptor descriptor(std::span<const std::uint8_t> value){
    if(value.size()<32)throw Invalid{};return {static_cast<std::uint16_t>(get(value,0,2)),static_cast<std::uint16_t>(get(value,2,2)),static_cast<std::uint32_t>(get(value,4,4)),uuid(value.subspan(8)),static_cast<std::uint16_t>(get(value,28,2))};
}
inline Bytes characteristic_value(std::span<const std::uint8_t> value){
    if(value.size()>512)throw Invalid{};Bytes result(4+value.size());put(result,0,4,value.size());std::copy(value.begin(),value.end(),result.begin()+4);return result;
}
inline Bytes characteristic_data(std::span<const std::uint8_t> value){const auto size=get(value,0,4);if(size>512 || size>value.size()-4)throw Invalid{};return {value.begin()+4,value.begin()+4+static_cast<std::ptrdiff_t>(size)};}
inline Bytes descriptor_data(std::span<const std::uint8_t> value,const Descriptor& descriptor){
    if(value.size()<76 || get(value,0,4)!=descriptor.type)throw Invalid{};
    const auto supplied=uuid(value.subspan(4));
    // Microsoft 的 CCC 範例只指定 DescriptorType；已知描述元可省略 UUID。
    if(supplied!=descriptor.uuid && (descriptor.type==6 || supplied!=Uuid{}))throw Invalid{};
    const auto size=get(value,72,4);if(size>512 || size>value.size()-76)throw Invalid{};
    const auto flag=[&](unsigned at){return value[at]!=0;};
    if(descriptor.type==0)return {static_cast<std::uint8_t>(flag(24)|(flag(25)<<1)),0};
    if(descriptor.type==2)return {static_cast<std::uint8_t>(flag(24)|(flag(25)<<1)),0};
    if(descriptor.type==3)return {static_cast<std::uint8_t>(flag(24)),0};
    if(descriptor.type==4){
        const auto unit=uuid(value.subspan(28)).short_value(),description=uuid(value.subspan(52)).short_value();if(!unit || !description)throw Invalid{};
        Bytes result(7);result[0]=value[24];result[1]=value[25];put(result,2,2,*unit);result[4]=value[48];put(result,5,2,*description);return result;
    }
    return {value.begin()+76,value.begin()+76+static_cast<std::ptrdiff_t>(size)};
}
inline Bytes descriptor_value(const Descriptor& descriptor,std::span<const std::uint8_t> data){
    if(data.size()>512 || descriptor.type>6)throw Invalid{};
    const bool typed=descriptor.type==0 || descriptor.type==2 || descriptor.type==3 || descriptor.type==4;
    if((descriptor.type==4 && data.size()!=7) || (typed && descriptor.type!=4 && data.size()!=2) || (descriptor.type==5 && data.size()%2))throw Invalid{};
    Bytes result(std::max<std::size_t>(80,typed?80:76+data.size()));put(result,0,4,descriptor.type);uuid(std::span(result).subspan(4),descriptor.uuid);
    if(descriptor.type==0 || descriptor.type==2){result[24]=data[0]&1U;result[25]=(data[0]>>1)&1U;}
    else if(descriptor.type==3)result[24]=data[0]&1U;
    else if(descriptor.type==4){
        result[24]=data[0];result[25]=data[1];uuid(std::span(result).subspan(28),Uuid::short_uuid(static_cast<std::uint16_t>(get(data,2,2))));result[48]=data[4];uuid(std::span(result).subspan(52),Uuid::short_uuid(static_cast<std::uint16_t>(get(data,5,2))));
    }else{put(result,72,4,data.size());std::copy(data.begin(),data.end(),result.begin()+76);}
    return result;
}
inline void name(std::span<std::uint8_t> target,std::u16string_view text){
    if(target.size()<496)throw Invalid{};std::fill_n(target.begin(),496,0);for(std::size_t i=0;i<std::min<std::size_t>(247,text.size());++i)put(target,2*i,2,text[i]);
}
inline Bytes radio(const Radio& value){
    Bytes result(520);put(result,0,4,result.size());put(result,8,8,value.address);name(std::span(result).subspan(16),value.name);put(result,512,4,value.device_class);put(result,516,2,value.subversion);put(result,518,2,value.manufacturer);return result;
}
inline Bytes device(const Device& value){
    Bytes result(560);put(result,0,4,result.size());put(result,8,8,value.address);put(result,16,4,value.device_class);
    put(result,20,4,value.connected);put(result,24,4,value.remembered);put(result,28,4,value.authenticated);
    for(unsigned i=0;i<8;++i){put(result,32+2*i,2,value.last_seen[i]);put(result,48+2*i,2,value.last_used[i]);}name(std::span(result).subspan(64),value.name);return result;
}
inline Bytes value_event(std::uint16_t handle,std::uint64_t value_size,std::uint64_t value_pointer,bool x86){
    if(x86 && (value_size>UINT32_MAX || value_pointer>UINT32_MAX))throw Invalid{};
    const unsigned unit=x86?4:8;Bytes result(3*unit);put(result,0,2,handle);put(result,unit,unit,value_size);put(result,2*unit,unit,value_pointer);return result;
}
}
