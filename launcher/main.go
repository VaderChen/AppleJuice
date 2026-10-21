package main

import (
	"embed"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	webview "github.com/webview/webview_go"
)

//go:embed web/index.html web/app.css web/app.js web/i18n.js web/smoke.js web/launch-smoke.js web/v8-smoke.js web/vendor/* assets/AppleJuice.png
var assets embed.FS

type pathsFlag []string

func (p *pathsFlag) String() string         { return strings.Join(*p, ", ") }
func (p *pathsFlag) Set(value string) error { *p = append(*p, value); return nil }

type request struct {
	Language       string          `json:"language"`
	GlobalSettings GlobalSettings  `json:"globalSettings"`
	Action         string          `json:"action"`
	ID             string          `json:"id"`
	RunID          string          `json:"runId"`
	Data           string          `json:"data"`
	Settings       Settings        `json:"settings"`
	Cols           uint16          `json:"cols"`
	Rows           uint16          `json:"rows"`
	PID            int             `json:"pid"`
	Paths          []string        `json:"paths"`
	Result         json.RawMessage `json:"result"`
}
type smokeConfig struct {
	Scenario       string             `json:"scenario,omitempty"`
	SettingsTest   string             `json:"settingsTest,omitempty"`
	Programs       []string           `json:"programs,omitempty"`
	Invalid        string             `json:"invalid,omitempty"`
	Output         string             `json:"output"`
	TimeoutSeconds int                `json:"timeoutSeconds,omitempty"`
	ProbePickers   *bool              `json:"probePickers,omitempty"`
	Launch         *launchSmokeConfig `json:"launch,omitempty"`
}
type launchSmokeConfig struct {
	Program          string   `json:"program"`
	ReuseExisting    bool     `json:"reuseExisting,omitempty"`
	Temporary        bool     `json:"temporary"`
	Settings         Settings `json:"settings"`
	ExpectedExitCode int      `json:"expectedExitCode"`
	ExpectedOutput   string   `json:"expectedOutput,omitempty"`
	RequireVisible   bool     `json:"requireVisible"`
	Capture          bool     `json:"capture"`
	ObserveSeconds   uint32   `json:"observeSeconds,omitempty"`
}

func enginePath(explicit string) string {
	if explicit != "" {
		path, _ := filepath.Abs(explicit)
		return path
	}
	executable, _ := os.Executable()
	dir := filepath.Dir(executable)
	candidates := []string{filepath.Join(dir, "x64arm-engine"), filepath.Join(dir, "x64arm")}
	cwd, _ := os.Getwd()
	for _, start := range []string{dir, cwd} {
		for root := start; ; root = filepath.Dir(root) {
			candidates = append(candidates, filepath.Join(root, "build", "release", "x64arm"))
			if filepath.Dir(root) == root {
				break
			}
		}
	}
	for _, path := range candidates {
		if info, err := os.Stat(path); err == nil && info.Mode().IsRegular() && info.Mode().Perm()&0111 != 0 {
			return path
		}
	}
	return candidates[0]
}
func readAsset(path string) string {
	data, err := assets.ReadFile("web/" + path)
	if err != nil {
		panic(err)
	}
	return string(data)
}
func htmlDocument(smoke *smokeConfig) string {
	html := readAsset("index.html")
	icon, err := assets.ReadFile("assets/AppleJuice.png")
	if err != nil {
		panic(err)
	}
	html = strings.ReplaceAll(html, "APP_ICON_DATA", "data:image/png;base64,"+base64.StdEncoding.EncodeToString(icon))
	html = strings.ReplaceAll(html, "/* APP_CSS */", readAsset("app.css")+"\n"+readAsset("vendor/xterm.css"))
	js := readAsset("vendor/xterm.js") + "\n" + readAsset("vendor/addon-fit.js") + "\n" + readAsset("i18n.js") + "\n" + readAsset("app.js")
	if smoke != nil {
		config, _ := json.Marshal(smoke)
		script := "smoke.js"
		if smoke.Scenario == "launch" {
			script = "launch-smoke.js"
		}
		if smoke.Scenario == "v8" {
			script = "v8-smoke.js"
		}
		js += "\nwindow.smokeConfig=" + string(config) + ";\n" + readAsset(script)
	}
	return strings.ReplaceAll(html, "/* APP_JS */", js)
}

func run() error {
	var add pathsFlag
	engine := flag.String("engine", "", "指定 x64arm 引擎")
	dataDir := flag.String("data-dir", "", "程式庫與獨立設定檔目錄")
	cDrive := flag.String("c-drive", "", "新加入程式的預設 C 槽映射")
	smokePath := flag.String("smoke", "", "原生 UI Smoke 設定檔（開發用）")
	backgroundDisplay := flag.Bool("background-display", false, "在非焦點螢幕顯示 Launcher 與客體，不主動取得焦點")
	flag.Var(&add, "add", "加入 EXE，可重複指定")
	flag.Parse()
	if *dataDir == "" {
		config, err := os.UserConfigDir()
		if err != nil {
			return err
		}
		*dataDir = filepath.Join(config, "Win64Launcher")
	}
	absolute, err := filepath.Abs(*dataDir)
	if err != nil {
		return err
	}
	*dataDir = absolute
	if *cDrive != "" {
		*cDrive, err = filepath.Abs(*cDrive)
		if err != nil {
			return err
		}
	}
	var smoke *smokeConfig
	if *smokePath != "" {
		data, err := os.ReadFile(*smokePath)
		if err != nil {
			return err
		}
		smoke = &smokeConfig{}
		if err = json.Unmarshal(data, smoke); err != nil {
			return err
		}
		if smoke.Scenario != "" && smoke.Scenario != "library" && smoke.Scenario != "launch" && smoke.Scenario != "settings" && smoke.Scenario != "v8" {
			return errors.New("不支援的 Smoke 情境")
		}
		if smoke.TimeoutSeconds == 0 {
			smoke.TimeoutSeconds = 110
		}
		if smoke.TimeoutSeconds < 5 || smoke.TimeoutSeconds > 3600 {
			return errors.New("Smoke 逾時必須介於 5 至 3600 秒")
		}
		if smoke.Scenario == "launch" {
			if smoke.Launch == nil || !filepath.IsAbs(smoke.Launch.Program) {
				return errors.New("啟動 Smoke 必須指定 EXE 絕對路徑")
			}
			if smoke.Launch.ObserveSeconds >= uint32(smoke.TimeoutSeconds-2) {
				return errors.New("觀察時間必須短於 Smoke 逾時")
			}
			// 單程式情境經正式 UI 輸入啟動設定；指令上限沿用產品預設。
			if instructionBudget(smoke.Launch.Settings.MaxInstructions) != defaultMaxInstructions {
				return errors.New("啟動 Smoke 的指令上限沿用 UI 預設值")
			}
			if !smoke.Launch.Temporary {
				if _, err = validateSettings(smoke.Launch.Settings); err != nil {
					return err
				}
			}
		}
		if !filepath.IsAbs(smoke.Output) {
			return errors.New("Smoke 輸出目錄必須是絕對路徑")
		}
		if err = os.MkdirAll(smoke.Output, 0700); err != nil {
			return err
		}
	}
	app, err := NewApp(enginePath(*engine), *dataDir, *cDrive)
	if err != nil {
		return err
	}
	defer app.Close()
	app.backgroundDisplay = *backgroundDisplay
	if smoke != nil && *backgroundDisplay {
		probe := false
		smoke.ProbePickers = &probe
	}
	if len(add) > 0 {
		result, err := app.Add(add)
		if err != nil {
			return err
		}
		for _, message := range result.Errors {
			fmt.Fprintln(os.Stderr, message)
		}
	}
	var w webview.WebView
	if *backgroundDisplay {
		window, err := nativeBackgroundWindow()
		if err != nil {
			return err
		}
		defer nativeCloseBackgroundWindow()
		app.backgroundDisplayID = nativeBackgroundDisplayID()
		w = webview.NewWindow(false, window)
	} else {
		w = webview.New(false)
	}
	if w == nil {
		return errors.New("無法建立 WebView 視窗")
	}
	defer w.Destroy()
	w.SetTitle("Win64 Launcher")
	w.SetSize(920, 650, webview.HintMin)
	w.SetSize(1180, 800, webview.HintNone)
	nativeConfigure(w.Window())
	var pickerPaths, pickerFolders []string
	finished := make(chan struct{})
	defer close(finished)
	var bridge *asyncBridge
	handleRequest := func(payload string) (any, error) {
		// 綁定套件的原生 JSON parser 不接受未跳脫的 DEL。以 ASCII 封裝
		// 所有 UTF-8 請求，避免按鍵、檔名或參數中的控制字元造成 RPC 消失。
		if len(payload) > 2*1024*1024 {
			return nil, errors.New("請求內容超過上限")
		}
		data, err := base64.StdEncoding.DecodeString(payload)
		if err != nil {
			return nil, errors.New("請求編碼不正確")
		}
		var req request
		if err = json.Unmarshal(data, &req); err != nil {
			return nil, errors.New("請求格式不正確")
		}
		if err := bridge.onUI(func() { nativeLanguage(req.Language) }); err != nil {
			return nil, err
		}
		defer func() {
			running := app.Snapshot().Running
			bridge.post(func() { nativeRunning(running) })
		}()
		if smoke != nil && req.Action != "poll" && req.Action != "resize" && req.Action != "smokeStopRequested" {
			if req.Action == "smokeLog" {
				fmt.Fprintf(os.Stderr, "%s %s\n", time.Now().Format("15:04:05.000"), req.Data)
			} else {
				fmt.Fprintf(os.Stderr, "%s %s\n", time.Now().Format("15:04:05.000"), req.Action)
			}
		}
		switch req.Action {
		case "snapshot":
			return app.Snapshot(), nil
		case "windowsFonts":
			return availableWindowsFonts(), nil
		case "poll":
			return app.Poll(), nil
		case "add":
			paths := pickerPaths
			pickerPaths = nil
			if paths == nil {
				var err error
				paths, err = onUI(bridge, func() ([]string, error) { return nativePick(false, false, "") })
				if err != nil {
					return nil, err
				}
			}
			return app.Add(paths)
		case "addNativeV8Tests":
			return app.AddNativeV8Tests()
		case "temporary":
			paths := pickerPaths
			pickerPaths = nil
			if paths == nil {
				paths, err = onUI(bridge, func() ([]string, error) { return nativePick(false, false, "選擇臨時執行的 EXE", true) })
				if err != nil {
					return nil, err
				}
			}
			if len(paths) == 0 {
				return nil, nil
			}
			if len(paths) != 1 {
				return nil, errors.New("臨時執行每次請選擇一個 EXE")
			}
			return app.StartTemporary(paths[0])
		case "select":
			err := app.Select(req.ID)
			return app.Snapshot(), err
		case "remove":
			err := app.Remove(req.ID)
			return app.Snapshot(), err
		case "saveGlobalSettings":
			if req.ID != "" {
				// 先確認整份偏好有效，再保存選取程式的 GPU 容量。
				if _, err := validateGlobalSettings(req.GlobalSettings); err != nil {
					return app.Snapshot(), err
				}
				if err := app.SaveGPUMemory(req.ID, req.Settings.GpuMemoryMiB); err != nil {
					return app.Snapshot(), err
				}
			}
			err := app.SaveGlobalSettings(req.GlobalSettings)
			return app.Snapshot(), err
		case "save":
			err := app.SaveSettings(req.ID, req.Settings)
			return app.Snapshot(), err
		case "analyze":
			return app.Analyze(req.ID)
		case "cancelAnalysis":
			err := app.CancelAnalysis(req.ID)
			return app.Snapshot(), err
		case "start":
			return app.Start(req.ID, req.Settings, req.Cols, req.Rows)
		case "stop":
			err := app.Stop(req.ID)
			return app.Snapshot(), err
		case "input":
			return nil, app.Input(req.ID, req.RunID, req.Data)
		case "resize":
			return nil, app.Resize(req.ID, req.RunID, req.Cols, req.Rows)
		case "folder":
			if pickerFolders != nil {
				paths := pickerFolders
				pickerFolders = nil
				return paths, nil
			}
			return onUI(bridge, func() ([]string, error) { return nativePick(true, false, req.Data) })
		case "diagnostics":
			return app.Diagnostics(req.ID)
		case "reveal":
			for _, p := range app.Snapshot().Programs {
				if p.ID == req.ID {
					if p.Temporary {
						return nil, errors.New("臨時程式沒有儲存的設定檔")
					}
					return nil, bridge.onUI(func() { nativeReveal(p.ConfigPath) })
				}
			}
			return nil, errors.New("找不到此程式")
		}
		// 測試入口只有明確指定 --smoke 的本機程序才會開放。
		if smoke != nil {
			switch req.Action {
			case "smokeDelay":
				if req.Cols > 2000 {
					return nil, errors.New("Smoke 延遲不可超過 2000 毫秒")
				}
				time.Sleep(time.Duration(req.Cols) * time.Millisecond)
				return nil, nil
			case "smokeLog":
				return nil, nil
			case "smokePick":
				pickerPaths = req.Paths
				return nil, nil
			case "smokePickFolder":
				pickerFolders = req.Paths
				return nil, nil
			case "smokeProbe":
				return onUI(bridge, func() ([]string, error) { return nativePick(false, true, "") })
			case "smokeProbeTemporary":
				return onUI(bridge, func() ([]string, error) { return nativePick(false, true, "選擇臨時執行的 EXE", true) })
			case "smokeProbeFolder":
				return onUI(bridge, func() ([]string, error) { return nativePick(true, true, "選擇安裝目錄") })
			case "smokeVisible":
				return onUI(bridge, func() (bool, error) { return nativeVisible(req.PID), nil })
			case "smokeStopRequested":
				_, err := os.Stat(filepath.Join(smoke.Output, "stop"))
				if err != nil && !os.IsNotExist(err) {
					return nil, err
				}
				return err == nil, nil
			case "smokeSize":
				return nil, bridge.onUI(func() { w.SetSize(int(req.Cols), int(req.Rows), webview.HintNone) })
			case "smokeSnapshot":
				name := filepath.Base(req.Data)
				if filepath.Ext(name) != ".png" {
					return nil, errors.New("快照必須是 PNG")
				}
				return nil, bridge.onUI(func() { nativeSnapshot(w.Window(), filepath.Join(smoke.Output, name)) })
			case "smokeSnapshotReady":
				_, err := os.Stat(filepath.Join(smoke.Output, filepath.Base(req.Data)))
				return err == nil, nil
			case "smokeResult":
				if !json.Valid(req.Result) {
					return nil, errors.New("Smoke 結果不是 JSON")
				}
				if err := os.WriteFile(filepath.Join(smoke.Output, "result.json"), req.Result, 0600); err != nil {
					return nil, err
				}
				// 先讓 JS Promise 回傳；之後從主執行緒結束事件迴圈。
				go func() { time.Sleep(100 * time.Millisecond); bridge.post(w.Terminate) }()
				return nil, nil
			}
		}
		return nil, errors.New("未知操作")
	}
	bridge = newAsyncBridge(handleRequest, w.Dispatch, w.Eval)
	defer bridge.close()
	err = w.Bind("launcherRequest", bridge.submit)
	if err != nil {
		return err
	}
	w.SetHtml(htmlDocument(smoke))
	if smoke != nil {
		go func() {
			select {
			case <-finished:
				return
			case <-time.After(time.Duration(smoke.TimeoutSeconds) * time.Second):
				_ = os.WriteFile(filepath.Join(smoke.Output, "timeout.txt"), []byte(fmt.Sprintf("原生 UI Smoke 超過 %d 秒", smoke.TimeoutSeconds)), 0600)
				bridge.post(w.Terminate)
			}
		}()
	}
	w.Run()
	return nil
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		if !strings.Contains(strings.Join(os.Args, " "), "--smoke") && !strings.Contains(strings.Join(os.Args, " "), "--background-display") {
			nativeAlert(err.Error())
		}
		os.Exit(1)
	}
}
