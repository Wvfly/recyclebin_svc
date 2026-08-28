# deploy.ps1 - 部署 RecycleBin for SMB
# 以管理员 PowerShell 运行
# 用途: 创建暂存区目录、写入配置、安装驱动、注册核心服务(C)与管理 API(Go)
$ErrorActionPreference = "Stop"

$Root   = Split-Path -Parent $MyInvocation.MyCommand.Definition
$DriverDir = Join-Path $Root "driver"

# ============================================================
# 配置 (可按环境修改)
# ============================================================
# 受保护共享根 (DOS 形式), 部署时解析为 NT 卷路径写入驱动注册表
$ProtectedDos = @("D:\Share", "E:\Public")
# 用户态配置 (HKLM\SOFTWARE\RecycleBin)
$UserCfg = @{
    StoreRoot     = "C:\RBStore"      # 元数据库存放位置 (落地目标为同卷 $Recycle.Bin)
    QuotaMB       = 5120              # 每用户回收站配额 (MB)
    RetentionDays = 30                # 保留天数
    EnableRestApi = 1                 # 开启管理 REST API
    RestApiPort   = 8800
    RestApiToken  = "change-me"       # REST 访问令牌 (留空则不鉴权)
    DiskFreeMinMB = 5120              # 磁盘剩余水位 (MB), 低于则启动清理
}

# ============================================================
# 路径转换: D:\Share -> \Device\HarddiskVolumeN\Share
# 用 QueryDosDevice 获得盘符对应的 NT 设备名, 与驱动内
# FltGetFileNameInformation(NORMALIZED) 输出的路径形式一致。
# ============================================================
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class DosPath {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint QueryDosDevice(string lpDeviceName,
                                             StringBuilder lpTargetPath,
                                             uint ucchMax);
}
"@

function DosToNt([string]$dos) {
    if ($dos -match '^([A-Za-z]):\\(.*)$') {
        $drive = $Matches[1] + ":"
        $sb = New-Object System.Text.StringBuilder 1024
        [void][DosPath]::QueryDosDevice($drive, $sb, 1024)
        $dev = $sb.ToString()
        if ([string]::IsNullOrEmpty($dev)) {
            Write-Warning "无法解析盘符 $drive 的设备路径 (挂载点?), 原样使用: $dos"
            return $dos
        }
        # dev 形如 \Device\HarddiskVolume3; 组合成 NT 路径
        $rest = $Matches[2]
        if ($rest) { return "$dev\$rest" }
        return $dev
    }
    return $dos
}

function Ensure-RegKey([string]$path) {
    if (-not (Test-Path $path)) { New-Item -Force $path | Out-Null }
}

Write-Host "[1/5] 解析受保护路径并写入注册表"
$NtPaths = $ProtectedDos | ForEach-Object { DosToNt $_ }
$drvReg = "HKLM:\SYSTEM\CurrentControlSet\Services\rbminiflt\Parameters"
Ensure-RegKey $drvReg
Set-ItemProperty $drvReg "ProtectedPaths" ([string[]]$NtPaths) -Type MultiString
Write-Host "  ProtectedPaths:"
$NtPaths | ForEach-Object { Write-Host "    $_" }

$userReg = "HKLM:\SOFTWARE\RecycleBin"
Ensure-RegKey $userReg
Set-ItemProperty $userReg "ProtectedPaths" ([string[]]$ProtectedDos) -Type MultiString
foreach ($k in $UserCfg.Keys) {
    $v = $UserCfg[$k]
    if ($v -is [int]) { Set-ItemProperty $userReg $k $v -Type DWord }
    else              { Set-ItemProperty $userReg $k ([string]$v) -Type String }
}
Write-Host "  用户态配置已写入 $userReg"

Write-Host "[2/5] 创建暂存区根目录"
$storeRoot = $UserCfg["StoreRoot"]
if (-not (Test-Path $storeRoot)) {
    New-Item -ItemType Directory -Force $storeRoot | Out-Null
    # 隐藏 + 系统属性, 避免共享用户看到
    $attrib = (Get-ItemProperty -Path $storeRoot -Name Attributes -ErrorAction SilentlyContinue).Attributes
    Set-ItemProperty -Path $storeRoot -Name Attributes -Value ($attrib -bor 0x2 -bor 0x4)
}
Write-Host "  StoreRoot: $storeRoot"

Write-Host "[3/5] 安装内核驱动 (rbminiflt.inf)"
$inf = Join-Path $DriverDir "rbminiflt.inf"
# build.cmd 输出在 driver\ 根目录; VS 输出在 x64\Release\ 目录, 两者都兼容
$sys = Join-Path $DriverDir "rbminiflt.sys"
if (-not (Test-Path $sys)) {
    $alt = Join-Path $DriverDir "x64\Release\rbminiflt.sys"
    if (Test-Path $alt) { $sys = $alt }
}
if (Test-Path $sys) {
    Copy-Item $sys "$env:SystemRoot\system32\drivers\rbminiflt.sys" -Force
} else {
    Write-Warning "未找到 rbminiflt.sys, 请先编译驱动 (driver\build.cmd Release)"
}
# pnputil 从 INF 同目录解析源文件, 确保 sys 在 driver\ 下
pnputil /add-driver $inf /install | Out-Null
Write-Host "  驱动已注册 (Altitude 370030)"

Write-Host "[4/5] 启动驱动"
try { sc.exe start rbminiflt } catch { Write-Warning "驱动启动失败, 检查签名/测试模式" }

Write-Host "[5/6] 安装核心服务 rbservice.exe (C, SYSTEM 自启)"
$CsvcDir = Join-Path $Root "service_c"
$coreBin = Join-Path $CsvcDir "rbservice.exe"
if (-not (Test-Path $coreBin)) {
    Write-Warning "未找到 rbservice.exe, 请先编译 (service_c\build.cmd Release)"
} else {
    sc.exe create RecycleBinSvc binPath= "`"$coreBin`"" type= own start= auto obj= LocalSystem
    sc.exe description RecycleBinSvc "RecycleBin for SMB - 删除拦截核心服务 (落地/配额/还原)"
    # 崩溃后自动重启, 避免服务掉线产生无法落地的孤儿文件
    sc.exe failure RecycleBinSvc reset= 86400 actions= restart/5000/restart/10000/restart/30000
    sc.exe start RecycleBinSvc
}

Write-Host "[6/6] 安装管理 API rbapi.exe (Go, 可选)"
$ApiDir = Join-Path $Root "service_go"
$apiBin = Join-Path $ApiDir "rbapi.exe"
if (-not (Test-Path $apiBin)) {
    Write-Warning "未找到 rbapi.exe, 跳过 REST API (cd service_go; go build -o rbapi.exe .)"
} elseif ($UserCfg["EnableRestApi"] -ne 1) {
    Write-Host "  EnableRestApi=0, 跳过 REST API 安装"
} else {
    $port = $UserCfg["RestApiPort"]
    sc.exe create RecycleBinApi binPath= "`"$apiBin`" --addr 127.0.0.1:$port" type= own start= auto obj= LocalSystem
    sc.exe description RecycleBinApi "RecycleBin for SMB - 管理 REST API"
    sc.exe failure RecycleBinApi reset= 86400 actions= restart/5000/restart/10000/restart/30000
    sc.exe start RecycleBinApi
    Write-Host "  REST API 已启动: http://127.0.0.1:$port"
}

Write-Host "部署完成。"
Write-Host ""
Write-Host "验证:"
Write-Host "  sc.exe query RecycleBinSvc"
Write-Host "  sc.exe query RecycleBinApi"
Write-Host "  Invoke-RestMethod http://127.0.0.1:$($UserCfg['RestApiPort'])/health -Headers @{'X-Auth-Token'='$($UserCfg['RestApiToken'])'}"
