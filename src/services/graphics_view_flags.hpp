#pragma once
#include <cstdint>
namespace x64arm {
// 內部取樣 view：原始深度紋理把 R 值複製到 RGBA；不與公開 UAV 旗標重疊。
inline constexpr std::uint32_t graphics_view_replicate_red=0x10000000U;
inline constexpr std::uint32_t graphics_view_red_only=0x20000000U;
// D3D12 平面式深度模板的子資源傳輸：只傳深度平面（每像素 4 位元組：32 位元浮點，或 24 位元 UNORM 置於低 24 位元）
// 或只傳 stencil 平面（每像素 1 位元組）。未設定時沿用交錯格式（D3D11／DXGI 的 D24S8、D32S8X24）。
inline constexpr std::uint32_t graphics_view_depth_plane=0x40000000U;
inline constexpr std::uint32_t graphics_view_stencil_plane=0x80000000U;
// 依平面旗標取得傳輸時每像素的格式：深度平面 R32_TYPELESS（39）、stencil 平面 R8_TYPELESS（60），其餘為資源格式。
inline std::uint32_t graphics_view_plane_format(std::uint32_t flags,std::uint32_t format){
    return (flags&graphics_view_stencil_plane)?60U:(flags&graphics_view_depth_plane)?39U:format;
}
}
