#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm {
// 共用 OpenSSL 編解碼；不含 Windows 的 PEM 標頭、換行或寬字元契約。
std::string crypto_base64_encode(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> crypto_base64_decode(std::string_view text);
// 可複製的串流雜湊狀態；客體僅見 Windows handle，不能取得主機指標。
class CryptoHash {
public:
    CryptoHash() = default;
    CryptoHash(const std::string& algorithm, bool hmac, std::span<const std::uint8_t> key);
    void update(std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> digest() const;
    bool operator==(const CryptoHash& other) const;
private:
    struct State;
    std::shared_ptr<State> state_;
    std::uint64_t length_{};
};
struct CryptoAlgorithm {
    std::string name;
    bool hmac{}, reusable{};
    std::string chaining{"ChainingModeCBC"};
    bool operator==(const CryptoAlgorithm&) const = default;
};
struct CryptoKey {
    CryptoAlgorithm algorithm;
    std::uint32_t bits{};
    bool finalized{}, private_key{};
    std::vector<std::uint8_t> secret, modulus, exponent, generator, public_value;
    // RSA／ECC 與 NCrypt 共用經過驗證的 DER，不保存客體可見的原生指標。
    std::vector<std::uint8_t> der;
    bool operator==(const CryptoKey&) const = default;
};
// 原生串流狀態採 copy-on-write，供金鑰複製與 Interpreter／JIT 狀態比較共用。
class CryptoCipher {
public:
    CryptoCipher() = default;
    CryptoCipher(const std::string&,std::span<const std::uint8_t> key,
                 std::span<const std::uint8_t> iv,unsigned effective_bits,bool encrypt);
    std::vector<std::uint8_t> update(std::span<const std::uint8_t>);
    explicit operator bool() const {return bool(state_);}
    bool operator==(const CryptoCipher&) const;
    static bool available(const std::string&);
private:
    struct State;
    std::shared_ptr<State> state_;
    CryptoHash history_;
};
struct LegacyCryptProvider {
    std::uint32_t type{},flags{},algorithm_cursor{},extended_cursor{},session_bits{};
    std::string name,container;
    std::uint32_t references{1},container_cursor{};
    std::vector<std::string> containers;
    std::string user_scope;
    std::uint64_t client_window{};
    bool operator==(const LegacyCryptProvider&) const = default;
};
struct LegacyCryptKey {
    std::uint64_t provider{};std::uint32_t algorithm{},bits{};
    bool private_key{},exportable{};std::vector<std::uint8_t> der;
    std::vector<std::uint8_t> secret,iv,salt,oaep_label;
    std::uint32_t mode{1},mode_bits{8},padding{1},effective_bits{40},permissions{0x1b};
    CryptoCipher cipher;bool encrypting{};
    bool operator==(const LegacyCryptKey&) const = default;
};
struct LegacyCryptHash {
    std::uint64_t provider{};std::uint32_t algorithm{};CryptoHash hash,tail;
    std::vector<std::uint8_t> value;bool finished{};
    bool operator==(const LegacyCryptHash&) const = default;
};
struct CryptoHashObject {
    CryptoAlgorithm algorithm;
    CryptoHash hash;
    std::vector<std::uint8_t> key;
    std::uint64_t buffer{};
    bool finished{};
    bool operator==(const CryptoHashObject&) const = default;
};
}
