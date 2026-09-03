# RB-33 diagnostic: determine whether Explorer/SMB deletes flow through
# FileDispositionInformation (intercepted by RbfPreSetInfo) or through
# FILE_DELETE_ON_CLOSE (intercepted by RbfPreCleanup).
#
# Run from an elevated PowerShell. It is read-only apart from creating and
# deleting two tiny test files.

$ErrorActionPreference = 'Continue'

$share = 'E:\tmp\share'
$unc   = '\\10.88.36.171\share'

function Get-MaxId {
    $raw = (Invoke-WebRequest "http://127.0.0.1:8800/items").Content
    $items = $raw | ConvertFrom-Json
    # /items is token-protected in the default deployment. If we got an error
    # object instead of a list, say so loudly instead of silently reporting 0.
    if ($items -isnot [array]) {
        Write-Host "  [WARN] /items did not return a list (auth?). Raw: $($raw.Substring(0, [Math]::Min(200, $raw.Length)))"
        return 0
    }
    if ($items.Count -eq 0) { return 0 }
    ($items | Measure-Object -Property id -Maximum).Maximum
}

function Show-Stats($label) {
    $s = (Invoke-WebRequest "http://127.0.0.1:8800/stats").Content | ConvertFrom-Json
    Write-Host ("  [{0}] intercepts={1} rename_ok={2} rename_fail={3} delete_denied={4} protected_count={5}" -f `
        $label, $s.intercepts, $s.rename_ok, $s.rename_fail, $s.delete_denied, $s.protected_count)
    if ($s.protected_count -eq 0) {
        Write-Host '  [FATAL] protected_count=0 => driver protects NOTHING, every delete is forwarded'
    }
}

Write-Host '===== RB-33 diagnostic ====='
Write-Host ''
Write-Host '--- 0. driver state ---'
fltmc | findstr rbminiflt
(Get-Item C:\Windows\System32\drivers\rbminiflt.sys).LastWriteTime

Write-Host ''
Write-Host '--- 1. baseline ---'
Show-Stats 'before'
$id0 = Get-MaxId
Write-Host "  max_id = $id0"

# ------------------------------------------------------------------
# Test A: command-line delete over the LOCAL path (disposition path)
# ------------------------------------------------------------------
Write-Host ''
Write-Host '--- 2. Test A: del over LOCAL path (E:\tmp\share) ---'
$fA = Join-Path $share 'diagA_local.txt'
Set-Content -Path $fA -Value 'diagA' -Encoding ASCII
Write-Host "  created, exists = $(Test-Path $fA)"
cmd /c "del /f /q `"$fA`""
Start-Sleep -Seconds 1
Write-Host "  after del: still in place = $(Test-Path $fA)"
Show-Stats 'after A'
$idA = Get-MaxId
Write-Host "  max_id = $idA  (delta = $($idA - $id0))"

# ------------------------------------------------------------------
# Test B: command-line delete over the UNC path (server-side IRPs)
# ------------------------------------------------------------------
Write-Host ''
Write-Host '--- 3. Test B: del over UNC path (\\10.88.36.171\share) ---'
$fB = Join-Path $unc 'diagB_unc.txt'
Set-Content -Path $fB -Value 'diagB' -Encoding ASCII
Write-Host "  created, exists = $(Test-Path $fB)"
cmd /c "del /f /q `"$fB`""
Start-Sleep -Seconds 1
Write-Host "  after del: still in place = $(Test-Path $fB)"
Show-Stats 'after B'
$idB = Get-MaxId
Write-Host "  max_id = $idB  (delta = $($idB - $idA))"

# ------------------------------------------------------------------
# Test C: instructions for the Explorer delete
# ------------------------------------------------------------------
Write-Host ''
Write-Host '--- 4. Test C: Explorer delete (MANUAL) ---'
$fC = Join-Path $unc 'diagC_explorer.txt'
Set-Content -Path $fC -Value 'diagC' -Encoding ASCII
Write-Host "  created: $fC"
Write-Host '  >>> NOW: open Explorer, browse to \\10.88.36.171\share,'
Write-Host '  >>> select diagC_explorer.txt and press Delete.'
Write-Host ''
Read-Host '  Press Enter AFTER you have deleted it in Explorer'
Start-Sleep -Seconds 1
Write-Host "  after Explorer delete: still in place = $(Test-Path $fC)"
Show-Stats 'after C'
$idC = Get-MaxId
Write-Host "  max_id = $idC  (delta = $($idC - $idB))"

# ------------------------------------------------------------------
Write-Host ''
Write-Host '--- 5. RBStore tail (staged files) ---'
if (Test-Path 'E:\RBStore') {
    Get-ChildItem -Recurse -File 'E:\RBStore' |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 10 FullName
} else {
    Write-Host '  E:\RBStore not found'
}

Write-Host ''
Write-Host '===== interpretation ====='
Write-Host '  Test A (local del)  : delta>0 => disposition interception works'
Write-Host '  Test B (UNC del)    : delta>0 => server-side disposition works'
Write-Host '  Test C (Explorer)   : delta=0 and file gone => NOT intercepted (data loss!)'
Write-Host '                        delta=0 and file present => DOC was stripped, refused'
Write-Host ''
