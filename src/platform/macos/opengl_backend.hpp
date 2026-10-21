#pragma once
#include "x64arm/windows_opengl.hpp"
#include <stdexcept>
#include <string_view>

namespace x64arm::detail {
// Choose once, before any guest state exists. Never turn a resource-budget
// failure into a second allocation attempt in a different driver.
template<class MetalFactory, class LegacyFactory>
std::shared_ptr<NativeOpenGlContext> select_opengl_backend(
    std::string_view mode, MetalFactory metal, LegacyFactory legacy) {
    if(mode=="legacy")return legacy(std::string{});
    if(mode=="metal")return metal();
    if(!mode.empty() && mode!="auto")
        throw std::invalid_argument("X64ARM_GL_BACKEND 必須為 auto、metal 或 legacy");
    try {return metal();}
    catch(const std::runtime_error& metal_error) {
        try {return legacy(std::string{metal_error.what()});}
        catch(const std::runtime_error& legacy_error) {
            throw std::runtime_error(std::string{"Metal 初始化失敗: "}+metal_error.what()+
                "; OpenGL fallback 初始化失敗: "+legacy_error.what());
        }
    }
}
}
