package main

import (
	"fmt"
	"sync"
)

// 兩種架構共用的預設核心數。超過雙核尚未最佳化：同引擎實測逆轉裁判 2 核 116.95 提交/秒、CPU 105 秒，
// 12 核只有 69.72、CPU 202 秒；咲稻姬 12 核 50.33、CPU 146 秒，2 核約 53、CPU 115 秒。需要多核的程式可再手動調高。
const defaultCPUCores uint32 = 2

// 舊版把 x64 預設存成宿主最大值，且設定表單會一次送出所有欄位，分不出是否使用者自選；
// 版本低於此值的設定若等於最大值就視為舊預設，改用新預設。之後明確儲存的值一律保留。
const cpuDefaultsVersion uint32 = 1

func migratedCPUCoresX64(saved, savedVersion, maximum uint32) uint32 {
	if savedVersion < cpuDefaultsVersion && saved == maximum {
		return 0
	}
	return saved
}

type CPULimits struct {
	PhysicalCores uint32 `json:"physicalCores"`
	X64Max        uint32 `json:"x64Max"`
	X86Max        uint32 `json:"x86Max"`
}

// 核心數受宿主實體拓樸及客體 affinity 遮罩寬度限制。
func cpuLimitsForSystem(physical uint32) CPULimits {
	physical = max(physical, 1)
	return CPULimits{PhysicalCores: physical, X64Max: min(physical, 64), X86Max: min(physical, 32)}
}

var hostCPU struct {
	once   sync.Once
	limits CPULimits
}

func systemCPULimits() CPULimits {
	hostCPU.once.Do(func() { hostCPU.limits = cpuLimitsForSystem(nativePhysicalProcessors()) })
	return hostCPU.limits
}

func normalizeCPUCount(value, maximum, fallback uint32) uint32 {
	if value == 0 {
		return fallback
	}
	return min(value, maximum)
}

func validateCPUCount(value, maximum, fallback uint32, architecture string) (uint32, error) {
	if value == 0 {
		value = fallback
	}
	if value > maximum {
		return 0, fmt.Errorf("Windows %s 核心數必須介於 1 與 %d，且不可超過宿主實體核心數", architecture, maximum)
	}
	return value, nil
}

// 個別程式的 0 沿用全域設定；舊版 profile 不需遷移。
func (a *App) programCPUCount(architecture string, value uint32) (uint32, error) {
	maximum, fallback := a.cpuLimits.X64Max, a.library.CPUCoresX64
	if architecture == "x86" {
		maximum, fallback = a.cpuLimits.X86Max, a.library.CPUCoresX86
	}
	return validateCPUCount(value, maximum, fallback, architecture)
}
