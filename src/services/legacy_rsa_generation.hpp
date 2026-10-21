#pragma once
#include "x64arm/windows_runtime.hpp"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <functional>
namespace x64arm::legacy_crypto {
// CSP 金鑰生成共用熵序列與差分執行快取。
inline std::optional<std::vector<std::uint8_t>> generate_rsa(RuntimeState& runtime,unsigned bits,const std::function<bool(std::span<std::uint8_t>)>& random,std::uint32_t* failure=nullptr){
    using Context=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
    using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
    std::vector<std::uint8_t> der;if(failure)*failure=8;
            auto& pool=*runtime.native_crypto_entropy;const auto entropy_key=std::pair{runtime.entropy_index,std::string("RSAKEY:")+std::to_string(bits)};
            const bool comparing=runtime.entropy.use_count()>1;const auto cached=comparing?pool.find(entropy_key):pool.end();if(comparing && cached==pool.end() && pool.size()>=4096)return {};
            std::array<std::uint8_t,4> reservation{};if(!random(reservation))return {};
            if(cached!=pool.end())der=cached->second;
            else {
            if(failure)*failure=0x80090005U;
            Context context(EVP_PKEY_CTX_new_from_name(nullptr,"RSA",nullptr),EVP_PKEY_CTX_free);EVP_PKEY* raw=nullptr;
            if(!context || EVP_PKEY_keygen_init(context.get())<=0 || EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(),static_cast<int>(bits))<=0 || EVP_PKEY_generate(context.get(),&raw)<=0){EVP_PKEY_free(raw);return {};}
            Key native(raw,EVP_PKEY_free);const auto size=i2d_PrivateKey(native.get(),nullptr);if(size<=0)return {};
            der.resize(static_cast<std::size_t>(size));auto* at=der.data();if(i2d_PrivateKey(native.get(),&at)!=size)return {};if(comparing)pool.emplace(entropy_key,der);
            }
    return der;
}
}
