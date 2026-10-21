#include "x64arm/windows_gui.hpp"
#include "x64arm/windows_control_images.hpp"
#include <stdexcept>
namespace x64arm {
std::shared_ptr<NativeGui> make_native_gui(const std::filesystem::path&) { throw std::runtime_error("此平台尚未提供原生 GUI presenter，請使用 headless 模式"); }
std::optional<GuiBitmap> render_gui_header_image(const GuiWindow&,std::optional<std::size_t>,const GuiHeaderRenderStyle&){return {};}
}
