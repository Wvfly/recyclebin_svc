# gen_token.ps1 - generate a strong random API token
# run in normal PowerShell; use -Set to write the registry (needs admin)
#
# usage:
#   .\gen_token.ps1                  generate and print a token
#   .\gen_token.ps1 -Set             generate and write registry RestApiToken
#   .\gen_token.ps1 -Bytes 32 -Format hex
param(
  [int]$Bytes = 32,
  [ValidateSet('base64','hex')]
  [string]$Format = 'base64',
  [switch]$Set,
  [string]$RegKey   = 'HKLM:\SOFTWARE\RecycleBin',
  [string]$RegValue = 'RestApiToken'
)

$ErrorActionPreference = "Stop"

# use the system CSP for strong randomness, not Get-Random (seedable)
$raw = [byte[]]::new($Bytes)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($raw)
$rng.Dispose()

$token = if ($Format -eq 'hex') {
  ($raw | ForEach-Object { $_.ToString('x2') }) -join ''
} else {
  [Convert]::ToBase64String($raw)
}

if ($Set) {
  $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  if (-not $admin) {
    Write-Error "writing the registry requires admin; run PowerShell as Administrator"
    exit 1
  }
  if (-not (Test-Path $RegKey)) { New-Item -Path $RegKey -Force | Out-Null }
  New-ItemProperty -Path $RegKey -Name $RegValue -Value $token `
    -PropertyType String -Force | Out-Null
  Write-Host "wrote $RegKey\$RegValue"
  Write-Host "restart the service to apply: Restart-Service rbapi"
}

# print just the token last, easy to copy
Write-Output $token
