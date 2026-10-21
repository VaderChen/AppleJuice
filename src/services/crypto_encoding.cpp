#include "x64arm/crypto.hpp"
#include <openssl/evp.h>
#include <limits>
#include <stdexcept>

namespace x64arm {
std::string crypto_base64_encode(std::span<const std::uint8_t> bytes) {
    if(bytes.empty())return {};
    if(bytes.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()/4)*3)throw std::length_error("Base64 輸入過長");
    std::string output(4*((bytes.size()+2)/3)+1,'\0');
    const auto size=EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),bytes.data(),static_cast<int>(bytes.size()));
    if(size<0)throw std::runtime_error("Base64 encoding failed");output.resize(static_cast<std::size_t>(size));return output;
}
std::vector<std::uint8_t> crypto_base64_decode(std::string_view text) {
    if(text.empty())return {};
    if(text.size()%4 || text.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::invalid_argument("Base64 長度不合法");
    const auto padding=text.ends_with("==")?2U:text.ends_with("=")?1U:0U;
    for(std::size_t i=0;i<text.size()-padding;++i){const auto c=text[i];if(!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='+' || c=='/'))throw std::invalid_argument("Base64 字元不合法");}
    std::vector<std::uint8_t> output(text.size()/4*3);
    const auto size=EVP_DecodeBlock(output.data(),reinterpret_cast<const unsigned char*>(text.data()),static_cast<int>(text.size()));
    if(size<0 || static_cast<unsigned>(size)<padding)throw std::invalid_argument("Base64 資料不合法");
    output.resize(static_cast<std::size_t>(size)-padding);return output;
}
}
