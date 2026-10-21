package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"crypto/x509/pkix"
	_ "embed"
	"encoding/pem"
	"errors"
	"fmt"
	"math/big"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

// 公開根來自固定版本的 Mozilla CA 套件；不讀取宿主憑證或私鑰。
//
//go:embed assets/cacert.pem
var publicCertificateRoots []byte

const publicCertificateRootsSHA256 = "a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505"
const defaultCertificateRootsPath = "ProgramData/MacWin/Certificates/ROOT.pem"

var certificateRootsMu sync.Mutex

// 本地根公開憑證於缺檔時才產生；時間只用於有效期，金鑰／序號使用安全亂數。
// 這項工作只保存公開憑證，Windows 私鑰仍由既有 Sandbox 金鑰服務管理。
func newSandboxRootCertificate() ([]byte, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return nil, err
	}
	serial.Add(serial, big.NewInt(1))
	now := time.Now().UTC().Truncate(time.Second)
	root := &x509.Certificate{
		SerialNumber: serial,
		Subject:      pkix.Name{CommonName: "MacWin Sandbox Root " + serial.Text(16), Organization: []string{"MacWin Sandbox"}},
		NotBefore:    now.Add(-time.Minute), NotAfter: now.AddDate(10, 0, 0),
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageCRLSign,
		BasicConstraintsValid: true, IsCA: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, root, root, &key.PublicKey, key)
	if err != nil {
		return nil, err
	}
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), nil
}

func certificateRootsRelativePath(value string) (string, error) {
	if value == "" {
		return defaultCertificateRootsPath, nil
	}
	value = strings.ReplaceAll(value, `\`, "/")
	if strings.HasPrefix(value, "/") {
		return "", errors.New("根憑證檔案必須使用相對於 Sandbox C 槽的路徑")
	}
	for _, part := range strings.Split(value, "/") {
		if part == "" || part == "." || part == ".." || strings.HasSuffix(part, ".") || strings.HasSuffix(part, " ") {
			return "", errors.New("根憑證相對路徑包含無效目錄")
		}
		for _, c := range part {
			if c < 32 || strings.ContainsRune(`:*?"<>|`, c) {
				return "", errors.New("根憑證相對路徑包含無效字元")
			}
		}
	}
	return value, nil
}

// 映射磁碟即使區分大小寫，也沿用 Windows 的既有路徑名稱。
func certificatePathComponent(directory, part string) (string, error) {
	target := filepath.Join(directory, part)
	if _, err := os.Lstat(target); err == nil {
		return target, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	entries, err := os.ReadDir(directory)
	if err != nil {
		return "", err
	}
	matched := ""
	for _, entry := range entries {
		if strings.EqualFold(entry.Name(), part) {
			if matched != "" {
				return "", errors.New("Sandbox 憑證路徑存在重複大小寫名稱")
			}
			matched = entry.Name()
		}
	}
	if matched != "" {
		return filepath.Join(directory, matched), nil
	}
	return target, nil
}

func validatePublicCertificateRoots(data []byte) error {
	if fmt.Sprintf("%x", sha256.Sum256(data)) != publicCertificateRootsSHA256 {
		return errors.New("公開根憑證套件雜湊不符")
	}
	count := 0
	for rest := data; len(rest) > 0; {
		block, tail := pem.Decode(rest)
		if block == nil {
			break
		}
		if block.Type != "CERTIFICATE" {
			return errors.New("公開根憑證套件包含非憑證資料")
		}
		cert, err := x509.ParseCertificate(block.Bytes)
		if err != nil || !cert.IsCA {
			return errors.New("公開根憑證套件包含無效 CA")
		}
		count++
		rest = tail
	}
	if count != 121 {
		return fmt.Errorf("公開根憑證數量不符：%d", count)
	}
	return nil
}

func ensureSandboxCertificateRoots(cDrive string) error {
	return ensureSandboxCertificateRootsWithPublisher(cDrive, publishFileExclusive)
}

func ensureSandboxCertificateRootsWithPublisher(cDrive string, publish func(string, string) error) error {
	return ensureSandboxCertificateRootsAtWithPublisher(cDrive, defaultCertificateRootsPath, publish)
}

func ensureSandboxCertificateRootsAt(cDrive, relativePath string) error {
	return ensureSandboxCertificateRootsAtWithPublisher(cDrive, relativePath, publishFileExclusive)
}

func ensureSandboxCertificateRootsAtWithPublisher(cDrive, relativePath string, publish func(string, string) error) error {
	relativePath, err := certificateRootsRelativePath(relativePath)
	if err != nil {
		return err
	}
	certificateRootsMu.Lock()
	defer certificateRootsMu.Unlock()
	root, err := filepath.EvalSymlinks(cDrive)
	if err != nil {
		return err
	}
	parts := strings.Split(relativePath, "/")
	for _, part := range parts[:len(parts)-1] {
		root, err = certificatePathComponent(root, part)
		if err != nil {
			return err
		}
		if err := os.Mkdir(root, 0700); err != nil && !errors.Is(err, os.ErrExist) {
			return err
		}
		info, err := os.Lstat(root)
		if err != nil {
			return err
		}
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("Sandbox 憑證目錄不是實體目錄：%s", root)
		}
	}
	// 既有檔案只需讀取；即使目錄唯讀或鎖檔不可用，也不進入生成流程。
	var target string
	existing := func() (bool, error) {
		target, err = certificatePathComponent(root, parts[len(parts)-1])
		if err != nil {
			return false, err
		}
		info, statErr := os.Lstat(target)
		if errors.Is(statErr, os.ErrNotExist) {
			return false, nil
		}
		if statErr != nil {
			return false, statErr
		}
		if !info.Mode().IsRegular() {
			return false, errors.New("Sandbox ROOT.pem 不是一般檔案")
		}
		return true, nil
	}
	if found, err := existing(); err != nil || found {
		return err
	}
	// 只有缺檔才建立鎖；一般 exclusive create 可用於不支援特殊 rename 的檔案系統。
	lock, err := os.OpenFile(filepath.Join(root, ".public-roots.lock"), os.O_CREATE|os.O_RDWR|syscall.O_NOFOLLOW, 0600)
	if err != nil {
		return err
	}
	defer lock.Close()
	if err = syscall.Flock(int(lock.Fd()), syscall.LOCK_EX); err != nil {
		return err
	}
	defer syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
	// 等待其他程序釋放鎖後重新查找，保留其建立的 Windows 大小寫名稱。
	if found, err := existing(); err != nil || found {
		return err
	}
	generated, err := newSandboxRootCertificate()
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(root, ".public-roots-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	defer f.Close()
	if _, err = f.Write(generated); err != nil {
		return err
	}
	if err = f.Chmod(0644); err != nil {
		return err
	}
	if err = f.Sync(); err != nil {
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	// 公開憑證維持一般未加密 PEM；不覆寫既有使用者來源。
	if err = publish(f.Name(), target); errors.Is(err, os.ErrExist) {
		info, statErr := os.Lstat(target)
		if statErr != nil {
			return statErr
		}
		if !info.Mode().IsRegular() {
			return errors.New("Sandbox ROOT.pem 不是一般檔案")
		}
		return nil
	}
	return err
}
