#pragma once
#include <cstdint>

namespace x64arm::cng_catalog {
// 內建 CNG 演算法與所屬介面（BCRYPT_*_INTERFACE；也是 BCryptEnumAlgorithms 的運算類別位元序號）：
// 1 對稱加密、2 雜湊、3 非對稱加密、4 秘密協商、5 簽章、6 亂數、7 金鑰衍生。BCryptEnumAlgorithms 與 CNG 設定的預設函式清單共用這份表。
struct Algorithm {const char16_t* name;std::uint32_t kind;};
inline constexpr Algorithm algorithms[]{{u"AES",1},{u"MD5",2},{u"SHA1",2},{u"SHA256",2},{u"SHA384",2},{u"SHA512",2},{u"RSA",3},{u"DH",4},{u"ECDH_P256",4},{u"ECDH_P384",4},{u"ECDH_P521",4},
    {u"RSA_SIGN",5},{u"ECDSA_P256",5},{u"ECDSA_P384",5},{u"ECDSA_P521",5},{u"RNG",6},{u"PBKDF2",7},{u"SP800_108_CTR_HMAC",7},{u"SP800_56A_CONCAT",7},{u"CAPI_KDF",7}};
inline constexpr char16_t primitive_provider[]=u"Microsoft Primitive Provider";
}
