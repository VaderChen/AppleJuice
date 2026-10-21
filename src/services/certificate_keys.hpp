#pragma once
#include "certificate_packet.hpp"
#include "x64arm/memory.hpp"
#include <tuple>
namespace x64arm::certificate_detail {
struct KeyProviderParameter {std::uint32_t id{},flags{};std::vector<std::uint8_t> bytes;};
struct KeyProviderInfo {std::string container,name;std::uint32_t type{},flags{},spec{};std::vector<KeyProviderParameter> parameters;};
inline KeyProviderInfo read_key_info(const GuestMemory& memory,std::uint64_t pointer,bool small){
    const unsigned unit=small?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
memory.check(GuestAddress{pointer},small?28:48,Access::Read);KeyProviderInfo info;
            const auto name=[&](unsigned off){const auto p=memory.load(GuestAddress{pointer+off},bits);return p?utf16_to_utf8(read_utf16(memory,GuestAddress{p},1024)):std::string{};};
            info.container=name(0);info.name=name(unit);info.type=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+2*unit},32));info.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+2*unit+4},32));info.spec=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+(small?24:40)},32));
            const auto count=memory.load(GuestAddress{pointer+2*unit+8},32),params=memory.load(GuestAddress{pointer+(small?20:32)},bits);if(count>64 || (count && !params))throw CertificateError{87};std::size_t total=0;
            for(std::uint64_t i=0;i<count;++i){const auto p=params+i*(small?16:24);KeyProviderParameter param;param.id=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));const auto data=memory.load(GuestAddress{p+unit},bits),length=memory.load(GuestAddress{p+2*unit},32);param.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{p+2*unit+4},32));if(length>16U*1024U*1024U-total || (length && !data))throw CertificateError{87};total+=length;param.bytes.resize(static_cast<std::size_t>(length));memory.read(GuestAddress{data},param.bytes);info.parameters.push_back(std::move(param));}
    return info;
}
inline void key_word(std::vector<std::uint8_t>& out,std::uint32_t value){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
inline std::uint32_t key_word(std::span<const std::uint8_t> bytes,std::size_t& at){if(at>bytes.size() || bytes.size()-at<4)throw CertificateError{87};std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(bytes[at++])<<(8*i);return value;}
inline std::vector<std::uint8_t> key_context(std::uint64_t provider,std::uint32_t spec,bool owned){std::vector<std::uint8_t> out;key_word(out,static_cast<std::uint32_t>(provider));key_word(out,static_cast<std::uint32_t>(provider>>32));key_word(out,spec);key_word(out,owned?1:0);return out;}
inline std::tuple<std::uint64_t,std::uint32_t,bool> key_context(const std::vector<std::uint8_t>& bytes){if(bytes.size()!=16)throw CertificateError{87};std::size_t at=0;const auto lo=key_word(bytes,at),hi=key_word(bytes,at),spec=key_word(bytes,at),owned=key_word(bytes,at);return {std::uint64_t(lo)|(std::uint64_t(hi)<<32),spec,owned!=0};}
inline std::vector<std::uint8_t> key_info(const KeyProviderInfo& info){
    std::vector<std::uint8_t> out;key_word(out,info.type);key_word(out,info.flags);key_word(out,info.spec);key_word(out,static_cast<std::uint32_t>(info.parameters.size()));
    const auto blob=[&](std::span<const std::uint8_t> bytes){key_word(out,static_cast<std::uint32_t>(bytes.size()));out.insert(out.end(),bytes.begin(),bytes.end());};
    blob({reinterpret_cast<const std::uint8_t*>(info.container.data()),info.container.size()});blob({reinterpret_cast<const std::uint8_t*>(info.name.data()),info.name.size()});
    for(const auto& param:info.parameters){key_word(out,param.id);key_word(out,param.flags);blob(param.bytes);}return out;
}
inline KeyProviderInfo key_info(const std::vector<std::uint8_t>& bytes){
    std::size_t at=0;KeyProviderInfo info;info.type=key_word(bytes,at);info.flags=key_word(bytes,at);info.spec=key_word(bytes,at);const auto count=key_word(bytes,at);if(count>64)throw CertificateError{87};
    const auto blob=[&](){const auto size=key_word(bytes,at);if(size>bytes.size()-at)throw CertificateError{87};std::vector<std::uint8_t> result(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+size));at+=size;return result;};
    const auto container=blob(),name=blob();info.container.assign(container.begin(),container.end());info.name.assign(name.begin(),name.end());
    for(unsigned i=0;i<count;++i){KeyProviderParameter param;param.id=key_word(bytes,at);param.flags=key_word(bytes,at);param.bytes=blob();info.parameters.push_back(std::move(param));}if(at!=bytes.size())throw CertificateError{87};return info;
}
}
