#pragma once
#include "x64arm/memory.hpp"
#include <array>
#include <cstdint>
// DxDiag（dxdiagn.dll）COM 識別碼，數值依 DirectX SDK dxdiag.h。
namespace x64arm::dxdiag {
using Guid=std::array<std::uint8_t,16>;
// CLSID_DxDiagProvider {A65B8071-3BFE-4213-9A5B-491DA4461CA7}
constexpr Guid clsid_provider{0x71,0x80,0x5b,0xa6,0xfe,0x3b,0x13,0x42,0x9a,0x5b,0x49,0x1d,0xa4,0x46,0x1c,0xa7};
// IID_IDxDiagProvider {9C6B4CB0-23F8-49CC-A3ED-45A55000A6D2}
constexpr Guid iid_provider{0xb0,0x4c,0x6b,0x9c,0xf8,0x23,0xcc,0x49,0xa3,0xed,0x45,0xa5,0x50,0x00,0xa6,0xd2};
// IID_IDxDiagContainer {7D0F462F-4064-4862-BC7F-933E5058C10F}
constexpr Guid iid_container{0x2f,0x46,0x0f,0x7d,0x64,0x40,0x62,0x48,0xbc,0x7f,0x93,0x3e,0x50,0x58,0xc1,0x0f};
constexpr Guid iid_unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr Guid iid_class_factory{1,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::uint32_t dx9_sdk_version=111;
constexpr std::uint32_t e_insufficient_buffer=0x8007007aU; // DXDIAG_E_INSUFFICIENT_BUFFER
inline bool is_class(const GuestMemory& memory,std::uint64_t p){if(!p)return false;Guid g{};memory.read(GuestAddress{p},g);return g==clsid_provider;}
}
