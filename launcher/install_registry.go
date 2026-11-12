package main

import (
	"encoding/binary"
	"os"
	"strconv"
	"strings"
)

type installedRegistration struct {
	Name, Location, Icon, Uninstall string
	Hidden                          bool
}
type installationRegistry struct {
	Programs map[string]installedRegistration
	Codepage uint32
}

// 只取引擎 RegistryStore 中的安裝資訊與字碼頁；不輸出或搬移其他登錄資料。
// 格式契約來源：src/services/registry_store.cpp，版本 1～4。
func readInstallationRegistry(path string) installationRegistry {
	out := installationRegistry{Programs: map[string]installedRegistration{}, Codepage: 65001}
	f, err := os.Open(path)
	if err != nil {
		return out
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil || info.Size() > 32*1024*1024 {
		return out
	}
	data := make([]byte, info.Size())
	if _, err = f.ReadAt(data, 0); err != nil {
		return out
	}
	if len(data) < 16 || string(data[:8]) != "XARMREG1" {
		return out
	}
	r := windowsData{data: data, at: 8}
	version := r.u32()
	count := r.u32()
	if version < 1 || version > 4 || count > 4096 {
		return out
	}
	text := func() string {
		size := r.u32()
		if size > 32768 {
			r.err = os.ErrInvalid
			return ""
		}
		return windowsUTF16(r.take(int(size) * 2))
	}
	for n := uint32(0); n < count && r.err == nil; n++ {
		key := strings.ToLower(text())
		text()
		r.take(8)
		values := r.u32()
		if values > 4096 {
			return installationRegistry{Programs: map[string]installedRegistration{}, Codepage: 65001}
		}
		entry := installedRegistration{}
		for i := uint32(0); i < values && r.err == nil; i++ {
			name := strings.ToLower(text())
			kind := r.u32()
			size := r.u32()
			value := r.take(int(size))
			if kind == 1 || kind == 2 {
				valueText := strings.TrimRight(windowsUTF16(value), "\x00")
				switch name {
				case "displayname":
					entry.Name = valueText
				case "installlocation":
					entry.Location = valueText
				case "displayicon":
					entry.Icon = valueText
				case "uninstallstring":
					entry.Uninstall = valueText
				case "acp":
					if strings.HasSuffix(key, "\\control\\nls\\codepage") {
						if v, e := strconv.ParseUint(valueText, 10, 32); e == nil {
							out.Codepage = uint32(v)
						}
					}
				}
			}
			if name == "systemcomponent" && kind == 4 && len(value) == 4 {
				entry.Hidden = binary.LittleEndian.Uint32(value) != 0
			}
		}
		if version >= 2 {
			size := r.u32()
			r.take(int(size))
		}
		if version >= 3 {
			text()
			r.take(4)
		}
		if strings.Contains(key, "\\microsoft\\windows\\currentversion\\uninstall\\") && !entry.Hidden {
			out.Programs[key] = entry
		}
	}
	if r.err != nil {
		return installationRegistry{Programs: map[string]installedRegistration{}, Codepage: 65001}
	}
	return out
}
