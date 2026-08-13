package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"sync"

	"github.com/mark3labs/mcp-go/server"
)

// daemonAddr is the fixed localhost address for the daemon's TCP listener.
//
// A fixed port mirrors the original Unix-socket design: if the port is
// already in use, another daemon is running and the new process falls back
// to client mode. TCP localhost is used instead of Unix domain sockets
// because Go's Unix socket support on Windows is unreliable — the path
// semantics differ, os.FindProcess always succeeds (so PID liveness checks
// are broken), and /tmp does not exist. TCP localhost works identically on
// all platforms with zero platform-specific code.
const daemonAddr = "127.0.0.1:51937"

var lockFile = resolveStateFile("ask-master.lock")

// resolveStateFile returns a per-user path for runtime state files, falling
// back to /tmp only if a per-user directory is not available.
func resolveStateFile(name string) string {
	if dir := os.Getenv("XDG_RUNTIME_DIR"); dir != "" {
		base := filepath.Join(dir, "ask-master")
		_ = os.MkdirAll(base, 0700)
		return filepath.Join(base, name)
	}
	if cache, err := os.UserCacheDir(); err == nil && cache != "" {
		base := filepath.Join(cache, "ask-master")
		_ = os.MkdirAll(base, 0700)
		return filepath.Join(base, name)
	}
	return filepath.Join(os.TempDir(), name)
}

// Daemon runs the persistent bridge and accepts MCP client connections over
// a TCP localhost socket.
type Daemon struct {
	bridge  Bridger
	logger  *slog.Logger
	mu      sync.Mutex
	active  bool
	clients map[net.Conn]struct{}
}

func NewDaemon(bridge Bridger, logger *slog.Logger) *Daemon {
	return &Daemon{
		bridge:  bridge,
		logger:  logger,
		clients: make(map[net.Conn]struct{}),
	}
}

// errDaemonAlreadyRunning means another process won the race to become the
// daemon; the caller should fall back to client mode.
var errDaemonAlreadyRunning = errors.New("another ask-master daemon is already running")

func (d *Daemon) Start() error {
	// Bind the fixed TCP port. If it's already taken, another daemon likely
	// owns it — verify by dialing, then fall back to client mode.
	listener, err := net.Listen("tcp", daemonAddr)
	if err != nil {
		if conn, derr := net.Dial("tcp", daemonAddr); derr == nil {
			_ = conn.Close()
			return errDaemonAlreadyRunning
		}
		// Port is taken but we can't connect — another app is using it.
		return fmt.Errorf("listen tcp %s: %w", daemonAddr, err)
	}

	d.mu.Lock()
	d.active = true
	d.mu.Unlock()

	go d.acceptLoop(listener)
	return nil
}

func (d *Daemon) acceptLoop(listener net.Listener) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			d.mu.Lock()
			active := d.active
			d.mu.Unlock()
			if !active {
				return
			}
			d.logger.Warn("daemon accept error", "error", err)
			continue
		}

		d.mu.Lock()
		d.clients[conn] = struct{}{}
		d.mu.Unlock()

		go d.handleClient(conn)
	}
}

func (d *Daemon) handleClient(conn net.Conn) {
	defer func() {
		_ = conn.Close()
		d.mu.Lock()
		delete(d.clients, conn)
		d.mu.Unlock()
	}()

	d.logger.Info("MCP client connected", "remote", conn.RemoteAddr())

	// Create an MCP server for this client connection
	s := server.NewMCPServer("ask-master", version,
		server.WithToolCapabilities(true),
		server.WithRecovery(),
	)
	RegisterTools(s, d.bridge, d.logger)

	// Run JSON-RPC loop over the TCP connection
	stdioServer := server.NewStdioServer(s)

	ctx, cancel := contextWithCancel(d.logger)
	defer cancel()

	// Use a pipe approach: TCP read -> stdin, stdout -> TCP write
	pr, pw := io.Pipe()
	defer pr.Close()
	defer pw.Close()

	// Goroutine: read from TCP, write to pipe (acts as stdin).
	// When the connection reaches EOF (client disconnected), cancel the
	// context so handleClient unblocks and returns instead of leaking.
	go func() {
		defer pw.Close()
		defer cancel()
		scanner := bufio.NewScanner(conn)
		scanner.Buffer(make([]byte, 0, 4096), wsReadLimitBytes)
		for scanner.Scan() {
			line := scanner.Bytes()
			if len(line) == 0 {
				continue
			}
			_, _ = pw.Write(append(line, '\n'))
		}
	}()

	// Goroutine: read MCP responses, write to TCP
	go func() {
		writer := &socketWriter{conn: conn}
		_ = stdioServer.Listen(ctx, pr, writer)
	}()

	<-ctx.Done()
	d.logger.Info("MCP client disconnected", "remote", conn.RemoteAddr())
}

// socketWriter implements io.Writer to send data back to the TCP connection
type socketWriter struct {
	conn net.Conn
	mu   sync.Mutex
}

func (w *socketWriter) Write(p []byte) (n int, err error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.conn.Write(p)
}

// runClientMode connects to the daemon via TCP and proxies
// stdin/stdout bidirectionally.
func runClientMode(logger *slog.Logger) error {
	conn, err := net.Dial("tcp", daemonAddr)
	if err != nil {
		return fmt.Errorf("connect to daemon: %w", err)
	}
	defer conn.Close()

	logger.Info("connected to ask-master daemon")

	var wg sync.WaitGroup
	wg.Add(2)

	// stdin -> TCP
	go func() {
		defer wg.Done()
		_, _ = io.Copy(conn, os.Stdin)
	}()

	// TCP -> stdout
	go func() {
		defer wg.Done()
		_, _ = io.Copy(os.Stdout, conn)
	}()

	wg.Wait()
	return nil
}

// isDaemonRunning checks if the daemon is alive by attempting to connect
// to its TCP port. This is cross-platform: unlike os.FindProcess (which
// always succeeds on Windows), a successful TCP dial guarantees the
// daemon is listening.
func isDaemonRunning() bool {
	conn, err := net.Dial("tcp", daemonAddr)
	if err != nil {
		_ = os.Remove(lockFile)
		return false
	}
	_ = conn.Close()
	return true
}

// writeLockFile writes the current PID to the lock file for debugging.
func writeLockFile() error {
	return os.WriteFile(lockFile, []byte(fmt.Sprintf("%d", os.Getpid())), 0644)
}

// removeLockFile removes the lock file (call on shutdown).
func removeLockFile() {
	_ = os.Remove(lockFile)
}
