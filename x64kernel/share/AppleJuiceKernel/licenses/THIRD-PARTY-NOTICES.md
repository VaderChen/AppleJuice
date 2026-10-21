# AppleJuiceKernel 第三方元件

CPU 核心採固定來源封存檔與 SHA-256 驗證，建置不下載浮動版本。

| 元件 | 用途 | 隨附授權 |
| --- | --- | --- |
| Zydis 4.1.1 | x86／x64 解碼 | [MIT](third_party/Zydis-LICENSE) |
| Zycore | Zydis 基礎工具 | [MIT](third_party/Zycore-LICENSE) |
| Berkeley SoftFloat 3e | 精確浮點語義 | [BSD 類授權](third_party/SoftFloat-LICENSE) |
| Sun fdlibm | atan／atan2／acos 原生配方的約化公式與係數 | [Sun 授權](third_party/fdlibm-LICENSE) |

可選 LLVM 區域編譯器使用建置環境提供的 LLVM 21.1 與其靜態相依函式庫；啟用時，散布者須一併保留該安裝的 LLVM 及相依元件聲明。預設核心不啟用 LLVM。

第三方元件保留各自授權；AppleJuiceKernel 的自訂授權不取代第三方條款。安裝 SDK 時會一併安裝本索引與上述授權全文。
