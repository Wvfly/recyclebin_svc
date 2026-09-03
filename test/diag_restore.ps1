# Diagnose why restore fails with "move failed (win32=5)" (ERROR_ACCESS_DENIED).
# Checks: service identity, source file existence/attributes, target dir writability.

$ErrorActionPreference = 'Continue'

Write-Host '===== restore failure diagnosis ====='
Write-Host ''

# --- 1. Who is rbservice running as? ---
Write-Host '--- 1. rbservice process identity ---'
$svc = Get-CimInstance Win32_Service -Filter "Name='rbservice'" -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host ("  StartName : {0}" -f $svc.StartName)
    Write-Host ("  State     : {0}" -f $svc.State)
} else {
    Write-Host '  rbservice service NOT FOUND'
}

Write-Host ''

# --- 2. The item we are trying to restore (id=2) ---
Write-Host '--- 2. item id=2 record ---'
try {
    $resp = (Invoke-WebRequest "http://127.0.0.1:8800/items/2").Content | ConvertFrom-Json
    $it = $resp
    if ($resp.item) { $it = $resp.item }
    if ($resp.data) { $it = $resp.data }
    Write-Host ("  orig_path  : {0}" -f $it.orig_path)
    Write-Host ("  store_path : {0}" -f $it.store_path)
    Write-Host ("  status     : {0}" -f $it.status)
    $storePath = $it.store_path
    $origPath  = $it.orig_path
} catch {
    Write-Host ("  could not read /items/2 : {0}" -f $_.Exception.Message)
    $storePath = $null
    $origPath  = $null
}

Write-Host ''

# --- 3. Does the staging source file physically exist? ---
Write-Host '--- 3. staged source file on disk ---'
if ($storePath) {
    # store_path is NT form (\Device\HarddiskVolume4\RBStore\...) -> find the DOS form
    $candidates = Get-ChildItem -Recurse -File E:\RBStore -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -like '*12.30*' }
    if ($candidates) {
        foreach ($c in $candidates) {
            Write-Host ("  FOUND : {0}" -f $c.FullName)
            Write-Host ("  attrs : {0}  size={1}" -f $c.Attributes, $c.Length)
        }
    } else {
        Write-Host '  NO staged file matching *12.30* under E:\RBStore'
        Write-Host '  (this is why StoreLandItem also stays silent -> item never reaches landed)'
    }
}

Write-Host ''

# --- 4. Is the restore target directory writable / does it exist? ---
Write-Host '--- 4. restore target ---'
$target = 'E:\tmp\share\测试'
Write-Host ("  exists : {0}" -f (Test-Path $target))
if (Test-Path $target) {
    $acl = Get-Acl $target -ErrorAction SilentlyContinue
    if ($acl) { Write-Host ("  owner  : {0}" -f $acl.Owner) }
}

Write-Host ''

# --- 5. Is a file with the same name already in the target? ---
Write-Host '--- 5. name collision at target ---'
$collide = Join-Path $target '12.30.txt'
Write-Host ("  {0} exists : {1}" -f $collide, (Test-Path $collide))

Write-Host ''
Write-Host '===== end ====='
