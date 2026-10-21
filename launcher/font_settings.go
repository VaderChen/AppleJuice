package main

import (
	"errors"
	"slices"
	"strings"
	"sync"
	"unicode"
)

var windowsFontCatalog struct {
	sync.Once
	families []string
}

// 與 Windows LOGFONT 的 31 個 UTF-16 字元名稱上限一致，只提供真實可縮放字型。
func availableWindowsFonts() []string {
	return slices.Clone(windowsFonts())
}

// 內部使用不可變目錄；公開給呼叫端的 slice 仍由 availableWindowsFonts 複製。
func windowsFonts() []string {
	windowsFontCatalog.Do(func() {
		for _, family := range nativeWindowsFonts() {
			if !validWindowsFontName(family) {
				continue
			}
			windowsFontCatalog.families = append(windowsFontCatalog.families, family)
		}
		slices.Sort(windowsFontCatalog.families)
		windowsFontCatalog.families = slices.Compact(windowsFontCatalog.families)
	})
	return windowsFontCatalog.families
}

func validWindowsFontName(family string) bool {
	if family == "" || strings.HasPrefix(family, ".") {
		return false
	}
	units := 0
	for _, character := range family {
		if unicode.IsControl(character) {
			return false
		}
		units++
		if character > 0xffff {
			units++
		}
		if units > 31 {
			return false
		}
	}
	return true
}

func validateWindowsFont(family string) (string, error) {
	if family == "" || family == "auto" {
		return "auto", nil
	}
	for _, installed := range windowsFonts() {
		if strings.EqualFold(family, installed) {
			return installed, nil
		}
	}
	return "", errors.New("請選擇已安裝的向量字型")
}

func normalizeWindowsFont(family string) string {
	value, err := validateWindowsFont(family)
	if err != nil {
		return "auto"
	}
	return value
}
