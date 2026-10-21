# Direct3D 與 x86 相容性

以下說明目前提供的功能範圍，並非所有應用程式、API 參數或硬體功能的完整相容承諾。實際可用性取決於程式使用的 API、著色器、資源格式及宿主 Metal 能力。

## Direct3D 9

提供 D3D9／D3D9Ex 裝置、紋理與緩衝區、固定功能與可程式化繪圖，以及交換鏈與呈現，透過 Metal 繪製。傳統程式的特殊格式、裝置能力與渲染狀態組合仍有相容性邊界。

## Direct3D 10／10.1

提供 D3D10／D3D10.1 裝置建立、資源、著色器、繪圖與 DXGI 交換鏈，連接 Metal。支援範圍須依實際功能與格式確認，不能只根據裝置建立成功或版本號判定完整支援。

## Direct3D 11

提供裝置與 context、資源與 view、DXBC 著色器轉譯、Draw／Dispatch 與 DXGI 呈現，包含圖形與 GPU 運算路徑。進階功能、格式及擴充介面仍須依程式需求確認；回報 feature level 不代表其全部功能均已完整支援。

## Direct3D 12

提供裝置、命令佇列與清單、資源與描述元、根簽章、繪圖／運算、fence 及 DXGI 呈現的功能子集，透過 Metal 執行。已有間接命令與 Bundle 路徑；DXR 光線追蹤、Mesh Shader、Work Graph 等進階功能不在完整相容承諾內。

## x86（32 位元 Windows）

除了 x86_64（64 位元），亦可載入 x86 的 PE32 EXE 與相符 DLL，依映像架構處理 32 位元指標及 Windows 呼叫慣例。Launcher 與 CLI 使用相同相容層，32／64 位元程式預設均使用 resident AOT。

應用程式附帶 DLL 必須與 EXE 的位元數相符；不能在同一程序混載 32 位元與 64 位元 DLL。支援 x86 不代表每個 Windows API 或圖形功能都與 x64 有相同涵蓋範圍，也不代表完整 WOW64、16 位元程式、Windows 核心驅動程式或 DRM 均可使用。

## 使用建議

若應用程式提供多種圖形 API，可分別確認所選版本需要的功能；較舊版本不保證一定可執行。保留原程式的 DLL 與資料目錄，使用符合 EXE 架構的相依元件。問題回報請附程式版本、32／64 位元、選用的圖形 API、重現步驟及移除個人資料的診斷。

[執行與相容性](realworld-support.md) · [Launcher 操作](launcher.md) · [OpenCL／Vulkan](opencl-vulkan-support.md)
