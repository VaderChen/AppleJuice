package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"time"
)

type ResourceUsage struct {
	Version               int      `json:"version"`
	PID                   int      `json:"pid"`
	Sequence              uint64   `json:"sequence"`
	SampledAt             int64    `json:"sampledAt"`
	IntervalSeconds       float64  `json:"intervalSeconds"`
	CPUPercent            *float64 `json:"cpuPercent"`
	GPUPercent            *float64 `json:"gpuPercent"`
	GPUTimingAvailable    bool     `json:"gpuTimingAvailable"`
	MemoryBytes           *uint64  `json:"memoryBytes"`
	GPUMemoryBytes        uint64   `json:"gpuMemoryBytes"`
	GPUMemoryLimitBytes   uint64   `json:"gpuMemoryLimitBytes"`
	ReceivedBytes         uint64   `json:"receivedBytes"`
	SentBytes             uint64   `json:"sentBytes"`
	ReceiveBytesPerSecond *float64 `json:"receiveBytesPerSecond"`
	SendBytesPerSecond    *float64 `json:"sendBytesPerSecond"`
}

func readResourceUsage(path string, pid int) (*ResourceUsage, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 4097))
	if err != nil {
		return nil, err
	}
	var usage ResourceUsage
	if len(data) > 4096 || json.Unmarshal(data, &usage) != nil || usage.Version != 1 || usage.PID != pid || usage.Sequence == 0 || usage.SampledAt <= 0 || usage.IntervalSeconds < 0 {
		return nil, fmt.Errorf("資源用量快照格式不正確")
	}
	for _, value := range []*float64{usage.CPUPercent, usage.GPUPercent, usage.ReceiveBytesPerSecond, usage.SendBytesPerSecond} {
		if value != nil && *value < 0 {
			return nil, fmt.Errorf("資源用量不能為負數")
		}
	}
	if usage.GPUPercent != nil && *usage.GPUPercent > 100 {
		return nil, fmt.Errorf("GPU 使用率不能超過 100%%")
	}
	return &usage, nil
}

func (r *execution) refreshUsage(now time.Time) {
	r.mu.Lock()
	if r.usagePath == "" || r.status != "running" || r.cmd.Process == nil || now.Before(r.nextUsageRead) {
		r.mu.Unlock()
		return
	}
	r.nextUsageRead = now.Add(250 * time.Millisecond)
	path, pid := r.usagePath, r.cmd.Process.Pid
	r.mu.Unlock()
	usage, err := readResourceUsage(path, pid)
	if err != nil {
		return
	}
	r.mu.Lock()
	if r.usage == nil || usage.Sequence > r.usage.Sequence {
		r.usage = usage
	}
	r.mu.Unlock()
}
