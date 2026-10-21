#include "x64arm/crypto.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <algorithm>
#include <stdexcept>

// Windows 相容層仍須讀取既有 MD5／SHA1 校驗資料；只在此轉接檔使用被棄用的演算法介面。
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
namespace x64arm {
struct CryptoHash::State {
    std::string algorithm;
    bool hmac{};
    CC_MD5_CTX md5{};
    CC_SHA1_CTX sha1{};
    CC_SHA256_CTX sha256{};
    CC_SHA512_CTX sha512{};
    CCHmacContext mac{};
};
CryptoHash::CryptoHash(const std::string& algorithm,bool hmac,std::span<const std::uint8_t> key) : state_(std::make_shared<State>()) {
    state_->algorithm=algorithm;state_->hmac=hmac;
    if (hmac) {
        const auto kind=algorithm=="MD5"?kCCHmacAlgMD5:algorithm=="SHA1"?kCCHmacAlgSHA1:algorithm=="SHA256"?kCCHmacAlgSHA256:
            algorithm=="SHA384"?kCCHmacAlgSHA384:algorithm=="SHA512"?kCCHmacAlgSHA512:UINT32_MAX;
        if (kind==UINT32_MAX) throw std::invalid_argument("尚未支援的雜湊演算法");
        CCHmacInit(&state_->mac,kind,key.data(),key.size());
    } else if (algorithm=="MD5") CC_MD5_Init(&state_->md5);
    else if (algorithm=="SHA1") CC_SHA1_Init(&state_->sha1);
    else if (algorithm=="SHA256") CC_SHA256_Init(&state_->sha256);
    else if (algorithm=="SHA384") CC_SHA384_Init(&state_->sha512);
    else if (algorithm=="SHA512") CC_SHA512_Init(&state_->sha512);
    else throw std::invalid_argument("尚未支援的雜湊演算法");
}
void CryptoHash::update(std::span<const std::uint8_t> bytes) {
    if (!state_ || bytes.size()>UINT64_MAX-length_) throw std::invalid_argument("雜湊狀態或長度不合法");
    if(bytes.empty())return;
    // 長度／狀態先驗證。CommonDigest Update 保證回傳 1，CCHmacUpdate 為 void，
    // 此後沒有可失敗的配置；只有真正共享的 clone 才複製狀態。
    auto next=state_.use_count()==1?state_:std::make_shared<State>(*state_);
    for (std::size_t offset=0;offset<bytes.size();) {
        const auto size=static_cast<CC_LONG>(std::min<std::size_t>(UINT32_MAX,bytes.size()-offset));
        const auto* data=bytes.data()+offset;
        if (next->hmac) CCHmacUpdate(&next->mac,data,size);
        else if (next->algorithm=="MD5") CC_MD5_Update(&next->md5,data,size);
        else if (next->algorithm=="SHA1") CC_SHA1_Update(&next->sha1,data,size);
        else if (next->algorithm=="SHA256") CC_SHA256_Update(&next->sha256,data,size);
        else if (next->algorithm=="SHA384") CC_SHA384_Update(&next->sha512,data,size);
        else CC_SHA512_Update(&next->sha512,data,size);
        offset+=size;
    }
    state_=std::move(next);length_+=bytes.size();
}
std::vector<std::uint8_t> CryptoHash::digest() const {
    if (!state_) return {};
    auto copy=*state_;const auto& name=copy.algorithm;
    const auto size=name=="MD5"?16U:name=="SHA1"?20U:name=="SHA256"?32U:name=="SHA384"?48U:64U;
    std::vector<std::uint8_t> bytes(size);
    if (copy.hmac) CCHmacFinal(&copy.mac,bytes.data());
    else if (name=="MD5") CC_MD5_Final(bytes.data(),&copy.md5);
    else if (name=="SHA1") CC_SHA1_Final(bytes.data(),&copy.sha1);
    else if (name=="SHA256") CC_SHA256_Final(bytes.data(),&copy.sha256);
    else if (name=="SHA384") CC_SHA384_Final(bytes.data(),&copy.sha512);
    else CC_SHA512_Final(bytes.data(),&copy.sha512);
    return bytes;
}
bool CryptoHash::operator==(const CryptoHash& other) const {
    return length_==other.length_ && (state_==other.state_ || digest()==other.digest());
}
}
