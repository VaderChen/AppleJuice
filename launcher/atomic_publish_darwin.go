package main

import (
	"io"
	"os"
)

// 呼叫端持有目錄內的 flock；O_EXCL 保護其他程序建立的既有檔案。
// 只需一般檔案建立／寫入，不依賴硬連結或 rename-exclusive。
func publishFileExclusive(source, target string) (err error) {
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	output, err := os.OpenFile(target, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
	if err != nil {
		return err
	}
	created, err := output.Stat()
	if err != nil {
		output.Close()
		return err
	}
	defer func() {
		if closeErr := output.Close(); err == nil {
			err = closeErr
		}
		if err != nil {
			if current, statErr := os.Lstat(target); statErr == nil && os.SameFile(created, current) {
				_ = os.Remove(target)
			}
		}
	}()
	if _, err = io.Copy(output, input); err != nil {
		return err
	}
	return output.Sync()
}
