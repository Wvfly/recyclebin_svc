// Package api exposes the read-only management REST surface plus a command
// channel for restore operations.
//
// Design constraints that shape this package:
//
//  1. This service NEVER touches the filesystem. Restore is a DB write into
//     `ops`; rbservice.exe (C) performs the rename and updates `ops.state`.
//  2. Everything is read-only against `items` -- enforced by opening that
//     handle with mode=ro, so even a bug here cannot corrupt metadata.
//  3. Bind to loopback only. Token auth when RestApiToken is non-empty.
package api

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"

	"rbapi/db"
)

// Server carries the dependencies for the HTTP handlers.
type Server struct {
	DB    *db.DB
	Token string // when empty, auth is disabled
	Stats func() map[string]interface{}
}

// New builds a Server. statsFn supplies live driver counters (may be nil).
func New(d *db.DB, token string, statsFn func() map[string]interface{}) *Server {
	return &Server{DB: d, Token: token, Stats: statsFn}
}

// ---------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------

func (s *Server) authorized(r *http.Request) bool {
	if s.Token == "" {
		return true
	}
	return r.Header.Get("X-Auth-Token") == s.Token
}

// dbUnavailable reports whether the database handle is missing. When it is,
// a 503 is written and true is returned so the caller can stop.
//
// Why this exists: main.go starts the listener while openWithRetry is still
// waiting for rbservice.exe to create/migrate recycle.db, so every handler can
// legitimately be called with s.DB == nil. Dereferencing it panics, and a
// panic in a handler only shows up as a dropped connection -- which is the
// worst possible signal for /health, the endpoint operations polls to decide
// whether protection is actually live (RB-29). A 503 with a readable body is
// actionable; a reset connection is not.
//
// This mirrors the existing s.Stats == nil check in handleStats; the two
// handlers simply had inconsistent guards.
func (s *Server) dbUnavailable(w http.ResponseWriter) bool {
	if s.DB == nil {
		writeErr(w, http.StatusServiceUnavailable, "database unavailable")
		return true
	}
	return false
}

func writeJSON(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(code)
	if v != nil {
		_ = json.NewEncoder(w).Encode(v)
	}
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

// maxBytes limits request bodies so a malformed client cannot exhaust memory.
const maxBytes = 1 << 20

// ---------------------------------------------------------------------
// routes
// ---------------------------------------------------------------------

// Register wires the routes onto mux.
func (s *Server) Register(mux *http.ServeMux) {
	mux.HandleFunc("/health", s.handleHealth)
	mux.HandleFunc("/stats", s.handleStats)
	mux.HandleFunc("/items", s.handleItems)
	mux.HandleFunc("/items/", s.handleItemByID)
	mux.HandleFunc("/search", s.handleSearch)
	mux.HandleFunc("/ops", s.handleOps)
	mux.HandleFunc("/ops/", s.handleOpByID)
}

// GET /health -> database counters + optional driver statistics.
func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "GET required")
		return
	}
	if s.dbUnavailable(w) {
		return
	}

	counts, err := s.DB.Stats()
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}

	resp := map[string]interface{}{
		"ok":     true,
		"ts":     float64(time.Now().Unix()),
		"counts": counts,
	}
	if s.Stats != nil {
		resp["driver"] = s.Stats()
	} else {
		resp["driver"] = nil
	}
	writeJSON(w, http.StatusOK, resp)
}

// GET /stats -> raw driver counters (or 503 when the driver is unreachable).
func (s *Server) handleStats(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "GET required")
		return
	}
	if s.Stats == nil {
		writeErr(w, http.StatusServiceUnavailable, "driver stats unavailable")
		return
	}
	st := s.Stats()
	if st == nil {
		writeErr(w, http.StatusServiceUnavailable, "driver offline")
		return
	}
	writeJSON(w, http.StatusOK, st)
}

// GET /items?limit=&offset=&status=&sid=
func (s *Server) handleItems(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "GET required")
		return
	}

	if s.dbUnavailable(w) {
		return
	}

	q := r.URL.Query()
	limit, _ := strconv.Atoi(q.Get("limit"))
	offset, _ := strconv.Atoi(q.Get("offset"))

	items, err := s.DB.ListItems(limit, offset, q.Get("status"), q.Get("sid"))
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	if items == nil {
		items = []db.Item{}
	}
	fillUsernames(items)
	// total drives front-end pagination; a count failure must not break the
	// page itself, so fall back to -1 (unknown) and let the UI cope.
	total, err := s.DB.CountItems(q.Get("status"), q.Get("sid"))
	if err != nil {
		total = -1
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"count": len(items),
		"total": total,
		"items": items,
	})
}

// GET /items/{id}
func (s *Server) handleItemByID(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "GET required")
		return
	}

	if s.dbUnavailable(w) {
		return
	}

	idStr := strings.Trim(strings.TrimPrefix(r.URL.Path, "/items/"), "/")
	id, err := strconv.ParseInt(idStr, 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad id")
		return
	}

	it, err := s.DB.GetItem(id)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	if it == nil {
		writeErr(w, http.StatusNotFound, "not found")
		return
	}
	it.Username = lookupSidName(it.Sid)
	writeJSON(w, http.StatusOK, it)
}

// GET /search?q=<substring>&limit=
func (s *Server) handleSearch(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "GET required")
		return
	}

	if s.dbUnavailable(w) {
		return
	}

	q := r.URL.Query()
	pattern := strings.TrimSpace(q.Get("q"))
	if pattern == "" {
		writeErr(w, http.StatusBadRequest, "q required")
		return
	}
	limit, _ := strconv.Atoi(q.Get("limit"))
	offset, _ := strconv.Atoi(q.Get("offset"))

	items, err := s.DB.SearchItems(pattern, limit, offset)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	if items == nil {
		items = []db.Item{}
	}
	fillUsernames(items)
	total, err := s.DB.CountSearch(pattern)
	if err != nil {
		total = -1
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"count": len(items),
		"total": total,
		"items": items,
	})
}

// POST /ops
//
//	{"type":"restore","id":123,"arg":"D:\\Share\\a.txt"}
//	    Restore one item. `arg` optionally overrides the destination.
//
//	{"type":"restore-tree","arg":"D:\\Share\\Project"}
//	    Restore every item whose original path starts with `arg`. Deleting a
//	    directory over SMB removes one entry at a time, so a tree arrives in
//	    the recycle bin as many scattered entries; this restores them in one
//	    request. `id` is unused.
//
// Queues a command for rbservice.exe. This endpoint does NOT perform the
// restore itself -- poll GET /ops/{id} for the outcome. For restore-tree the
// result message summarises how many entries succeeded and how many failed.
func (s *Server) handleOps(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if s.dbUnavailable(w) {
		return
	}

	switch r.Method {
	case http.MethodPost:
		r.Body = http.MaxBytesReader(w, r.Body, maxBytes)
		var req struct {
			Type string `json:"type"`
			ID   int64  `json:"id"`
			Arg  string `json:"arg"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeErr(w, http.StatusBadRequest, "invalid JSON body")
			return
		}
		if req.Type == "" {
			req.Type = "restore"
		}
		// Validated against the same registry that backs the DB CHECK
		// constraint on ops.type, so the two cannot disagree.
		if !db.IsSupportedOp(req.Type) {
			writeErr(w, http.StatusBadRequest,
				fmt.Sprintf("unsupported op type %q; supported: %v",
					req.Type, db.SupportedOps))
			return
		}
		// Each op type carries a different payload:
		//   restore      -> id  (the item to restore), arg optional
		//   restore-tree -> arg (path prefix),          id unused (stored as 0)
		if req.Type == "restore-tree" {
			if strings.TrimSpace(req.Arg) == "" {
				writeErr(w, http.StatusBadRequest,
					"arg required for restore-tree: path prefix, e.g. D:\\Share\\Project")
				return
			}
			req.ID = 0 // not a single-item op; the schema requires a value
		} else {
			if req.ID <= 0 {
				writeErr(w, http.StatusBadRequest, "id required")
				return
			}

			// Only existence is checked here. Whether an item is *restorable*
			// is a business rule owned by rbservice.exe -- duplicating the
			// landed/staged check here would let the two services disagree.
			// Queue the request and let the C service decide; it reports back
			// via the op's state/message.
			it, err := s.DB.GetItem(req.ID)
			if err != nil {
				writeErr(w, http.StatusInternalServerError, err.Error())
				return
			}
			if it == nil {
				writeErr(w, http.StatusNotFound, "not found")
				return
			}
		}

		opID, err := s.DB.EnqueueRestore(req.ID, req.Arg)
		if err != nil {
			writeErr(w, http.StatusInternalServerError, err.Error())
			return
		}
		writeJSON(w, http.StatusAccepted, map[string]interface{}{
			"ok":    true,
			"op_id": opID,
			"note":  "queued; poll /ops/" + strconv.FormatInt(opID, 10) +
				" for the outcome (rbservice decides if it is restorable)",
		})

	case http.MethodGet:
		ops, err := s.DB.RecentOps(50)
		if err != nil {
			writeErr(w, http.StatusInternalServerError, err.Error())
			return
		}
		if ops == nil {
			ops = []db.Op{}
		}
		writeJSON(w, http.StatusOK, map[string]interface{}{
			"count": len(ops),
			"ops":   ops,
		})

	default:
		writeErr(w, http.StatusMethodNotAllowed, "GET or POST required")
	}
}

// GET /ops/{id} -> poll the state of a queued command.
func (s *Server) handleOpByID(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	if r.Method != http.MethodGet {
		writeErr(w, http.StatusMethodNotAllowed, "GET required")
		return
	}

	if s.dbUnavailable(w) {
		return
	}

	idStr := strings.Trim(strings.TrimPrefix(r.URL.Path, "/ops/"), "/")
	id, err := strconv.ParseInt(idStr, 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad id")
		return
	}

	op, err := s.DB.OpStatus(id)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	if op == nil {
		writeErr(w, http.StatusNotFound, "not found")
		return
	}
	writeJSON(w, http.StatusOK, op)
}

// LoggingMiddleware writes one line per request to the standard logger.
func LoggingMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("%s %s %s", r.Method, r.URL.Path, time.Since(start))
	})
}
