package main

import (
	"encoding/json"
	"errors"
	"sync"
)

var errBridgeClosed = errors.New("啟動器已關閉")

type bridgeRequest struct {
	id, payload string
}
type bridgeResponse struct {
	Value any    `json:"value"`
	Error string `json:"error,omitempty"`
}

// 原生回呼只入列；磁碟、程序與 JSON 工作由同一個背景工作者依序處理。
type asyncBridge struct {
	mu       sync.Mutex
	closed   bool
	done     chan struct{}
	queue    chan bridgeRequest
	handle   func(string) (any, error)
	dispatch func(func())
	eval     func(string)
}

func newAsyncBridge(handle func(string) (any, error), dispatch func(func()), eval func(string)) *asyncBridge {
	b := &asyncBridge{done: make(chan struct{}), queue: make(chan bridgeRequest, 64), handle: handle, dispatch: dispatch, eval: eval}
	go b.work()
	return b
}

func (b *asyncBridge) submit(id, payload string) error {
	if id == "" || len(id) > 64 {
		return errors.New("請求識別碼不正確")
	}
	if len(payload) > 2*1024*1024 {
		return errors.New("請求內容超過上限")
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.closed {
		return errBridgeClosed
	}
	select {
	case b.queue <- bridgeRequest{id: id, payload: payload}:
		return nil
	default:
		return errors.New("啟動器請求佇列忙碌，請稍後再試")
	}
}

func (b *asyncBridge) work() {
	for {
		select {
		case <-b.done:
			return
		case req := <-b.queue:
			b.mu.Lock()
			closed := b.closed
			b.mu.Unlock()
			if closed {
				return
			}
			value, err := b.handle(req.payload)
			reply := bridgeResponse{Value: value}
			if err != nil {
				reply.Value = nil
				reply.Error = err.Error()
			}
			data, err := json.Marshal(reply)
			if err != nil {
				data, _ = json.Marshal(bridgeResponse{Error: err.Error()})
			}
			id, _ := json.Marshal(req.id)
			script := "window.launcherResponse(" + string(id) + "," + string(data) + ")"
			b.post(func() { b.eval(script) })
		}
	}
}

// 排程與關閉使用同一把鎖，避免工作者在 WebView 銷毀後仍呼叫 Dispatch。
func (b *asyncBridge) post(fn func()) bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.closed {
		return false
	}
	b.dispatch(func() {
		b.mu.Lock()
		closed := b.closed
		b.mu.Unlock()
		if !closed {
			fn()
		}
	})
	return true
}

func onUI[T any](b *asyncBridge, fn func() (T, error)) (T, error) {
	type outcome struct {
		value T
		err   error
	}
	result := make(chan outcome, 1)
	if b.post(func() {
		value, err := fn()
		result <- outcome{value, err}
	}) {
		select {
		case value := <-result:
			return value.value, value.err
		case <-b.done:
		}
	}
	var empty T
	return empty, errBridgeClosed
}

func (b *asyncBridge) onUI(fn func()) error {
	_, err := onUI(b, func() (struct{}, error) { fn(); return struct{}{}, nil })
	return err
}

func (b *asyncBridge) close() {
	b.mu.Lock()
	defer b.mu.Unlock()
	if !b.closed {
		b.closed = true
		close(b.done)
	}
}
