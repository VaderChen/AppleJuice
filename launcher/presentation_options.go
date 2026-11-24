package main

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"path/filepath"
)

// 建立客體子程序時重新讀取，避免長駐啟動器把舊環境傳給之後開啟的程式。
// 內容只含播放偏好與程式身分；個別 EXE 的設定優先，不依程式名稱判斷。
func (a *App) presentationOptionsPath() string {
	return filepath.Join(a.dataDir, "child-presentation.bin")
}

func (a *App) savePresentationOptionsLocked(library Library) error {
	if len(library.Programs) > 4096 {
		return errors.New("程式顯示設定數量超過上限")
	}
	var data bytes.Buffer
	// v2：每筆第 4 位元組為旗標（位元 0 畫面內插、位元 1 效能統計）。
	header := []byte{'A', 'J', 'P', 'O', 2, 0, 0, 0}
	if library.SpatialHeadphones {
		header[5] = 1
	}
	data.Write(header)
	_ = binary.Write(&data, binary.LittleEndian, uint32(len(library.Programs)))
	for _, program := range library.Programs {
		id, err := hex.DecodeString(program.ID)
		if err != nil || len(id) != 16 || len(program.Path) == 0 || len(program.Path) > 32768 {
			return errors.New("程式顯示設定的身分或路徑不正確")
		}
		_ = binary.Write(&data, binary.LittleEndian, uint16(len(program.Path)))
		upscale, flags := byte(1), byte(0)
		if program.PresentUpscale {
			upscale = 2
		}
		if program.PresentInterpolation {
			flags |= 1
		}
		if program.PerformanceHUD {
			flags |= 2
		}
		data.WriteByte(upscale)
		data.WriteByte(flags)
		data.Write(id)
		data.WriteString(program.Path)
	}
	if data.Len() > 4*1024*1024 {
		return errors.New("程式顯示設定大小超過上限")
	}
	return writeAtomicFile(a.presentationOptionsPath(), data.Bytes())
}
