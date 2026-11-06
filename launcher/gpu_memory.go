package main

import (
	"errors"
	"fmt"
	"sync"
)

const (
	gpuMemoryStepMiB    uint64 = 512
	// 額度只是上限，不會預先占用。UE5 大型遊戲（33 號遠征隊）開場就用到 6～8 GiB，
	// 舊預設 4096 會讓遊戲以「顯示記憶體不足」結束；上限仍不超過系統記憶體的一半。
	gpuMemoryDefaultMiB uint64 = 16384
	// 舊版把預設值 4096 存進個別程式設定，分不出是否使用者自選；版本低於此值的 4096 視為舊預設。
	gpuMemoryDefaultsVersion uint32 = 1
	gpuMemoryLegacyDefaultMiB uint64 = 4096
)

type GPUMemoryLimits struct {
	SystemMemoryBytes uint64 `json:"systemMemoryBytes"`
	MinMiB            uint64 `json:"minMiB"`
	MaxMiB            uint64 `json:"maxMiB"`
	StepMiB           uint64 `json:"stepMiB"`
	DefaultMiB        uint64 `json:"defaultMiB"`
}

// 以實體記憶體的一半計算，向下對齊刻度，避免超過系統的 50%。
func gpuMemoryLimitsForSystem(physical uint64) (GPUMemoryLimits, error) {
	maximum := physical / 2 / (gpuMemoryStepMiB * 1024 * 1024) * gpuMemoryStepMiB
	if maximum < gpuMemoryStepMiB {
		return GPUMemoryLimits{}, errors.New("無法取得可用的系統實體記憶體容量")
	}
	return GPUMemoryLimits{SystemMemoryBytes: physical, MinMiB: gpuMemoryStepMiB, MaxMiB: maximum, StepMiB: gpuMemoryStepMiB, DefaultMiB: min(gpuMemoryDefaultMiB, maximum)}, nil
}

var hostGPUMemory struct {
	once   sync.Once
	limits GPUMemoryLimits
	err    error
}

func systemGPUMemoryLimits() (GPUMemoryLimits, error) {
	hostGPUMemory.once.Do(func() { hostGPUMemory.limits, hostGPUMemory.err = gpuMemoryLimitsForSystem(nativePhysicalMemory()) })
	return hostGPUMemory.limits, hostGPUMemory.err
}

func validateGPUMemoryWithLimits(value uint64, limits GPUMemoryLimits) (uint64, error) {
	if value == 0 {
		value = limits.DefaultMiB
	}
	if value < limits.MinMiB || value > limits.MaxMiB || value%limits.StepMiB != 0 {
		return 0, fmt.Errorf("GPU 記憶體必須介於 %d 與 %d MiB，且為 %d MiB 的倍數；上限為系統記憶體的 50%%", limits.MinMiB, limits.MaxMiB, limits.StepMiB)
	}
	return value, nil
}

func validateGPUMemory(value uint64) (uint64, error) {
	limits, err := systemGPUMemoryLimits()
	if err != nil {
		return 0, err
	}
	return validateGPUMemoryWithLimits(value, limits)
}

// 缺少或低於新刻度的舊容量採用預設；其餘向下對齊上限，保留有效自訂容量。
func migratedGPUMemory(value uint64, version uint32, limits GPUMemoryLimits) uint64 {
	if version < gpuMemoryDefaultsVersion && value == gpuMemoryLegacyDefaultMiB {
		return limits.DefaultMiB
	}
	return normalizeGPUMemory(value, limits)
}

func normalizeGPUMemory(value uint64, limits GPUMemoryLimits) uint64 {
	if value < limits.MinMiB {
		return limits.DefaultMiB
	}
	return max(limits.MinMiB, min(value, limits.MaxMiB)/limits.StepMiB*limits.StepMiB)
}
