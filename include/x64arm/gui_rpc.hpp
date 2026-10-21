#pragma once
#include <cstdint>

namespace x64arm::gui_rpc {
// 引擎內部 callback packet；兩種客體位元數共用固定偏移，不是公開 Windows 結構。
// Invoke(thread,packet,result64,transport_hresult32) 以 out 參數分離 callback 結果與傳輸錯誤。
inline constexpr unsigned target=0,count=8,wide_arguments=12,flags=16,register_arguments=20,arguments=24,size=152;
inline constexpr std::uint32_t wide_result=1;
inline constexpr std::uint32_t invalid_argument=0x80070057U,disconnected=0x80010108U,out_of_memory=0x8007000eU;
}
