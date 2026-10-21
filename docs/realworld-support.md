# Windows 程式執行與相容性

命令列程式為 `x64arm`。請自行準備 EXE、附帶 DLL 與資料檔；儲存庫不提供商用 Windows 程式或 Windows 系統檔案。

```sh
./build/release/x64arm doctor
./build/release/x64arm run-pe examples/program.exe \
  --backend jit --gui native --unresolved-imports trap \
  --fs-root examples/program-directory \
  --command-line 'program.exe' --max-instructions 10000000000
```

將範例路徑換成自己的程式與目錄。文字程式可使用 `--stdio live`；需要寫入映射檔案時加入 `--fs-mode read-write`。未知 API 的 `trap` 行為會在實際呼叫時停止，不能使未支援功能自動成功。指令預算耗盡時可調整 `--max-instructions` 並重新確認進度。

AppleJuice 提供 PE／DLL 載入及 Windows 相容服務，將視窗、繪圖、音訊與輸入連接至 macOS 原生介面。完整 Windows、任意遊戲與所有 API 參數組合並未全面支援；DRM、核心驅動程式及特定硬體功能亦可能阻止應用程式執行。成功載入或顯示視窗不代表後續功能均可使用。

OpenCL／Vulkan 另見[環境與限制](opencl-vulkan-support.md)。遇到問題時提供程式版本、操作步驟、預期及實際結果、AppleJuice 版本與已移除個人資料的診斷片段。
