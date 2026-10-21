# 開始使用 AppleJuice

準備 Apple Silicon Mac、Xcode Command Line Tools、CMake 3.24+、Ninja、Python 3、Perl 與 make。Launcher 另需 Go 1.23+ 與 MinGW x64 GCC（`x86_64-w64-mingw32-gcc`）。Go 首次建置需要網路或已備妥的 module 快取；前端資源已隨附，一般建置不需 Node.js。

保留儲存庫隨附的完整 `x64kernel/`；不同版本的標頭與靜態函式庫不可混用。在根目錄執行，或由 Finder 雙擊同一檔案：

```sh
./run.command
```

完成後會開啟 `build/launcher/Win64 Launcher.app`。第一次建置會編譯固定相依套件，所需時間取決於硬體與快取。

1. 按「加入 EXE」，選取自行準備的 Windows 程式。
2. 確認工作資料夾、命令列與磁碟映射，保留程式附帶的 DLL 及資料目錄結構。
3. 需要安裝或儲存資料時啟用寫入權限。
4. 按播放圖示；CLI 使用內嵌終端，GUI 會開啟獨立視窗。
5. 查看退出碼與診斷紀錄；停止圖示可結束執行。

只建置命令列程式的方法見[建置與封裝](build-and-release.md)。設定及操作見[Launcher](launcher.md)，命令列參數見[執行與相容性](realworld-support.md)。

磁碟映射不是完整 OS 沙箱；啟用的相容服務可能修改映射檔案或使用網路。請只執行信任的程式，並依需要設定存取權限。
