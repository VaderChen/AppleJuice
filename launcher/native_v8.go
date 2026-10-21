package main

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strconv"
)

// 依引擎同目錄與 app 資源位置尋找；不依應用程式名稱或客體特徵選擇。
func (a *App) nativeV8Host() string {
	for _, name := range []string{"x64arm-v8-host", "x64arm_v8_host"} {
		path := filepath.Join(filepath.Dir(a.engine), name)
		if executableFile(path) {
			return path
		}
	}
	return ""
}
func executableFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular() && info.Mode().Perm()&0111 != 0
}
func (a *App) nativeV8Tests() []string {
	for _, folder := range []string{filepath.Join(filepath.Dir(a.engine), "..", "Resources", "V8Tests"), filepath.Join(filepath.Dir(a.engine), "v8-tests")} {
		paths := []string{filepath.Clean(filepath.Join(folder, "native-v8-x86.exe")), filepath.Clean(filepath.Join(folder, "native-v8-x64.exe"))}
		ready := true
		for _, path := range paths {
			if _, err := inspectProgram(path); err != nil {
				ready = false
				break
			}
		}
		if ready {
			return paths
		}
	}
	return nil
}
func (a *App) AddNativeV8Tests() (AddResult, error) {
	paths := a.nativeV8Tests()
	if len(paths) != 2 {
		return AddResult{}, errors.New("此版本未包含 V8 測試程式")
	}
	// 執行與 SMAP 保存在使用者資料目錄，避免修改 app 的已簽署資源。
	folder := filepath.Join(a.dataDir, "native-v8-tests")
	if err := os.MkdirAll(folder, 0700); err != nil {
		return AddResult{}, err
	}
	installed := []string{}
	for _, source := range paths {
		contents, err := os.ReadFile(source)
		if err != nil {
			return AddResult{}, err
		}
		destination := filepath.Join(folder, filepath.Base(source))
		existing, readErr := os.ReadFile(destination)
		if readErr != nil || !bytes.Equal(existing, contents) {
			temporary, err := os.CreateTemp(folder, ".v8-test-")
			if err != nil {
				return AddResult{}, err
			}
			name := temporary.Name()
			_, err = temporary.Write(contents)
			closeErr := temporary.Close()
			if err == nil {
				err = closeErr
			}
			if err == nil {
				err = os.Rename(name, destination)
			}
			if err != nil {
				os.Remove(name)
				return AddResult{}, err
			}
		}
		installed = append(installed, destination)
	}
	return a.Add(installed)
}
func (a *App) engineEnvironmentLocked() ([]string, error) {
	host := ""
	if a.library.NativeV8Bridge {
		host = a.nativeV8Host()
		if host == "" {
			return nil, errors.New("找不到 ARM64 V8 模組，請使用含 V8 的 Launcher 測試版")
		}
	}
	environment := engineEnvironment(a.library.CompilerWorkers, a.library.OpenGLBackend, a.library.NativeLibraryBridge, host)
	if a.backgroundDisplay {
		if a.backgroundDisplayID == 0 {
			return nil, errors.New("背景螢幕尚未選定，已停止客體啟動")
		}
		environment = append(environment, "X64ARM_GUI_BACKGROUND_DISPLAY=1", "X64ARM_GUI_BACKGROUND_DISPLAY_ID="+strconv.FormatUint(uint64(a.backgroundDisplayID), 10))
	}
	if font := a.library.WindowsFont; font != "" && font != "auto" {
		environment = append(environment, "X64ARM_WINDOWS_UI_FONT="+font)
	}
	return environment, nil
}
