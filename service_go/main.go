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
// Windows service: the binary detects it was launched by the Service Control
// Manager and switches to service mode automatically, so it is safe to run
// under:
//
//	sc create RecycleBinApi binPath= "<path>\rbapi.exe" start= auto obj= LocalSystem
//
// (The service reports Running to the SCM before the HTTP listener is
// up, so the SCM never times out waiting on the DB-open retry window. A
// startup failure still stops the service with a non-zero exit code and the
// SCM failure actions restart it.)
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
	"golang.org/x/sys/windows/svc"

	"rbapi/api"
	"rbapi/db"
)

const regKey = `SOFTWARE\RecycleBin`

// serviceName must match the SCM service name used by deploy.ps1.
const serviceName = "RecycleBinApi"

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
		fmt.Println("rbapi 1.1.0 (RecycleBin for SMB management API)")
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

	// Same binary, two modes: launched by the SCM -> Windows service;
	// launched from a console / shell -> plain foreground process.
	if isSvc, err := svc.IsWindowsService(); err == nil && isSvc {
		if err := svc.Run(serviceName, &apiService{cfg: cfg}); err != nil {
			log.Fatalf("service run failed: %v", err)
		}
		return
	} else if err != nil {
		log.Printf("svc.IsWindowsService: %v (falling back to foreground)", err)
	}

	runForeground(cfg)
}

// withCORS lets browser-based front-ends (e.g. web/index.html served from any
// origin, including file://) call the management API. The listener is bound to
// loopback only, so exposing CORS here stays local. It also answers the
// preflight OPTIONS that browsers send before a POST (the restore op endpoint).
func withCORS(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type, X-Auth-Token")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// ---------------------------------------------------------------------------
// Foreground mode (console, debugging, manual --db/--addr/--token usage)
// ---------------------------------------------------------------------------

func runForeground(cfg config) {
	stop := make(chan struct{})
	go func() {
		sigCh := make(chan os.Signal, 1)
		signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
		<-sigCh
		close(stop)
	}()

	if err := serve(cfg, stop); err != nil {
		log.Fatalf("server error: %v", err)
	}
	log.Println("stopped")
}

// ---------------------------------------------------------------------------
// Windows service mode
// ---------------------------------------------------------------------------

// apiService implements svc.Handler. Execute runs on the SCM dispatch thread.
type apiService struct {
	cfg   config
	stop  chan struct{}
	errCh chan error
}

func (s *apiService) Execute(args []string, r <-chan svc.ChangeRequest, changes chan<- svc.Status) (bool, uint32) {
	const cmdsAccepted = svc.AcceptStop | svc.AcceptShutdown
	s.stop = make(chan struct{})
	s.errCh = make(chan error, 1)

	changes <- svc.Status{State: svc.StartPending}

	// Start the HTTP server in the background. Open the DB and bind the
	// listener off the SCM thread so the SCM never blocks on our startup.
	go func() {
		if err := serve(s.cfg, s.stop); err != nil {
			s.errCh <- err
		}
	}()

	// Report Running immediately. The DB open retry window (up to ~60s when
	// rbservice.exe is still creating the schema) must not keep the service
	// in StartPending, or the SCM gives up and kills it (events 7000/7009).
	// A genuine failure still surfaces via errCh and stops the service.
	changes <- svc.Status{State: svc.Running, Accepts: cmdsAccepted}
	log.Printf("service %s running", serviceName)

	for {
		select {
		case c := <-r:
			switch c.Cmd {
			case svc.Interrogate:
				changes <- c.CurrentStatus
			case svc.Stop, svc.Shutdown:
				log.Println("stop requested by SCM")
				close(s.stop)
				changes <- svc.Status{State: svc.StopPending}
				select {
				case err := <-s.errCh:
					if err != nil {
						log.Printf("server error during stop: %v", err)
					}
				case <-time.After(15 * time.Second):
					log.Println("forced stop after shutdown timeout")
				}
				changes <- svc.Status{State: svc.Stopped}
				return false, 0
			}
		case err := <-s.errCh:
			// Fatal startup / runtime error (DB unusable, bind failed, ...).
			log.Printf("server error: %v", err)
			changes <- svc.Status{State: svc.Stopped}
			return false, 1
		}
	}
}

// ---------------------------------------------------------------------------
// Shared server startup (both modes)
// ---------------------------------------------------------------------------

// serve opens the database, registers the HTTP handlers and serves until
// stop is closed. It returns nil for a graceful shutdown.
func serve(cfg config, stop <-chan struct{}) error {
	// The C service creates the schema; wait briefly for the file to appear so
	// starting both services simultaneously works.
	database, err := openWithRetry(cfg.DBPath, 30, 2*time.Second)
	if err != nil {
		return fmt.Errorf("cannot open database %s: %w", cfg.DBPath, err)
	}
	defer database.Close()

	// Driver statistics (RB-13).
	//
	// The kernel port accepts a single connection and rbservice.exe holds it,
	// so we cannot ask the driver ourselves -- a second FilterSendMessage would
	// be refused. Instead rbservice.exe samples the counters into the
	// driver_stats table and we read that. Nil means "unknown", which the API
	// reports as 503 rather than as a misleading set of zeros.
	srv := api.New(database, cfg.Token, database.DriverStatsMap)

	mux := http.NewServeMux()
	srv.Register(mux)

	httpSrv := &http.Server{
		Addr:              cfg.Addr,
		Handler:           api.LoggingMiddleware(withCORS(mux)),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      60 * time.Second,
		IdleTimeout:       120 * time.Second,
	}

	// Graceful shutdown when stop fires (Ctrl+C in foreground mode, SCM stop
	// request in service mode).
	go func() {
		<-stop
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
		return err
	}
	return nil
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
