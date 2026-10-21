#pragma once
#include "x64arm/crypto.hpp"
#include <algorithm>
#include <string_view>
namespace x64arm::crypto_detail {
struct HashInfo{std::uint32_t id;const char* name;unsigned size;};
inline constexpr HashInfo algorithms[]{{0x8003,"MD5",16},{0x8004,"SHA1",20},{0x8008,"MD5-SHA1",36},{0x800c,"SHA256",32},{0x800d,"SHA384",48},{0x800e,"SHA512",64}};
inline const HashInfo* hash_info(std::uint32_t id){const auto found=std::find_if(std::begin(algorithms),std::end(algorithms),[&](const auto& info){return info.id==id;});return found==std::end(algorithms)?nullptr:found;}
// 單次憑證雜湊與傳統 CryptoAPI 共用演算法識別及後端。
inline std::vector<std::uint8_t> digest(std::string_view algorithm,std::span<const std::uint8_t> input){
    const bool combined=algorithm=="MD5-SHA1";
    CryptoHash hash(combined?"MD5":std::string(algorithm),false,{});hash.update(input);auto result=hash.digest();
    if(combined){CryptoHash tail("SHA1",false,{});tail.update(input);const auto bytes=tail.digest();result.insert(result.end(),bytes.begin(),bytes.end());}
    return result;
}
}
