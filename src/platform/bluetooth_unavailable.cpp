#include "x64arm/bluetooth.hpp"

namespace x64arm::bluetooth {
// 沒有原生後端的平台明確回報缺少能力，由呼叫端依同一契約處理。
std::shared_ptr<Backend> make_bluetooth_backend(NativeOptions){return {};}
}
