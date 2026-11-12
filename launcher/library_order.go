package main

import "errors"

// 以相鄰目標移動單一項目，避免前端較舊的列表覆寫剛加入的程式。
func (a *App) MoveProgram(id, target string, after bool) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	i, j := a.indexLocked(id), a.indexLocked(target)
	if i < 0 || j < 0 || a.library.Programs[i].Temporary || a.library.Programs[j].Temporary {
		return errors.New("找不到可排序的程式")
	}
	if i == j {
		return nil
	}
	next := a.library
	next.Programs = append([]Program(nil), a.library.Programs...)
	program := next.Programs[i]
	next.Programs = append(next.Programs[:i], next.Programs[i+1:]...)
	if i < j {
		j--
	}
	if after {
		j++
	}
	next.Programs = append(next.Programs, Program{})
	copy(next.Programs[j+1:], next.Programs[j:])
	next.Programs[j] = program
	return a.saveLocked(next)
}
