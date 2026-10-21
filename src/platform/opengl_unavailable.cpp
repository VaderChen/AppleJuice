#include "x64arm/windows_opengl.hpp"
#include <stdexcept>
namespace x64arm {
std::shared_ptr<NativeOpenGlContext> make_metal_opengl(void*,std::uint32_t,std::uint32_t){throw std::runtime_error("此平台未提供 Metal 後端");}
std::optional<std::pair<std::string,std::string>> metal_opengl_identity(){return std::nullopt;}
std::shared_ptr<NativeOpenGlContext> make_native_opengl(void*,std::uint32_t,std::uint32_t){throw std::runtime_error("此平台未提供硬體 OpenGL 後端");}
}
