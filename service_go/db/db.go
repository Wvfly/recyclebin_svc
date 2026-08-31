// Package db provides READ-ONLY access to the shared recycle.db plus the
// ability to insert commands into the `ops` table.
//
// Architecture contract (shared SQLite + independent services):
//
//	rbservice.exe (C)  -- sole WRITER of the `items` table and the ONLY
//	                      process that touches the filesystem.
//	rbapi.exe    (Go)  -- reads `items`, writes `ops` requests.
//
// This process MUST NOT modify `items` or rename/delete any file. Restore is
// performed by inserting an `ops` row and polling its state.
//
// We open the database in read-only mode for queries. The ops insert needs
// write access, so we keep a second read-write handle opened with WAL and a
// busy timeout. SQLite WAL makes one writer + many readers safe across
// processes.
package db

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	_ "modernc.org/sqlite" // pure-Go driver, no cgo required
)

// SchemaVersion must match RB_SCHEMA_VERSION in service_c/rbsvc.h.
//
// This is the cross-process contract guard: if the C service and this service
// were built against different schemas, we refuse to start instead of
// silently mis-reading columns. Bump BOTH together.
const SchemaVersion = 1

// expectedItemCols is the exact column set of the `items` table, in the order
// returned by ITEM_SELECT_COLS. It mirrors db/schema.sql; verifySchema() fails
// startup if the live database disagrees.
var expectedItemCols = []string{
	"id", "orig_path", "orig_path_dos", "store_path", "sid", "session_id",
	"client_ip", "delete_time", "file_size", "is_dir", "status", "recycle_path",
}

// SupportedOps mirrors the CHECK constraint on ops.type in db/schema.sql.
// Keep both in sync; the CHECK rejects anything else at the database level,
// and this list gives the API a clean 400 before hitting the DB.
var SupportedOps = []string{"restore", "restore-tree"}

// Item mirrors a row of the `items` table.
type Item struct {
	ID          int64   `json:"id"`
	OrigPath    string  `json:"orig_path"`
	OrigPathDos string  `json:"orig_path_dos"`
	StorePath   string  `json:"store_path"`
	// DisplayPath is the most human-readable path available: the DOS form
	// once the C service has landed the item, otherwise the raw kernel path.
	DisplayPath string  `json:"display_path"`
	Sid         string  `json:"sid"`
	SessionID   int     `json:"session_id"`
	ClientIP    string  `json:"client_ip"`
	DeleteTime  float64 `json:"delete_time"`
	FileSize    int64   `json:"file_size"`
	IsDir       int     `json:"is_dir"`
	Status      string  `json:"status"`
	RecyclePath string  `json:"recycle_path"`
}

// Op is a command row written by this service and executed by rbservice.exe.
type Op struct {
	ID      int64  `json:"id"`
	Type    string `json:"type"`
	ItemID  int64  `json:"item_id"`
	Arg     string `json:"arg,omitempty"`
	State   string `json:"state"` // pending|done|failed
	Message string `json:"message,omitempty"`
}

// DB holds the two handles described in the package comment.
type DB struct {
	ro *sql.DB // read-only queries
	rw *sql.DB // ops insert/update
}

// Open attaches to the shared database. dbPath is <StoreRoot>\recycle.db.
func Open(dbPath string) (*DB, error) {
	// IMPORTANT: do NOT set journal_mode on the read-only handle. Changing
	// journal mode is itself a write, which fails against mode=ro with
	// "attempt to write a readonly database". The C service already put the
	// database into WAL mode, so readers just inherit it.
	dsnRO := fmt.Sprintf("file:%s?mode=ro&_pragma=busy_timeout(5000)", dbPath)
	ro, err := sql.Open("sqlite", dsnRO)
	if err != nil {
		return nil, fmt.Errorf("open read-only: %w", err)
	}
	ro.SetMaxOpenConns(8)

	// Writes go only to the `ops` table; `items` is never modified here.
	dsnRW := fmt.Sprintf("file:%s?mode=rw&_pragma=busy_timeout(5000)", dbPath)
	rw, err := sql.Open("sqlite", dsnRW)
	if err != nil {
		ro.Close()
		return nil, fmt.Errorf("open read-write: %w", err)
	}
	// Single writer connection: SQLite performs better with serialized writes
	// and this avoids self-inflicted lock contention.
	rw.SetMaxOpenConns(1)

	if err := ro.Ping(); err != nil {
		rw.Close()
		ro.Close()
		return nil, fmt.Errorf("ping database: %w", err)
	}

	d := &DB{ro: ro, rw: rw}

	// Fail fast on any schema skew. Without this, a column rename on the C
	// side would compile fine here and then 500 at query time -- the exact
	// silent-drift failure this architecture is meant to prevent.
	//
	// Wrapped in ErrSchemaFatal so callers can tell "database not ready yet"
	// (retry) apart from "contract violated" (give up immediately).
	if err := d.verifySchema(); err != nil {
		d.Close()
		return nil, &ErrSchemaFatal{Msg: err.Error()}
	}

	return d, nil
}

// ErrSchemaFatal marks a broken contract between this service and the database
// schema. Retrying cannot fix it -- only rebuilding/migrating can -- so the
// caller must fail fast instead of waiting for the C service.
type ErrSchemaFatal struct {
	Msg string
}

func (e *ErrSchemaFatal) Error() string {
	return "schema contract violation: " + e.Msg
}

// verifySchema checks the schema version and the `items` column set.
func (d *DB) verifySchema() error {
	var version int
	if err := d.ro.QueryRow("PRAGMA user_version").Scan(&version); err != nil {
		return fmt.Errorf("read schema version: %w", err)
	}
	if version == 0 {
		return fmt.Errorf(
			"database is uninitialized (user_version=0); start rbservice.exe first")
	}
	if version != SchemaVersion {
		return fmt.Errorf(
			"schema version mismatch: database is %d, this build expects %d; "+
				"upgrade rbapi.exe or migrate the database",
			version, SchemaVersion)
	}

	rows, err := d.ro.Query("PRAGMA table_info(items)")
	if err != nil {
		return fmt.Errorf("inspect items table: %w", err)
	}
	defer rows.Close()

	actual := map[string]bool{}
	var order []string
	for rows.Next() {
		var cid int
		var name, ctype string
		var notNull, pk int
		var defaultVal sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notNull, &defaultVal, &pk); err != nil {
			return err
		}
		actual[name] = true
		order = append(order, name)
	}
	if err := rows.Err(); err != nil {
		return err
	}

	if len(order) == 0 {
		return fmt.Errorf("table 'items' is missing")
	}

	var missing, extra []string
	for _, want := range expectedItemCols {
		if !actual[want] {
			missing = append(missing, want)
		}
		delete(actual, want)
	}
	for name := range actual {
		extra = append(extra, name)
	}

	if len(missing) > 0 || len(extra) > 0 {
		return fmt.Errorf(
			"items schema mismatch (missing=%v unexpected=%v); "+
				"db/schema.sql and this build disagree", missing, extra)
	}

	return nil
}

// Close releases both handles.
func (d *DB) Close() error {
	err1 := d.ro.Close()
	err2 := d.rw.Close()
	if err1 != nil {
		return err1
	}
	return err2
}

// itemCols must stay in the same order as expectedItemCols (and therefore as
// ITEM_SELECT_COLS in the C service). verifySchema() guards against drift.
const itemCols = `id, orig_path, orig_path_dos, store_path, sid, session_id,
	client_ip, delete_time, file_size, is_dir, status, recycle_path`

func scanItems(rows *sql.Rows) ([]Item, error) {
	var out []Item
	for rows.Next() {
		var it Item
		var orig, origDos, store, sid, cip, status, recycle sql.NullString
		var sess, isDir sql.NullInt64
		var delTime, fileSize sql.NullFloat64
		var id sql.NullInt64

		if err := rows.Scan(&id, &orig, &origDos, &store, &sid, &sess, &cip,
			&delTime, &fileSize, &isDir, &status, &recycle); err != nil {
			return nil, err
		}

		it.ID = id.Int64
		it.OrigPath = orig.String
		it.OrigPathDos = origDos.String
		it.StorePath = store.String
		it.Sid = sid.String
		it.SessionID = int(sess.Int64)
		it.ClientIP = cip.String
		it.DeleteTime = delTime.Float64
		it.FileSize = int64(fileSize.Float64)
		it.IsDir = int(isDir.Int64)
		it.Status = status.String
		it.RecyclePath = recycle.String

		// Prefer the resolved DOS path; fall back to the kernel's NT path for
		// items that have not landed yet.
		if it.OrigPathDos != "" {
			it.DisplayPath = it.OrigPathDos
		} else {
			it.DisplayPath = it.OrigPath
		}

		out = append(out, it)
	}
	return out, rows.Err()
}

// ListItems returns a page of items, optionally filtered by status and/or SID.
func (d *DB) ListItems(limit, offset int, status, sid string) ([]Item, error) {
	if limit <= 0 {
		limit = 100
	}
	if limit > 1000 {
		limit = 1000
	}
	if offset < 0 {
		offset = 0
	}

	q := "SELECT " + itemCols + " FROM items"
	var args []interface{}
	var conds []string

	if status != "" {
		conds = append(conds, "status = ?")
		args = append(args, status)
	}
	if sid != "" {
		conds = append(conds, "sid = ?")
		args = append(args, sid)
	}
	if len(conds) > 0 {
		q += " WHERE " + conds[0]
		for i := 1; i < len(conds); i++ {
			q += " AND " + conds[i]
		}
	}

	// orderBy is not user-controlled: it never comes from the request.
	q += " ORDER BY id DESC LIMIT ? OFFSET ?"
	args = append(args, limit, offset)

	rows, err := d.ro.Query(q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	return scanItems(rows)
}

// likeEscape neutralises the LIKE metacharacters in user input (RB-09).
//
// Without it a search for "50%" silently becomes "match anything", and a
// search for "_" matches every single-character path. The ESCAPE clause below
// tells SQLite which character introduces a literal %, _ or backslash, so the
// caller's text is matched verbatim.
func likeEscape(s string) string {
	const esc = `\`
	var b strings.Builder
	b.Grow(len(s) * 2)

	for _, r := range s {
		switch r {
		case '%', '_', '\\':
			b.WriteString(esc)
		}
		b.WriteRune(r)
	}
	return b.String()
}

// SearchItems performs a substring match on the original path.
func (d *DB) SearchItems(pattern string, limit int) ([]Item, error) {
	if limit <= 0 {
		limit = 100
	}
	if limit > 1000 {
		limit = 1000
	}

	// The surrounding % are intentional wildcards; anything inside the user's
	// pattern is escaped so it is matched literally (RB-09).
	q := "SELECT " + itemCols +
		" FROM items WHERE orig_path LIKE ? ESCAPE '\\'" +
		" ORDER BY id DESC LIMIT ?"
	rows, err := d.ro.Query(q, "%"+likeEscape(pattern)+"%", limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanItems(rows)
}

// GetItem returns a single item by id.
func (d *DB) GetItem(id int64) (*Item, error) {
	q := "SELECT " + itemCols + " FROM items WHERE id = ?"
	rows, err := d.ro.Query(q, id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	items, err := scanItems(rows)
	if err != nil {
		return nil, err
	}
	if len(items) == 0 {
		return nil, nil
	}
	return &items[0], nil
}

// Counts holds the per-status row counts reported by /health.
type Counts struct {
	Staged   int `json:"staged"`
	Landed   int `json:"landed"`
	Purged   int `json:"purged"`
	Restored int `json:"restored"`
	Total    int `json:"total"`
}

// Stats returns row counts grouped by status.
func (d *DB) Stats() (*Counts, error) {
	c := &Counts{}
	for _, s := range []string{"staged", "landed", "purged", "restored"} {
		var n int
		if err := d.ro.QueryRow("SELECT COUNT(*) FROM items WHERE status = ?", s).Scan(&n); err != nil {
			return nil, err
		}
		switch s {
		case "staged":
			c.Staged = n
		case "landed":
			c.Landed = n
		case "purged":
			c.Purged = n
		case "restored":
			c.Restored = n
		}
	}
	if err := d.ro.QueryRow("SELECT COUNT(*) FROM items").Scan(&c.Total); err != nil {
		return nil, err
	}
	return c, nil
}

// EnqueueOp inserts an `ops` row asking rbservice.exe to perform work.
// It returns the new op id, which clients can poll via OpStatus.
//
// The CHECK constraint on ops.type rejects unsupported types, so a mismatch
// between this service and the C service surfaces as an insert error rather
// than a row that silently never executes.
func (d *DB) EnqueueOp(opType string, itemID int64, arg string) (int64, error) {
	if !IsSupportedOp(opType) {
		return 0, fmt.Errorf("unsupported op type %q; supported: %v",
			opType, SupportedOps)
	}
	res, err := d.rw.Exec(
		"INSERT INTO ops(type, item_id, arg, state, ts) VALUES (?, ?, ?, 'pending', ?)",
		opType, itemID, arg, float64(time.Now().Unix()),
	)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

// EnqueueRestore is a convenience wrapper around EnqueueOp.
func (d *DB) EnqueueRestore(itemID int64, targetOverride string) (int64, error) {
	return d.EnqueueOp("restore", itemID, targetOverride)
}

// OpStatus returns the current state of a queued op.
func (d *DB) OpStatus(opID int64) (*Op, error) {
	var op Op
	var arg, msg sql.NullString
	err := d.ro.QueryRow(
		"SELECT id, type, item_id, arg, state, message FROM ops WHERE id = ?",
		opID).Scan(&op.ID, &op.Type, &op.ItemID, &arg, &op.State, &msg)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	op.Arg = arg.String
	op.Message = msg.String
	return &op, nil
}

// IsSupportedOp reports whether an op type is accepted.
func IsSupportedOp(t string) bool {
	for _, s := range SupportedOps {
		if s == t {
			return true
		}
	}
	return false
}

// RecentOps returns the newest ops, newest first. Useful for an audit trail.
func (d *DB) RecentOps(limit int) ([]Op, error) {
	if limit <= 0 {
		limit = 50
	}
	rows, err := d.ro.Query(
		"SELECT id, type, item_id, arg, state, message FROM ops ORDER BY id DESC LIMIT ?",
		limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []Op
	for rows.Next() {
		var op Op
		var arg, msg sql.NullString
		if err := rows.Scan(&op.ID, &op.Type, &op.ItemID, &arg, &op.State, &msg); err != nil {
			return nil, err
		}
		op.Arg = arg.String
		op.Message = msg.String
		out = append(out, op)
	}
	return out, rows.Err()
}

// MarshalJSON is not needed on Item; kept explicit for stable field order.
var _ = json.Marshal
