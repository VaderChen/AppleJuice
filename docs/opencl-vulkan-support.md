# OpenCL 與 Vulkan 支援

AppleJuice 的 Windows 相容層已加入 x64 OpenCL 與 Vulkan 原生橋接。客體使用內建 `OpenCL.dll`／`vulkan-1.dll`，不需要複製 Windows 系統 DLL；宿主仍需具備相應的 macOS 原生後端。目前不提供這兩個橋接的 x86（32 位元）支援。

## 後端與支援範圍

| API | 宿主後端 | 已加入的處理路徑 |
| --- | --- | --- |
| OpenCL | macOS OpenCL.framework | 平台與裝置查詢、context／queue、緩衝與影像、sampler、程式編譯、kernel 參數與 NDRange 派送、讀寫／複製／填入、映射與同步 |
| Vulkan | 原生 Vulkan loader 或 MoltenVK | instance／device、屬性與功能查詢、資源與記憶體、descriptor、pipeline、命令緩衝與運算派送 |

OpenCL 目前以宿主 OpenCL 1.2 路徑為基礎，實際裝置能力以查詢結果為準；這條路徑直接使用 OpenCL.framework。Vulkan 使用 MoltenVK 時由其對接 Metal，不保證所有 Vulkan 版本、擴充或應用程式可用。

## 環境設定

OpenCL 於建置時連結 macOS OpenCL.framework。Vulkan 標頭與產生器登錄檔隨原始碼提供，原生 Vulkan loader／MoltenVK 動態函式庫需另外準備。執行時會搜尋原生函式庫，也可用 `X64ARM_VULKAN_LIBRARY` 指定，例如 `./vendor/vulkan/libMoltenVK.dylib`。未找到可用後端時，建立 Vulkan instance 會回報不相容驅動。

需要追蹤呼叫時可設定 `X64ARM_TRACE_OPENCL=1` 或 `X64ARM_TRACE_VULKAN=1`，搭配 CLI 診斷紀錄排查。

## 目前限制

- OpenCL 尚不支援 SVM、pipes、device queue 與 SPIR-V IL 程式建立；較新 API 的入口存在不表示功能已實作。回呼及跨 API 資源共享也不可視為全面支援。
- Vulkan 公告的擴充取橋接允許清單與宿主能力的交集；不公開驗證 layer。部分 surface／swapchain 入口供查詢使用，不代表 Win32 surface 與視窗呈現流程已支援。
- 兩者的原生後端可用、API 可解析或單一工作完成，都不能替代完整應用程式驗收。
