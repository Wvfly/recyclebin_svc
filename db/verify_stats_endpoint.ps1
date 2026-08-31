# RB-13 end-to-end: does /stats really return driver counters now?
# Exercises the real rbapi.exe against a scratch database.
# Native tools are invoked through cmd.exe: rbservice logs to stderr, which
# PowerShell would otherwise turn into a terminating ErrorRecord.

$ErrorActionPreference = 'Stop'

$root = 'c:\RecycleBin\smb_intercept\recyclebin_svc'
$exe  = Join-Path $root 'target\Release\rbapi.exe'
$svc  = Join-Path $root 'target\Release\rbservice.exe'
$tmp  = Join-Path $env:TEMP ('rb13_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
$port = 18931
$hdr  = @{ 'X-Auth-Token' = 'testtok' }

function Invoke-Stats([string]$db) {
    $p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden `
         -ArgumentList @('--db', $db, '--addr', "127.0.0.1:$port", '--token', 'testtok')
    try {
        Start-Sleep -Seconds 2
        try {
            $r = Invoke-RestMethod "http://127.0.0.1:$port/stats" -Headers $hdr
            return @{ Ok = $true; Body = $r }
        } catch {
            $code = [int]$_.Exception.Response.StatusCode
            return @{ Ok = $false; Code = $code }
        }
    } finally {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 300
    }
}

try {
    $db = Join-Path $tmp 'recycle.db'

    Write-Host '=== case A: no snapshot yet (driver never loaded) ==='
    & cmd /c "`"$svc`" once --db `"$db`" > nul 2>&1"
    $res = Invoke-Stats $db
    if ($res.Ok) {
        Write-Host "  [FAIL] returned data when it should be unknown: $($res.Body)"
    } else {
        Write-Host "  /stats -> HTTP $($res.Code)  (503 expected: unknown, not zeros) [PASS]"
    }

    Write-Host ''
    Write-Host '=== case B: fresh snapshot present ==='
    $py1 = Join-Path $tmp 'w1.py'
    Set-Content -Path $py1 -Encoding UTF8 -Value @'
import sqlite3, time, sys
c = sqlite3.connect(sys.argv[1])
c.execute("""INSERT INTO driver_stats
 (id, ts, intercepts, rename_ok, rename_fail, delete_denied,
  notify_sent, notify_dropped, notify_queue_full, queue_depth, max_queue_depth)
 VALUES (1, ?, 100, 95, 3, 2, 90, 5, 1, 7, 42)
 ON CONFLICT(id) DO UPDATE SET ts=excluded.ts, intercepts=excluded.intercepts,
   delete_denied=excluded.delete_denied, max_queue_depth=excluded.max_queue_depth""",
 (time.time(),))
c.commit()
'@
    & python $py1 $db
    $res = Invoke-Stats $db
    if ($res.Ok -and $res.Body.intercepts -eq 100 -and $res.Body.delete_denied -eq 2) {
        Write-Host "  /stats -> $($res.Body | ConvertTo-Json -Compress)"
        Write-Host '  [PASS] real counters exposed'
    } else {
        Write-Host "  [FAIL] $($res.Body)"
    }

    Write-Host ''
    Write-Host '=== case C: stale snapshot (driver stopped answering) ==='
    $py2 = Join-Path $tmp 'w2.py'
    Set-Content -Path $py2 -Encoding UTF8 -Value @'
import sqlite3, time, sys
c = sqlite3.connect(sys.argv[1])
c.execute("UPDATE driver_stats SET ts = ? WHERE id = 1", (time.time() - 600,))
c.commit()
'@
    & python $py2 $db
    $res = Invoke-Stats $db
    if ($res.Ok) {
        Write-Host "  [FAIL] stale snapshot served as live data: $($res.Body)"
    } else {
        Write-Host "  /stats -> HTTP $($res.Code)  (503 expected: stale = offline) [PASS]"
    }

} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
