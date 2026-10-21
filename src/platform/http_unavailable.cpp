#include "x64arm/autoproxy.hpp"
namespace x64arm {
std::shared_ptr<NativeHttp> start_native_http(HttpInput){return {};}
std::shared_ptr<NativeAutoProxy> start_native_autoproxy(AutoProxyInput){return {};}
}
