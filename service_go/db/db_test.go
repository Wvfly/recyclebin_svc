package db

// L1 单元测试 — 数据访问层 (test-plan: L1-1 ~ L1-3, L1-5)
//
// 说明：Go 要求 *_test.go 与被测包位于同一目录，因此本文件放在
// service_go/db/ 下；运行入口与说明见 test/l1_unit/go/。
//
// 纯逻辑用例（likeEscape / IsSupportedOp）不依赖数据库；
// 涉及 *DB 的用例使用临时数据库，由 newTestDB 构造。

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"
)

// ---------------------------------------------------------------------
// L1-1  likeEscape (RB-09: LIKE 通配符注入)
// ---------------------------------------------------------------------

func TestLikeEscape(t *testing.T) {
	cases := []struct {
		name string
		in   string
		want string
	}{
		{"plain text unchanged", "report", "report"},
		{"percent escaped", "50%", `50\%`},
		{"underscore escaped", "a_b", `a\_b`},
		{"backslash escaped", `a\b`, `a\\b`},
		{"all metachars", `%_\`, `\%\_\\`},
		{"empty", "", ""},
		{"unicode untouched", "财务资料", "财务资料"},
		{"mixed", "50%_财务\\x", `50\%\_财务\\x`},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got := likeEscape(c.in)
			if got != c.want {
				t.Errorf("likeEscape(%q) = %q, want %q", c.in, got, c.want)
			}
		})
	}
}

// L1-1 (regression intent): 搜索 "50%" 不得变成"匹配任意路径"。
// 这是 RB-09 的核心：未转义时 % 是通配符，会返回全部条目。
func TestSearchEscapesWildcards(t *testing.T) {
	d := newTestDB(t)
	seed := []struct{ path, dos string }{
		{`\Device\HarddiskVolume4\tmp\share\50%.txt`, `E:\tmp\share\50%.txt`},
		{`\Device\HarddiskVolume4\tmp\share\50X.txt`, `E:\tmp\share\50X.txt`},
		{`\Device\HarddiskVolume4\tmp\share\other.txt`, `E:\tmp\share\other.txt`},
	}
	for _, s := range seed {
		if err := insertItem(d, s.path, s.dos, "landed"); err != nil {
			t.Fatalf("seed: %v", err)
		}
	}

	items, err := d.SearchItems("50%", 100)
	if err != nil {
		t.Fatalf("SearchItems: %v", err)
	}
	if len(items) != 1 {
		t.Fatalf("search for literal '50%%' matched %d items, want 1 (RB-09 regression)",
			len(items))
	}
	if !strings.Contains(items[0].OrigPath, "50%") {
		t.Errorf("wrong item matched: %s", items[0].OrigPath)
	}

	// 下划线同样不得当作单字符通配符
	items, err = d.SearchItems("_", 100)
	if err != nil {
		t.Fatalf("SearchItems: %v", err)
	}
	if len(items) != 0 {
		t.Errorf("search for literal '_' matched %d items, want 0", len(items))
	}
}

// ---------------------------------------------------------------------
// L1-5  ops 类型白名单
// ---------------------------------------------------------------------

func TestIsSupportedOp(t *testing.T) {
	cases := map[string]bool{
		"restore":      true,
		"restore-tree": true,
		"delete":       false,
		"purge":        false,
		"":             false,
		"RESTORE":      false, // 大小写敏感
	}
	for op, want := range cases {
		if got := IsSupportedOp(op); got != want {
			t.Errorf("IsSupportedOp(%q) = %v, want %v", op, got, want)
		}
	}
}

// ---------------------------------------------------------------------
// L1-2 / L1-3  驱动统计 (RB-29 ProtectedCount, RB-13 陈旧判定)
// ---------------------------------------------------------------------

func TestDriverStatsReadsProtectedCount(t *testing.T) {
	d := newTestDB(t)
	// 直接写入 driver_stats（生产中由 rbservice 采样写入）
	if _, err := d.rw.Exec(`INSERT INTO driver_stats
		(id, ts, intercepts, rename_ok, rename_fail, delete_denied,
		 notify_sent, notify_dropped, notify_queue_full,
		 queue_depth, max_queue_depth, protected_count)
		VALUES (1, ?, 7, 7, 0, 0, 7, 0, 0, 0, 1, 1)`,
		time.Now().Unix()); err != nil {
		t.Fatalf("insert driver_stats: %v", err)
	}

	st, err := d.DriverStats()
	if err != nil {
		t.Fatalf("DriverStats: %v", err)
	}
	if st == nil {
		t.Fatal("DriverStats returned nil, want a row")
	}
	if st.ProtectedCount != 1 {
		t.Errorf("ProtectedCount = %d, want 1 (RB-29: 0 means the share is unprotected)",
			st.ProtectedCount)
	}
	if st.Intercepts != 7 {
		t.Errorf("Intercepts = %d, want 7", st.Intercepts)
	}

	// L1-3: JSON 输出必须带上 protected_count（供 /health 告警）
	m := d.DriverStatsMap()
	if m == nil {
		t.Fatal("DriverStatsMap returned nil")
	}
	v, ok := m["protected_count"]
	if !ok {
		t.Fatal("DriverStatsMap missing protected_count (RB-29 observability)")
	}
	if n, _ := v.(int); n != 1 {
		t.Errorf("DriverStatsMap[protected_count] = %v, want 1", v)
	}
}

// L1-3: 无快照时返回 (nil, nil)，调用方不得渲染成"健康零值"（RB-13）
func TestDriverStatsNoSnapshot(t *testing.T) {
	d := newTestDB(t)
	st, err := d.DriverStats()
	if err != nil {
		t.Fatalf("DriverStats on empty db: %v", err)
	}
	if st != nil {
		t.Errorf("DriverStats on empty db = %+v, want nil (must not render as healthy zeros)", st)
	}
	if m := d.DriverStatsMap(); m != nil {
		t.Errorf("DriverStatsMap on empty db = %v, want nil", m)
	}
}

// ---------------------------------------------------------------------
// L1-6  只读句柄：API 侧物理上不得改写 items
// ---------------------------------------------------------------------

func TestReadOnlyHandleCannotWriteItems(t *testing.T) {
	d := newTestDB(t)
	if err := insertItem(d, `\Device\X\a.txt`, `E:\a.txt`, "landed"); err != nil {
		t.Fatalf("seed: %v", err)
	}
	// ro 句柄对 items 的任何写操作都必须失败
	if _, err := d.ro.Exec("UPDATE items SET status='restored' WHERE id=1"); err == nil {
		t.Error("read-only handle allowed UPDATE on items (design §5: must be impossible)")
	}
}

// ---------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------

// testSchemaVersion 必须与 service_c/rbsvc.h 的 RB_SCHEMA_VERSION 一致。
// schema.sql 本身不写 user_version（见其文件头注释），版本由 rbdb.c 施加，
// 因此测试建库时需自行盖上，否则 Open() 的 schema 校验会拒绝。
const testSchemaVersion = 1

// newTestDB 用仓库的 db/schema.sql 建一个临时数据库。
func newTestDB(t *testing.T) *DB {
	t.Helper()

	schemaPath := filepath.Join("..", "..", "db", "schema.sql")
	raw, err := os.ReadFile(schemaPath)
	if err != nil {
		t.Skipf("schema.sql not reachable from this package dir (%v); "+
			"run via test/l1_unit/go/run.ps1", err)
	}

	dir := t.TempDir()
	path := filepath.Join(dir, "recycle.db")

	if err := createSchema(path, string(raw)); err != nil {
		t.Fatalf("create schema: %v", err)
	}

	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open(%s): %v", path, err)
	}
	t.Cleanup(func() { d.Close() })
	return d
}

// createSchema 执行 DDL 并盖上 schema 版本号（模拟 rbservice 首次运行）。
func createSchema(path, ddl string) error {
	conn, err := sql.Open("sqlite", "file:"+path+"?mode=rwc&_pragma=busy_timeout(5000)")
	if err != nil {
		return err
	}
	defer conn.Close()

	if _, err := conn.Exec(ddl); err != nil {
		return err
	}
	_, err = conn.Exec(fmt.Sprintf("PRAGMA user_version = %d", testSchemaVersion))
	return err
}

// guardSchemaVersionDrift 提醒：若 C 侧版本号变更，本常量需同步，
// 否则所有 *DB 用例都会以 "version mismatch" 失败。
func TestSchemaVersionMatchesHeader(t *testing.T) {
	hdr := filepath.Join("..", "..", "service_c", "rbsvc.h")
	raw, err := os.ReadFile(hdr)
	if err != nil {
		t.Skipf("rbsvc.h not reachable (%v)", err)
	}
	m := regexp.MustCompile(`#define\s+RB_SCHEMA_VERSION\s+(\d+)`).FindSubmatch(raw)
	if m == nil {
		t.Fatal("RB_SCHEMA_VERSION not found in rbsvc.h")
	}
	want, err := strconv.Atoi(string(m[1]))
	if err != nil {
		t.Fatalf("parse RB_SCHEMA_VERSION: %v", err)
	}
	if want != testSchemaVersion {
		t.Errorf("test schema version %d != RB_SCHEMA_VERSION %d; update db_test.go",
			testSchemaVersion, want)
	}
}

func insertItem(d *DB, origPath, dosPath, status string) error {
	_, err := d.rw.Exec(`INSERT INTO items(orig_path, store_path, sid, delete_time,
		file_size, is_dir, status, orig_path_dos)
		VALUES (?,?,?,?,?,?,?,?)`,
		origPath, `\Device\HarddiskVolume4\RBStore\S-1\1_x`, "S-1-5-21-100",
		time.Now().Unix(), 10, 0, status, dosPath)
	return err
}
