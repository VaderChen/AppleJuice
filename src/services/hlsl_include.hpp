#pragma once
#include "x64arm/windows_graphics.hpp"
#include <functional>
#include <optional>
namespace x64arm {
struct HlslIncludeSource {std::string name;std::vector<std::uint8_t> bytes;};
std::vector<std::uint8_t> graphics_signature_blob(std::span<const std::uint8_t>,unsigned);
// DXBC 容器的區塊：取出某個標籤的原始資料；或只保留 keep 為真的區塊並重新序列化（含校驗和）。
std::optional<std::vector<std::uint8_t>> graphics_dxbc_section(std::span<const std::uint8_t>,std::uint32_t tag);
std::vector<std::uint8_t> graphics_dxbc_filter(std::span<const std::uint8_t>,const std::function<bool(std::uint32_t)>& keep,
    std::optional<std::uint32_t> append_tag={},std::span<const std::uint8_t> append_data={});
using HlslIncludeLoader=std::function<std::optional<HlslIncludeSource>(std::string_view,bool,std::string_view)>;
GraphicsCompilation graphics_preprocess_shader(std::span<const std::uint8_t>,const std::string&,
    const std::vector<std::pair<std::string,std::string>>&,const HlslIncludeLoader&);
GraphicsCompilation graphics_assemble_shader(std::span<const std::uint8_t>,const std::string&,
    const std::vector<std::pair<std::string,std::string>>&,std::uint32_t,const HlslIncludeLoader&,bool* assembly_started=nullptr);
GraphicsCompilation graphics_compile_hlsl_includes(std::span<const std::uint8_t>,const std::string&,
    const std::string&,const std::string&,const std::vector<std::pair<std::string,std::string>>&,std::uint32_t,const HlslIncludeLoader&,std::uint32_t effect_flags=0);
}
