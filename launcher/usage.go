package main

import (
	"encoding/json"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type ResourceUsage struct {
	Version               int                 `json:"version"`
	PID                   int                 `json:"pid"`
	Sequence              uint64              `json:"sequence"`
	SampledAt             int64               `json:"sampledAt"`
	IntervalSeconds       float64             `json:"intervalSeconds"`
	CPUPercent            *float64            `json:"cpuPercent"`
	GPUPercent            *float64            `json:"gpuPercent"`
	GPUTimingAvailable    bool                `json:"gpuTimingAvailable"`
	MemoryBytes           *uint64             `json:"memoryBytes"`
	GPUMemoryBytes        uint64              `json:"gpuMemoryBytes"`
	GPUMemoryLimitBytes   uint64              `json:"gpuMemoryLimitBytes"`
	ReceivedBytes         uint64              `json:"receivedBytes"`
	SentBytes             uint64              `json:"sentBytes"`
	ReceiveBytesPerSecond *float64            `json:"receiveBytesPerSecond"`
	SendBytesPerSecond    *float64            `json:"sendBytesPerSecond"`
	HostStart             float64             `json:"hostStart,omitempty"`
	HostEnd               float64             `json:"hostEnd,omitempty"`
	GPUIntervals          [][2]float64        `json:"gpuIntervals,omitempty"`
	ProcessCount          int                 `json:"processCount"`
	CompilerCache         *CompilerCacheUsage `json:"compilerCache,omitempty"`
}

type CompilerCacheUsage struct {
	PID                int    `json:"pid"`
	Workers            uint64 `json:"workers"`
	ActiveWorkers      uint64 `json:"activeWorkers"`
	Hits               uint64 `json:"hits"`
	Misses             uint64 `json:"misses"`
	Rejects            uint64 `json:"rejects"`
	Unavailable        uint64 `json:"unavailable"`
	Stores             uint64 `json:"stores"`
	BackgroundJobs     uint64 `json:"backgroundJobs"`
	BackgroundStores   uint64 `json:"backgroundStores"`
	BackgroundHits     uint64 `json:"backgroundHits"`
	BackgroundWaits    uint64 `json:"backgroundWaits"`
	BackgroundDrops    uint64 `json:"backgroundDrops"`
	RestoreNanoseconds uint64 `json:"restoreNanoseconds"`
	CompileNanoseconds uint64 `json:"compileNanoseconds"`
}

const usageSizeLimit = 1024 * 1024

func readResourceUsage(path string, pid int) (*ResourceUsage, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, usageSizeLimit+1))
	if err != nil {
		return nil, err
	}
	var usage ResourceUsage
	if len(data) > usageSizeLimit || json.Unmarshal(data, &usage) != nil || usage.Version != 1 || usage.PID != pid || usage.Sequence == 0 || usage.SampledAt <= 0 || usage.IntervalSeconds < 0 {
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
	for _, span := range usage.GPUIntervals {
		if span[0] < usage.HostStart || span[1] > usage.HostEnd+0.001 || span[1] <= span[0] {
			return nil, fmt.Errorf("GPU 計時區間不正確")
		}
	}
	return &usage, nil
}

// 報告群組由根程序建立，子程序繼承路徑。只採用存活且未過期的取樣；已退出程序的快取累計仍保留。
func readResourceUsageTree(path string, pid int, now time.Time) (*ResourceUsage, error) {
	root, err := readResourceUsage(path, pid)
	if err != nil {
		return nil, err
	}
	rows := []*ResourceUsage{root}
	entries, _ := os.ReadDir(path + ".processes")
	for _, entry := range entries {
		child, parseErr := strconv.Atoi(strings.TrimSuffix(entry.Name(), ".json"))
		if parseErr != nil || child <= 0 || !strings.HasSuffix(entry.Name(), ".json") || entry.Type() != 0 || syscall.Kill(child, 0) != nil {
			continue
		}
		snapshot, readErr := readResourceUsage(filepath.Join(path+".processes", entry.Name()), child)
		if readErr == nil && now.UnixMilli()-snapshot.SampledAt <= 7500 && snapshot.SampledAt <= now.UnixMilli()+3000 {
			rows = append(rows, snapshot)
		}
	}
	merged := aggregateResourceUsage(rows)
	entries, _ = os.ReadDir(path + ".cache")
	for _, entry := range entries {
		child, parseErr := strconv.Atoi(strings.TrimSuffix(entry.Name(), ".json"))
		if parseErr != nil || child <= 0 || !strings.HasSuffix(entry.Name(), ".json") || entry.Type() != 0 {
			continue
		}
		file, openErr := os.Open(filepath.Join(path+".cache", entry.Name()))
		if openErr != nil {
			continue
		}
		data, readErr := io.ReadAll(io.LimitReader(file, 8193))
		file.Close()
		var cache CompilerCacheUsage
		if readErr != nil || len(data) > 8192 || json.Unmarshal(data, &cache) != nil || cache.PID != child {
			continue
		}
		if merged.CompilerCache == nil {
			merged.CompilerCache = &CompilerCacheUsage{}
		}
		c := merged.CompilerCache
		c.Workers = max(c.Workers, cache.Workers)
		if syscall.Kill(child, 0) == nil {
			c.ActiveWorkers += cache.ActiveWorkers
		}
		c.Hits += cache.Hits
		c.Misses += cache.Misses
		c.Rejects += cache.Rejects
		c.Unavailable += cache.Unavailable
		c.Stores += cache.Stores
		c.BackgroundJobs += cache.BackgroundJobs
		c.BackgroundStores += cache.BackgroundStores
		c.BackgroundHits += cache.BackgroundHits
		c.BackgroundWaits += cache.BackgroundWaits
		c.BackgroundDrops += cache.BackgroundDrops
		c.RestoreNanoseconds += cache.RestoreNanoseconds
		c.CompileNanoseconds += cache.CompileNanoseconds
	}
	return merged, nil
}

func aggregateResourceUsage(rows []*ResourceUsage) *ResourceUsage {
	out := *rows[0]
	out.ProcessCount = len(rows)
	// 各程序的 GPU 取樣相位不同，只計共同時間區間，並合併重疊 command buffer。
	start, end := out.HostStart, out.HostEnd
	intervals := append([][2]float64(nil), out.GPUIntervals...)
	addFloat := func(a, b *float64) *float64 {
		if a == nil || b == nil {
			return nil
		}
		value := *a + *b
		return &value
	}
	for _, row := range rows[1:] {
		out.CPUPercent = addFloat(out.CPUPercent, row.CPUPercent)
		if out.MemoryBytes != nil && row.MemoryBytes != nil {
			value := *out.MemoryBytes + *row.MemoryBytes
			out.MemoryBytes = &value
		} else {
			out.MemoryBytes = nil
		}
		out.GPUMemoryBytes += row.GPUMemoryBytes
		out.ReceivedBytes += row.ReceivedBytes
		out.SentBytes += row.SentBytes
		out.ReceiveBytesPerSecond = addFloat(out.ReceiveBytesPerSecond, row.ReceiveBytesPerSecond)
		out.SendBytesPerSecond = addFloat(out.SendBytesPerSecond, row.SendBytesPerSecond)
		out.GPUTimingAvailable = out.GPUTimingAvailable && row.GPUTimingAvailable
		start = math.Max(start, row.HostStart)
		end = math.Min(end, row.HostEnd)
		intervals = append(intervals, row.GPUIntervals...)
	}
	if len(rows) > 1 || out.HostEnd > 0 {
		out.GPUPercent = nil
		if out.GPUTimingAvailable && end > start {
			sort.Slice(intervals, func(i, j int) bool { return intervals[i][0] < intervals[j][0] })
			busy, covered := 0.0, start
			for _, span := range intervals {
				first, last := math.Max(covered, math.Max(start, span[0])), math.Min(end, span[1])
				if last > first {
					busy += last - first
					covered = last
				}
			}
			percent := math.Min(100, busy/(end-start)*100)
			out.GPUPercent = &percent
		}
	}
	out.GPUIntervals = nil
	return &out
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
	usage, err := readResourceUsageTree(path, pid, now)
	if err != nil {
		return
	}
	r.mu.Lock()
	if r.usage == nil || usage.Sequence >= r.usage.Sequence {
		r.usage = usage
	}
	r.mu.Unlock()
}
