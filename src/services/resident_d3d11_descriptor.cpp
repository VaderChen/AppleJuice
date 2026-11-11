// 核心 D3D11 命令串流純量方法入口（resident_d3d11_arm64.inc）與服務端共用的描述元。
// 這個檔案不參與 LTO（見 CMakeLists.txt）：定義留在原生目的檔，連結時不會被內部化，核心原生碼與服務端都綁到這一份。
// 核心另有弱定義只供沒有服務端的核心測試連結；舊核心不引用時這份無人讀取。格式見 d3d11_command_stream.inc 的 ResidentD3D11StreamDescriptor。
#include <cstdint>
extern "C" { alignas(64) std::uint8_t x64arm_resident_d3d11_stream[512]={}; }
