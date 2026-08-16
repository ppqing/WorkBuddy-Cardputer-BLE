package main

import (
	"bufio"
	"context"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf16"
	"unicode/utf8"
)

//go:embed ble_proxy.py
var bleProxyScript []byte

// errBLEProxyMissing is returned when the Python BLE proxy can't start.
var errBLEProxyMissing = errors.New("ble proxy unavailable (need python + bleak)")

const (
	bleProxyName         = "ask-master"
	bleProxyStartTimeout = 30 * time.Second
	// The proxy needs a scan + connect cycle before the link is usable.
	// Give a question that arrives during that window a chance to land
	// instead of immediately falling back to the offline answer.
	bleDeviceWaitTimeout = 20 * time.Second
)

// BLEBridge implements Bridger over BLE. It spawns a Python helper
// (ble_proxy.py) that speaks GATT (Nordic UART Service) with the Cardputer,
// and relays line-delimited JSON between the proxy and this daemon.
//
// Device state: DeviceOnline() is true while the proxy reports a live BLE
// connection. Presence works the same way as the WebSocket bridge — the
// device must be advertising, the proxy must have connected, and the reply
// channel must be ready.
type BLEBridge struct {
	mu       sync.Mutex
	queue    []*pendingQuestion
	current  *pendingQuestion
	shutdown context.Context
	cancel   context.CancelFunc
	logger   *slog.Logger

	// proxy process
	cmd     *exec.Cmd
	stdin   io.WriteCloser
	writeMu sync.Mutex
	scanner *bufio.Scanner

	connected bool
	ready     bool
}

// NewBLEBridge creates a BLE-backed bridge.
func NewBLEBridge(logger *slog.Logger) *BLEBridge {
	if logger == nil {
		logger = slog.Default()
	}
	ctx, cancel := context.WithCancel(context.Background())
	b := &BLEBridge{
		shutdown: ctx,
		cancel:   cancel,
		logger:   logger,
	}
	// Serialize questions: several MCP clients (e.g. multiple editor windows)
	// share one daemon and one device, so only one question may occupy the
	// Cardputer screen at a time.
	go b.queueProcessor()
	return b
}

// queueProcessor dispatches queued questions one at a time, waiting for each
// to be answered (or to time out) before sending the next one.
func (b *BLEBridge) queueProcessor() {
	for {
		select {
		case <-b.shutdown.Done():
			return
		default:
		}

		b.mu.Lock()
		if len(b.queue) == 0 {
			b.mu.Unlock()
			time.Sleep(100 * time.Millisecond)
			continue
		}
		pq := b.queue[0]
		b.current = pq
		b.mu.Unlock()

		b.processQuestion(pq)

		b.mu.Lock()
		b.current = nil
		if len(b.queue) > 0 && b.queue[0] == pq {
			b.queue = b.queue[1:]
		}
		b.mu.Unlock()
	}
}

// processQuestion writes one question to the device and blocks until it is
// answered, errored, or timed out.
func (b *BLEBridge) processQuestion(pq *pendingQuestion) {
	if err := b.writePayload(pq.payload); err != nil {
		select {
		case pq.errCh <- err:
		default:
		}
		return
	}
	<-pq.done
}

// resolveProxyScript locates ble_proxy.py without relying on the working
// directory, which an MCP client may set to anything.
//
// The script is embedded into the binary (go:embed), so in production it is
// extracted to a temporary file at startup and executed from there. During
// development an external ble_proxy.py next to the executable or in the working
// directory is preferred so edits take effect without a rebuild.
func resolveProxyScript() (string, bool, error) {
	const name = "ble_proxy.py"

	// Development: an external file on disk wins over the embedded copy, so
	// editing ble_proxy.py and re-running `go run` picks up changes instantly.
	if exe, err := os.Executable(); err == nil {
		candidate := filepath.Join(filepath.Dir(exe), name)
		if _, err := os.Stat(candidate); err == nil {
			return candidate, false, nil
		}
	}
	if abs, err := filepath.Abs(name); err == nil {
		if _, err := os.Stat(abs); err == nil {
			return abs, false, nil
		}
	}

	// Production: extract the embedded script to a temp file.
	tmp, err := os.CreateTemp("", "ask-master-ble-proxy-*.py")
	if err != nil {
		return "", false, fmt.Errorf("create temp proxy script: %w", err)
	}
	if _, err := tmp.Write(bleProxyScript); err != nil {
		_ = tmp.Close()
		_ = os.Remove(tmp.Name())
		return "", false, fmt.Errorf("write embedded proxy script: %w", err)
	}
	if err := tmp.Close(); err != nil {
		_ = os.Remove(tmp.Name())
		return "", false, fmt.Errorf("close temp proxy script: %w", err)
	}
	return tmp.Name(), true, nil
}

// Start launches the Python BLE proxy and begins relaying events.
func (b *BLEBridge) Start() error {
	py, err := findPython()
	if err != nil {
		return errBLEProxyMissing
	}

	script, isTemp, err := resolveProxyScript()
	if err != nil {
		return err
	}
	if isTemp {
		// Clean up the extracted temp file once the proxy exits.
		defer os.Remove(script)
	}

	cmd := exec.Command(py, script)
	// Run the proxy from its own directory so it never depends on the
	// working directory the MCP client happened to launch us with.
	cmd.Dir = filepath.Dir(script)
	// Tell the proxy where the executable lives so it can locate local
	// assets (e.g. the whisper model under models/) in both dev and the
	// extracted-temp-file production layout.
	if exe, err := os.Executable(); err == nil {
		cmd.Env = append(os.Environ(), "ASK_MASTER_BLE_DIR="+filepath.Dir(exe))
	}
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return fmt.Errorf("proxy stdin: %w", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("proxy stdout: %w", err)
	}
	cmd.Stderr = os.Stderr

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("proxy start: %w", err)
	}

	b.cmd = cmd
	b.stdin = stdin
	b.scanner = bufio.NewScanner(stdout)
	b.scanner.Buffer(make([]byte, 0, 64*1024), 64*1024)

	go b.readLoop()

	// Wait until the proxy process reports it is alive. The BLE link itself
	// comes up later (scan + connect); SendAndWait waits for that separately.
	deadline := time.Now().Add(bleProxyStartTimeout)
	for !b.isReady() && time.Now().Before(deadline) {
		time.Sleep(100 * time.Millisecond)
	}
	if !b.isReady() {
		return errors.New("ble proxy did not become ready")
	}
	b.logger.Info("BLE proxy started")
	return nil
}

func (b *BLEBridge) isReady() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.ready
}

// waitConnected blocks until the BLE link is up or the grace period expires.
func (b *BLEBridge) waitConnected(d time.Duration) bool {
	if b.Connected() {
		return true
	}
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		select {
		case <-b.shutdown.Done():
			return false
		case <-time.After(200 * time.Millisecond):
		}
		if b.Connected() {
			return true
		}
	}
	return false
}

// readLoop consumes line-delimited JSON events from the proxy.
func (b *BLEBridge) readLoop() {
	for b.scanner.Scan() {
		line := b.scanner.Text()
		if len(line) == 0 {
			continue
		}
		var ev struct {
			Event     string `json:"event"`
			Connected bool   `json:"connected"`
			Line      string `json:"line"`
			Msg       string `json:"msg"`
		}
		if err := json.Unmarshal([]byte(line), &ev); err != nil {
			b.logger.Debug("ble proxy: unparsable event", "line", line)
			continue
		}
		switch ev.Event {
		case "ready":
			b.mu.Lock()
			b.ready = true
			b.mu.Unlock()
		case "connected":
			b.mu.Lock()
			b.connected = ev.Connected
			b.mu.Unlock()
			b.logger.Info("BLE device connection changed", "connected", ev.Connected)
			if !ev.Connected {
				b.onDisconnect()
			}
		case "recv":
			b.receive(ev.Line)
		case "log":
			b.logger.Debug("ble proxy log", "msg", ev.Msg)
		}
	}
	// Proxy exited
	b.mu.Lock()
	b.connected = false
	b.ready = false
	b.mu.Unlock()
	b.logger.Warn("BLE proxy exited")
	b.onDisconnect()
}

func (b *BLEBridge) onDisconnect() {
	b.mu.Lock()
	pq := b.current
	if pq != nil && !pq.answered {
		pq.answered = true
		pq.errCh <- errBridgeDisconnected
		pq.closeDone()
	}
	b.current = nil
	b.mu.Unlock()
}

// Connected reports whether the BLE link is up.
func (b *BLEBridge) Connected() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.connected && b.ready
}

// DeviceOnline reports whether a Cardputer is currently connected over BLE.
func (b *BLEBridge) DeviceOnline() bool {
	return b.Connected()
}

// SendAndWait sends an ask-master payload to the device and waits for a reply.
func (b *BLEBridge) SendAndWait(payload string, questionType string, options []string, timeout time.Duration) (string, error) {
	// The proxy may still be scanning/connecting (or reconnecting after a
	// device reset). Give it a grace period before declaring the device offline.
	if !b.waitConnected(bleDeviceWaitTimeout) {
		return "", errDeviceOffline
	}

	replyCh := make(chan string, 1)
	errCh := make(chan error, 1)

	pq := &pendingQuestion{
		payload:      payload,
		questionType: questionType,
		options:      append([]string(nil), options...),
		replyCh:      replyCh,
		errCh:        errCh,
		totalTimer:   time.NewTimer(timeout),
		done:         make(chan struct{}),
	}

	b.mu.Lock()
	if len(b.queue) >= wsMaxQueueDepth {
		b.mu.Unlock()
		return "", errQueueFull
	}
	b.queue = append(b.queue, pq)
	b.mu.Unlock()

	defer pq.totalTimer.Stop()
	// Always drop the question from the queue, on every exit path, otherwise
	// answered questions accumulate and eventually trip errQueueFull.
	defer b.removeQuestion(pq)

	// queueProcessor picks the question up and writes it to the device.
	select {
	case reply, ok := <-replyCh:
		if !ok {
			return "", errBridgeDisconnected
		}
		return reply, nil
	case err := <-errCh:
		return "", err
	case <-pq.totalTimer.C:
		return "", context.DeadlineExceeded
	case <-b.shutdown.Done():
		return "", b.shutdown.Err()
	}
}

// writePayload sends one payload line to the proxy process.
func (b *BLEBridge) writePayload(payload string) error {
	b.mu.Lock()
	stdin := b.stdin
	b.mu.Unlock()

	if stdin == nil {
		return errBLEProxyMissing
	}

	msg, err := json.Marshal(map[string]string{
		"cmd":     "send",
		"payload": payload,
	})
	if err != nil {
		return fmt.Errorf("encode proxy command: %w", err)
	}

	b.writeMu.Lock()
	defer b.writeMu.Unlock()
	if _, err := stdin.Write(append(asciiEscapeJSON(msg), '\n')); err != nil {
		return fmt.Errorf("write to ble proxy: %w", err)
	}
	return nil
}

// asciiEscapeJSON rewrites non-ASCII characters as \uXXXX escapes so the JSON
// line stays pure ASCII. Go's encoding/json emits raw UTF-8, but the proxy's
// stdin is a pipe whose decoding depends on the platform (on Windows the
// default is the ANSI code page), which would corrupt non-ASCII payloads.
// Escaped output is decodable under any ASCII-compatible encoding.
func asciiEscapeJSON(b []byte) []byte {
	if isASCII(b) {
		return b
	}
	out := make([]byte, 0, len(b)+16)
	for _, r := range string(b) {
		switch {
		case r < utf8.RuneSelf:
			out = append(out, byte(r))
		case r > 0xFFFF:
			// Outside the BMP: encode as a UTF-16 surrogate pair.
			r1, r2 := utf16.EncodeRune(r)
			out = append(out, fmt.Sprintf("\\u%04x\\u%04x", r1, r2)...)
		default:
			out = append(out, fmt.Sprintf("\\u%04x", r)...)
		}
	}
	return out
}

func isASCII(b []byte) bool {
	for _, c := range b {
		if c >= utf8.RuneSelf {
			return false
		}
	}
	return true
}

// receive processes a reply line from the BLE device.
// Supports both:
//   - new protocol: {"cmd":"permission"/"input","id":"...","decision":"..."}
//   - legacy text: "y"/"n"/"some text"
// The new protocol extracts the text field; legacy passes through verbatim.
func (b *BLEBridge) receive(line string) {
	b.mu.Lock()
	pq := b.current
	if pq != nil {
		pq.answered = true
	}
	b.mu.Unlock()

	if pq == nil {
		return
	}

	reply := strings.TrimSpace(line)

	// 新协议格式：JSON cmd 对象
	isCustomInput := false
	if strings.HasPrefix(reply, "{") {
		var resp struct {
			Cmd  string `json:"cmd"`
			ID   string `json:"id"`
			Text string `json:"text"`
			Decision string `json:"decision"`
			Option int    `json:"option"`
		}
		if err := json.Unmarshal([]byte(reply), &resp); err == nil && resp.Cmd != "" {
			switch resp.Cmd {
			case "permission":
				// choose 界面按数字键时固件发送
				// {"decision":"once","option":N}，option 才是选项序号。
				// 优先取 option，否则取 decision（confirm 的 once/deny）。
				if resp.Option > 0 {
					reply = strconv.Itoa(resp.Option)
				} else {
					reply = resp.Decision // "once" / "deny"
				}
			case "input":
				// 自由文本（ask/escalate 输入，或 choose 的「自定义输入」选项）
				reply = resp.Text
				isCustomInput = true
			}
		}
		// else: unrecognized JSON, pass through as-is
	}

	// choose 的「自定义输入」选项会以 input 命令返回自由文本，
	// 应原样返回，不经过选项序号映射。
	if pq.questionType == "choose" && !isCustomInput {
		mapped, err := mapChooseReply(reply, pq.options)
		if err != nil {
			select {
			case pq.errCh <- err:
			default:
			}
			return
		}
		reply = mapped
	}

	select {
	case pq.replyCh <- reply:
	default:
	}
	pq.closeDone()
}

func (b *BLEBridge) removeQuestion(pq *pendingQuestion) {
	b.mu.Lock()
	defer b.mu.Unlock()
	for i, q := range b.queue {
		if q == pq {
			b.queue = append(b.queue[:i], b.queue[i+1:]...)
			q.closeDone()
			break
		}
	}
	if b.current == pq {
		b.current = nil
	}
}

// Shutdown stops the proxy process.
func (b *BLEBridge) Shutdown() error {
	b.cancel()
	if b.cmd != nil && b.cmd.Process != nil {
		_ = b.cmd.Process.Kill()
	}
	return nil
}

func findPython() (string, error) {
	for _, name := range []string{"python", "python3", "py"} {
		if p, err := exec.LookPath(name); err == nil {
			return p, nil
		}
	}
	return "", errors.New("no python interpreter found")
}
