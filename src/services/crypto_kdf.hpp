#pragma once
// CNG 金鑰衍生（BCryptDeriveKeyPBKDF2、BCryptKeyDerivation、BCryptDeriveKeyCapi 共用）：
// - PBKDF2（RFC 8018）：PRF 為 HMAC；T_i = U_1 ^ … ^ U_c，U_1 = PRF(密碼, 鹽 ‖ INT(i))。
// - SP 800-108 計數模式 HMAC：K_i = HMAC(K, [i]_32 ‖ Label ‖ 0x00 ‖ Context ‖ [L]_32)，L 為輸出位元數。
// - SP 800-56A 串接：H([i]_32 ‖ Z ‖ OtherInfo)，OtherInfo 為 AlgorithmID ‖ PartyUInfo ‖ PartyVInfo ‖ SuppPubInfo ‖ SuppPrivInfo。
// - CryptoAPI 相容：以 64 位元組的 0x36／0x5C 緩衝與雜湊值 XOR 後各自再雜湊並串接（CryptDeriveKey 的延伸步驟）。
#include "x64arm/crypto.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace x64arm::crypto_kdf {
using Bytes=std::vector<std::uint8_t>;
// BCryptOpenAlgorithmProvider 可開啟的金鑰衍生演算法與 KDF_HASH_ALGORITHM 可用的雜湊。
inline bool supported(std::string_view name){return name=="PBKDF2" || name=="SP800_108_CTR_HMAC" || name=="SP800_56A_CONCAT" || name=="CAPI_KDF";}
inline bool hash_supported(std::string_view name){return name=="MD5" || name=="SHA1" || name=="SHA256" || name=="SHA384" || name=="SHA512";}
inline std::array<std::uint8_t,4> big_endian(std::uint32_t value){return {static_cast<std::uint8_t>(value>>24),static_cast<std::uint8_t>(value>>16),static_cast<std::uint8_t>(value>>8),static_cast<std::uint8_t>(value)};}
inline Bytes pbkdf2(const std::string& hash,std::span<const std::uint8_t> password,std::span<const std::uint8_t> salt,std::uint64_t iterations,std::size_t length){
    const CryptoHash keyed(hash,true,password);Bytes derived;derived.reserve(length);
    for(std::uint32_t block=1;derived.size()<length;++block){
        auto first=keyed;first.update(salt);first.update(big_endian(block));auto u=first.digest();auto t=u;
        for(std::uint64_t round=1;round<iterations;++round){auto next=keyed;next.update(u);u=next.digest();for(std::size_t i=0;i<t.size();++i)t[i]^=u[i];}
        derived.insert(derived.end(),t.begin(),t.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(t.size(),length-derived.size())));
    }
    return derived;
}
inline Bytes sp800_108_counter_hmac(const std::string& hash,std::span<const std::uint8_t> key,std::span<const std::uint8_t> label,std::span<const std::uint8_t> context,std::size_t length){
    const CryptoHash keyed(hash,true,key);Bytes derived;derived.reserve(length);const auto bits=big_endian(static_cast<std::uint32_t>(length*8));const std::array<std::uint8_t,1> separator{0};
    for(std::uint32_t counter=1;derived.size()<length;++counter){
        auto round=keyed;round.update(big_endian(counter));round.update(label);round.update(separator);round.update(context);round.update(bits);const auto block=round.digest();
        derived.insert(derived.end(),block.begin(),block.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(block.size(),length-derived.size())));
    }
    return derived;
}
inline Bytes sp800_56a_concat(const std::string& hash,std::span<const std::uint8_t> secret,std::span<const std::uint8_t> other,std::size_t length){
    Bytes derived;derived.reserve(length);
    for(std::uint32_t counter=1;derived.size()<length;++counter){
        CryptoHash round(hash,false,{});round.update(big_endian(counter));round.update(secret);round.update(other);const auto block=round.digest();
        derived.insert(derived.end(),block.begin(),block.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(block.size(),length-derived.size())));
    }
    return derived;
}
// 雜湊值 value 的 CryptoAPI 延伸：需要的長度不超過雜湊長度時直接截取，否則為兩段 64 位元組緩衝的雜湊串接。
inline Bytes capi_expand(const std::string& hash,const Bytes& value,std::size_t length){
    if(length<=value.size())return Bytes(value.begin(),value.begin()+static_cast<std::ptrdiff_t>(length));
    Bytes inner(64,0x36),outer(64,0x5c);for(std::size_t i=0;i<value.size() && i<64;++i){inner[i]^=value[i];outer[i]^=value[i];}
    CryptoHash first(hash,false,{}),second(hash,false,{});first.update(inner);second.update(outer);auto material=first.digest();const auto tail=second.digest();
    material.insert(material.end(),tail.begin(),tail.end());material.resize(std::min(material.size(),length));return material;
}
}
