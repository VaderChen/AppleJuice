package main

import (
	"encoding/json"
	"os"
	"strconv"
	"strings"
)

const defaultCompilerWorkers uint32 = 2
const maxCompilerWorkers uint32 = 8

// 缺少欄位的舊設定沿用 2；明確保存的 0 必須維持同步編譯。
func (settings *GlobalSettings) UnmarshalJSON(data []byte) error {
	type plain GlobalSettings
	value := plain{CompilerWorkers: defaultCompilerWorkers}
	if err := json.Unmarshal(data, &value); err != nil {
		return err
	}
	*settings = GlobalSettings(value)
	return nil
}

func normalizeCompilerWorkers(value uint32) uint32 {
	if value > maxCompilerWorkers {
		return defaultCompilerWorkers
	}
	return value
}

// 共用原生區域（核心另一工作中的 JIT 功能，核心預設開啟）在 2026-10-18 的實測使 32 位元遊戲退到
// 首次呈現 14–15 秒、約 35 FPS；Launcher 先固定關閉，待該功能修正並重測後移除此設定。
const sharedRegionsSetting = "X64ARM_JIT_SHARED_REGIONS=0"

// 執行及離線分析使用相同的已驗證設定，不繼承外部的引擎實驗開關。
func engineEnvironment(workers uint32, openGL string, nativeLibraryBridge bool, nativeV8Host string) []string {
	environment := make([]string, 0, len(os.Environ())+4)
	for _, entry := range os.Environ() {
		if !strings.HasPrefix(entry, "X64ARM_") && !strings.HasPrefix(entry, "TERM=") {
			environment = append(environment, entry)
		}
	}
	providers := []string{}
	if nativeLibraryBridge {
		providers = append(providers, "zlib")
	}
	if nativeV8Host != "" {
		providers = append(providers, "v8")
		environment = append(environment, "X64ARM_NATIVE_V8_HOST="+nativeV8Host)
	}
	if len(providers) != 0 {
		environment = append(environment, "X64ARM_NATIVE_LIBRARIES="+strings.Join(providers, ","))
	}
	return append(environment, "TERM=xterm-256color", "X64ARM_GL_BACKEND="+normalizeOpenGLBackend(openGL),
		"X64ARM_JIT_COMPILER_WORKERS="+strconv.FormatUint(uint64(workers), 10), "X64ARM_AOT_MEMORY_CACHE=*", sharedRegionsSetting)
}
