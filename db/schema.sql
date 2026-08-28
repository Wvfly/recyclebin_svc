-- ============================================================================
-- recycle.db -- SINGLE SOURCE OF TRUTH for the shared database schema
-- ============================================================================
--
-- Owner:  rbservice.exe (C) is the ONLY process that creates and mutates this
--         schema. rbapi.exe (Go) opens it read-only for `items` and appends
--         commands to `ops`.
--
-- Contract:
--   * Every statement MUST be idempotent (IF NOT EXISTS). rbservice.exe
--     re-executes this script on every startup so a partially-created or
--     older database is healed automatically.
--   * This file does NOT set PRAGMA user_version. The schema version lives in
--     RB_SCHEMA_VERSION (service_c/rbsvc.h) and is applied by rbdb.c so that
--     a version mismatch can be detected BEFORE any statement runs.
--   * Changing a column name or adding/removing a column is a BREAKING change:
--     bump RB_SCHEMA_VERSION, add a migration, and update the expected column
--     list in service_go/db/db.go. Both services refuse to start on mismatch
--     rather than silently mis-reading rows.
--
-- Generation:
--   service_c/schema_sql.h is GENERATED from this file by db/gen_schema.ps1.
--   Do not edit schema_sql.h by hand -- edit this file and rebuild.
-- ============================================================================


-- ---------------------------------------------------------------------------
-- items: one row per intercepted deletion.
--
-- Path columns exist in two forms:
--   *_path      NT form, straight from the kernel (\Device\HarddiskVolumeN\..)
--   *_path_dos  Win32 form (D:\..), resolved by rbservice.exe so that the Go
--               API can render human-readable paths without duplicating the
--               NT<->DOS volume mapping.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS items (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,

    -- Identity of what was deleted
    orig_path     TEXT,     -- NT form, from the kernel notification
    orig_path_dos TEXT,     -- DOS form, filled in at landing time
    store_path    TEXT,     -- staging location (NT form)

    -- Who deleted it
    sid           TEXT,     -- requestor SID, no leading backslash
    session_id    INTEGER,
    client_ip     TEXT,

    -- When / what
    delete_time   REAL,     -- unix epoch seconds
    file_size     INTEGER,
    is_dir        INTEGER,

    -- Lifecycle: staged -> landed -> (restored | purged)
    status        TEXT CHECK (status IN
                        ('staged', 'landed', 'restored', 'purged')),
    recycle_path  TEXT      -- $R file (DOS form); NULL while staged
);

CREATE INDEX IF NOT EXISTS idx_items_status      ON items(status);
CREATE INDEX IF NOT EXISTS idx_items_status_sid  ON items(status, sid);
CREATE INDEX IF NOT EXISTS idx_items_delete_time ON items(delete_time);


-- ---------------------------------------------------------------------------
-- ops: command queue. rbapi.exe INSERTs, rbservice.exe executes and updates.
--
-- This is the ONLY table the Go service writes to. It never touches the
-- filesystem -- restore is requested here, performed by rbservice.exe, and
-- the outcome is polled back through `state` / `message`.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS ops (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,

    type    TEXT NOT NULL CHECK (type IN ('restore')),
    item_id INTEGER NOT NULL,

    arg     TEXT,   -- optional restore target override
    state   TEXT NOT NULL DEFAULT 'pending'
                    CHECK (state IN ('pending', 'done', 'failed')),
    message TEXT,   -- human-readable outcome

    ts      REAL,
    result  TEXT    -- reserved for structured results
);

CREATE INDEX IF NOT EXISTS idx_ops_state ON ops(state);
