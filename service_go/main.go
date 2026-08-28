// rbapi.exe - management REST API for RecycleBin for SMB (Go)
//
// Companion to rbservice.exe (C). Per the shared-SQLite architecture:
//
//	rbservice.exe  owns the filesystem and the `items` table
//	rbapi.exe      serves read-only queries and queues restore commands
//
// This binary never renames, deletes, or creates any file under a protected
// share or inside $Recycle.Bin. It only reads the database and inserts rows
// into `ops`.
//
// Usage:
//
//	rbapi.exe              serve using registry configuration
//	rbapi.exe --db <path> --addr 127.0.0.1:8800 --token <t>
//	rbapi.exe --help
//
// Run as a Windows service with:
//
//	sc create RecycleBinApi binPath= "<path>\rbapi.exe" start= auto obj= LocalSystem
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"syscall"
	"time"

	"golang.org/x/sys/windows/registry"
	"rbapi/api"
	"rbapi/db"
)

const regKey = `SOFTWARE\RecycleBin`

// config holds the runtime settings for the API service.
type config struct {
	DBPath    string
	Addr      string
	Token     string
	EnableAPI bool
	Port      int
	StoreRoot string
}

// loadConfig reads HKLM\SOFTWARE\RecycleBin, falling back to the same defaults
// the C service uses so both processes agree without extra files.
func loadConfig() config {
	cfg := config{
		StoreRoot: `C:\RBStore`,
		Port:      8800,
		EnableAPI: false,
		Token:     "",
	}
	cfg.DBPath = filepath.Join(cfg.StoreRoot, "recycle.db")
	cfg.Addr = fmt.Sprintf("127.0.0.1:%d", cfg.Port)

	key, err := registry.OpenKey(registry.LOCAL_MACHINE, regKey,
		registry.QUERY_VALUE)
	if err != nil {
		return cfg
	}
	defer key.Close()

	if v, _, err := key.GetStringValue("StoreRoot"); err == nil && v != "" {
		cfg.StoreRoot = v
		cfg.DBPath = filepath.Join(v, "recycle.db")
	}
	if v, _, err := key.GetIntegerValue("RestApiPort"); err == nil {
		cfg.Port = int(v)
		cfg.Addr = fmt.Sprintf("127.0.0.1:%d", cfg.Port)
	}
	if v, _, err := key.GetStringValue("RestApiToken"); err == nil {
		cfg.Token = v
	}
	if v, _, err := key.GetIntegerValue("EnableRestApi"); err == nil {
		cfg.EnableAPI = v != 0
	}

	return cfg
}

func (c config) String() string {
	auth := "disabled"
	if c.Token != "" {
		auth = "enabled"
	}
	return fmt.Sprintf("db=%s addr=%s auth=%s", c.DBPath, c.Addr, auth)
}

func main() {
	var (
		dbFlag   = flag.String("db", "", "path to recycle.db (overrides registry)")
		addrFlag = flag.String("addr", "", "listen address (overrides registry)")
		tokFlag  = flag.String("token", "", "auth token (overrides registry)")
		showVer  = flag.Bool("version", false, "print version and exit")
	)
	flag.Parse()

	if *showVer {
		fmt.Println("rbapi 1.0.0 (RecycleBin for SMB management API)")
		return
	}

	cfg := loadConfig()

	if *dbFlag != "" {
		cfg.DBPath = *dbFlag
	}
	if *addrFlag != "" {
		cfg.Addr = *addrFlag
	}
	if *tokFlag != "" {
		cfg.Token = *tokFlag
	}

	log.SetFlags(log.LstdFlags | log.LUTC)
	log.Printf("rbapi starting: %s", cfg)

	// The C service creates the schema; wait briefly for the file to appear so
	// starting both services simultaneously works.
	database, err := openWithRetry(cfg.DBPath, 30, 2*time.Second)
	if err != nil {
		log.Fatalf("cannot open database %s: %v", cfg.DBPath, err)
	}
	defer database.Close()

	// Driver statistics are fetched from the kernel port, which lives in the C
	// service. Expose them as nil for now: the C side writes counters into the
	// DB via an optional `stats` table if you want them here.
	srv := api.New(database, cfg.Token, nil)

	mux := http.NewServeMux()
	srv.Register(mux)

	httpSrv := &http.Server{
		Addr:              cfg.Addr,
		Handler:           api.LoggingMiddleware(mux),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      60 * time.Second,
		IdleTimeout:       120 * time.Second,
	}

	// Graceful shutdown on Ctrl+C / service stop
	go func() {
		sigCh := make(chan os.Signal, 1)
		signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
		<-sigCh

		log.Println("shutting down...")
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := httpSrv.Shutdown(ctx); err != nil {
			log.Printf("shutdown error: %v", err)
		}
	}()

	log.Printf("listening on %s", cfg.Addr)
	if err := httpSrv.ListenAndServe(); err != nil &&
		!errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("server error: %v", err)
	}

	log.Println("stopped")
}

// openWithRetry waits for the database file to exist (the C service may still
// be starting) and then opens it.
//
// It distinguishes two failure classes:
//   - file not ready yet  -> keep waiting, the C service will create it
//   - ErrSchemaFatal      -> give up instantly; waiting cannot repair a
//     version/column mismatch and would only delay a clear error message
func openWithRetry(path string, attempts int, delay time.Duration) (*db.DB, error) {
	var lastErr error
	for i := 0; i < attempts; i++ {
		if _, statErr := os.Stat(path); statErr == nil {
			d, err := db.Open(path)
			if err == nil {
				return d, nil
			}
			var fatal *db.ErrSchemaFatal
			if errors.As(err, &fatal) {
				// Contract violation: fail immediately with a clear message.
				return nil, err
			}
			lastErr = err
		} else {
			lastErr = statErr
		}
		if i == 0 {
			log.Printf("waiting for database %s ...", path)
		}
		time.Sleep(delay)
	}
	if lastErr == nil {
		lastErr = errors.New("unknown error")
	}
	return nil, fmt.Errorf("after %d attempts: %w", attempts, lastErr)
}

var _ = strconv.Itoa // keep strconv referenced for future flag parsing
