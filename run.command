#!/bin/zsh
# 可從 Finder 雙擊，或在終端執行；路徑一律以本腳本所在目錄為準。
set -euo pipefail

project_dir="${0:A:h}"
app_path="$project_dir/build/launcher/Win64 Launcher.app"
engine_path="$app_path/Contents/MacOS/x64arm-engine"

# Finder 啟動的終端也能找到 Homebrew 與使用者安裝的建置工具。
export PATH="/opt/homebrew/bin:/usr/local/bin:$HOME/.local/go/bin:$PATH"

# 跨架構授權（4 KiB 頁／硬體 TSO）是 x86 啟用原生 MMU 的前提；
# 沒有授權的 16 KiB 宿主上，x86 不建立原生 MMU，記憶體存取都走受檢路徑。
profile="${X64ARM_CROSS_ARCH_PROFILE:-$project_dir/config/engine.provisionprofile}"
build_args=()
if [[ -f "$profile" ]]; then
    team="$(/usr/bin/security cms -D -i "$profile" 2>/dev/null | /usr/bin/plutil -extract TeamIdentifier.0 raw -o - - 2>/dev/null || true)"
    identity="${X64ARM_CODESIGN_IDENTITY:-}"
    if [[ -z "$identity" && -n "$team" ]]; then
        # 依 profile 的 Team ID 選擇對應的 Developer ID 簽署憑證。
        identity="$(/usr/bin/security find-identity -v -p codesigning \
            | /usr/bin/awk -v team="($team)" 'index($0,"\"Developer ID Application: ") && index($0,team"\"") {print $2; exit}')"
    fi
    if [[ -z "$identity" ]]; then
        print -u2 "找不到 profile 對應的 Developer ID 簽署憑證（Team ${team:-未知}），無法套用跨架構授權。"
        exit 1
    fi
    build_args=(--sign-identity "$identity" --cross-arch-profile "$profile")
else
    print -u2 "未找到跨架構 profile：$profile"
    print -u2 '將沿用既有建置設定；若未授權，x86 程式不會使用原生 MMU。'
fi

print '正在確認增量建置；沒有修改時直接沿用既有產物…'
python3 "$project_dir/scripts/build_launcher.py" "${build_args[@]}"

if (( ${#build_args[@]} )); then
    if ! /usr/bin/codesign -d --entitlements - --xml "$engine_path" 2>/dev/null \
        | /usr/bin/grep -q 'com.apple.developer.cross-architecture-support'; then
        print -u2 '包內引擎缺少跨架構授權，x86 程式將無法使用原生 MMU；請檢查簽署憑證與 profile。'
        exit 1
    fi
    print '包內引擎已帶跨架構授權：4 KiB 頁、硬體 TSO 與 x86 原生 MMU 可用。'
fi

# open 會切回既有程序；等舊版結束後才開啟剛建置的版本。
# 由使用者正常關閉，讓啟動器處理尚在執行的 Windows 程式。
if /usr/bin/pgrep -x Win64Launcher >/dev/null; then
    print '建置完成。請關閉目前的 Win64 Launcher；關閉後會自動啟動最新版。'
    while /usr/bin/pgrep -x Win64Launcher >/dev/null; do
        sleep 1
    done
fi

print '正在啟動最新的 Win64 Launcher…'
exec /usr/bin/open "$app_path"
