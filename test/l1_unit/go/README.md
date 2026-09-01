# L1 单元测试 — Go 层

## 为什么测试源码不在 test/ 目录

Go 语言规定 `*_test.go` 必须与被测包位于**同一目录**。被测的有两个包：

- `service_go/db/` → `service_go/db/db_test.go`
- `service_go/api/` → `service_go/api/api_test.go`

放在 test/ 目录将无法访问非导出符号（如 `likeEscape`），而它正是 RB-09
（LIKE 通配符注入）的修复点——必须被直接测试。

本目录的 `run.ps1` 是统一入口，让 CI 与 `test/run_all.ps1` 只面向 test/ 目录，
不必关心 Go 的包布局约束。

## 运行

```powershell
.\test\l1_unit\go\run.ps1            # 汇总输出
.\test\l1_unit\go\run.ps1 -Verbose   # 显示每个用例
cd service_go && go test ./...       # 等价的直接调用
```

## 用例清单（对应 test-plan L1）

| 用例 | 文件 | 验证点 | 缺陷 |
|---|---|---|---|
| L1-1 | `db_test.go` `TestLikeEscape` | `%` `_` `\` 转义正确，中文不受影响 | RB-09 |
| L1-1 | `db_test.go` `TestSearchEscapesWildcards` | 搜 `50%` 只命中字面量，不得匹配全部 | RB-09 |
| L1-2 | `db_test.go` `TestDriverStatsReadsProtectedCount` | `protected_count` 读出并进 JSON | RB-29 |
| L1-3 | `db_test.go` `TestDriverStatsNoSnapshot` | 无快照返回 nil，**不**伪装成健康零值 | RB-13 |
| L1-5 | `db_test.go` `TestIsSupportedOp` | ops 类型白名单，大小写敏感 | — |
| L1-6 | `db_test.go` `TestReadOnlyHandleCannotWriteItems` | 只读句柄物理上无法改 items | design §5 |
| — | `db_test.go` `TestSchemaVersionMatchesHeader` | 测试用版本号与 `RB_SCHEMA_VERSION` 同步 | 防漂移 |
| L1-4 | `api_test.go` `TestTokenCompareTiming_RB16Pending` | token 比较时序（RB-16 待修，短 token 难以测出差异） | RB-16 |
| L1-5 | `api_test.go` `TestUnauthorizedRequestIsRejected` | 未授权 401 且响应不泄露数据 | RB-16 |
| L1-5 | `api_test.go` `TestOversizedBodyRejected` | 超 1 MiB 请求体被拒 | — |
| L1-5 | `api_test.go` `TestMethodNotAllowed` | 方法白名单 | — |
| — | `api_test.go` `TestHealthWhenDatabaseUnavailable` | DB 未就绪时 `/health` 不得 panic | **新发现** |

## 当前结果

```
rbapi/db    7 PASS
rbapi/api   6 PASS, 1 SKIP
```

唯一的 SKIP 是 `TestHealthWhenDatabaseUnavailable`：数据库未就绪时
`/health` 会 panic（nil 解引用），已作为发现记录，见 `test/README.md`。
