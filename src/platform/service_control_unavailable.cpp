#include "x64arm/windows_service_control.hpp"
namespace x64arm {
std::optional<NativeServiceCatalog> native_service_catalog(){return {};}
std::optional<NativeServiceStatus> native_service_status(std::u16string_view,bool){return {};}
}
