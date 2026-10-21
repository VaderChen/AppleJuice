package main

import (
	"fmt"
	"sync"
)

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
