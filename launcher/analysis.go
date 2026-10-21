package main

import (
	"bufio"
	"errors"
	"fmt"
	"os/exec"
	"strings"
	"syscall"
	"time"
)

type AnalysisView struct {
	Stage     string `json:"stage"`
	Status    string `json:"status"`
	Completed int    `json:"completed"`
	Total     int    `json:"total"`
	Message   string `json:"message"`
}
type analysisJob struct {
	view AnalysisView
	cmd  *exec.Cmd
	done chan struct{}
}

func (job *analysisJob) active() bool {
	return job.view.Status == "running" || job.view.Status == "stopping"
}

func (a *App) Analyze(id string) (Snapshot, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	i := a.indexLocked(id)
	if i < 0 {
		return Snapshot{}, errors.New("找不到此程式")
	}
	for _, job := range a.analyses {
		if job.active() {
			return Snapshot{}, errors.New("已有加速分析正在進行")
		}
	}
	if r := a.runs[id]; r != nil {
		select {
		case <-r.done:
		default:
			return Snapshot{}, errors.New("請先停止此程式再進行加速分析")
		}
	}
	if !a.engineReady() {
		return Snapshot{}, errors.New("找不到執行引擎")
	}
	cmd := exec.Command(a.engine, "precompile-pe", a.library.Programs[i].Path, "--scan-dlls", "adjacent", "--ordering", "host")
	environment, err := a.engineEnvironmentLocked()
	if err != nil {
		return Snapshot{}, err
	}
	cmd.Env = environment
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		return Snapshot{}, err
	}
	cmd.Stderr = cmd.Stdout
	job := &analysisJob{view: AnalysisView{Stage: "讀取程式", Status: "running", Message: "正在讀取程式與 DLL…"}, cmd: cmd, done: make(chan struct{})}
	if err = cmd.Start(); err != nil {
		pipe.Close()
		return Snapshot{}, fmt.Errorf("無法開始分析：%w", err)
	}
	if a.analyses == nil {
		a.analyses = make(map[string]*analysisJob)
	}
	a.analyses[id] = job
	go func() {
		scanner := bufio.NewScanner(pipe)
		scanner.Buffer(make([]byte, 4096), 1024*1024)
		var detail string
		for scanner.Scan() {
			line := scanner.Text()
			a.mu.Lock()
			if strings.HasPrefix(line, "smap_progress=") {
				var done, total int
				if _, err := fmt.Sscanf(line, "smap_progress=%d/%d", &done, &total); err == nil && total > 0 && done >= 0 && done <= total {
					switch {
					case strings.Contains(line, "phase=compile"):
						job.view.Stage = "編譯 resident AOT 區塊"
					case strings.Contains(line, "phase=save"):
						job.view.Stage = "儲存快取"
					case strings.Contains(line, "phase=scan"):
						job.view.Stage = "掃描程式與 DLL"
					}
					job.view.Completed = done
					job.view.Total = total
					job.view.Message = fmt.Sprintf("已分析 %d / %d 個檔案", done, total)
					if marker := strings.Index(line, " source="); marker >= 0 {
						job.view.Message = "正在分析 " + strings.Trim(line[marker+8:], "\"")
					}
				}
			} else if line != "" && !strings.HasPrefix(line, "precompile_source=") {
				detail = line
				if len(detail) > 1000 {
					detail = detail[:1000]
				}
			}
			a.mu.Unlock()
		}
		scanErr := scanner.Err()
		err := cmd.Wait()
		a.mu.Lock()
		defer a.mu.Unlock()
		if job.view.Status == "stopping" {
			job.view.Stage = "已取消"
			job.view.Status = "cancelled"
			job.view.Message = "已取消分析。已完成檔案的快取仍可使用。"
		} else if err != nil || scanErr != nil {
			job.view.Stage = "分析失敗"
			job.view.Status = "failed"
			job.view.Message = "分析未全部完成"
			if detail != "" {
				job.view.Message += "：" + detail
			}
		} else {
			job.view.Stage = "完成"
			job.view.Status = "completed"
			job.view.Completed = job.view.Total
			job.view.Message = "分析完成，已建立 resident AOT 快取；需啟用持久快取才會載入。"
		}
		close(job.done)
	}()
	return a.snapshotLocked(), nil
}
func (a *App) CancelAnalysis(id string) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	job := a.analyses[id]
	if job == nil || !job.active() {
		return nil
	}
	job.view.Status = "stopping"
	job.view.Message = "正在取消…"
	// 分析只包含引擎子程序；停止整個 process group，避免留下背景工作。
	if job.cmd.Process != nil {
		err := syscall.Kill(-job.cmd.Process.Pid, syscall.SIGTERM)
		go func() {
			select {
			case <-job.done:
				return
			case <-time.After(time.Second):
				_ = syscall.Kill(-job.cmd.Process.Pid, syscall.SIGKILL)
			}
		}()
		if errors.Is(err, syscall.ESRCH) {
			return nil
		}
		return err
	}
	return nil
}
func (a *App) closeAnalyses() {
	a.mu.Lock()
	jobs := make(map[string]*analysisJob)
	for id, job := range a.analyses {
		if job.active() {
			jobs[id] = job
		}
	}
	a.mu.Unlock()
	for id, job := range jobs {
		_ = a.CancelAnalysis(id)
		select {
		case <-job.done:
		case <-time.After(3 * time.Second):
		}
	}
}
