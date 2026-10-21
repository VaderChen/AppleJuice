#include "x64arm/crypto.hpp"
#include <openssl/evp.h>
#include <openssl/provider.h>
#include <climits>
#include <stdexcept>
namespace x64arm {
namespace {void require(bool value){if(!value)throw std::runtime_error("原生密碼串流操作失敗");}}

namespace {
void cipher_providers() {
    struct Providers {
        OSSL_PROVIDER* normal{OSSL_PROVIDER_load(nullptr,"default")};
        OSSL_PROVIDER* legacy{OSSL_PROVIDER_load(nullptr,"legacy")};
        ~Providers(){OSSL_PROVIDER_unload(legacy);OSSL_PROVIDER_unload(normal);}
    };
    static const Providers providers;
    require(providers.normal!=nullptr);
}
}
struct CryptoCipher::State {
    EVP_CIPHER_CTX* context{};
    std::string algorithm;std::vector<std::uint8_t> key,iv;unsigned effective_bits{};bool encrypt{};
    State() = default;
    State(const State& other):algorithm(other.algorithm),key(other.key),iv(other.iv),effective_bits(other.effective_bits),encrypt(other.encrypt){
        context=EVP_CIPHER_CTX_new();if(!context)throw std::bad_alloc();
        if(EVP_CIPHER_CTX_copy(context,other.context)!=1){EVP_CIPHER_CTX_free(context);context=nullptr;require(false);}
    }
    ~State(){EVP_CIPHER_CTX_free(context);}
};
bool CryptoCipher::available(const std::string& name){
    cipher_providers();auto* cipher=EVP_CIPHER_fetch(nullptr,name.c_str(),nullptr);const bool found=cipher!=nullptr;EVP_CIPHER_free(cipher);return found;
}
CryptoCipher::CryptoCipher(const std::string& name,std::span<const std::uint8_t> key,std::span<const std::uint8_t> iv,unsigned effective_bits,bool encrypt):state_(std::make_shared<State>()),history_("SHA256",false,{}){
    cipher_providers();auto* cipher=EVP_CIPHER_fetch(nullptr,name.c_str(),nullptr);
    state_->context=EVP_CIPHER_CTX_new();state_->algorithm=name;state_->key.assign(key.begin(),key.end());state_->iv.assign(iv.begin(),iv.end());state_->effective_bits=effective_bits;state_->encrypt=encrypt;
    const bool initialized=cipher && state_->context && EVP_CipherInit_ex(state_->context,cipher,nullptr,nullptr,nullptr,encrypt?1:0)==1;
    EVP_CIPHER_free(cipher);require(initialized);
    require(EVP_CIPHER_CTX_set_key_length(state_->context,static_cast<int>(key.size()))==1);
    if(name.starts_with("RC2-"))require(EVP_CIPHER_CTX_ctrl(state_->context,EVP_CTRL_SET_RC2_KEY_BITS,static_cast<int>(effective_bits),nullptr)==1);
    require(EVP_CIPHER_CTX_set_padding(state_->context,0)==1);
    require(EVP_CipherInit_ex(state_->context,nullptr,nullptr,key.data(),iv.empty()?nullptr:iv.data(),encrypt?1:0)==1);
}
std::vector<std::uint8_t> CryptoCipher::update(std::span<const std::uint8_t> bytes){
    if(!state_ || bytes.size()>static_cast<std::size_t>(INT_MAX)-EVP_MAX_BLOCK_LENGTH)throw std::invalid_argument("密碼串流狀態或長度不合法");
    auto next=std::make_shared<State>(*state_);std::vector<std::uint8_t> output(bytes.size()+EVP_MAX_BLOCK_LENGTH);int size=0;
    require(EVP_CipherUpdate(next->context,output.data(),&size,bytes.data(),static_cast<int>(bytes.size()))==1);
    require(static_cast<std::size_t>(size)==bytes.size());output.resize(static_cast<std::size_t>(size));history_.update(bytes);state_=std::move(next);return output;
}
bool CryptoCipher::operator==(const CryptoCipher& other) const {
    if(state_==other.state_)return true;if(!state_ || !other.state_)return false;
    return state_->algorithm==other.state_->algorithm && state_->key==other.state_->key && state_->iv==other.state_->iv && state_->effective_bits==other.state_->effective_bits && state_->encrypt==other.state_->encrypt && history_==other.history_;
}
}
