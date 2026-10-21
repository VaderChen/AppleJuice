#include "x64arm/crypto.hpp"
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <stdexcept>
#include <array>
namespace x64arm {
namespace {
void require(bool result) { if (!result) throw std::runtime_error("主機密碼函式庫操作失敗"); }
}
struct CryptoHash::State {
    EVP_MD_CTX* digest{};
    EVP_MAC_CTX* mac{};
    State() = default;
    State(const State& other) {
        if (other.digest) {
            digest=EVP_MD_CTX_new();
            if (!digest) throw std::bad_alloc();
            if (EVP_MD_CTX_copy_ex(digest,other.digest)!=1) {EVP_MD_CTX_free(digest);digest=nullptr;require(false);}
        }
        if (other.mac) {mac=EVP_MAC_CTX_dup(other.mac);if (!mac) throw std::bad_alloc();}
    }
    ~State() {EVP_MD_CTX_free(digest);EVP_MAC_CTX_free(mac);}
};
CryptoHash::CryptoHash(const std::string& algorithm, bool hmac, std::span<const std::uint8_t> key) : state_(std::make_shared<State>()) {
    if (hmac) {
        auto* implementation=EVP_MAC_fetch(nullptr,"HMAC",nullptr);require(implementation!=nullptr);
        state_->mac=EVP_MAC_CTX_new(implementation);EVP_MAC_free(implementation);require(state_->mac!=nullptr);
        auto name=algorithm;
        OSSL_PARAM parameters[]{OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,name.data(),0),OSSL_PARAM_construct_end()};
        const std::uint8_t empty=0;
        require(EVP_MAC_init(state_->mac,key.empty()?&empty:key.data(),key.size(),parameters)==1);
    } else {
        const auto* implementation=EVP_get_digestbyname(algorithm.c_str());require(implementation!=nullptr);
        state_->digest=EVP_MD_CTX_new();require(state_->digest!=nullptr);
        require(EVP_DigestInit_ex(state_->digest,implementation,nullptr)==1);
    }
}
void CryptoHash::update(std::span<const std::uint8_t> bytes) {
    if (!state_ || bytes.size()>UINT64_MAX-length_) throw std::invalid_argument("雜湊狀態或長度不合法");
    auto next=std::make_shared<State>(*state_);
    if (next->digest) require(EVP_DigestUpdate(next->digest,bytes.data(),bytes.size())==1);
    else require(EVP_MAC_update(next->mac,bytes.data(),bytes.size())==1);
    state_=std::move(next);length_+=bytes.size();
}
std::vector<std::uint8_t> CryptoHash::digest() const {
    if (!state_) return {};
    State copy(*state_);std::array<std::uint8_t,EVP_MAX_MD_SIZE> result{};std::size_t size=0;
    if (copy.digest) {unsigned length=0;require(EVP_DigestFinal_ex(copy.digest,result.data(),&length)==1);size=length;}
    else require(EVP_MAC_final(copy.mac,result.data(),&size,result.size())==1);
    return {result.begin(),result.begin()+static_cast<std::ptrdiff_t>(size)};
}
bool CryptoHash::operator==(const CryptoHash& other) const {
    return length_==other.length_ && (state_==other.state_ || digest()==other.digest());
}
}
