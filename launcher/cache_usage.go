package main

/*
#include <unistd.h>
#include <stdlib.h>
static char* launcher_user_directory(int cache) {
	const int name = cache ? _CS_DARWIN_USER_CACHE_DIR : _CS_DARWIN_USER_TEMP_DIR;
	size_t size = confstr(name, NULL, 0);
	if (!size) return NULL;
	char* text = malloc(size);
	if (text && !confstr(name, text, size)) { free(text); return NULL; }
	return text;
}
*/
import "C"

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"syscall"
	"unsafe"
)

// 引擎在宿主建立的可重建快取。每個客體程式有自己的系統快取目錄（Metal 著色器快取等，
// 引擎以主程式路徑雜湊命名並留下 x64arm-application.txt）；其餘為全部程式共用。
// 著色器翻譯快取與 EXE／DLL 轉譯快取存放於共享記憶體，不佔磁碟，不在此列。
type CacheEntry struct {
	ID    string `json:"id"`
	Kind  string `json:"kind"` // program、legacyShaders、resources、identity
	Name  string `json:"name,omitempty"`
	Path  string `json:"path,omitempty"`
	Bytes uint64 `json:"bytes"`
}

type CacheUsage struct {
	Entries    []CacheEntry `json:"entries"`
	TotalBytes uint64       `json:"totalBytes"`
	Running    bool         `json:"running"`
}

const (
	applicationCachePrefix   = "x64arm-app-"
	applicationCacheManifest = "x64arm-application.txt"
	legacyShaderCacheName    = "org.macwin.x64arm"
	resourceCacheName        = "x64arm-generated-resources-v1"
	identityCacheName        = "x64arm-identity-cache"
)

var applicationCacheDirectory = regexp.MustCompile(`^x64arm-app-([0-9a-f]{16})$`)

// 測試可替換；回傳 DARWIN_USER_CACHE_DIR 與 DARWIN_USER_TEMP_DIR。
var hostUserDirectories = func() (cache, temp string) {
	read := func(kind C.int) string {
		value := C.launcher_user_directory(kind)
		if value == nil {
			return ""
		}
		defer C.free(unsafe.Pointer(value))
		return C.GoString(value)
	}
	return read(1), read(0)
}

// 與引擎相同的鍵：正規化絕對路徑、ASCII 轉小寫後 SHA-256 的前 8 位元組。
func applicationCacheKey(path string) string {
	text := []byte(filepath.Clean(path))
	for i, c := range text {
		if c >= 'A' && c <= 'Z' {
			text[i] = c + 32
		}
	}
	digest := sha256.Sum256(text)
	return hex.EncodeToString(digest[:8])
}

// 實際配置的磁碟空間；不追蹤符號連結，讀不到的項目略過。
func directoryAllocatedBytes(root string) uint64 {
	var total uint64
	_ = filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		info, err := entry.Info()
		if err != nil || info.Mode()&os.ModeSymlink != 0 {
			return nil
		}
		if stat, ok := info.Sys().(*syscall.Stat_t); ok {
			total += uint64(stat.Blocks) * 512
		} else if info.Mode().IsRegular() {
			total += uint64(info.Size())
		}
		return nil
	})
	return total
}

// 每個快取項目對應的目錄；只由固定名稱與通過格式檢查的鍵組成，不接受呼叫端提供的路徑。
func cacheDirectories(id string) ([]string, error) {
	cache, temp := hostUserDirectories()
	if cache == "" {
		return nil, errors.New("無法取得系統快取目錄")
	}
	switch id {
	case "legacy-shaders":
		return []string{filepath.Join(cache, legacyShaderCacheName)}, nil
	case "resources":
		return []string{filepath.Join(cache, resourceCacheName)}, nil
	case "identity":
		if temp == "" {
			return nil, errors.New("無法取得系統快取目錄")
		}
		return []string{filepath.Join(temp, identityCacheName)}, nil
	}
	if key, ok := strings.CutPrefix(id, "program:"); ok && applicationCacheDirectory.MatchString(applicationCachePrefix+key) {
		directories := []string{filepath.Join(cache, applicationCachePrefix+key)}
		if temp != "" {
			directories = append(directories, filepath.Join(temp, applicationCachePrefix+key))
		}
		return directories, nil
	}
	return nil, errors.New("未知的快取項目")
}

func cacheEntryBytes(id string) uint64 {
	directories, err := cacheDirectories(id)
	if err != nil {
		return 0
	}
	var total uint64
	for _, directory := range directories {
		total += directoryAllocatedBytes(directory)
	}
	return total
}

func (a *App) CacheUsage() CacheUsage {
	a.mu.Lock()
	names := make(map[string]string, len(a.library.Programs))
	for _, program := range a.library.Programs {
		names[applicationCacheKey(program.Path)] = program.Name
	}
	running := len(a.runs) > 0
	a.mu.Unlock()
	usage := CacheUsage{Entries: []CacheEntry{}, Running: running}
	cache, _ := hostUserDirectories()
	if cache == "" {
		return usage
	}
	var programs []CacheEntry
	if entries, err := os.ReadDir(cache); err == nil {
		for _, entry := range entries {
			match := applicationCacheDirectory.FindStringSubmatch(entry.Name())
			if match == nil || !entry.IsDir() {
				continue
			}
			item := CacheEntry{ID: "program:" + match[1], Kind: "program"}
			if data, err := os.ReadFile(filepath.Join(cache, entry.Name(), applicationCacheManifest)); err == nil && len(data) <= 4096 {
				item.Path = strings.TrimSpace(string(data))
			}
			if name := names[match[1]]; name != "" {
				item.Name = name
			} else if item.Path != "" {
				item.Name = filepath.Base(item.Path)
			}
			item.Bytes = cacheEntryBytes(item.ID)
			programs = append(programs, item)
		}
	}
	sort.SliceStable(programs, func(i, j int) bool { return programs[i].Bytes > programs[j].Bytes })
	usage.Entries = append(usage.Entries, programs...)
	for _, shared := range []CacheEntry{{ID: "legacy-shaders", Kind: "legacyShaders"}, {ID: "resources", Kind: "resources"}, {ID: "identity", Kind: "identity"}} {
		shared.Bytes = cacheEntryBytes(shared.ID)
		if shared.Bytes > 0 {
			usage.Entries = append(usage.Entries, shared)
		}
	}
	for _, entry := range usage.Entries {
		usage.TotalBytes += entry.Bytes
	}
	return usage
}

// 清除單一快取項目。執行中的程式仍開著快取檔案，清除會讓它寫回不完整的內容，因此有程式執行時拒絕。
func (a *App) ClearCache(id string) (CacheUsage, error) {
	directories, err := cacheDirectories(id)
	if err != nil {
		return a.CacheUsage(), err
	}
	a.mu.Lock()
	running := len(a.runs) > 0
	a.mu.Unlock()
	if running {
		return a.CacheUsage(), errors.New("有程式執行中，請先停止再清除快取")
	}
	for _, directory := range directories {
		if err := os.RemoveAll(directory); err != nil {
			return a.CacheUsage(), errors.New("無法清除快取：" + err.Error())
		}
	}
	return a.CacheUsage(), nil
}
