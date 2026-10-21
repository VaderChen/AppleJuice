# AppleJuice 建置與封裝

所有命令由儲存庫根目錄執行。工具需求見[開始使用](getting-started.md)。儲存庫包含程式來源、前端資源、固定第三方來源與 `x64kernel/` 靜態 SDK；一般建置不需要核心來源。

## 命令列程式

```sh
cmake --preset release
cmake --build --preset release --target x64arm --parallel 4
./build/release/x64arm doctor
```

`cmake --build --preset release --parallel 4` 可建置預設目標。Debug 使用 `debug` preset。CMake 會檢查 SDK 的宿主系統、架構與 sanitizer 設定是否匹配；隨附 SDK 的設定見 `x64kernel/manifest.json`。

需要使用另一份完整且相容的 SDK 時，可指定相對路徑：

```sh
cmake --preset release -DAPPLEJUICE_KERNEL_ROOT=vendor/x64kernel
```

回到隨附版本時指定 `-DAPPLEJUICE_KERNEL_ROOT=x64kernel`。標頭、函式庫、CMake 套件及授權檔案必須來自同一份 SDK。

## 執行後端與快取

`--backend jit` 在 32／64 位元程式均預設使用 resident AOT；Launcher 執行及 `precompile-pe` 也採用相同選擇。`X64ARM_JIT_MODULE_AOT=0` 明確切回原 JIT，`=1` 明確指定 resident AOT；未設定時仍尊重既有的 LLVM 後端選項。`--backend interpreter` 仍使用直譯器。

resident AOT 的預編譯快取為 `.aot.smap`，執行期快取為 `.smap`。一般執行預設不讀寫持久快取；只有 `X64ARM_AOT_PERSISTENT=1` 才啟用。手動 `precompile-pe` 仍會產生預編譯快取。原 JIT 則以 `X64ARM_SMAP=1` 開啟快取。

## Launcher

```sh
python3 scripts/build_launcher.py --jobs 4
```

產物為 `build/launcher/Win64 Launcher.app`。腳本建置引擎與 Go 前端、收集資源及授權、簽署套件，並以隨附的最小 PE 來源執行封裝檢查；因此需安裝 MinGW x64 GCC。封裝失敗時保留既有 app。

指定另一個輸出位置：

```sh
python3 scripts/build_launcher.py --engine-build-dir build/release \
  --output build/Launcher-Test.app --jobs 4
```

已有相容引擎時可用 `--skip-engine`；`--force-package` 重新封裝。前端資源已隨附，只有使用 `--refresh-assets` 時才需 Node.js／npm。完整參數可用 `python3 scripts/build_launcher.py --help` 查看。

## 散布成品

散布完整 Launcher app 時保留內部目錄、簽章及 `Contents/Resources/Licenses/`。單獨散布 CLI 亦需附帶適用授權。不要封裝個人設定、客體程式、Windows 磁碟資料、憑證或私鑰。

本機預設為 ad-hoc 簽署；Developer ID 與公證須自行準備並完成，見[macOS 簽署](macos-signing.md)。授權見[適用範圍](licensing.md)及[第三方聲明](../THIRD-PARTY-NOTICES.md)。
