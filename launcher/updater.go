package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// 版本更新只接受 GitHub 正式 Release：DMG 須符合 SHA256SUMS 與 GitHub 摘要，
// App 須與目前 App 同一開發者、同一 bundle ID 且通過 Apple 公證，才會替換。
const (
	updateRepository   = "VaderChen/AppleJuice"
	updateDownloadBase = "https://github.com/" + updateRepository + "/releases/download/"
	updateDMGLimit     = 2 << 30
	updateSmallLimit   = 4 << 20
)

var (
	updateTagPattern     = regexp.MustCompile(`^v([0-9]+(?:\.[0-9]+)*)-build-([0-9]+)$`)
	updateVersionPattern = regexp.MustCompile(`^([0-9]+(?:\.[0-9]+)*) build ([0-9]+)$`)
	errUpdateNetwork     = errors.New("無法取得 GitHub 最新版本，請確認網路連線。")
	errUpdateAsset       = errors.New("最新版本沒有可用的 macOS 更新檔。")
	errUpdateDownload    = errors.New("下載的更新檔驗證失敗，已停止更新。")
	errUpdateSignature   = errors.New("更新檔的簽章或公證驗證失敗，已停止更新。")
	errUpdateInstall     = errors.New("安裝更新失敗，已保留目前版本。")
)

type updateStatus struct {
	Phase    string `json:"phase"`
	Current  string `json:"current"`
	Latest   string `json:"latest,omitempty"`
	Received int64  `json:"received,omitempty"`
	Total    int64  `json:"total,omitempty"`
	Error    string `json:"error,omitempty"`
}

type releaseVersion struct {
	number string
	parts  []int
	build  int
}

type githubRelease struct {
	TagName string `json:"tag_name"`
	Draft   bool   `json:"draft"`
	Pre     bool   `json:"prerelease"`
	Assets  []struct {
		Name   string `json:"name"`
		Size   int64  `json:"size"`
		Digest string `json:"digest"`
		URL    string `json:"browser_download_url"`
	} `json:"assets"`
}

type updater struct {
	mu      sync.Mutex
	status  updateStatus
	current string
	// 開發測試用：把目前版本視為指定值，並允許替換未公證的 App。
	testing bool
	running func() int
	restart func(target string) error
	client  *http.Client
}

func newUpdater(current string, testing bool, running func() int, restart func(string) error) *updater {
	client := &http.Client{Timeout: 30 * time.Minute, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if req.URL.Scheme != "https" || len(via) > 5 {
			return errors.New("更新下載轉址不安全")
		}
		return nil
	}}
	return &updater{status: updateStatus{Phase: "idle", Current: current}, current: current, testing: testing, running: running, restart: restart, client: client}
}

func (u *updater) Status() updateStatus {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.status
}

func updateBusy(phase string) bool {
	switch phase {
	case "checking", "downloading", "verifying", "installing", "restarting":
		return true
	}
	return false
}

// Start 在背景偵測最新版；install 為真時接著下載、驗證與安裝。
// 進行中重複呼叫只回傳目前進度。
func (u *updater) Start(install bool) updateStatus {
	u.mu.Lock()
	defer u.mu.Unlock()
	if !updateBusy(u.status.Phase) {
		u.status = updateStatus{Phase: "checking", Current: u.current}
		go u.run(install)
	}
	return u.status
}

func (u *updater) set(change func(*updateStatus)) {
	u.mu.Lock()
	defer u.mu.Unlock()
	change(&u.status)
}

func (u *updater) fail(phase string, err error) {
	u.set(func(s *updateStatus) { s.Phase, s.Error = phase, err.Error() })
}

func (u *updater) run(install bool) {
	current, ok := parseReleaseVersion(updateVersionPattern, u.current)
	if !ok {
		u.fail("unavailable", errors.New("目前版本不是正式版本號，無法比較更新。"))
		return
	}
	release, latest, err := u.latestRelease()
	if err != nil {
		fmt.Fprintln(os.Stderr, "更新檢查失敗：", err)
		u.fail("error", errUpdateNetwork)
		return
	}
	label := latest.String()
	u.set(func(s *updateStatus) { s.Latest = label })
	if compareReleaseVersions(latest, current) <= 0 {
		u.set(func(s *updateStatus) { s.Phase = "latest" })
		return
	}
	bundle, err := currentAppBundle()
	if err == nil && !u.testing && exec.Command("/usr/sbin/spctl", "--assess", "--type", "execute", bundle).Run() != nil {
		err = errors.New("開發建置不支援自動更新，請以 run.command 重新建置。")
	}
	if err == nil && syscall.Access(filepath.Dir(bundle), 2) != nil {
		err = errors.New("沒有權限寫入 App 所在資料夾，請改為手動下載更新。")
	}
	if err == nil && !install {
		u.set(func(s *updateStatus) { s.Phase = "available" })
		return
	}
	if err == nil && u.running() > 0 {
		err = errors.New("請先結束執行中的程式，再更新。")
	}
	if err != nil {
		u.fail("unavailable", err)
		return
	}
	work, err := os.MkdirTemp("", "applejuice-update-")
	if err != nil {
		u.fail("error", errUpdateInstall)
		return
	}
	defer os.RemoveAll(work)
	dmg, err := u.download(release, latest, work)
	if err != nil {
		fmt.Fprintln(os.Stderr, "更新下載失敗：", err)
		u.fail("error", errUpdateDownload)
		return
	}
	u.set(func(s *updateStatus) { s.Phase = "verifying" })
	staged, err := stageUpdate(dmg, work, bundle, latest)
	if err != nil {
		fmt.Fprintln(os.Stderr, "更新驗證失敗：", err)
		if staged != "" {
			os.RemoveAll(staged)
		}
		u.fail("error", errUpdateSignature)
		return
	}
	u.set(func(s *updateStatus) { s.Phase = "installing" })
	target, err := installUpdate(bundle, staged)
	if err != nil {
		fmt.Fprintln(os.Stderr, "更新安裝失敗：", err)
		os.RemoveAll(staged)
		u.fail("error", errUpdateInstall)
		return
	}
	if err := u.restart(target); err != nil {
		fmt.Fprintln(os.Stderr, "無法重新啟動新版：", err)
		u.fail("error", errors.New("已安裝新版，請手動重新開啟 AppleJuice。"))
		return
	}
	u.set(func(s *updateStatus) { s.Phase = "restarting" })
}

func (u *updater) get(url string, limit int64, accept string) ([]byte, error) {
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", accept)
	req.Header.Set("User-Agent", "AppleJuice/"+u.current)
	resp, err := u.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("%s 回應 %s", url, resp.Status)
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err == nil && int64(len(data)) > limit {
		err = errors.New("回應超過大小上限")
	}
	return data, err
}

func (u *updater) latestRelease() (githubRelease, releaseVersion, error) {
	var release githubRelease
	data, err := u.get("https://api.github.com/repos/"+updateRepository+"/releases/latest", updateSmallLimit, "application/vnd.github+json")
	if err == nil {
		err = json.Unmarshal(data, &release)
	}
	if err != nil {
		return release, releaseVersion{}, err
	}
	version, ok := parseReleaseVersion(updateTagPattern, release.TagName)
	if !ok || release.Draft || release.Pre {
		return release, version, fmt.Errorf("最新 Release 標籤不正確：%q", release.TagName)
	}
	return release, version, nil
}

func (u *updater) download(release githubRelease, version releaseVersion, work string) (string, error) {
	name := fmt.Sprintf("AppleJuice-%s-build-%d-macos-arm64.dmg", version.Number(), version.build)
	var dmgURL, sumsURL, digest string
	var size int64
	for _, asset := range release.Assets {
		if !strings.HasPrefix(asset.URL, updateDownloadBase+release.TagName+"/") {
			continue
		}
		switch asset.Name {
		case name:
			dmgURL, size, digest = asset.URL, asset.Size, asset.Digest
		case "SHA256SUMS":
			sumsURL = asset.URL
		}
	}
	if dmgURL == "" || sumsURL == "" || size <= 0 || size > updateDMGLimit {
		return "", errUpdateAsset
	}
	sums, err := u.get(sumsURL, updateSmallLimit, "application/octet-stream")
	if err != nil {
		return "", err
	}
	expected := ""
	for _, line := range strings.Split(string(sums), "\n") {
		if fields := strings.Fields(line); len(fields) == 2 && strings.TrimPrefix(fields[1], "*") == name {
			expected = strings.ToLower(fields[0])
		}
	}
	if len(expected) != 64 || (digest != "" && !strings.EqualFold(digest, "sha256:"+expected)) {
		return "", errors.New("SHA256SUMS 與 GitHub 摘要不一致")
	}
	u.set(func(s *updateStatus) { s.Phase, s.Total, s.Received = "downloading", size, 0 })
	req, err := http.NewRequest(http.MethodGet, dmgURL, nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("User-Agent", "AppleJuice/"+u.current)
	resp, err := u.client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("下載回應 %s", resp.Status)
	}
	path := filepath.Join(work, name)
	file, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return "", err
	}
	hash := sha256.New()
	written, err := io.Copy(io.MultiWriter(file, hash, progressWriter{u}), io.LimitReader(resp.Body, size+1))
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return "", err
	}
	if written != size || hex.EncodeToString(hash.Sum(nil)) != expected {
		return "", errors.New("DMG 大小或 SHA-256 不符")
	}
	return path, nil
}

type progressWriter struct{ u *updater }

func (p progressWriter) Write(data []byte) (int, error) {
	p.u.set(func(s *updateStatus) { s.Received += int64(len(data)) })
	return len(data), nil
}

// stageUpdate 掛載 DMG，驗證 App 後複製到目前 App 所在資料夾的暫存名稱。
func stageUpdate(dmg, work, bundle string, version releaseVersion) (string, error) {
	mount := filepath.Join(work, "volume")
	if out, err := exec.Command("/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount, dmg).CombinedOutput(); err != nil {
		return "", fmt.Errorf("掛載 DMG 失敗：%s", strings.TrimSpace(string(out)))
	}
	defer exec.Command("/usr/bin/hdiutil", "detach", "-force", mount).Run()
	entries, err := os.ReadDir(mount)
	if err != nil {
		return "", err
	}
	var apps []string
	for _, entry := range entries {
		if entry.IsDir() && strings.HasSuffix(entry.Name(), ".app") {
			apps = append(apps, entry.Name())
		}
	}
	if len(apps) != 1 {
		return "", fmt.Errorf("DMG 內應只有一個 App，實際 %d 個", len(apps))
	}
	source := filepath.Join(mount, apps[0])
	if err := verifyUpdateApp(source, bundle, version); err != nil {
		return "", err
	}
	staged := filepath.Join(filepath.Dir(bundle), fmt.Sprintf(".%s.update-%d", apps[0], os.Getpid()))
	if out, err := exec.Command("/usr/bin/ditto", source, staged).CombinedOutput(); err != nil {
		return staged, fmt.Errorf("複製 App 失敗：%s", strings.TrimSpace(string(out)))
	}
	if out, err := exec.Command("/usr/bin/codesign", "--verify", "--deep", "--strict", staged).CombinedOutput(); err != nil {
		return staged, fmt.Errorf("複製後簽章不符：%s", strings.TrimSpace(string(out)))
	}
	return staged, nil
}

func verifyUpdateApp(app, bundle string, version releaseVersion) error {
	if out, err := exec.Command("/usr/bin/codesign", "--verify", "--deep", "--strict", app).CombinedOutput(); err != nil {
		return fmt.Errorf("簽章驗證失敗：%s", strings.TrimSpace(string(out)))
	}
	if out, err := exec.Command("/usr/sbin/spctl", "--assess", "--type", "execute", app).CombinedOutput(); err != nil {
		return fmt.Errorf("未通過 Apple 公證：%s", strings.TrimSpace(string(out)))
	}
	team, current := signingTeam(app), signingTeam(bundle)
	if team == "" || team != current {
		return fmt.Errorf("開發者不符：%q／%q", team, current)
	}
	identifier, currentIdentifier := plistValue(app, "CFBundleIdentifier"), plistValue(bundle, "CFBundleIdentifier")
	if identifier == "" || identifier != currentIdentifier {
		return fmt.Errorf("bundle ID 不符：%q／%q", identifier, currentIdentifier)
	}
	if got := plistValue(app, "CFBundleShortVersionString") + " build " + plistValue(app, "CFBundleVersion"); got != version.String() {
		return fmt.Errorf("App 版本 %q 與 Release %q 不符", got, version.String())
	}
	return nil
}

func signingTeam(app string) string {
	out, _ := exec.Command("/usr/bin/codesign", "-dv", app).CombinedOutput()
	for _, line := range strings.Split(string(out), "\n") {
		if value, ok := strings.CutPrefix(line, "TeamIdentifier="); ok && value != "not set" {
			return value
		}
	}
	return ""
}

func plistValue(app, key string) string {
	out, err := exec.Command("/usr/bin/plutil", "-extract", key, "raw", "-o", "-", filepath.Join(app, "Contents", "Info.plist")).Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

// installUpdate 以同一資料夾內的改名完成替換；舊 App 移到垃圾桶，失敗時移回原位。
func installUpdate(bundle, staged string) (string, error) {
	name := strings.TrimPrefix(filepath.Base(staged), ".")
	name = name[:strings.LastIndex(name, ".update-")]
	target := filepath.Join(filepath.Dir(bundle), name)
	if target != bundle {
		// 發布名稱改變時（例如 Win64 Launcher.app → AppleJuice.app），同名舊副本一併移到垃圾桶。
		if _, err := os.Lstat(target); err == nil {
			if _, err := nativeTrash(target); err != nil {
				return "", err
			}
		}
		if err := os.Rename(staged, target); err != nil {
			return "", err
		}
		if _, err := nativeTrash(bundle); err != nil {
			fmt.Fprintln(os.Stderr, "舊版 App 未能移到垃圾桶：", bundle, err)
		}
		return target, nil
	}
	trashed, err := nativeTrash(bundle)
	if err != nil {
		return "", err
	}
	if err := os.Rename(staged, target); err != nil {
		os.Rename(trashed, bundle)
		return "", err
	}
	return target, nil
}

// currentAppBundle 由執行檔位置取得 .app，供替換與驗證開發者身分。
func currentAppBundle() (string, error) {
	executable, err := os.Executable()
	if err == nil {
		executable, err = filepath.EvalSymlinks(executable)
	}
	if err != nil {
		return "", err
	}
	bundle := filepath.Dir(filepath.Dir(filepath.Dir(executable)))
	if filepath.Ext(bundle) != ".app" || filepath.Base(filepath.Dir(executable)) != "MacOS" {
		return "", errors.New("目前不是從 App 執行，無法自動更新。")
	}
	return bundle, nil
}

func parseReleaseVersion(pattern *regexp.Regexp, text string) (releaseVersion, bool) {
	match := pattern.FindStringSubmatch(text)
	if match == nil {
		return releaseVersion{}, false
	}
	version := releaseVersion{number: match[1]}
	for _, part := range strings.Split(match[1], ".") {
		value, err := strconv.Atoi(part)
		if err != nil {
			return releaseVersion{}, false
		}
		version.parts = append(version.parts, value)
	}
	build, err := strconv.Atoi(match[2])
	if err != nil {
		return releaseVersion{}, false
	}
	version.build = build
	return version, true
}

// 版本號含日期（0.26.MMDD），同日再比 build；逐段數值比較。
func compareReleaseVersions(a, b releaseVersion) int {
	for i := 0; i < len(a.parts) || i < len(b.parts); i++ {
		var x, y int
		if i < len(a.parts) {
			x = a.parts[i]
		}
		if i < len(b.parts) {
			y = b.parts[i]
		}
		if x != y {
			if x < y {
				return -1
			}
			return 1
		}
	}
	switch {
	case a.build < b.build:
		return -1
	case a.build > b.build:
		return 1
	}
	return 0
}

// Number 保留原字串（含日期補零），用於比對發布檔名。
func (v releaseVersion) Number() string { return v.number }

func (v releaseVersion) String() string { return v.Number() + " build " + strconv.Itoa(v.build) }

// relaunchArguments 沿用原本的啟動參數，但不把開發測試與 Smoke 旗標帶給新版。
func relaunchArguments(args []string) []string {
	kept := []string{}
	for i := 0; i < len(args); i++ {
		name := strings.TrimLeft(args[i], "-")
		if name == "update-test-version" || name == "smoke" {
			i++
			continue
		}
		if strings.HasPrefix(name, "update-test-version=") || strings.HasPrefix(name, "smoke=") {
			continue
		}
		kept = append(kept, args[i])
	}
	return kept
}

// relaunchAfterExit 等目前程序結束後開啟新版，沿用原本的啟動參數。
func relaunchAfterExit(target string, args []string) error {
	script := `while /bin/kill -0 "$0" 2>/dev/null; do /bin/sleep 0.2; done; exec /usr/bin/open -n "$1" --args "${@:2}"`
	command := exec.Command("/bin/bash", append([]string{"-c", script, strconv.Itoa(os.Getpid()), target}, args...)...)
	command.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	return command.Start()
}
