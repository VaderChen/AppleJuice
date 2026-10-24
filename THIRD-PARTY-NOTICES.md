# AppleJuice 第三方授權索引

AppleJuice 的[自訂授權](LICENSE.md)適用於著作權人有權提供的自有內容。以下元件及其他另附授權的內容，仍依各自的授權與著作權聲明處理；本索引不取代原授權全文，也不代表所有元件使用相同條款。

## 引擎與圖形工具

版本、固定來源、封存檔雜湊及整合方式見 [third_party/README.md](third_party/README.md)。

| 元件 | 原始授權全文 |
| --- | --- |
| Zydis／Zycore | [Zydis](x64kernel/share/AppleJuiceKernel/licenses/Zydis-LICENSE)、[Zycore](x64kernel/share/AppleJuiceKernel/licenses/Zycore-LICENSE) |
| Berkeley SoftFloat | [SoftFloat](x64kernel/share/AppleJuiceKernel/licenses/SoftFloat-LICENSE) |
| OpenSSL | [OpenSSL](third_party/OPENSSL-LICENSE) |
| ICU | [ICU](third_party/ICU-LICENSE) |
| Microsoft ESE HSIAO ECC／XOR 校驗算法（MIT，移植為有界 C++20 讀取） | [ESE](third_party/ese-reference-LICENSE)；[固定來源](third_party/ese-reference-source.json) |
| HLSLcc／vkd3d-shader | [HLSLcc](third_party/HLSLCC-LICENSE)、[vkd3d](third_party/VKD3D-LICENSE) |
| SPIR-V headers／glslang／SPIRV-Cross | [Headers](third_party/SPIRV-HEADERS-LICENSE)、[glslang](third_party/GLSLANG-LICENSE)、[SPIRV-Cross](third_party/SPIRV-CROSS-LICENSE) |
| LLVM Microsoft demangler | [LLVM](third_party/LLVM-DEMANGLER-LICENSE) |
| DirectXTex／DirectXMath／DirectX-Headers | [DirectXTex](third_party/DIRECTXTEX-LICENSE)、[DirectXMath](third_party/DIRECTXMATH-LICENSE)、[Headers](third_party/DIRECTX-HEADERS-LICENSE) |
| Vulkan-Headers 1.4.357（標頭與 vk.xml，供內建 vulkan-1.dll 及產生器使用） | [Vulkan-Headers](third_party/vulkan-headers-1.4.357/LICENSE.md) |
| DirectML 1.15.4 標頭（`include/`，MIT）與 DirectMLSchema.h／DirectMLX.h（microsoft/DirectML，MIT）；不含 DirectML.dll | [DirectML 標頭](third_party/directml-1.15.4/LICENSE-CODE.txt)、[microsoft/DirectML](third_party/directml-1.15.4/schema/LICENSE) |
| DirectXMesh／UVAtlas／SAL header | [DirectXMesh](third_party/DIRECTXMESH-LICENSE)、[UVAtlas](third_party/UVATLAS-LICENSE)、[SAL](third_party/directx-sal/LICENSE) |

LLVM demangler 的聲明對應專案保存的 demangler 來源；另外使用的系統工具鏈與 LLVM 執行期來源，應依各自隨附的授權處理，不以 demangler 聲明代替其授權範圍。

## 啟動器

| 元件 | 原始授權全文 |
| --- | --- |
| webview_go／webview | [Go binding](launcher/licenses/webview-go-LICENSE)、[webview](launcher/licenses/webview-LICENSE) |
| creack/pty | [pty](launcher/licenses/pty-LICENSE) |
| xterm.js／fit addon | [xterm.js](launcher/web/vendor/xterm-LICENSE)、[fit addon](launcher/web/vendor/addon-fit-LICENSE) |

## 其他來源與散布

封存檔及 vendored 來源內的逐檔聲明仍須保留；以原授權全文及實際散布內容為準。SDK 樣本、外部 EXE／DLL、字型及測試素材依來源另行判斷。Apple SDK 與系統框架依其提供者條款使用，不因本索引而獲得重新散布權利。

專案範圍說明見[授權文件](docs/licensing.md)。
