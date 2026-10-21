package main

import (
	"errors"
	"fmt"
	"sync"
)

const (
	gpuMemoryStepMiB    uint64 = 512
	gpuMemoryDefaultMiB uint64 = 4096
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
func normalizeGPUMemory(value uint64, limits GPUMemoryLimits) uint64 {
	if value < limits.MinMiB {
		return limits.DefaultMiB
	}
	return max(limits.MinMiB, min(value, limits.MaxMiB)/limits.StepMiB*limits.StepMiB)
}
