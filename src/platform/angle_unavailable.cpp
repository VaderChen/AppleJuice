#include "x64arm/angle.hpp"
#include <cstdlib>
namespace x64arm {
std::unique_ptr<NativeAngle> make_native_angle(){if(const auto* choice=std::getenv("X64ARM_ANGLE_BACKEND");choice && std::string_view(choice)=="metal")throw std::runtime_error("此平台無 ANGLE Metal 後端");return {};}
}
