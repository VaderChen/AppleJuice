#pragma once
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::cng {
using Bytes=std::vector<std::uint8_t>;
using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
using Context=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
using Big=std::unique_ptr<BIGNUM,decltype(&BN_clear_free)>;
// 共用 CNG 金鑰格式及運算；BCrypt 邊界將 NTE 錯誤轉成 NTSTATUS。
constexpr std::uint32_t invalid=0x80090027,bad_flags=0x80090009,unsupported=0x80090029;
constexpr std::uint32_t bad_data=0x80090005,no_memory=0x8009000e,bad_signature=0x80090006;
struct Error {std::uint32_t code;};
inline void require(bool condition,std::uint32_t code=invalid){if(!condition)throw Error{code};}
struct Curve {const char* algorithm;const char* group;unsigned bits;std::uint32_t magic;};
const Curve* curve(std::string_view);
bool rsa(std::string_view);
bool supported(std::string_view);
void word(Bytes&,std::uint32_t);
std::uint32_t word(std::span<const std::uint8_t>,std::size_t);
Bytes number(const BIGNUM*,unsigned);
Big component(EVP_PKEY*,const char*);
Bytes der(EVP_PKEY*,bool private_key);
Key decode(std::span<const std::uint8_t>,bool private_key);
void validate(EVP_PKEY*,bool private_key);
std::string algorithm(EVP_PKEY*);
Key generate(std::string_view,unsigned bits);
Key from_blob(const Bytes&,std::string_view,std::string& algorithm,bool& private_key);
Bytes blob(EVP_PKEY*,std::string_view algorithm,unsigned bits,std::string type,bool private_key);
struct SignatureOptions {unsigned padding{};const EVP_MD* digest{};unsigned salt{};};
unsigned signature_size(EVP_PKEY*);
Bytes sign(EVP_PKEY*,std::span<const std::uint8_t>,const SignatureOptions&,bool size_only=false);
void verify(EVP_PKEY*,std::span<const std::uint8_t>,std::span<const std::uint8_t>,const SignatureOptions&);
struct CipherOptions {unsigned padding{};const EVP_MD* digest{};Bytes label;};
Bytes crypt(EVP_PKEY*,std::span<const std::uint8_t>,const CipherOptions&,bool encrypt,bool size_only=false);
Bytes agreement(EVP_PKEY*,EVP_PKEY*);
}
