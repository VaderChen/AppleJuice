#pragma once
namespace x64arm {
// D3D12 PSO 建立遇到尚未翻譯完成的著色器：協調者本地路徑（持獨占映射鎖、暫時切換目前執行緒）不能讓呼叫者等待，
// 丟出此例外交回完整路徑，由完整路徑以 ShaderRetry 讓呼叫的客體執行緒等待（windows_d3d12.cpp 的 shaders_pending）。
struct D3D12ShaderPending {};
// 目前是否在 D3D12 協調者本地路徑內執行服務（每執行緒）。
inline bool& d3d12_local_call_flag() noexcept {static thread_local bool value=false;return value;}
}
