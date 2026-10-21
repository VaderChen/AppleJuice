#include "x64arm/crypto_aead.hpp"
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <memory>
#include <stdexcept>
namespace x64arm {
bool crypto_aes_gcm(bool encrypt,std::span<const std::uint8_t> key,std::span<const std::uint8_t> nonce,std::span<const std::uint8_t> aad,std::span<const std::uint8_t> input,std::vector<std::uint8_t>& output,std::array<std::uint8_t,16>& tag){
    if(key.size()!=32 || nonce.size()!=12 || input.size()>16*1024*1024 || aad.size()>16*1024*1024)throw std::invalid_argument("AES-GCM 金鑰、nonce 或資料長度不合法");
    std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);if(!ctx)throw std::bad_alloc();int count=0,total=0;
    const auto require=[](bool value){if(!value){ERR_clear_error();throw std::runtime_error("AES-GCM 原生運算失敗");}};
    require(EVP_CipherInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,key.data(),nonce.data(),encrypt?1:0)==1);
    if(!aad.empty())require(EVP_CipherUpdate(ctx.get(),nullptr,&count,aad.data(),static_cast<int>(aad.size()))==1);
    output.resize(input.size()+16);
    try {require(EVP_CipherUpdate(ctx.get(),output.data(),&count,input.data(),static_cast<int>(input.size()))==1);total=count;
        if(!encrypt)require(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_TAG,16,tag.data())==1);
        const auto verified=EVP_CipherFinal_ex(ctx.get(),output.data()+total,&count);
        if(verified!=1){OPENSSL_cleanse(output.data(),output.size());output.clear();ERR_clear_error();if(encrypt)require(false);return false;}total+=count;
        if(encrypt)require(EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_GET_TAG,16,tag.data())==1);
        require(total>=0 && static_cast<std::size_t>(total)==input.size());output.resize(static_cast<std::size_t>(total));return true;
    }catch(...){OPENSSL_cleanse(output.data(),output.size());output.clear();throw;}
}
}
