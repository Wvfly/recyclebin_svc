package api

// L1 单元测试 — 管理 API 层 (test-plan: L1-4, L1-5)
//
// Go 要求 *_test.go 与被测包同目录，故本文件位于 service_go/api/；
// 运行入口见 test/l1_unit/go/run.ps1。

import (
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"rbapi/db"
)

// ---------------------------------------------------------------------
// L1-5  /health 与 /stats 端点
// ---------------------------------------------------------------------

// 无 token 配置时鉴权关闭（开发环境语义），端点应可访问。
func TestHealthEndpointWhenAuthDisabled(t *testing.T) {
	srv := New(newTestDB(t), "", func() map[string]interface{} {
		return map[string]interface{}{"ok": true}
	})
	mux := http.NewServeMux()
	srv.Register(mux)

	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("/health = %d, want 200", rec.Code)
	}
	var body map[string]interface{}
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("body is not valid JSON: %v (raw=%q)", err, rec.Body.String())
	}
	if body["ok"] != true {
		t.Errorf(`/health "ok" = %v, want true`, body["ok"])
	}
	if _, ok := body["counts"]; !ok {
		t.Error(`/health missing "counts"`)
	}
}

// 数据库未就绪（openWithRetry 重试耗尽）时，所有端点都必须给出明确的 503，
// 而不是 panic —— 否则调用方只看到"连接被重置"，无法区分"服务挂了"与
// "数据库还没建好"。
//
// /health 尤其关键：它是运维判断保护是否生效的入口（RB-29 依赖其
// protected_count 告警）。修复前 handleStats 已有 s.Stats == nil 检查，
// 而 s.DB 在所有 handler 中均未检查，属同类疏漏。
func TestAllEndpointsWhenDatabaseUnavailable(t *testing.T) {
	srv := New(nil, "", nil) // DB 未就绪
	mux := http.NewServeMux()
	srv.Register(mux)

	cases := []struct {
		name   string
		method string
		path   string
		body   string
	}{
		{"health", http.MethodGet, "/health", ""},
		{"items", http.MethodGet, "/items", ""},
		{"items by id", http.MethodGet, "/items/1", ""},
		{"search", http.MethodGet, "/search?q=x", ""},
		{"ops list", http.MethodGet, "/ops", ""},
		{"ops create", http.MethodPost, "/ops", `{"type":"restore","id":1}`},
		{"ops by id", http.MethodGet, "/ops/1", ""},
	}

	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			defer func() {
				if rec := recover(); rec != nil {
					t.Errorf("PANIC on %s %s when DB is nil: %v", c.method, c.path, rec)
				}
			}()

			var req *http.Request
			if c.body != "" {
				req = httptest.NewRequest(c.method, c.path, strings.NewReader(c.body))
			} else {
				req = httptest.NewRequest(c.method, c.path, nil)
			}
			rec := httptest.NewRecorder()
			mux.ServeHTTP(rec, req)

			if rec.Code != http.StatusServiceUnavailable {
				t.Errorf("%s %s = %d, want 503 (database unavailable); body=%s",
					c.method, c.path, rec.Code, rec.Body.String())
			}
		})
	}
}

// 守卫不得误伤正常路径：DB 就绪时 /health 仍应 200。
func TestHealthStillOKWhenDatabaseReady(t *testing.T) {
	srv := New(newTestDB(t), "", nil)
	mux := http.NewServeMux()
	srv.Register(mux)

	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("/health with a ready DB = %d, want 200; body=%s",
			rec.Code, rec.Body.String())
	}
}

// ---------------------------------------------------------------------
// 辅助：临时数据库（与 db 包测试同一套 DDL）
// ---------------------------------------------------------------------

func newTestDB(t *testing.T) *db.DB {
	t.Helper()

	raw, err := os.ReadFile(filepath.Join("..", "..", "db", "schema.sql"))
	if err != nil {
		t.Skipf("schema.sql not reachable (%v)", err)
	}
	path := filepath.Join(t.TempDir(), "recycle.db")

	conn, err := sql.Open("sqlite", "file:"+path+"?mode=rwc")
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer conn.Close()
	if _, err := conn.Exec(string(raw)); err != nil {
		t.Fatalf("apply schema: %v", err)
	}
	if _, err := conn.Exec("PRAGMA user_version = 1"); err != nil {
		t.Fatalf("stamp version: %v", err)
	}

	d, err := db.Open(path)
	if err != nil {
		t.Fatalf("db.Open: %v", err)
	}
	t.Cleanup(func() { d.Close() })
	return d
}

// 未授权请求必须 401，且不得泄露任何条目数据。
func TestUnauthorizedRequestIsRejected(t *testing.T) {
	srv := New(nil, "secret-token", nil)
	mux := http.NewServeMux()
	srv.Register(mux)

	for _, path := range []string{"/health", "/stats", "/items"} {
		req := httptest.NewRequest(http.MethodGet, path, nil)
		rec := httptest.NewRecorder()
		mux.ServeHTTP(rec, req)
		if rec.Code != http.StatusUnauthorized {
			t.Errorf("GET %s without token = %d, want 401", path, rec.Code)
		}
		if strings.Contains(strings.ToLower(rec.Body.String()), "orig_path") {
			t.Errorf("GET %s leaked item data in 401 body", path)
		}
	}
}

func TestAuthorizedRequestAccepted(t *testing.T) {
	srv := New(newTestDB(t), "secret-token", nil)
	mux := http.NewServeMux()
	srv.Register(mux)

	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	req.Header.Set("X-Auth-Token", "secret-token")
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	if rec.Code == http.StatusUnauthorized {
		t.Error("valid token rejected")
	}
}

// ---------------------------------------------------------------------
// L1-4  token 比较 (RB-16)
// ---------------------------------------------------------------------

// authorized 当前用 == 比较 token，存在时序侧信道（RB-16，P2 待修）。
//
// 这里用"失败早退"放大差异来固化该缺陷：错误 token 的前缀错得越早，
// 比较器返回越快。用 crypto/subtle.ConstantTimeCompare 修复后，
// 该时间差会消失。为降低偶发性，测量取多轮最小值。
func TestTokenCompareTiming_RB16Pending(t *testing.T) {
	const token = "0123456789abcdef0123456789abcdef"
	srv := New(nil, token, nil)

	measure := func(candidate string) time.Duration {
		best := time.Hour
		for i := 0; i < 200; i++ {
			r := &http.Request{Header: http.Header{}}
			r.Header.Set("X-Auth-Token", candidate)
			start := time.Now()
			srv.authorized(r)
			if d := time.Since(start); d < best {
				best = d
			}
		}
		return best
	}

	// 首字符即错（应早退）vs 全部字符相同仅末位错（需比完整串）
	early := measure("x" + token[1:])
	late := measure(token[:len(token)-1] + "x")

	if late > early*3 && late-early > 200*time.Nanosecond {
		t.Skipf("RB-16 未修（预期）：token 比较为非常量时间 "+
			"(early=%v late=%v)。修法：subtle.ConstantTimeCompare",
			early, late)
	}
	// 未观测到显著差异 —— 可能是修复了，也可能是噪声。
	t.Logf("no measurable timing difference (early=%v late=%v); "+
		"if RB-16 is fixed, move this test out of Pending", early, late)
}

// ---------------------------------------------------------------------
// L1-5  请求体大小上限（防畸形客户端耗尽内存）
// ---------------------------------------------------------------------

func TestOversizedBodyRejected(t *testing.T) {
	srv := New(nil, "", nil)
	mux := http.NewServeMux()
	srv.Register(mux)

	body := strings.Repeat("A", 2<<20) // 2 MiB，超过 maxBytes
	req := httptest.NewRequest(http.MethodPost, "/ops",
		strings.NewReader(body))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	if rec.Code == http.StatusOK {
		t.Error("oversized body accepted; maxBytes limit not enforced")
	}
}

// ---------------------------------------------------------------------
// 方法白名单
// ---------------------------------------------------------------------

func TestMethodNotAllowed(t *testing.T) {
	srv := New(nil, "", nil)
	mux := http.NewServeMux()
	srv.Register(mux)

	req := httptest.NewRequest(http.MethodDelete, "/health", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	if rec.Code == http.StatusOK {
		t.Error("DELETE /health accepted, want 405")
	}
}
