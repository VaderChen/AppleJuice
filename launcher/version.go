package main

import (
	"os"
	"time"
)

// 正式打包透過 Go linker 寫入同一份版本；直接 go build 時依產物時間顯示。
var launcherVersion string
var developmentVersion = func() string {
	executable, err := os.Executable()
	if err != nil {
		return "開發版本"
	}
	info, err := os.Stat(executable)
	if err != nil {
		return "開發版本"
	}
	return info.ModTime().In(time.FixedZone("Asia/Taipei", 8*60*60)).Format("0.06.0102 build 1504")
}()

func applicationVersion() string {
	if launcherVersion != "" {
		return launcherVersion
	}
	return developmentVersion
}
