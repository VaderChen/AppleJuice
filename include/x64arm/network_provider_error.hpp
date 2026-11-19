#pragma once

namespace x64arm::network_resources {
// 原生同步呼叫的補充錯誤；MPR 在返回客體前轉存到對應客體執行緒。
inline thread_local int native_provider_error{};
}
