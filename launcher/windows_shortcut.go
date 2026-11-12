package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

// MS-SHLLINK 的所有長度、偏移皆先檢查；不執行捷徑或載入其 COM 物件。
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-shllink/
type windowsShortcut struct {
	Targets            []string
	Arguments, WorkDir string
}

type windowsData struct {
	data []byte
	at   int
	err  error
}

func (r *windowsData) take(n int) []byte {
	if r.err != nil || n < 0 || n > len(r.data)-r.at {
		r.err = errors.New("Windows 資料截斷")
		return nil
	}
	b := r.data[r.at : r.at+n]
	r.at += n
	return b
}
func (r *windowsData) u16() uint16 {
	b := r.take(2)
	if b == nil {
		return 0
	}
	return binary.LittleEndian.Uint16(b)
}
func (r *windowsData) u32() uint32 {
	b := r.take(4)
	if b == nil {
		return 0
	}
	return binary.LittleEndian.Uint32(b)
}
func windowsUTF16(data []byte) string {
	values := make([]uint16, len(data)/2)
	for i := range values {
		values[i] = binary.LittleEndian.Uint16(data[i*2:])
	}
	return string(utf16.Decode(values))
}
func windowsANSI(data []byte, codepage uint32) string {
	if codepage == 0 || codepage == 65001 {
		if utf8.Valid(data) {
			return string(data)
		}
		codepage = 1252
	}
	return nativeWindowsText(data, codepage)
}
func windowsZ(data []byte, offset uint32, wide bool, codepage uint32) string {
	if offset == 0 || uint64(offset) >= uint64(len(data)) {
		return ""
	}
	b := data[offset:]
	if wide {
		for n := 0; n+1 < len(b); n += 2 {
			if b[n] == 0 && b[n+1] == 0 {
				return windowsUTF16(b[:n])
			}
		}
		return ""
	}
	if end := bytes.IndexByte(b, 0); end >= 0 {
		return windowsANSI(b[:end], codepage)
	}
	return ""
}
func windowsOffset(data []byte, at int) uint32 {
	if at < 0 || at+4 > len(data) {
		return 0
	}
	return binary.LittleEndian.Uint32(data[at:])
}
func joinWindowsPath(base, suffix string) string {
	base = strings.TrimRight(strings.ReplaceAll(base, "/", "\\"), "\\")
	suffix = strings.Trim(strings.ReplaceAll(suffix, "/", "\\"), "\\")
	if base == "" {
		return suffix
	}
	if suffix == "" {
		return base
	}
	a, b := strings.Split(base, "\\"), strings.Split(suffix, "\\")
	for n := min(len(a), len(b)); n > 0; n-- {
		if strings.EqualFold(strings.Join(a[len(a)-n:], "\\"), strings.Join(b[:n], "\\")) {
			return strings.Join(append(a, b[n:]...), "\\")
		}
	}
	return base + "\\" + suffix
}
func parseWindowsShortcut(data []byte, codepage uint32) (windowsShortcut, error) {
	var out windowsShortcut
	clsid := []byte{1, 0x14, 2, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46}
	if len(data) < 76 || windowsOffset(data, 0) != 76 || !bytes.Equal(data[4:20], clsid) {
		return out, errors.New("不是 Windows 捷徑")
	}
	flags := windowsOffset(data, 20)
	r := windowsData{data: data, at: 76}
	if flags&1 != 0 {
		size := r.u16()
		r.take(int(size))
	}
	if flags&2 != 0 {
		start := r.at
		size := r.u32()
		if size < 28 || uint64(size) > uint64(len(data)-start) {
			return out, errors.New("捷徑 LinkInfo 範圍不正確")
		}
		info := data[start : start+int(size)]
		r.at = start + int(size)
		header := windowsOffset(info, 4)
		if header < 28 || header > size {
			return out, errors.New("捷徑 LinkInfo 標頭不正確")
		}
		base, suffix := "", ""
		if header >= 36 {
			base = windowsZ(info, windowsOffset(info, 28), true, codepage)
			suffix = windowsZ(info, windowsOffset(info, 32), true, codepage)
		}
		if base == "" {
			base = windowsZ(info, windowsOffset(info, 16), false, codepage)
		}
		if suffix == "" {
			suffix = windowsZ(info, windowsOffset(info, 24), false, codepage)
		}
		if base == "" && windowsOffset(info, 8)&2 != 0 {
			offset := windowsOffset(info, 20)
			if offset >= header && uint64(offset)+20 <= uint64(len(info)) {
				network := info[offset:]
				n := windowsOffset(network, 0)
				if n >= 20 && n <= uint32(len(network)) {
					network = network[:n]
					if windowsOffset(network, 8) > 20 {
						base = windowsZ(network, windowsOffset(network, 24), true, codepage)
						if base == "" {
							base = windowsZ(network, windowsOffset(network, 20), true, codepage)
						}
					}
					if base == "" {
						base = windowsZ(network, windowsOffset(network, 12), false, codepage)
					}
					if base == "" {
						base = windowsZ(network, windowsOffset(network, 8), false, codepage)
					}
				}
			}
		}
		if base != "" && flags&0x100 == 0 {
			out.Targets = append(out.Targets, joinWindowsPath(base, suffix))
		}
	}
	text := func() string {
		count := int(r.u16())
		wide := flags&0x80 != 0
		if wide {
			return strings.TrimRight(windowsUTF16(r.take(count*2)), "\x00")
		}
		return strings.TrimRight(windowsANSI(r.take(count), codepage), "\x00")
	}
	if flags&4 != 0 {
		text()
	}
	if flags&8 != 0 {
		out.Targets = append(out.Targets, text())
	}
	if flags&16 != 0 {
		out.WorkDir = text()
	}
	if flags&32 != 0 {
		out.Arguments = text()
	}
	if flags&64 != 0 {
		text()
	}
	for r.err == nil && r.at+4 <= len(data) {
		start := r.at
		size := r.u32()
		if size == 0 {
			break
		}
		if size < 8 || uint64(size) > uint64(len(data)-start) {
			r.err = errors.New("捷徑附加區塊範圍不正確")
			break
		}
		block := data[start : start+int(size)]
		r.at = start + int(size)
		if windowsOffset(block, 4) == 0xa0000001 && size >= 0x314 {
			value := windowsZ(block, 268, true, codepage)
			if value == "" {
				value = windowsZ(block, 8, false, codepage)
			}
			if flags&0x02000000 != 0 {
				out.Targets = append([]string{value}, out.Targets...)
			} else {
				out.Targets = append(out.Targets, value)
			}
		}
	}
	if r.err != nil {
		return windowsShortcut{}, r.err
	}
	if len(out.Targets) == 0 {
		return out, errors.New("捷徑沒有可解析的檔案路徑")
	}
	return out, nil
}
