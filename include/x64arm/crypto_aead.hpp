#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace x64arm {
// AES-256-GCM 共用原生運算；認證失敗會清除暫存明文，不發布部分結果。
bool crypto_aes_gcm(bool encrypt,std::span<const std::uint8_t> key,std::span<const std::uint8_t> nonce,
                    std::span<const std::uint8_t> associated,std::span<const std::uint8_t> input,
                    std::vector<std::uint8_t>& output,std::array<std::uint8_t,16>& tag);
}
