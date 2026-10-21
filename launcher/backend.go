package main

import (
	"crypto/rand"
	"crypto/sha256"
	"debug/pe"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/creack/pty"
)

const defaultMaxInstructions uint64 = 0
const previousMaxInstructions uint64 = 10000000000000
const legacyMaxInstructions uint64 = 10000000000

// 啟動器以 0 表示不限指令數；遷移歷代預設，保留其他自訂測試預算。
func instructionBudget(value uint64) uint64 {
	if value == 0 || value == legacyMaxInstructions || value == previousMaxInstructions {
		return defaultMaxInstructions
	}
	return value
}

type Program struct {
	Version      int    `json:"version"`
	ID           string `json:"id"`
	Name         string `json:"name"`
	Path         string `json:"path"`
	Kind         string `json:"kind"`
	Architecture string `json:"architecture"`
	Temporary    bool   `json:"temporary,omitempty"`
	Settings
}
type Settings struct {
	CPUCores        uint32 `json:"cpuCores,omitempty"`
	GpuMemoryMiB    uint64 `json:"gpuMemoryMiB"`
	Arguments       string `json:"arguments"`
	WorkDir         string `json:"workDir"`
	CDrive          string `json:"cDrive"`
	InstallRoot     string `json:"installRoot"`
	Writable        bool   `json:"writable"`
	MaxInstructions uint64 `json:"maxInstructions"`
}
type GlobalSettings struct {
	CertificateRootsFile bool              `json:"certificateRootsFile"`
	CertificateRootsPath string            `json:"certificateRootsPath"`
	WindowsFont          string            `json:"windowsFont"`
	NativeLibraryBridge  bool              `json:"nativeLibraryBridge"`
	NativeV8Bridge       bool              `json:"nativeV8Bridge"`
	CompilerWorkers      uint32            `json:"compilerWorkers"`
	CPUCoresX64          uint32            `json:"cpuCoresX64"`
	CPUCoresX86          uint32            `json:"cpuCoresX86"`
	OpenGLBackend        string            `json:"openGLBackend"`
	Language             string            `json:"language"`
	Theme                string            `json:"theme"`
	CDrive               string            `json:"cDrive"`
	Drives               map[string]string `json:"drives"`
}
type Library struct {
	CertificateRootsFile bool              `json:"certificateRootsFile"`
	CertificateRootsPath string            `json:"certificateRootsPath"`
	WindowsFont          string            `json:"windowsFont"`
	NativeLibraryBridge  bool              `json:"nativeLibraryBridge"`
	NativeV8Bridge       bool              `json:"nativeV8Bridge"`
	CompilerWorkers      uint32            `json:"compilerWorkers"`
	CPUCoresX64          uint32            `json:"cpuCoresX64"`
	CPUCoresX86          uint32            `json:"cpuCoresX86"`
	OpenGLBackend        string            `json:"openGLBackend"`
	Language             string            `json:"language"`
	Theme                string            `json:"theme"`
	Drives               map[string]string `json:"drives"`
	Version              int               `json:"version"`
	Selected             string            `json:"selected"`
	Programs             []Program         `json:"programs"`
	CDrive               string            `json:"cDrive"`
}
type ProgramView struct {
	Analysis *AnalysisView  `json:"analysis,omitempty"`
	Usage    *ResourceUsage `json:"usage,omitempty"`
	Program
	Missing    bool   `json:"missing"`
	Status     string `json:"status"`
	RunID      string `json:"runId"`
	PID        int    `json:"pid"`
	ExitCode   *int   `json:"exitCode"`
	Error      string `json:"error"`
	ConfigPath string `json:"configPath"`
}
type Snapshot struct {
	NativeV8Ready      bool            `json:"nativeV8Ready"`
	NativeV8TestsReady bool            `json:"nativeV8TestsReady"`
	CPULimits          CPULimits       `json:"cpuLimits"`
	Version            string          `json:"version"`
	GPUMemoryLimits    GPUMemoryLimits `json:"gpuMemoryLimits"`
	GlobalSettings     GlobalSettings  `json:"globalSettings"`
	Programs           []ProgramView   `json:"programs"`
	Selected           string          `json:"selected"`
	EngineReady        bool            `json:"engineReady"`
	Engine             string          `json:"engine"`
	Running            int             `json:"running"`
}
type Stream struct {
	ID    string `json:"id"`
	RunID string `json:"runId"`
	Data  []byte `json:"data"`
}
type PollResult struct {
	State   Snapshot `json:"state"`
	Streams []Stream `json:"streams"`
}
type AddResult struct {
	AnalysisCandidates []string `json:"analysisCandidates"`
	State              Snapshot `json:"state"`
	Added              int      `json:"added"`
	Errors             []string `json:"errors"`
}
type execution struct {
	mu                               sync.Mutex
	id, status, message, diagnostics string
	cmd                              *exec.Cmd
	terminal                         *os.File
	terminalMu                       sync.Mutex
	terminalClosed                   bool
	exitCode                         *int
	stopping                         bool
	output                           chan []byte
	input                            chan string
	done, cancel                     chan struct{}
	cancelOnce                       sync.Once
	usagePath                        string
	usage                            *ResourceUsage
	nextUsageRead                    time.Time
}
type App struct {
	cpuLimits           CPULimits
	gpuMemoryLimits     GPUMemoryLimits
	gpuMemoryMigrations map[string]bool
	analyses            map[string]*analysisJob
	mu                  sync.Mutex
	library             Library
	engine, dataDir     string
	runs                map[string]*execution
	lockFile            *os.File
	closeOnce           sync.Once
	persistentSelected  string
	backgroundDisplay   bool
	backgroundDisplayID uint32
	mappingSessions     map[string]*launcherMappingSession
}

type launcherMappingSession struct {
	path  string
	lease *os.File
}

func identifier() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b[:])
}

func inspectProgram(path string) (Program, error) {
	if !strings.EqualFold(filepath.Ext(path), ".exe") {
		return Program{}, errors.New("請選擇 .exe 檔案")
	}
	path, err := filepath.Abs(path)
	if err != nil {
		return Program{}, err
	}
	path, err = filepath.EvalSymlinks(path)
	if err != nil {
		return Program{}, fmt.Errorf("找不到檔案：%w", err)
	}
	f, err := pe.Open(path)
	if err != nil {
		return Program{}, errors.New("不是有效的 Windows PE 程式")
	}
	defer f.Close()
	if f.Characteristics&pe.IMAGE_FILE_DLL != 0 {
		return Program{}, errors.New("請選擇 Windows EXE，不能使用 DLL")
	}
	var subsystem uint16
	architecture := ""
	switch optional := f.OptionalHeader.(type) {
	case *pe.OptionalHeader64:
		if f.Machine == pe.IMAGE_FILE_MACHINE_AMD64 {
			architecture = "x64"
			subsystem = optional.Subsystem
		}
	case *pe.OptionalHeader32:
		if f.Machine == pe.IMAGE_FILE_MACHINE_I386 {
			architecture = "x86"
			subsystem = optional.Subsystem
		}
	}
	if architecture == "" {
		return Program{}, errors.New("目前支援 Windows x86／x64 EXE")
	}
	kind := "cli"
	if subsystem == pe.IMAGE_SUBSYSTEM_WINDOWS_GUI {
		kind = "gui"
	} else if subsystem != pe.IMAGE_SUBSYSTEM_WINDOWS_CUI {
		return Program{}, errors.New("不支援此 EXE 的子系統")
	}
	digest := sha256.Sum256([]byte(path))
	return Program{Version: 1, ID: hex.EncodeToString(digest[:16]), Name: filepath.Base(path), Path: path, Kind: kind, Architecture: architecture, Settings: Settings{GpuMemoryMiB: gpuMemoryDefaultMiB, WorkDir: filepath.Dir(path), Writable: true, MaxInstructions: defaultMaxInstructions}}, nil
}

type libraryIndex struct {
	Version  int      `json:"version"`
	Selected string   `json:"selected"`
	Programs []string `json:"programs"`
}

func (a *App) profilePath(id string) string { return filepath.Join(a.dataDir, "programs", id+".json") }

func NewApp(engine, dataDir, cDrive string) (*App, error) {
	limits, err := systemGPUMemoryLimits()
	if err != nil {
		return nil, err
	}
	a := &App{cpuLimits: systemCPULimits(), gpuMemoryLimits: limits, gpuMemoryMigrations: make(map[string]bool), analyses: make(map[string]*analysisJob), engine: engine, dataDir: dataDir, runs: make(map[string]*execution), library: Library{Version: 1, Programs: []Program{}}}
	if err := os.MkdirAll(dataDir, 0700); err != nil {
		return nil, err
	}
	lock, err := os.OpenFile(filepath.Join(dataDir, ".lock"), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err = syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		lock.Close()
		return nil, errors.New("此程式庫已由另一個啟動器開啟，請使用原有視窗")
	}
	a.lockFile = lock
	loaded := false
	defer func() {
		if !loaded {
			lock.Close()
		}
	}()
	data, err := os.ReadFile(filepath.Join(dataDir, "library.json"))
	if err == nil {
		var index libraryIndex
		if err = json.Unmarshal(data, &index); err != nil {
			return nil, fmt.Errorf("程式庫資料無法讀取，已保留原檔：%w", err)
		}
		if index.Version != 1 {
			return nil, errors.New("不支援此程式庫版本")
		}
		if len(index.Programs) > 512 {
			return nil, errors.New("程式庫超過 512 個程式")
		}
		a.library.Selected = index.Selected
		for _, id := range index.Programs {
			if len(id) != 32 {
				return nil, errors.New("程式設定索引不正確")
			}
			if _, e := hex.DecodeString(id); e != nil {
				return nil, errors.New("程式設定索引不正確")
			}
			bytes, e := os.ReadFile(a.profilePath(id))
			if e != nil {
				return nil, fmt.Errorf("無法讀取個別程式設定：%w", e)
			}
			var p Program
			if e = json.Unmarshal(bytes, &p); e != nil || p.ID != id || p.Version != 1 || p.Temporary {
				return nil, fmt.Errorf("程式設定格式不正確：%s", a.profilePath(id))
			}
			if current, inspectErr := inspectProgram(p.Path); inspectErr == nil {
				p.Architecture = current.Architecture
			}
			p.MaxInstructions = instructionBudget(p.MaxInstructions)
			originalGPUMemory := p.GpuMemoryMiB
			p.GpuMemoryMiB = normalizeGPUMemory(p.GpuMemoryMiB, limits)
			if p.GpuMemoryMiB != originalGPUMemory {
				a.gpuMemoryMigrations[p.ID] = true
			}
			a.library.Programs = append(a.library.Programs, p)
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	defaults := GlobalSettings{CompilerWorkers: defaultCompilerWorkers}
	if data, e := os.ReadFile(filepath.Join(dataDir, "settings.json")); e == nil {
		if e = json.Unmarshal(data, &defaults); e != nil {
			return nil, fmt.Errorf("全域設定格式不正確：%w", e)
		}
		a.library.CDrive = defaults.CDrive
		a.library.Drives = defaults.Drives
	} else if !errors.Is(e, os.ErrNotExist) {
		return nil, e
	}
	a.library.Language, a.library.Theme = normalizeAppearance(defaults.Language, defaults.Theme)
	a.library.WindowsFont = normalizeWindowsFont(defaults.WindowsFont)
	a.library.NativeLibraryBridge = defaults.NativeLibraryBridge
	a.library.NativeV8Bridge = defaults.NativeV8Bridge
	a.library.CertificateRootsFile = defaults.CertificateRootsFile
	a.library.CertificateRootsPath, err = certificateRootsRelativePath(defaults.CertificateRootsPath)
	if err != nil {
		return nil, fmt.Errorf("全域根憑證設定：%w", err)
	}
	a.library.OpenGLBackend = normalizeOpenGLBackend(defaults.OpenGLBackend)
	a.library.CompilerWorkers = normalizeCompilerWorkers(defaults.CompilerWorkers)
	a.library.CPUCoresX64 = normalizeCPUCount(defaults.CPUCoresX64, a.cpuLimits.X64Max, a.cpuLimits.X64Max)
	a.library.CPUCoresX86 = normalizeCPUCount(defaults.CPUCoresX86, a.cpuLimits.X86Max, 1)
	if cDrive != "" {
		a.library.CDrive = cDrive
	}
	if a.library.CDrive == "" {
		a.library.CDrive = filepath.Join(dataDir, "c_drive")
	}
	if err = os.MkdirAll(a.library.CDrive, 0700); err != nil {
		return nil, err
	}
	seen := map[string]bool{}
	for _, p := range a.library.Programs {
		if p.ID == "" || seen[p.ID] || !filepath.IsAbs(p.Path) {
			return nil, errors.New("程式庫記錄格式不正確，已保留原檔")
		}
		seen[p.ID] = true
	}
	if !seen[a.library.Selected] && len(a.library.Programs) > 0 {
		a.library.Selected = a.library.Programs[0].ID
	}
	a.persistentSelected = a.library.Selected
	loaded = true
	return a, nil
}

// 以同目錄的暫存檔原子替換；寫入失敗時保留既有程式庫。
func writeJSONAtomic(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	if err = os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".settings-*")
	if err != nil {
		return err
	}
	name := f.Name()
	defer os.Remove(name)
	if _, err = f.Write(append(data, '\n')); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err == nil {
		err = closeErr
	}
	if err == nil {
		err = os.Rename(name, path)
	}
	return err
}
func persistentIndex(library Library, previousSelected string) libraryIndex {
	index := libraryIndex{Version: 1, Selected: library.Selected, Programs: []string{}}
	for _, p := range library.Programs {
		if !p.Temporary {
			index.Programs = append(index.Programs, p.ID)
		}
	}
	if !slices.Contains(index.Programs, index.Selected) {
		index.Selected = previousSelected
		if !slices.Contains(index.Programs, index.Selected) {
			index.Selected = ""
			if len(index.Programs) > 0 {
				index.Selected = index.Programs[0]
			}
		}
	}
	return index
}
func (a *App) saveLocked(next Library) error {
	index := persistentIndex(next, a.persistentSelected)
	previous := persistentIndex(a.library, a.persistentSelected)
	if index.Selected != previous.Selected || !slices.Equal(index.Programs, previous.Programs) {
		if err := writeJSONAtomic(filepath.Join(a.dataDir, "library.json"), index); err != nil {
			return err
		}
	}
	a.persistentSelected = index.Selected
	a.library = next
	return nil
}
func (a *App) indexLocked(id string) int {
	for i, p := range a.library.Programs {
		if p.ID == id {
			return i
		}
	}
	return -1
}
func (a *App) engineReady() bool {
	f, err := os.Stat(a.engine)
	return err == nil && f.Mode().IsRegular() && f.Mode().Perm()&0111 != 0
}
func (a *App) snapshotLocked() Snapshot {
	s := Snapshot{Programs: make([]ProgramView, 0, len(a.library.Programs)), Selected: a.library.Selected, Engine: a.engine, GlobalSettings: GlobalSettings{OpenGLBackend: a.library.OpenGLBackend, CDrive: a.library.CDrive, Drives: copyDrives(a.library.Drives), Language: a.library.Language, Theme: a.library.Theme}}
	s.GlobalSettings.WindowsFont = a.library.WindowsFont
	s.GPUMemoryLimits = a.gpuMemoryLimits
	s.CPULimits = a.cpuLimits
	s.GlobalSettings.CPUCoresX64, s.GlobalSettings.CPUCoresX86 = a.library.CPUCoresX64, a.library.CPUCoresX86
	s.GlobalSettings.NativeLibraryBridge = a.library.NativeLibraryBridge
	s.GlobalSettings.NativeV8Bridge = a.library.NativeV8Bridge
	s.NativeV8Ready = a.nativeV8Host() != ""
	s.NativeV8TestsReady = len(a.nativeV8Tests()) == 2
	s.GlobalSettings.CompilerWorkers = a.library.CompilerWorkers
	s.GlobalSettings.CertificateRootsFile = a.library.CertificateRootsFile
	s.GlobalSettings.CertificateRootsPath = a.library.CertificateRootsPath
	s.Version = applicationVersion()
	if a.engineReady() {
		s.EngineReady = true
	}
	for _, p := range a.library.Programs {
		v := ProgramView{Program: p, Status: "ready", ConfigPath: a.profilePath(p.ID)}
		if job := a.analyses[p.ID]; job != nil {
			view := job.view
			v.Analysis = &view
		}
		if p.Temporary {
			v.ConfigPath = ""
		}
		_, err := os.Stat(p.Path)
		v.Missing = err != nil
		if r := a.runs[p.ID]; r != nil {
			r.mu.Lock()
			v.Status = r.status
			v.RunID = r.id
			v.Error = r.message
			v.ExitCode = r.exitCode
			v.Usage = r.usage
			if r.cmd.Process != nil {
				v.PID = r.cmd.Process.Pid
			}
			r.mu.Unlock()
		}
		if v.Status == "running" || v.Status == "stopping" {
			s.Running++
		}
		s.Programs = append(s.Programs, v)
	}
	return s
}
func (a *App) Snapshot() Snapshot { a.mu.Lock(); defer a.mu.Unlock(); return a.snapshotLocked() }
func (a *App) Add(paths []string) (AddResult, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	next := a.library
	next.Programs = append([]Program{}, next.Programs...)
	result := AddResult{Errors: []string{}}
	if len(paths) > 128 {
		return result, errors.New("一次最多加入 128 個程式")
	}
	selected := ""
	for _, path := range paths {
		p, err := inspectProgram(path)
		if err != nil {
			result.Errors = append(result.Errors, filepath.Base(path)+"："+err.Error())
			continue
		}
		duplicate := false
		info, _ := os.Stat(p.Path)
		for _, old := range next.Programs {
			if old.Temporary {
				continue
			}
			oldInfo, e := os.Stat(old.Path)
			if old.Path == p.Path || (e == nil && info != nil && os.SameFile(oldInfo, info)) {
				duplicate = true
				if selected == "" {
					selected = old.ID
				}
				break
			}
		}
		if duplicate {
			continue
		}
		if len(next.Programs) >= 512 {
			result.Errors = append(result.Errors, "程式庫上限為 512 個程式")
			break
		}
		p.CDrive = a.library.CDrive
		// 移出後重新加入相同路徑時，沿用該 EXE 的獨立設定。
		firstAddition := false
		if data, readErr := os.ReadFile(a.profilePath(p.ID)); readErr == nil {
			var saved Program
			if json.Unmarshal(data, &saved) != nil || saved.Version != 1 || saved.ID != p.ID || saved.Path != p.Path {
				result.Errors = append(result.Errors, p.Name+"：原設定檔格式不正確，已保留原檔")
				continue
			}
			p.Settings = saved.Settings
			p.MaxInstructions = instructionBudget(p.MaxInstructions)
		} else if !errors.Is(readErr, os.ErrNotExist) {
			return result, readErr
		} else {
			firstAddition = true
		}
		p.GpuMemoryMiB = normalizeGPUMemory(p.GpuMemoryMiB, a.gpuMemoryLimits)
		if err = writeJSONAtomic(a.profilePath(p.ID), p); err != nil {
			return result, err
		}
		next.Programs = append(next.Programs, p)
		result.Added++
		if firstAddition {
			result.AnalysisCandidates = append(result.AnalysisCandidates, p.ID)
		}
		if selected == "" {
			selected = p.ID
		}
	}
	if selected != "" {
		next.Selected = selected
	}
	if err := a.saveLocked(next); err != nil {
		return result, err
	}
	result.State = a.snapshotLocked()
	return result, nil
}
func (a *App) StartTemporary(path string) (Snapshot, error) {
	prepared, err := a.PrepareTemporary(path)
	if err != nil {
		return Snapshot{}, err
	}
	for _, p := range prepared.Programs {
		if p.ID == prepared.Selected {
			started, err := a.Start(p.ID, p.Settings, 80, 24)
			if err != nil {
				_ = a.Remove(p.ID)
			}
			return started, err
		}
	}
	return Snapshot{}, errors.New("找不到臨時工作階段")
}

func (a *App) PrepareTemporary(path string) (Snapshot, error) {
	p, err := inspectProgram(path)
	if err != nil {
		return Snapshot{}, err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	if len(a.library.Programs) >= 512 {
		return Snapshot{}, errors.New("請先關閉部分臨時程式")
	}
	p.ID, p.Temporary = identifier(), true
	p.CDrive = a.library.CDrive
	p.GpuMemoryMiB = a.gpuMemoryLimits.DefaultMiB
	a.library.Programs = append(a.library.Programs, p)
	a.library.Selected = p.ID
	return a.snapshotLocked(), nil
}
func (a *App) Select(id string) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.indexLocked(id) < 0 {
		return errors.New("找不到此程式")
	}
	next := a.library
	next.Selected = id
	return a.saveLocked(next)
}
func (a *App) Remove(id string) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	i := a.indexLocked(id)
	if i < 0 {
		return errors.New("找不到此程式")
	}
	if job := a.analyses[id]; job != nil && job.active() {
		return errors.New("請先取消加速分析")
	}
	if r := a.runs[id]; r != nil {
		select {
		case <-r.done:
		default:
			return errors.New("請先停止執行中的程式")
		}
	}
	next := a.library
	next.Programs = append([]Program{}, next.Programs...)
	temporary := next.Programs[i].Temporary
	next.Programs = append(next.Programs[:i], next.Programs[i+1:]...)
	if next.Selected == id {
		next.Selected = ""
		if temporary && a.persistentSelected != "" {
			next.Selected = a.persistentSelected
		} else if len(next.Programs) > 0 {
			next.Selected = next.Programs[0].ID
		}
	}
	if err := a.saveLocked(next); err != nil {
		return err
	}
	if r := a.runs[id]; r != nil {
		r.cancelOnce.Do(func() { close(r.cancel) })
	}
	delete(a.runs, id)
	return nil
}

// 只引述 Windows argv[0]；使用者參數作為同一個 --command-line 值交給客體解析。
// 整個啟動流程不經宿主 shell，空白、Unicode、分號與 $() 不會變成宿主命令。
func quoteWindows(value string) string {
	var b strings.Builder
	b.WriteByte('"')
	slashes := 0
	for _, r := range value {
		if r == '\\' {
			slashes++
			continue
		}
		if r == '"' {
			b.WriteString(strings.Repeat("\\", slashes*2+1))
		} else {
			b.WriteString(strings.Repeat("\\", slashes))
		}
		slashes = 0
		b.WriteRune(r)
	}
	b.WriteString(strings.Repeat("\\", slashes*2))
	b.WriteByte('"')
	return b.String()
}
func validateSettings(settings Settings) (Settings, error) {
	if strings.ContainsRune(settings.Arguments, 0) || len(settings.Arguments) > 32768 {
		return settings, errors.New("啟動參數過長或含有無效字元")
	}
	paths := []string{settings.WorkDir, settings.CDrive}
	if settings.InstallRoot != "" {
		paths = append(paths, settings.InstallRoot)
	}
	for _, path := range paths {
		if !filepath.IsAbs(path) {
			return settings, errors.New("資料夾必須是絕對路徑")
		}
		f, e := os.Stat(path)
		if e != nil || !f.IsDir() {
			return settings, fmt.Errorf("資料夾不存在：%s", path)
		}
	}
	var err error
	if settings.GpuMemoryMiB, err = validateGPUMemory(settings.GpuMemoryMiB); err != nil {
		return settings, err
	}
	settings.MaxInstructions = instructionBudget(settings.MaxInstructions)
	return settings, nil
}

// GPU 分頁只更新選取程式的容量，保留既有啟動參數與路徑設定。
func (a *App) SaveGPUMemory(id string, value uint64) error {
	value, err := validateGPUMemory(value)
	if err != nil {
		return err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	i := a.indexLocked(id)
	if i < 0 {
		return errors.New("找不到此程式")
	}
	p := a.library.Programs[i]
	if p.GpuMemoryMiB == value && !a.gpuMemoryMigrations[id] {
		return nil
	}
	p.GpuMemoryMiB = value
	if !p.Temporary {
		if err = writeJSONAtomic(a.profilePath(id), p); err != nil {
			return err
		}
	}
	a.library.Programs[i] = p
	delete(a.gpuMemoryMigrations, id)
	return nil
}
func copyDrives(drives map[string]string) map[string]string {
	out := make(map[string]string, len(drives))
	for key, value := range drives {
		out[key] = value
	}
	return out
}

// 舊設定或未來版本的未知偏好，載入時回退至安全預設。
func normalizeAppearance(language, theme string) (string, string) {
	switch language {
	case "auto", "zh-Hant", "en", "ja", "ko":
	default:
		language = "auto"
	}
	switch theme {
	case "forest", "ocean", "violet", "amber", "dark":
	default:
		theme = "forest"
	}
	return language, theme
}
func normalizeOpenGLBackend(mode string) string {
	switch mode {
	case "auto", "metal", "legacy":
		return mode
	default:
		return "auto"
	}
}
func validateGlobalSettings(settings GlobalSettings) (GlobalSettings, error) {
	certificatePath, err := certificateRootsRelativePath(settings.CertificateRootsPath)
	if err != nil {
		return GlobalSettings{}, err
	}
	font, err := validateWindowsFont(settings.WindowsFont)
	if err != nil {
		return GlobalSettings{}, err
	}
	if settings.CompilerWorkers > maxCompilerWorkers {
		return GlobalSettings{}, errors.New("編譯執行緒數必須介於 0 與 8")
	}
	limits := systemCPULimits()
	x64, err := validateCPUCount(settings.CPUCoresX64, limits.X64Max, limits.X64Max, "x64")
	if err != nil {
		return GlobalSettings{}, err
	}
	x86, err := validateCPUCount(settings.CPUCoresX86, limits.X86Max, 1, "x86")
	if err != nil {
		return GlobalSettings{}, err
	}
	backend := normalizeOpenGLBackend(settings.OpenGLBackend)
	if settings.OpenGLBackend != "" && backend != settings.OpenGLBackend {
		return GlobalSettings{}, errors.New("不支援的 OpenGL 後端")
	}
	language, theme := normalizeAppearance(settings.Language, settings.Theme)
	if (settings.Language != "" && language != settings.Language) || (settings.Theme != "" && theme != settings.Theme) {
		return GlobalSettings{}, errors.New("不支援的語言或介面配色")
	}
	directory := func(path string) (string, error) {
		if strings.TrimSpace(path) == "" || !filepath.IsAbs(path) {
			return "", errors.New("請指定存在的絕對目錄路徑")
		}
		info, err := os.Stat(path)
		if err != nil || !info.IsDir() {
			return "", fmt.Errorf("目錄不存在：%s", path)
		}
		return filepath.Clean(path), nil
	}
	c, err := directory(settings.CDrive)
	if err != nil {
		return GlobalSettings{}, fmt.Errorf("C: 為必要設定：%w", err)
	}
	out := GlobalSettings{NativeV8Bridge: settings.NativeV8Bridge, NativeLibraryBridge: settings.NativeLibraryBridge, CompilerWorkers: settings.CompilerWorkers, CPUCoresX64: x64, CPUCoresX86: x86, OpenGLBackend: backend, CDrive: c, Drives: map[string]string{}, Language: language, Theme: theme}
	out.WindowsFont = font
	out.CertificateRootsFile, out.CertificateRootsPath = settings.CertificateRootsFile, certificatePath
	for key, value := range settings.Drives {
		letter := strings.ToUpper(strings.TrimSuffix(key, ":"))
		if len(letter) != 1 || letter[0] < 'D' || letter[0] > 'Z' {
			return GlobalSettings{}, errors.New("附加磁碟代號限 D: 至 Z:")
		}
		if _, exists := out.Drives[letter]; exists {
			return GlobalSettings{}, errors.New("磁碟代號不可重複")
		}
		path, err := directory(value)
		if err != nil {
			return GlobalSettings{}, fmt.Errorf("%s: %w", letter, err)
		}
		out.Drives[letter] = path
	}
	return out, nil
}
func (a *App) SaveGlobalSettings(settings GlobalSettings) error {
	settings, err := validateGlobalSettings(settings)
	if err != nil {
		return err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	if settings.NativeV8Bridge && a.nativeV8Host() == "" {
		return errors.New("找不到 ARM64 V8 模組，請使用含 V8 的 Launcher 測試版")
	}
	if err = writeJSONAtomic(filepath.Join(a.dataDir, "settings.json"), settings); err != nil {
		return err
	}
	a.library.NativeLibraryBridge = settings.NativeLibraryBridge
	a.library.NativeV8Bridge = settings.NativeV8Bridge
	a.library.CertificateRootsFile, a.library.CertificateRootsPath = settings.CertificateRootsFile, settings.CertificateRootsPath
	a.library.OpenGLBackend = settings.OpenGLBackend
	a.library.CompilerWorkers = settings.CompilerWorkers
	a.library.CPUCoresX64, a.library.CPUCoresX86 = settings.CPUCoresX64, settings.CPUCoresX86
	a.library.CDrive = settings.CDrive
	a.library.Drives = copyDrives(settings.Drives)
	a.library.Language, a.library.Theme = settings.Language, settings.Theme
	a.library.WindowsFont = settings.WindowsFont
	return nil
}
func (a *App) SaveSettings(id string, settings Settings) error {
	settings, err := validateSettings(settings)
	if err != nil {
		return err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	i := a.indexLocked(id)
	if i < 0 {
		return errors.New("找不到此程式")
	}
	p := a.library.Programs[i]
	if _, err = a.programCPUCount(p.Architecture, settings.CPUCores); err != nil {
		return err
	}
	p.Settings = settings
	if !p.Temporary {
		if err = writeJSONAtomic(a.profilePath(id), p); err != nil {
			return err
		}
	}
	a.library.Programs[i] = p
	delete(a.gpuMemoryMigrations, id)
	return nil
}
func (a *App) Start(id string, settings Settings, cols, rows uint16) (Snapshot, error) {
	settings, err := validateSettings(settings)
	if err != nil {
		return Snapshot{}, err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	i := a.indexLocked(id)
	if i < 0 {
		return Snapshot{}, errors.New("找不到此程式")
	}
	if job := a.analyses[id]; job != nil && job.active() {
		return Snapshot{}, errors.New("請先取消加速分析")
	}
	if r := a.runs[id]; r != nil {
		select {
		case <-r.done:
		default:
			return Snapshot{}, errors.New("程式已在執行中")
		}
	}
	p, err := inspectProgram(a.library.Programs[i].Path)
	if err != nil {
		return Snapshot{}, err
	}
	cores, err := a.programCPUCount(p.Architecture, settings.CPUCores)
	if err != nil {
		return Snapshot{}, err
	}
	if !a.engineReady() {
		return Snapshot{}, errors.New("找不到執行引擎，請重新建置啟動器")
	}
	next := a.library
	next.Programs = append([]Program{}, next.Programs...)
	p.ID = id
	p.Temporary = a.library.Programs[i].Temporary
	p.Settings = settings
	if err = ensureSandboxCertificateRootsAt(p.CDrive, a.library.CertificateRootsPath); err != nil {
		return Snapshot{}, fmt.Errorf("初始化 Sandbox 公開根憑證檔案：%w", err)
	}
	next.Programs[i] = p
	next.Selected = id
	if !p.Temporary {
		if err = writeJSONAtomic(a.profilePath(id), p); err != nil {
			return Snapshot{}, err
		}
	}
	if err = a.saveLocked(next); err != nil {
		return Snapshot{}, err
	}
	delete(a.gpuMemoryMigrations, id)
	runID := identifier()
	folder := filepath.Join(a.dataDir, "logs", runID)
	if err = os.MkdirAll(folder, 0700); err != nil {
		return Snapshot{}, err
	}
	command := quoteWindows(filepath.Base(p.Path))
	if p.Arguments != "" {
		command += " " + p.Arguments
	}
	gui := "headless"
	if p.Kind == "gui" {
		gui = "native"
	}
	diagnostics := filepath.Join(folder, "engine.log")
	mode := "read-only"
	if p.Writable {
		mode = "read-write"
	}
	budget := p.MaxInstructions
	if budget == 0 {
		// 引擎已有 UINT64_MAX 的無限制慣例；CLI 的 0 仍保留零步測試語意。
		budget = ^uint64(0)
	}
	// GPU 配額由引擎共用於 OpenGL、D3D9／D3D11；一般與臨時程式沿用同一啟動流程。
	args := []string{"run-pe", p.Path, "--backend", "jit", "--unresolved-imports", "trap", "--gui", gui, "--gpu-memory-mib", fmt.Sprint(p.GpuMemoryMiB), "--clock", "host", "--stdio", "live", "--command-line", command, "--max-instructions", fmt.Sprint(budget), "--fs-root", p.WorkDir, "--fs-mode", mode, "--c-drive", p.CDrive, "--registry-file", filepath.Join(a.dataDir, "registry.dat"), "--diagnostics-file", diagnostics}
	// 同一 Launcher／C 槽使用同一個 Windows IPC 工作階段；不同 C 槽維持隔離。
	sessionKey, err := filepath.EvalSymlinks(p.CDrive)
	if err != nil {
		return Snapshot{}, fmt.Errorf("無法識別 Sandbox C 槽：%w", err)
	}
	sessionOutput := ""
	if shared := a.mappingSessions[sessionKey]; shared != nil {
		args = append(args, "--mapping-session", shared.path)
	} else {
		sessionOutput = filepath.Join(folder, "mapping-session.txt")
		args = append(args, "--mapping-session-output", sessionOutput)
	}
	args = append(args, "--cpu-count", fmt.Sprint(cores))
	if a.library.CertificateRootsFile {
		args = append(args, "--certificate-roots-file", a.library.CertificateRootsPath)
	}
	args = append(args, "--memory-limit-mib", fmt.Sprint(processMemoryLimitMiB(p.Architecture, a.gpuMemoryLimits.SystemMemoryBytes)))
	usagePath := ""
	if p.Kind == "gui" {
		usagePath = filepath.Join(folder, "usage.json")
		// D3D9／D3D11 共用 Metal command buffer 計時與資源用量，不另開取樣流程。
		args = append(args, "--usage-file", usagePath)
	}
	letters := make([]string, 0, len(a.library.Drives))
	for letter := range a.library.Drives {
		letters = append(letters, letter)
	}
	slices.Sort(letters)
	for _, letter := range letters {
		args = append(args, "--drive", letter+"="+a.library.Drives[letter])
	}
	if p.InstallRoot != "" {
		args = append(args, "--install-root", p.InstallRoot)
	}
	cmd := exec.Command(a.engine, args...)
	cmd.Dir = p.WorkDir
	environment, err := a.engineEnvironmentLocked()
	if err != nil {
		return Snapshot{}, err
	}
	cmd.Env = environment
	r := &execution{id: runID, status: "running", cmd: cmd, diagnostics: diagnostics, usagePath: usagePath, output: make(chan []byte, 64), input: make(chan string, 32), done: make(chan struct{}), cancel: make(chan struct{})}
	var readDone chan struct{}
	if p.Kind == "cli" {
		if cols < 2 {
			cols = 80
		}
		if rows < 2 {
			rows = 24
		}
		r.terminal, err = pty.StartWithSize(cmd, &pty.Winsize{Cols: cols, Rows: rows})
		if err == nil {
			if err = nativePTY(r.terminal.Fd()); err != nil {
				_ = r.terminal.Close()
				_ = cmd.Process.Kill()
				_ = cmd.Wait()
			}
		}
		if err == nil {
			readDone = make(chan struct{})
			go func() {
				defer close(readDone)
				buffer := make([]byte, 16384)
				for {
					n, e := r.terminal.Read(buffer)
					if n > 0 {
						select {
						case r.output <- append([]byte{}, buffer[:n]...):
						case <-r.cancel:
							return
						}
					}
					if e != nil {
						return
					}
				}
			}()
			go func() {
				for {
					select {
					case data := <-r.input:
						if _, err := r.terminal.Write([]byte(data)); err != nil {
							return
						}
					case <-r.done:
						return
					case <-r.cancel:
						return
					}
				}
			}()
		}
	} else {
		cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
		cmd.Stdout = streamWriter{r}
		cmd.Stderr = streamWriter{r}
		err = cmd.Start()
	}
	if err != nil {
		return Snapshot{}, fmt.Errorf("無法啟動程式：%w", err)
	}
	if sessionOutput != "" {
		if shared := acquireLauncherMappingSession(sessionOutput); shared != nil {
			if a.mappingSessions == nil {
				a.mappingSessions = make(map[string]*launcherMappingSession)
			}
			a.mappingSessions[sessionKey] = shared
		}
	}
	if old := a.runs[id]; old != nil {
		old.cancelOnce.Do(func() { close(old.cancel) })
	}
	a.runs[id] = r
	go func() {
		err := cmd.Wait()
		if readDone != nil {
			<-readDone
			r.closeTerminal()
		}
		r.mu.Lock()
		defer r.mu.Unlock()
		code := cmd.ProcessState.ExitCode()
		if cmd.ProcessState.Exited() {
			code = guestProcessExitCode(r.diagnostics, code)
		}
		r.exitCode = &code
		switch {
		case r.stopping:
			r.status = "stopped"
		case err == nil && code == 0:
			r.status = "exited"
		default:
			r.status = "failed"
			r.message = fmt.Sprintf("程式已結束（退出碼 %d）", code)
		}
		if status, ok := cmd.ProcessState.Sys().(syscall.WaitStatus); ok && status.Signaled() && status.Signal() == syscall.SIGINT {
			r.status = "stopped"
			r.message = ""
		}
		close(r.done)
	}()
	return a.snapshotLocked(), nil
}

// 保留原始 namespace 字面路徑，避免 macOS /tmp 別名改變 Unix socket 的 peer 身分。
// 引擎自己驗證 namespace 的簽章與歸屬；Launcher 只持有同 UID、一般檔案的共享 lease。
func acquireLauncherMappingSession(report string) *launcherMappingSession {
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(report)
		if err == nil {
			path := strings.TrimSuffix(string(data), "\n")
			if !filepath.IsAbs(path) || strings.ContainsAny(path, "\r\n\x00") || !strings.HasPrefix(filepath.Base(path), "x64arm-mapping-") {
				return nil
			}
			fd, err := syscall.Open(filepath.Join(path, ".lifetime"), syscall.O_RDONLY|syscall.O_CLOEXEC|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
			if err != nil {
				return nil
			}
			file := os.NewFile(uintptr(fd), "Windows 工作階段 lease")
			info, err := file.Stat()
			if err != nil || !info.Mode().IsRegular() {
				file.Close()
				return nil
			}
			identity, ok := info.Sys().(*syscall.Stat_t)
			if !ok || identity.Uid != uint32(os.Geteuid()) || info.Mode().Perm()&0077 != 0 || syscall.Flock(fd, syscall.LOCK_SH|syscall.LOCK_NB) != nil {
				file.Close()
				return nil
			}
			return &launcherMappingSession{path: path, lease: file}
		}
		if !os.IsNotExist(err) {
			return nil
		}
		time.Sleep(5 * time.Millisecond)
	}
	return nil
}

// POSIX 程序退出狀態只保留低 8 位；正常完成時以同一執行紀錄中的 Windows DWORD 為準。
// 引擎故障、訊號終止、截斷／過期紀錄及子程序診斷均不能取代宿主的失敗狀態。
func guestProcessExitCode(path string, host int) int {
	file, err := os.Open(path)
	if err != nil {
		return host
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return host
	}
	const limit int64 = 64 * 1024
	start := max(int64(0), info.Size()-limit)
	if _, err := file.Seek(start, io.SeekStart); err != nil {
		return host
	}
	data, err := io.ReadAll(io.LimitReader(file, limit))
	if err != nil {
		return host
	}
	lines := strings.Split(string(data), "\n")
	if start > 0 {
		lines = lines[1:]
	}
	code, found, exited := uint64(0), false, false
	for _, line := range lines {
		if value, ok := strings.CutPrefix(line, "exit_code="); ok {
			parsed, err := strconv.ParseUint(value, 10, 32)
			code, found = parsed, err == nil
		}
		if value, ok := strings.CutPrefix(line, "reason="); ok {
			fields := strings.Fields(value)
			exited = len(fields) > 0 && fields[0] == "exited"
		}
	}
	if found && exited && host >= 0 && uint64(host)&255 == code&255 {
		return int(code)
	}
	return host
}

type streamWriter struct{ run *execution }

func (w streamWriter) Write(p []byte) (int, error) {
	total := len(p)
	for len(p) > 0 {
		n := min(len(p), 16384)
		select {
		case w.run.output <- append([]byte{}, p[:n]...):
			p = p[n:]
		case <-w.run.cancel:
			return 0, io.ErrClosedPipe
		}
	}
	return total, nil
}

func (a *App) getRun(id, runID string) (*execution, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	r := a.runs[id]
	if r == nil || r.id != runID {
		return nil, errors.New("此執行階段已結束")
	}
	return r, nil
}
func (a *App) Input(id, runID, data string) error {
	if len(data) > 65536 {
		return errors.New("單次輸入超過 64 KiB")
	}
	r, e := a.getRun(id, runID)
	if e != nil {
		return e
	}
	if r.terminal == nil {
		return errors.New("此程式沒有終端")
	}
	select {
	case <-r.done:
		return errors.New("程式已結束")
	default:
	}
	select {
	case r.input <- data:
		return nil
	default:
		return errors.New("輸入佇列忙碌，請稍後再試")
	}
}
func (a *App) Resize(id, runID string, cols, rows uint16) error {
	if cols < 2 || rows < 2 || cols > 1000 || rows > 1000 {
		return errors.New("終端尺寸不合法")
	}
	r, e := a.getRun(id, runID)
	if e != nil {
		return e
	}
	if r.terminal == nil {
		return nil
	}
	// pty.Setsize 內部會呼叫 File.Fd，必須與 Close 序列化。
	r.terminalMu.Lock()
	defer r.terminalMu.Unlock()
	if r.terminalClosed {
		return nil
	}
	select {
	case <-r.done:
		return nil
	default:
	}
	return pty.Setsize(r.terminal, &pty.Winsize{Cols: cols, Rows: rows})
}
func (r *execution) closeTerminal() {
	r.terminalMu.Lock()
	defer r.terminalMu.Unlock()
	if r.terminal != nil && !r.terminalClosed {
		r.terminalClosed = true
		_ = r.terminal.Close()
	}
}
func (a *App) Stop(id string) error {
	a.mu.Lock()
	r := a.runs[id]
	a.mu.Unlock()
	if r == nil {
		return nil
	}
	r.mu.Lock()
	select {
	case <-r.done:
		r.mu.Unlock()
		return nil
	default:
	}
	r.stopping = true
	r.status = "stopping"
	r.mu.Unlock()
	// GUI 使用 Setpgid，PTY 使用新 session；兩者的 group ID 皆為主程序 PID。
	// 子程序可能繼承 stdout／stderr，僅終止主程序會讓 cmd.Wait 持續等待 EOF。
	group := -r.cmd.Process.Pid
	err := syscall.Kill(group, syscall.SIGTERM)
	go func() {
		select {
		case <-r.done:
		case <-time.After(2 * time.Second):
		}
		// 主程序退出仍可能留下忽略 TERM 的子程序；明確停止須清理同群組。
		_ = syscall.Kill(group, syscall.SIGKILL)
	}()
	if errors.Is(err, syscall.ESRCH) {
		return nil
	}
	return err
}
func (a *App) Poll() PollResult {
	a.mu.Lock()
	defer a.mu.Unlock()
	result := PollResult{Streams: []Stream{}}
	for id, r := range a.runs {
		r.refreshUsage(time.Now())
		data := []byte{}
	drain:
		for len(data) < 262144 {
			select {
			case bytes := <-r.output:
				data = append(data, bytes...)
			default:
				break drain
			}
		}
		if len(data) > 0 {
			result.Streams = append(result.Streams, Stream{ID: id, RunID: r.id, Data: data})
		}
	}
	result.State = a.snapshotLocked()
	return result
}
func (a *App) Diagnostics(id string) (string, error) {
	a.mu.Lock()
	r := a.runs[id]
	a.mu.Unlock()
	if r == nil {
		return "尚無執行紀錄。", nil
	}
	f, e := os.Open(r.diagnostics)
	if errors.Is(e, os.ErrNotExist) {
		return "正在等待執行紀錄…", nil
	}
	if e != nil {
		return "", e
	}
	defer f.Close()
	info, e := f.Stat()
	if e != nil {
		return "", e
	}
	if info.Size() > 65536 {
		_, _ = f.Seek(-65536, io.SeekEnd)
	}
	b, e := io.ReadAll(f)
	return strings.ToValidUTF8(string(b), "�"), e
}
func (a *App) Close() {
	a.closeOnce.Do(a.close)
}
func (a *App) close() {
	a.closeAnalyses()
	a.mu.Lock()
	runs := make(map[string]*execution, len(a.runs))
	for id, r := range a.runs {
		runs[id] = r
	}
	a.mu.Unlock()
	for id, r := range runs {
		_ = a.Stop(id)
		r.cancelOnce.Do(func() { close(r.cancel) })
		r.closeTerminal()
	}
	for _, r := range runs {
		select {
		case <-r.done:
		case <-time.After(3 * time.Second):
		}
	}
	for _, shared := range a.mappingSessions {
		_ = shared.lease.Close()
	}
	if a.lockFile != nil {
		_ = a.lockFile.Close()
	}
}
