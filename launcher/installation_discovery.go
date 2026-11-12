package main

import (
	"crypto/sha256"
	"debug/pe"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

type programAddition struct {
	Path, Name string
	Defaults   *Settings
}
type installationFile struct {
	Size, Modified int64
	Digest         [32]byte
}
type installationSnapshot struct {
	Program                Program
	Drives                 map[string]string
	Executables, Shortcuts map[string]installationFile
	Registry               installationRegistry
	LikelyInstaller        bool
	Roots                  []string
}

// 最長映射優先；C:\Program Files 的自訂覆疊與引擎 ReadOnlyFiles 共用同一契約。
func (s *installationSnapshot) hostPath(value, relative string) string {
	if strings.TrimSpace(value) == "" {
		return ""
	}
	vars := map[string]string{
		"programfiles": `C:\Program Files`, "programw6432": `C:\Program Files`, "programfiles(x86)": `C:\Program Files (x86)`,
		"systemdrive": `C:`, "username": "User", "homedrive": `C:`, "homepath": `\Users\User`, "systemroot": `C:\Windows`, "windir": `C:\Windows`,
		"userprofile": `C:\Users\User`, "appdata": `C:\Users\User\AppData\Roaming`,
		"localappdata": `C:\Users\User\AppData\Local`, "public": `C:\Users\Public`,
		"programdata": `C:\ProgramData`, "allusersprofile": `C:\ProgramData`,
	}
	for strings.Contains(value, "%") {
		start := strings.IndexByte(value, '%')
		end := strings.IndexByte(value[start+1:], '%')
		if end < 0 {
			return ""
		}
		end += start + 1
		replacement, ok := vars[strings.ToLower(value[start+1:end])]
		if !ok {
			return ""
		}
		value = value[:start] + replacement + value[end+1:]
	}
	value = strings.Trim(strings.TrimSpace(value), `"`)
	value = strings.ReplaceAll(value, "/", `\`)
	for _, prefix := range []string{`\\?\`, `\??\`} {
		value = strings.TrimPrefix(value, prefix)
	}
	if strings.HasPrefix(value, `\\`) || strings.ContainsRune(value, 0) {
		return ""
	}
	if strings.HasPrefix(value, `\`) {
		value = "C:" + value
	}
	if len(value) < 3 || value[1] != ':' || value[2] != '\\' {
		if relative == "" {
			return ""
		}
		return s.allowedHostPath(filepath.Join(relative, filepath.FromSlash(strings.ReplaceAll(value, `\`, "/"))))
	}
	drive := strings.ToUpper(value[:1])
	root := s.Program.CDrive
	if drive != "C" {
		root = s.Drives[drive]
	}
	components := []string{}
	for _, part := range strings.Split(value[3:], `\`) {
		if part == "" || part == "." {
			continue
		}
		if part == ".." {
			if len(components) > 0 {
				components = components[:len(components)-1]
			}
			continue
		}
		components = append(components, part)
	}
	if drive == "C" && s.Program.InstallRoot != "" && len(components) > 0 && strings.EqualFold(components[0], "Program Files") {
		root = s.Program.InstallRoot
		components = components[1:]
	}
	return caseInsensitivePath(root, components)
}
func caseInsensitivePath(root string, components []string) string {
	if root == "" {
		return ""
	}
	path, err := filepath.EvalSymlinks(root)
	if err != nil {
		return ""
	}
	for _, name := range components {
		if name == "." {
			continue
		}
		entries, err := os.ReadDir(path)
		if err != nil {
			return ""
		}
		found := ""
		for _, entry := range entries {
			if entry.Name() == name {
				found = entry.Name()
				break
			}
		}
		if found == "" {
			for _, entry := range entries {
				if strings.EqualFold(entry.Name(), name) {
					if found != "" {
						return ""
					}
					found = entry.Name()
				}
			}
		}
		if found == "" {
			return ""
		}
		next := filepath.Join(path, found)
		info, err := os.Lstat(next)
		if err != nil || info.Mode()&os.ModeSymlink != 0 {
			return ""
		}
		path = next
	}
	return path
}
func withinDirectory(path, root string) bool {
	rel, err := filepath.Rel(root, path)
	return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}
func (s *installationSnapshot) allowedHostPath(path string) string {
	path = filepath.Clean(path)
	roots := []string{s.Program.CDrive, s.Program.InstallRoot}
	for _, root := range s.Drives {
		roots = append(roots, root)
	}
	for _, root := range roots {
		canonical, err := filepath.EvalSymlinks(root)
		if root == "" || err != nil {
			continue
		}
		base := canonical
		if !withinDirectory(path, base) {
			base, err = filepath.Abs(root)
			if err != nil || !withinDirectory(path, base) {
				continue
			}
		}
		rel, _ := filepath.Rel(base, path)
		return caseInsensitivePath(canonical, strings.Split(rel, string(filepath.Separator)))
	}
	return ""
}
func (s *installationSnapshot) scan() (map[string]installationFile, map[string]installationFile) {
	exes, links := map[string]installationFile{}, map[string]installationFile{}
	roots := append([]string{}, s.Roots...)
	c := s.Program.CDrive
	usersPath := s.hostPath(`C:\Users`, "")
	var users []os.DirEntry
	if usersPath != "" {
		users, _ = os.ReadDir(usersPath)
	}
	for _, user := range users {
		if !user.IsDir() {
			continue
		}
		base := filepath.Join(usersPath, user.Name())
		roots = append(roots, filepath.Join(base, "Desktop"), filepath.Join(base, "AppData/Roaming/Microsoft/Windows/Start Menu"))
		roots = append(roots, filepath.Join(base, "AppData/Local/Programs"))
	}
	roots = append(roots, filepath.Join(c, "ProgramData/Microsoft/Windows/Start Menu"))
	visited := map[string]bool{}
	for _, root := range roots {
		canonical := s.allowedHostPath(root)
		if canonical == "" || visited[canonical] {
			continue
		}
		visited[canonical] = true
		filepath.WalkDir(canonical, func(path string, entry fs.DirEntry, err error) error {
			if err != nil {
				return nil
			}
			if entry.Type()&os.ModeSymlink != 0 {
				return nil
			}
			if entry.IsDir() {
				if path != canonical && strings.HasPrefix(entry.Name(), ".") {
					return fs.SkipDir
				}
				return nil
			}
			ext := strings.ToLower(filepath.Ext(path))
			if ext != ".exe" && ext != ".lnk" {
				return nil
			}
			info, e := entry.Info()
			if e != nil || !info.Mode().IsRegular() {
				return nil
			}
			stamp := installationFile{Size: info.Size(), Modified: info.ModTime().UnixNano()}
			if ext == ".lnk" {
				if info.Size() > 1024*1024 {
					return nil
				}
				data, e := os.ReadFile(path)
				if e != nil {
					return nil
				}
				stamp.Digest = sha256.Sum256(data)
				links[path] = stamp
			} else {
				exes[path] = stamp
			}
			return nil
		})
	}
	return exes, links
}
func captureInstallation(p Program, drives map[string]string, registry string) *installationSnapshot {
	if !p.Writable {
		return nil
	}
	s := &installationSnapshot{Program: p, Drives: copyDrives(drives), Registry: readInstallationRegistry(registry)}
	s.Roots = []string{filepath.Join(p.CDrive, "Program Files (x86)")}
	if p.InstallRoot != "" {
		s.Roots = append(s.Roots, p.InstallRoot)
	} else {
		s.Roots = append(s.Roots, filepath.Join(p.CDrive, "Program Files"))
	}
	s.LikelyInstaller = likelyInstallationProgram(p.Path)
	s.Executables, s.Shortcuts = s.scan()
	return s
}
func likelyInstallationProgram(path string) bool {
	name := strings.ToLower(filepath.Base(path))
	for _, word := range []string{"setup", "install", "bootstrap"} {
		if strings.Contains(name, word) {
			return true
		}
	}
	image, err := pe.Open(path)
	if err != nil {
		return false
	}
	defer image.Close()
	for _, section := range image.Sections {
		if section.Name != ".rsrc" || section.Size > 16*1024*1024 {
			continue
		}
		data, err := section.Data()
		if err != nil {
			continue
		}
		text := strings.ToLower(windowsUTF16(data)) + strings.ToLower(string(data))
		for _, word := range []string{"installer", "setup", "installation", "安裝", "安装", "インストール", "설치"} {
			if strings.Contains(text, word) {
				return true
			}
		}
	}
	return false
}
func installationMaintenance(path, args string) bool {
	name := strings.ToLower(strings.TrimSuffix(filepath.Base(path), filepath.Ext(path)))
	for _, prefix := range []string{"unins", "uninst", "uninstall", "setup", "installer", "repair", "maintenancetool"} {
		if strings.HasPrefix(name, prefix) {
			return true
		}
	}
	text := strings.ToLower(args)
	for _, flag := range []string{"/uninstall", "--uninstall", "/remove", "--remove", "/repair", "--repair"} {
		if strings.Contains(text, flag) {
			return true
		}
	}
	return false
}
func fallbackExecutable(path string) bool {
	if installationMaintenance(path, "") {
		return false
	}
	name := strings.ToLower(filepath.Base(path))
	for _, token := range []string{"crash", "helper", "updater", "redist", "service", "setup"} {
		if strings.Contains(name, token) {
			return false
		}
	}
	for _, part := range strings.Split(filepath.ToSlash(filepath.Dir(path)), "/") {
		switch strings.ToLower(part) {
		case "common files", "redist", "_redist", "uninstall", "cache", "temp", "plugins":
			return false
		}
	}
	return true
}
func (s *installationSnapshot) addition(path, name, args, work string) (programAddition, bool) {
	if path == "" || strings.ContainsRune(args, 0) || len(args) > 32768 || installationMaintenance(path, args) {
		return programAddition{}, false
	}
	p, err := inspectProgram(path)
	if err != nil || p.Path == s.Program.Path {
		return programAddition{}, false
	}
	info, _ := os.Stat(s.Program.Path)
	target, _ := os.Stat(p.Path)
	if info != nil && target != nil && os.SameFile(info, target) {
		return programAddition{}, false
	}
	if work == "" {
		work = filepath.Dir(p.Path)
	} else if info, err := os.Stat(work); err != nil || !info.IsDir() {
		work = filepath.Dir(p.Path)
	}
	settings := Settings{CDrive: s.Program.CDrive, InstallRoot: s.Program.InstallRoot, WorkDir: work, Writable: true, Arguments: args}
	return programAddition{Path: p.Path, Name: name, Defaults: &settings}, true
}
func (s *installationSnapshot) candidates(registryPath string) []programAddition {
	_, links := s.scan()
	registry := readInstallationRegistry(registryPath)
	additions := []programAddition{}
	registeredRoots := []string{}
	covered := map[string]bool{}
	linkNames := make([]string, 0, len(links))
	for path := range links {
		linkNames = append(linkNames, path)
	}
	slices.Sort(linkNames)
	for _, path := range linkNames {
		if old, ok := s.Shortcuts[path]; ok && old == links[path] {
			continue
		}
		data, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		link, err := parseWindowsShortcut(data, registry.Codepage)
		if err != nil {
			continue
		}
		for _, target := range link.Targets {
			resolved := s.hostPath(target, filepath.Dir(path))
			candidate, ok := s.addition(resolved, strings.TrimSuffix(filepath.Base(path), filepath.Ext(path)), link.Arguments, s.hostPath(link.WorkDir, filepath.Dir(path)))
			if !ok {
				continue
			}
			additions = append(additions, candidate)
			covered[s.productDirectory(candidate.Path)] = true
			break
		}
	}
	keys := make([]string, 0, len(registry.Programs))
	for key := range registry.Programs {
		keys = append(keys, key)
	}
	slices.Sort(keys)
	for _, key := range keys {
		entry := registry.Programs[key]
		if old, ok := s.Registry.Programs[key]; ok && old == entry {
			continue
		}
		location := s.hostPath(entry.Location, "")
		if location == "" && entry.Uninstall != "" {
			command := strings.TrimSpace(entry.Uninstall)
			if end := strings.Index(strings.ToLower(command), ".exe"); end >= 0 {
				target := s.hostPath(strings.Trim(command[:end+4], `"`), "")
				windows := s.hostPath(`C:\Windows`, "")
				if target != "" && (windows == "" || !withinDirectory(target, windows)) {
					location = filepath.Dir(target)
				}
			}
		}
		if location != "" {
			s.Roots = append(s.Roots, location)
			registeredRoots = append(registeredRoots, location)
		}
		icon := strings.TrimSpace(entry.Icon)
		if index := strings.LastIndex(icon, ","); index >= 0 {
			icon = icon[:index]
		}
		if candidate, ok := s.addition(s.hostPath(icon, ""), entry.Name, "", ""); ok {
			additions = append(additions, candidate)
			covered[s.productDirectory(candidate.Path)] = true
		}
	}
	exes, _ := s.scan()
	groups := map[string][]Program{}
	for path, stamp := range exes {
		registered := slices.ContainsFunc(registeredRoots, func(root string) bool { return withinDirectory(path, root) })
		if old, ok := s.Executables[path]; ok && old == stamp && !registered {
			continue
		}
		if !fallbackExecutable(path) {
			continue
		}
		group := s.productDirectory(path)
		if covered[group] {
			continue
		}
		if p, err := inspectProgram(path); err == nil && p.Path != s.Program.Path {
			groups[group] = append(groups[group], p)
		}
	}
	for group, programs := range groups {
		if !s.LikelyInstaller && !slices.ContainsFunc(registeredRoots, func(root string) bool { return withinDirectory(group, root) || withinDirectory(root, group) }) {
			existed := false
			for path := range s.Executables {
				if s.productDirectory(path) == group {
					existed = true
					break
				}
			}
			if existed {
				continue
			}
		}
		gui := slices.ContainsFunc(programs, func(p Program) bool { return p.Kind == "gui" })
		depth := int(^uint(0) >> 1)
		for _, p := range programs {
			if !gui || p.Kind == "gui" {
				depth = min(depth, strings.Count(filepath.ToSlash(p.Path), "/"))
			}
		}
		for _, p := range programs {
			if (gui && p.Kind != "gui") || strings.Count(filepath.ToSlash(p.Path), "/") != depth {
				continue
			}
			if candidate, ok := s.addition(p.Path, "", "", ""); ok {
				additions = append(additions, candidate)
			}
		}
	}
	slices.SortStableFunc(additions, func(a, b programAddition) int { return strings.Compare(a.Path, b.Path) })
	return additions
}
func (s *installationSnapshot) productDirectory(path string) string {
	best := ""
	for _, root := range s.Roots {
		canonical, err := filepath.EvalSymlinks(root)
		if err != nil || !withinDirectory(path, canonical) {
			continue
		}
		if len(canonical) > len(best) {
			best = canonical
		}
	}
	if best == "" {
		return filepath.Dir(path)
	}
	rel, _ := filepath.Rel(best, path)
	parts := strings.Split(rel, string(filepath.Separator))
	if len(parts) > 1 {
		return filepath.Join(best, parts[0])
	}
	return best
}
func (a *App) finishInstallation(s *installationSnapshot, run *execution) {
	candidates := s.candidates(filepath.Join(a.dataDir, "registry.dat"))
	if len(candidates) == 0 {
		return
	}
	a.mu.Lock()
	run.mu.Lock()
	stopped := run.stopping
	run.mu.Unlock()
	if stopped || a.closing {
		a.mu.Unlock()
		return
	}
	result, err := a.addProgramsLocked(candidates, false)
	a.mu.Unlock()
	file, e := os.OpenFile(run.diagnostics, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0600)
	if e == nil {
		defer file.Close()
		fmt.Fprintf(file, "launcher_install_discovery added=%d errors=%d\n", result.Added, len(result.Errors))
		if err != nil {
			fmt.Fprintln(file, "launcher_install_discovery_error="+err.Error())
		}
	}
}
