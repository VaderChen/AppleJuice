# Geekbench 7 單核 CPU 測試

本文說明如何以 Win64 Launcher 執行 Windows 版 Geekbench 7 的單核 CPU 測試，並在同一台 Mac 上取得 macOS 原生與 Rosetta 2 的對照結果。依照相同步驟、相同版本與相同設定，就能重現測試。

Geekbench 是 Primate Labs 的商用軟體，本儲存庫不提供安裝檔或授權。請自行取得 Windows 與 macOS 版 Geekbench 7，兩者版本須相同。

## 需求

- Apple Silicon Mac，並已依[開始使用](getting-started.md)建置 Launcher。
- Windows 版 Geekbench 7，以及版本相同的 macOS 版 Geekbench 7。
- Geekbench 7 Pro 授權。`--single-core`、`--section`、`--workload` 與 `--workload-list` 都是 Pro 選項，未啟用 Pro 時 Geekbench 會拒絕執行。

以下命令都從儲存庫根目錄開始執行。範例使用這些路徑，可依需要更改：

```sh
export ENGINE="$PWD/build/launcher/Win64 Launcher.app/Contents/MacOS/x64arm-engine"
export SANDBOX_C="$HOME/AppleJuice/geekbench-c"
export GB_DIR="$SANDBOX_C/Program Files/Geekbench 7"
export GB_MAC="/Applications/Geekbench 7.app/Contents/Resources"
export RESULTS="$HOME/AppleJuice/geekbench-results"
mkdir -p "$SANDBOX_C" "$RESULTS"
```

`SANDBOX_C` 是專供測試使用的 Windows C 槽資料夾。不要與日常使用的 C 槽共用，以免其他程式的檔案影響結果。

## 1. 記錄引擎狀態

```sh
"$ENGINE" doctor
```

請記錄輸出中的以下幾行。這些值不同的結果不能直接比較：

| 欄位 | 用途 |
|---|---|
| `x64arm` 版本與「建置」 | 確認引擎版本 |
| `hw.model`、`machdep.cpu.brand_string` | 確認硬體 |
| 「宿主頁大小」 | 確認頁面大小 |
| 「客體記憶體排序」 | 例如「硬體 TSO」 |
| 「AF／PF 旗標」 | 例如「硬體 APFLG」 |

記憶體排序與旗標模式取決於 Launcher 的簽署與宿主環境。

## 2. 安裝 Windows 版 Geekbench 7

1. 在 Launcher 按「臨時執行」，選取 Windows 版 Geekbench 7 安裝程式。
2. 在「程式設定」把「C 槽映射」設為 `SANDBOX_C` 資料夾。
3. 勾選「允許程式寫入上述資料夾」，再按「執行」。
4. 安裝到預設位置 `C:\Program Files\Geekbench 7`。

安裝完成後，`GB_DIR` 內應有 `geekbench7.exe`、`geekbench_avx2.exe` 與 `geekbench_x86_64.exe`。`geekbench7.exe` 會依 CPU 功能選擇實際執行的版本，測試時一律啟動它。

```sh
ls "$GB_DIR"/geekbench*.exe
```

## 3. 啟用 Geekbench Pro

Launcher 不會把 Windows 登錄檔保存到下一次執行，因此在 Launcher 內用 `--unlock` 解鎖不會持續生效。請改用 Geekbench 的獨立授權檔。

在已解鎖 Pro 的 macOS 版 Geekbench 7 匯出授權檔，再複製到 Windows 版的程式資料夾。Windows 版讀取的檔名是 `Geekbench.preferences`，與 macOS 匯出的檔名不同，複製時必須改名：

```sh
"$GB_MAC/geekbench_aarch64" --export-license "$RESULTS"
cp "$RESULTS/Geekbench 7.preferences" "$GB_DIR/Geekbench.preferences"
```

確認 Pro 已生效。第一行應顯示 `Geekbench 7.0.0 Pro`，並列出各區段與測項：

```sh
cd "$GB_DIR"
"$ENGINE" run-pe "$GB_DIR/geekbench7.exe" \
  --backend jit --unresolved-imports trap --gui headless \
  --clock host --stdio live --max-instructions 18446744073709551615 \
  --cpu-count 1 --memory-limit-mib 8192 \
  --c-drive "$SANDBOX_C" --fs-mode read-write \
  --command-line '"geekbench7.exe" --workload-list'
cd - >/dev/null
```

授權檔含個人授權資料。不要提交、上傳或分享它，也不要把含有它的 C 槽資料夾打包散布。

## 4. 在 Launcher 執行單核測試

1. 按「加入 EXE」，選取 `GB_DIR` 內的 `geekbench7.exe`。
2. 開啟這個程式的「程式設定」，依下表設定。
3. 按「執行」。結果顯示在內嵌終端，JSON 寫到 `SANDBOX_C` 根目錄。

| 設定 | 值 |
|---|---|
| 工作資料夾 | `GB_DIR` |
| C 槽映射 | `SANDBOX_C` |
| 允許程式寫入上述資料夾 | 勾選 |
| 此程式 CPU 核心數 | 1 |
| 啟動參數 | `--cpu --single-core --no-upload --export-json C:\geekbench-single-core.json` |

只執行單一測項時，加上 `--section 1 --workload <編號>`。可用逗號列出多個編號：

```text
--cpu --single-core --section 1 --workload 502 --no-upload --export-json C:\geekbench-502.json
```

`--no-upload` 避免把結果上傳到 Geekbench Browser。執行狀態、退出碼與引擎診斷紀錄可在「執行紀錄」查看。

## 5. 等效的終端命令

下列命令使用 Launcher 內附的引擎，套用 Launcher 執行 CLI 程式時傳給引擎的執行參數，適合腳本化或重複量測。Launcher 另外傳遞的程序間工作階段參數在此省略：

```sh
cd "$GB_DIR"
"$ENGINE" run-pe "$GB_DIR/geekbench7.exe" \
  --backend jit --unresolved-imports trap --gui headless \
  --gpu-memory-mib 256 --clock host --stdio live \
  --max-instructions 18446744073709551615 \
  --cpu-count 1 --memory-limit-mib 8192 \
  --c-drive "$SANDBOX_C" --fs-mode read-write \
  --diagnostics-file "$RESULTS/applejuice-engine.log" \
  --command-line '"geekbench7.exe" --cpu --single-core --no-upload --export-json C:\geekbench-single-core.json'
cd - >/dev/null
cp "$SANDBOX_C/geekbench-single-core.json" "$RESULTS/applejuice.json"
```

各參數與 Launcher 設定的對應：

| 參數 | 對應 |
|---|---|
| `--cpu-count` | 「此程式 CPU 核心數」 |
| `--c-drive` | 「C 槽映射」 |
| `--fs-mode read-write` | 「允許程式寫入上述資料夾」 |
| `--gpu-memory-mib` | 「GPU 記憶體」，預設 256 |
| `--memory-limit-mib` | 由 Launcher 自動設定，見下方說明 |
| `--command-line` | 程式名稱加上「啟動參數」 |

`--memory-limit-mib` 由 Launcher 自動設為 8192 與宿主實體記憶體一半兩者中較小的值。宿主記憶體為 16 GB 以上時是 8192，請依自己的機器調整。

直接使用 `x64arm` 命令列時，不要省略 `--max-instructions` 與 `--memory-limit-mib`。命令列的預設指令預算是 10,000,000 條，記憶體上限是 1024 MiB，只適合短程測試。Geekbench 連 `--workload-list` 都會在預設預算內耗盡而停止。

## 6. macOS 原生與 Rosetta 2 對照

使用版本相同的 macOS 版 Geekbench 7。`geekbench_aarch64` 是原生 ARM64 版本；`geekbench_avx2` 是 x86-64 版本，透過 `arch -x86_64` 由 Rosetta 2 執行：

```sh
"$GB_MAC/geekbench_aarch64" --cpu --single-core --no-upload \
  --export-json "$RESULTS/native.json"
arch -x86_64 "$GB_MAC/geekbench_avx2" --cpu --single-core --no-upload \
  --export-json "$RESULTS/rosetta.json"
```

單一測項同樣可加上 `--section 1 --workload <編號>`。macOS 與 Windows 版 Geekbench 使用相同的測項與工作量，但屬於不同平台的建置，作業系統程式庫也不同。比較結果代表整體執行路徑的差異，不只是指令轉譯本身。

## 7. 讀取結果

Geekbench 7 的單核區段（`--section 1`）共有 16 個測項：

| 編號 | 測項 | 編號 | 測項 |
|---|---|---|---|
| 101 | File Compression | 403 | HDR |
| 102 | Navigation | 404 | Photo Editor |
| 103 | HTML5 Browser | 501 | Ray Tracer |
| 104 | PDF Viewer | 502 | Structure from Motion |
| 105 | Photo Library | 603 | Game Physics |
| 201 | Clang | 701 | Video Encoder |
| 202 | Text Processing | 702 | Audio Encoder |
| 203 | Asset Compression | 703 | Video Player |

可用 `--workload-list` 確認你的版本所列的測項。

JSON 中的 `sections[].workloads[]` 為各測項結果，欄位如下：

| 欄位 | 內容 |
|---|---|
| `id`、`name` | 測項編號與名稱 |
| `score` | 分數 |
| `runtime` | 執行時間，單位為秒 |
| `valid` | 結果是否有效 |

`version` 與 `options` 記錄 Geekbench 版本、迭代次數與測項間隔，比較的兩方必須相同。

以下指令列出兩份結果的各測項分數，以及執行時間比值。比值是 AppleJuice 除以 Rosetta 2，小於 1 代表 AppleJuice 較快：

```sh
python3 - "$RESULTS/applejuice.json" "$RESULTS/rosetta.json" <<'EOF'
import json, sys
def load(path):
    data = json.load(open(path))
    return {w['id']: w for s in data['sections'] if s['id'] == 1 for w in s['workloads']}
ours, rosetta = load(sys.argv[1]), load(sys.argv[2])
print('編號  測項                      AppleJuice  Rosetta  時間比')
for key in sorted(ours.keys() & rosetta.keys()):
    a, b = ours[key], rosetta[key]
    print(f"{key:<5} {a['name']:<25} {a['score']:>10} {b['score']:>8} {a['runtime'] / b['runtime']:>7.3f}")
EOF
```

## 量測建議

- 接上電源，關閉低耗電模式，並結束其他會占用 CPU 的程式。
- AppleJuice 第一次執行包含程式碼轉譯成本。正式比較前先完整執行一次，報告時註明採用的是第幾次執行。
- 每組至少執行三次，取中位數。AppleJuice、Rosetta 2 與原生三方交錯執行，以降低溫度與背景負載的影響。
- 單核比較時，「此程式 CPU 核心數」維持 1，三方都使用 `--single-core`。
- 分享結果時附上以下資料：
  - Mac 型號與 macOS 版本。
  - AppleJuice 的提交版本。
  - 第 1 節記錄的 `doctor` 欄位。
  - Geekbench 版本，以及上述測試命令。

匯出的 JSON 含 `uuid`、`system_uuid` 等識別值，公開前請移除。

## 疑難排解

| 現象 | 處理 |
|---|---|
| ``Error: The `--workload-list` switch is only available in Geekbench 7 Pro.`` | 授權檔名稱或位置錯誤。確認檔名為 `Geekbench.preferences`，且與 `geekbench7.exe` 在同一資料夾。 |
| 沒有產生 JSON | 確認已允許寫入，且 `--export-json` 使用 `C:\` 開頭的 Windows 路徑。 |
| 程式異常結束 | 查看「執行紀錄」中的退出碼與引擎日誌。回報問題時，請先移除日誌中的個人路徑。 |
