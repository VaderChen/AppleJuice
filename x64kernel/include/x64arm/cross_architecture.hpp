#pragma once
namespace x64arm {
// 只在目前宿主執行緒成功啟用後回傳 true；編譯器 worker 不必呼叫。
bool enter_hardware_tso() noexcept;
// APFLG 必須另外以 S 指令實測。產碼政策在程序內固定，所有原生入口
// 仍驗證目前執行緒；X64ARM_APFLG=0 保留相同核心的軟體對照。
bool hardware_apflg_enabled() noexcept;
void require_hardware_apflg();
// 必須在 CLI 建立執行狀態與客體資源之前呼叫；SETEXEC 保留 PID 與傳入描述符。
void initialize_cross_architecture(char** argv);
}
