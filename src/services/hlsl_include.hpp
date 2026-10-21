#pragma once
#include "x64arm/windows_graphics.hpp"
#include <functional>
#include <optional>
namespace x64arm {
struct HlslIncludeSource {std::string name;std::vector<std::uint8_t> bytes;};
std::vector<std::uint8_t> graphics_signature_blob(std::span<const std::uint8_t>,unsigned);
using HlslIncludeLoader=std::function<std::optional<HlslIncludeSource>(std::string_view,bool,std::string_view)>;
GraphicsCompilation graphics_preprocess_shader(std::span<const std::uint8_t>,const std::string&,
    const std::vector<std::pair<std::string,std::string>>&,const HlslIncludeLoader&);
GraphicsCompilation graphics_assemble_shader(std::span<const std::uint8_t>,const std::string&,
    const std::vector<std::pair<std::string,std::string>>&,std::uint32_t,const HlslIncludeLoader&);
GraphicsCompilation graphics_compile_hlsl_includes(std::span<const std::uint8_t>,const std::string&,
    const std::string&,const std::string&,const std::vector<std::pair<std::string,std::string>>&,std::uint32_t,const HlslIncludeLoader&,std::uint32_t effect_flags=0);
}
