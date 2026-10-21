#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace x64arm::display_identity {
// 相容層的 ROOT 裝置，不宣稱宿主有 PCI Windows 驅動。User32、SetupAPI、CM 與 DXGI 共用此身分。
inline constexpr std::u16string_view instance=u"ROOT\\X64ARM_DISPLAY\\0000";
inline constexpr std::u16string_view class_name=u"{4d36e968-e325-11ce-bfc1-08002be10318}";
inline constexpr std::array<std::uint8_t,16> class_guid{0x68,0xe9,0x36,0x4d,0x25,0xe3,0xce,0x11,0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18};
inline constexpr std::u16string_view provider=u"x64arm Metal";
inline constexpr std::u16string_view version=u"0.1.0.0";
inline constexpr std::u16string_view date=u"10-06-2026";
// DriverDateData：與 date 相同日期（UTC 午夜）的 FILETIME。
inline constexpr std::uint64_t date_filetime=UINT64_C(0x01dd5525a1578000);
inline constexpr std::uint64_t version_number=UINT64_C(1)<<32;
// DXGI、DXCore 與 D3D12 使用相同的虛擬配接器 LUID。
inline constexpr std::uint64_t adapter_luid=0x78363461;
inline constexpr std::uint32_t vendor_id=0x106b;
inline constexpr std::u16string_view adapter_name=u"Apple Metal / VideoToolbox 共用圖形裝置";
inline constexpr std::string_view adapter_name_utf8="Apple Metal / VideoToolbox 共用圖形裝置";
}
