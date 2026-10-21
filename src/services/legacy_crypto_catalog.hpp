#pragma once
#include "x64arm/crypto.hpp"
#include <algorithm>
#include <array>
#include <string_view>
namespace x64arm::legacy_crypto {
struct Algorithm {
    std::uint32_t id,default_bits,min_bits,max_bits,block;
    const char* name;const char* description;const char* cipher;
};
inline constexpr Algorithm algorithms[]{
    {0x8003,128,128,128,0,"MD5","MD5",nullptr},{0x8004,160,160,160,0,"SHA","SHA-1",nullptr},
    {0x8008,288,288,288,0,"SSL3 SHAMD5","MD5 + SHA-1",nullptr},
    {0x800c,256,256,256,0,"SHA-256","SHA-256",nullptr},{0x800d,384,384,384,0,"SHA-384","SHA-384",nullptr},
    {0x800e,512,512,512,0,"SHA-512","SHA-512",nullptr},
    {0x2400,1024,512,8192,0,"RSA_SIGN","RSA Signature",nullptr},{0xa400,1024,512,8192,0,"RSA_KEYX","RSA Key Exchange",nullptr},
    {0x6601,56,56,56,8,"DES","DES","DES"},{0x6603,168,168,168,8,"3DES","Triple DES","DES-EDE3"},
    {0x6609,112,112,112,8,"3DES TWO KEY","Two-key Triple DES","DES-EDE"},
    {0x6602,128,40,128,8,"RC2","RC2","RC2"},{0x6801,128,40,128,1,"RC4","RC4","RC4"},
    {0x660e,128,128,128,16,"AES-128","AES-128","AES-128"},
    {0x660f,192,192,192,16,"AES-192","AES-192","AES-192"},
    {0x6610,256,256,256,16,"AES-256","AES-256","AES-256"}
};
inline const Algorithm* find(std::uint32_t id){
    const auto it=std::find_if(std::begin(algorithms),std::end(algorithms),[&](const auto& a){return a.id==id;});return it==std::end(algorithms)?nullptr:it;
}
inline std::string cipher_name(const Algorithm& a,std::uint32_t mode){return std::string(a.cipher)+(a.block==1?"":mode==2?"-ECB":"-CBC");}
inline bool supported(const Algorithm& a,std::uint32_t provider){
    if(provider==1 && ((a.id>=0x800c && a.id<=0x800e) || (a.id>=0x660e && a.id<=0x6610)))return false;
    return !a.cipher || CryptoCipher::available(cipher_name(a,1));
}
struct Provider {std::uint32_t type;const char* name;const char* description;};
inline constexpr Provider providers[]{
    {1,"Microsoft Enhanced Cryptographic Provider v1.0","RSA Full (Signature and Key Exchange)"},
    {24,"Microsoft Enhanced RSA and AES Cryptographic Provider","RSA Full and AES"}
};
inline bool rsa(std::uint32_t algorithm){return algorithm==0x2400 || algorithm==0xa400;}
inline std::uint32_t key_bits(const Algorithm& a,std::size_t size){
    return a.id==0x6601?size==8?56U:0U:a.id==0x6603?size==24?168U:0U:a.id==0x6609?size==16?112U:0U:static_cast<std::uint32_t>(size*8);
}
}
