// 沒有宿主色彩管理時無法建立轉換。
#include "x64arm/color_transform.hpp"

namespace x64arm {
struct NativeColorTransform {};
std::shared_ptr<NativeColorTransform> native_color_transform(const std::vector<std::vector<std::uint8_t>>&,const std::vector<std::uint32_t>&){return nullptr;}
bool native_color_convert(const NativeColorTransform&,std::size_t,unsigned,const float*,unsigned,float*,bool){return false;}
}
