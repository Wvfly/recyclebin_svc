# deploy.ps1 - 部署 RecycleBin for SMB
# 以管理员 PowerShell 运行
# 用途: 创建暂存区目录、安装驱动、注册用户态服务

$ErrorActionPreference = "Stop"

$Root   = Split-Path -Parent $MyInvocation.MyCommand.Definition
$DriverDir = Join-Path $Root "driver"
$SvcDir    = Join-Path $Root "service"

# 受保护共享根 (DOS 形式), 部署时解析为 NT 卷路径写入注册表
$ProtectedDos = @("D:\Share", "E:\Public")

function DosToNt($dos) {
    # D:\Share -> \Device\HarddiskVolumeN\Share
    if ($dos -match '^([A-Za-z]):\\(.*)$') {
        $drive = $Matches[1] + ":"
        $rest  = $Matches[2]
        $vol = (Get-Item "FileSystem::$drive").Target
        # Target 形如 \\?\Volume{guid}\
        $dev = (Get-CimInstance Win32_Volume | Where-Object { $_.DriveLetter -eq $drive } |
                Select-Object -First 1).DeviceID
        # DeviceID 形如 \\?\Volume{guid}\
        # 转为 \Device\HarddiskVolumeN\ 需查询; 简化用 GLOBALROOT 形式
        $nt = "\GLOBALROOT$dev$rest" -replace '\\\\', '\'
        return $nt
    }
    return $dos
}

Write-Host "[1/4] 解析受保护路径并写入注册表"
$NtPaths = $ProtectedDos | ForEach-Object { DosToNt $_ }
$regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\rbminiflt\Parameters"
if (-not (Test-Path $regPath)) { New-Item -Force $regPath | Out-Null }
Set-ItemProperty $regPath "ProtectedPaths" $NtPaths -Type MultiString

Write-Host "[2/4] 安装内核驱动 (rbminiflt.inf)"
$inf = Join-Path $DriverDir "rbminiflt.inf"
$sys = Join-Path $DriverDir "x64\Release\rbminiflt.sys"
if (Test-Path $sys) {
    Copy-Item $sys "$env:SystemRoot\system32\drivers\rbminiflt.sys" -Force
} else {
    Write-Warning "未找到 rbminiflt.sys, 请先编译驱动 (VS + WDK)"
}
pnputil /add-driver $inf /install | Out-Null
Write-Host "  驱动已注册 (Altitude 370030)"

Write-Host "[3/4] 启动驱动"
try { sc.exe start rbminiflt } catch { Write-Warning "驱动启动失败, 检查签名/测试模式" }

Write-Host "[4/4] 注册用户态服务为 SYSTEM 自启"
$py = (Get-Command python).Source
$svcBin = Join-Path $SvcDir "rb_service.py"
sc.exe create RecycleBinSvc binPath= "`"$py`" `"$svcBin`" run" type= own start= auto obj= LocalSystem
sc.exe description RecycleBinSvc "RecycleBin for SMB - 远程删除拦截回收站服务"
sc.exe start RecycleBinSvc

Write-Host "部署完成。"
