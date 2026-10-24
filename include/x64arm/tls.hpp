#pragma once
#include <array>
#include "x64arm/sspi_digest.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <set>
namespace x64arm {
struct NativeTlsSession;
struct TlsCredentials {
    std::uint32_t protocols{},flags{},minimum_bits{},maximum_bits{};
    bool operator==(const TlsCredentials&) const = default;
};
struct TlsState {
    sspi_digest::State digest;
    std::map<std::uint64_t,TlsCredentials> credentials;
    std::map<std::uint64_t,std::shared_ptr<NativeTlsSession>> sessions;
    std::set<std::uint64_t> buffers;
    std::uint64_t next_handle{0xb0000};
    // InitSecurityInterfaceA／W 傳回的 SecurityFunctionTable（程序內固定位址，0 表示尚未建立）。
    std::array<std::uint64_t,2> function_tables{};
    TlsState() = default;
    TlsState(const TlsState& other);
    TlsState& operator=(const TlsState& other);
    TlsState(TlsState&&) noexcept = default;
    TlsState& operator=(TlsState&&) noexcept = default;
    bool operator==(const TlsState&) const = default;
};
}
