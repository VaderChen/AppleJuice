package main

/*
#cgo CFLAGS: -x objective-c -fobjc-arc
#cgo LDFLAGS: -framework Cocoa -framework WebKit -framework UniformTypeIdentifiers -framework CoreText
#include <stdlib.h>
#include <stdint.h>
uint64_t launcher_physical_memory(void);
uint32_t launcher_physical_processors(void);
char* launcher_windows_fonts(void);
char* launcher_pick(int folder, int probe, const char* title, int single);
void launcher_language(const char* language);
void launcher_configure(void* window);
void* launcher_background_window(void);
uint32_t launcher_background_display_id(void);
void launcher_close_background_window(void);
void launcher_running(int count);
void launcher_snapshot(void* window, const char* path);
int launcher_visible(int pid);
void launcher_alert(const char* text);
void launcher_reveal(const char* path);
int launcher_pty_utf8(int fd);
*/
import "C"
import (
	"encoding/json"
	"errors"
	"unsafe"
)

func nativePhysicalMemory() uint64     { return uint64(C.launcher_physical_memory()) }
func nativePhysicalProcessors() uint32 { return uint32(C.launcher_physical_processors()) }

func nativeWindowsFonts() []string {
	value := C.launcher_windows_fonts()
	if value == nil {
		return nil
	}
	defer C.free(unsafe.Pointer(value))
	var families []string
	if json.Unmarshal([]byte(C.GoString(value)), &families) != nil {
		return nil
	}
	return families
}

func nativePick(folder, probe bool, title string, single ...bool) ([]string, error) {
	f, p, one := C.int(0), C.int(0), C.int(0)
	if folder {
		f = 1
	}
	if probe {
		p = 1
	}
	if len(single) > 0 && single[0] {
		one = 1
	}
	label := C.CString(title)
	defer C.free(unsafe.Pointer(label))
	value := C.launcher_pick(f, p, label, one)
	if value == nil {
		return nil, errors.New("無法開啟檔案選擇器")
	}
	defer C.free(unsafe.Pointer(value))
	paths := []string{}
	err := json.Unmarshal([]byte(C.GoString(value)), &paths)
	return paths, err
}
func nativeConfigure(window unsafe.Pointer) { C.launcher_configure(window) }
func nativeBackgroundWindow() (unsafe.Pointer, error) {
	window := C.launcher_background_window()
	if window == nil {
		return nil, errors.New("找不到非焦點螢幕，已停止背景 UI 啟動")
	}
	return window, nil
}
func nativeCloseBackgroundWindow()      { C.launcher_close_background_window() }
func nativeBackgroundDisplayID() uint32 { return uint32(C.launcher_background_display_id()) }
func nativeRunning(count int)           { C.launcher_running(C.int(count)) }
func nativeSnapshot(window unsafe.Pointer, path string) {
	text := C.CString(path)
	defer C.free(unsafe.Pointer(text))
	C.launcher_snapshot(window, text)
}
func nativeVisible(pid int) bool { return C.launcher_visible(C.int(pid)) != 0 }
func nativeAlert(message string) {
	value := C.CString(message)
	defer C.free(unsafe.Pointer(value))
	C.launcher_alert(value)
}
func nativeReveal(path string) {
	value := C.CString(path)
	defer C.free(unsafe.Pointer(value))
	C.launcher_reveal(value)
}
func nativePTY(fd uintptr) error {
	if C.launcher_pty_utf8(C.int(fd)) != 0 {
		return errors.New("無法設定終端 UTF-8 模式")
	}
	return nil
}

func nativeLanguage(language string) {
	label := C.CString(language)
	defer C.free(unsafe.Pointer(label))
	C.launcher_language(label)
}
