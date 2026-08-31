# deploy.ps1 - 部署 RecycleBin for SMB
# 以管理员 PowerShell 运行
# 用途: 创建暂存区目录、写入配置、安装驱动、注册核心服务(C)与管理 API(Go)
#
# 用法1 (推荐): 先运行 build_all.cmd 收集产物, 然后把整个 target\Release
#               目录拷贝到目标机, 在其中以管理员 PowerShell 运行 .\deploy.ps1。
#               所有文件与脚本同目录, 无需额外复制。
# 用法2 (兼容): 在仓库根运行 .\deploy.ps1, 自动从 target\ 或源码目录找二进制。
$ErrorActionPreference = "Stop"

# $PSScriptRoot 始终是脚本所在目录的绝对路径, 与调用方式无关
# (dot-source / -File 绝对或相对路径 / 经 PATH 调用均安全)
$Root   = $PSScriptRoot

# ============================================================
# 配置 (可按环境修改)
# ============================================================
# ============================================================
# !! 重要: StoreRoot 必须与下面所有受保护共享在【同一卷】!!
#
# 内核 rename 不能跨卷。StoreRoot 放错卷 => 重定向失败 => 驱动
# fail-open 放行真实删除 => 文件直接丢失且回收站里没有。
# 脚本开头的 [0/6] 步骤会强制校验这一点, 不匹配直接中止。
#
# 以下是自洽的示例 (同在 D: 卷), 请按实际环境修改。
# 多卷保护当前不支持 (需驱动改造为"拷贝+删源")。
# ============================================================
# 受保护共享根 (DOS 形式), 部署时解析为 NT 卷路径写入驱动注册表
$ProtectedDos = @("E:\tmp\share")

# 暂存失败时的策略 (RB-04)
#   1 = 拒绝删除, 保住数据 (默认, 生产推荐)
#   0 = 放行真删, 文件永久丢失 —— 仅作为暂存区故障时的应急旁路
$FailClosed = 1

# 用户态配置 (HKLM\SOFTWARE\RecycleBin)
$UserCfg = @{
    StoreRoot     = "E:\RBStore"      # 必须与 $ProtectedDos 同卷!
    QuotaMB       = 5120              # 每用户回收站配额 (MB)
    RetentionDays = 30                # 保留天数
    EnableRestApi = 1                 # 开启管理 REST API
    RestApiPort   = 8800
    RestApiToken  = ""                # REST 访问令牌 —— 留空则不鉴权 (本机部署建议), 设值则启用 token 校验
    DiskFreeMinMB = 5120              # 磁盘剩余水位 (MB), 低于则启动清理
}

# ============================================================
# 【关键校验】StoreRoot 必须与所有受保护共享在同一卷
#
# 内核 FileRenameInformation 不支持跨卷。StoreRoot 放错卷会导致:
#   rename 失败 -> 驱动 fail-open -> 文件被真删且回收站里没有
# 这是静默数据丢失, 用户在客户端完全无感, 只能从驱动统计的
# rename_fail 计数发现。所以必须在部署期就拦住。
# ============================================================
function Get-DriveRoot([string]$path) {
    # "D:\Share\Sub" -> "D:"; "D:" -> "D:"; 无盘符则返回 $null
    if ($path -match '^([A-Za-z]):') { return ($Matches[1] + ":").ToUpper() }
    return $null
}

# 查找部署文件: 优先与脚本同目录 (target 自包含部署包), 找不到再回退到
# 仓库根工作流 (target 收集目录 / 源码目录)
function Find-File([string]$name) {
    $here = Join-Path $Root $name
    if (Test-Path $here) { return $here }
    $candidates = @(
        (Join-Path $Root "target\Release\$name"),
        (Join-Path $Root "target\Debug\$name"),
        (Join-Path $Root "driver\$name"),
        (Join-Path $Root "service_c\$name"),
        (Join-Path $Root "service_go\$name")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

Write-Host "[0/6] 校验卷一致性 (StoreRoot 必须与受保护共享同卷)"
$storeDrive = Get-DriveRoot $UserCfg.StoreRoot
if (-not $storeDrive) {
    Write-Error "StoreRoot '$($UserCfg.StoreRoot)' 未包含盘符, 无法校验卷。"
    exit 1
}

$badPaths = @()
foreach ($p in $ProtectedDos) {
    $d = Get-DriveRoot $p
    if (-not $d) {
        Write-Error "受保护路径 '$p' 未包含盘符, 无法校验卷。"
        exit 1
    }
    if ($d -ne $storeDrive) { $badPaths += "$p (卷 $d)" }
}

if ($badPaths.Count -gt 0) {
    Write-Host ""
    Write-Host "  [X] 配置错误: StoreRoot 与受保护共享不在同一卷" -ForegroundColor Red
    Write-Host ""
    Write-Host "      StoreRoot      = $($UserCfg.StoreRoot)  (卷 $storeDrive)"
    Write-Host "      受保护路径     = $($ProtectedDos -join ', ')"
    Write-Host "      卷不匹配       = $($badPaths -join '; ')"
    Write-Host ""
    Write-Host "  原因: 内核 rename 不能跨卷。当前配置下重定向必定失败,"
    Write-Host "        驱动会 fail-open 放行真实删除 —— 文件直接丢失且"
    Write-Host "        回收站里没有, 客户端完全无感。"
    Write-Host ""
    # 建议方向: 以"用户想保护的卷"为准, 而不是把共享挪到 StoreRoot 所在卷。
    # 找出受保护路径涉及的卷
    $shareDrives = @()
    foreach ($p in $ProtectedDos) {
        $d = Get-DriveRoot $p
        if ($d -and ($shareDrives -notcontains $d)) { $shareDrives += $d }
    }

    Write-Host "  修复: 把 StoreRoot 移到受保护共享所在卷, 并且只保护该卷。"
    Write-Host ""

    if ($shareDrives.Count -eq 1) {
        # 单一卷: 给出可直接照抄的配置
        $target = $shareDrives[0]
        Write-Host ("      把 StoreRoot 改到 " + $target + " 卷:")
        Write-Host ('          $UserCfg.StoreRoot = "' + $target + '\RBStore"')
    } else {
        # 多卷: 只能选一个, 逐个列出选项
        Write-Host ("      受保护路径分布在 " + $shareDrives.Count +
                    " 个卷, 但 StoreRoot 只能落在其中一个。请选择要保护的卷:")
        foreach ($d in $shareDrives) {
            $keep = @($ProtectedDos | Where-Object { (Get-DriveRoot $_) -eq $d })
            Write-Host ("        选项 " + $d + ":")
            Write-Host ('          $ProtectedDos   = @("' + ($keep -join '", "') + '")')
            Write-Host ('          $UserCfg.StoreRoot = "' + $d + '\RBStore"')
        }
    }

    Write-Host ""
    Write-Host "  注: 多卷同时保护需要驱动改造 (拷贝+删源), 当前版本不支持。"
    Write-Host "      若确实需要多卷, 请分多次部署或先做驱动改造。"
    Write-Host ""
    exit 1
}

Write-Host "  OK: StoreRoot (卷 $storeDrive) 与所有受保护共享同卷"

# REST 令牌提示 (不阻断: API 只监听 127.0.0.1)
if ($UserCfg["EnableRestApi"] -eq 1) {
    $tok = [string]$UserCfg["RestApiToken"]
    if ($tok -eq "") {
        Write-Host ""
        Write-Host "  [i] RestApiToken 为空: REST API 未启用鉴权 (仅本机 127.0.0.1, 可接受)" -ForegroundColor Cyan
        Write-Host ""
    } elseif ($tok -eq "change-me") {
        Write-Host ""
        Write-Host "  [!] 警告: RestApiToken 仍为默认值, 建议改成强随机值" -ForegroundColor Yellow
        Write-Host ""
    }
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

Write-Host "[1/6] 解析受保护路径并写入注册表"
$NtPaths = $ProtectedDos | ForEach-Object { DosToNt $_ }
$drvReg = "HKLM:\SYSTEM\CurrentControlSet\Services\rbminiflt\Parameters"
Ensure-RegKey $drvReg
Set-ItemProperty $drvReg "ProtectedPaths" ([string[]]$NtPaths) -Type MultiString
Write-Host "  ProtectedPaths:"
$NtPaths | ForEach-Object { Write-Host "    $_" }

# RB-04: 无法暂存时拒绝删除, 而不是静默真删
Set-ItemProperty $drvReg "FailClosed" $FailClosed -Type DWord
if ($FailClosed -eq 1) {
    Write-Host "  FailClosed = 1 (暂存失败时拒绝删除, 数据不丢)"
} else {
    Write-Host ""
    Write-Host "  [!] 警告: FailClosed=0, 暂存失败将放行真删 (数据永久丢失)" -ForegroundColor Yellow
}

$userReg = "HKLM:\SOFTWARE\RecycleBin"
Ensure-RegKey $userReg
Set-ItemProperty $userReg "ProtectedPaths" ([string[]]$ProtectedDos) -Type MultiString
foreach ($k in $UserCfg.Keys) {
    $v = $UserCfg[$k]
    if ($v -is [int]) { Set-ItemProperty $userReg $k $v -Type DWord }
    else              { Set-ItemProperty $userReg $k ([string]$v) -Type String }
}
Write-Host "  用户态配置已写入 $userReg"

Write-Host "[2/6] 创建暂存区根目录"
$storeRoot = $UserCfg["StoreRoot"]
if (-not (Test-Path $storeRoot)) {
    New-Item -ItemType Directory -Force $storeRoot | Out-Null
}
# 隐藏 + 系统属性, 避免共享用户看到。
# 注意: PowerShell 的 Set-ItemProperty 对"目录"不支持设置 Attributes
# (仅文件有效), 旧写法会导致部署在 [2/6] 中断。改用 .NET
# File.SetAttributes, 对目录同样有效, 且目录已存在时重跑也能幂等补上。
try {
    [System.IO.File]::SetAttributes($storeRoot,
        [System.IO.FileAttributes]::Hidden -bor [System.IO.FileAttributes]::System)
} catch {
    Write-Warning "设置暂存区隐藏/系统属性失败: $($_.Exception.Message)"
}
Write-Host "  StoreRoot: $storeRoot"

Write-Host "[3/6] 安装内核驱动 (rbminiflt.inf)"
$inf = Find-File "rbminiflt.inf"
$sys = Find-File "rbminiflt.sys"
if ($sys) {
    Copy-Item $sys "$env:SystemRoot\system32\drivers\rbminiflt.sys" -Force
    # pnputil 从 INF 同目录解析源文件; 若 sys 不在 INF 同目录则先同步过去
    if ($inf -and ((Split-Path $sys) -ne (Split-Path $inf))) {
        Copy-Item $sys (Join-Path (Split-Path $inf) "rbminiflt.sys") -Force
    }
    Write-Host "  驱动二进制: $sys"
} else {
    Write-Warning "未找到 rbminiflt.sys, 请先运行 build_all.cmd Release"
}
if ($inf) {
    # 不要吞掉 pnputil 输出: 未签名驱动 + 未开启 testsigning 时这里会
    # "Access is denied" 而静默失败, 导致 [4/6] sc start 报 1060 (服务不存在)。
    pnputil /add-driver $inf /install
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "pnputil 安装驱动失败 (exit $LASTEXITCODE): 第三方 INF 无数字签名,"
        Write-Warning "  setupapi 的签名检查不受 testsigning 豁免 (testsigning 只豁免内核加载器)。"
        Write-Warning "  回退: 按 legacy 文件系统驱动方式直接注册服务 (testsigning 已开启时内核可加载未签名驱动)。"
        # 确保 sys 就位 (复制到 system32\drivers 这一步上面已做, 这里兜底)
        if (-not (Test-Path "$env:SystemRoot\system32\drivers\rbminiflt.sys")) {
            if ($sys) { Copy-Item $sys "$env:SystemRoot\system32\drivers\rbminiflt.sys" -Force }
        }
        # 服务已存在则跳过创建 (重跑场景), 否则创建 (模拟 INF 的 Services 段)
        sc.exe query rbminiflt | Out-Null
        if ($LASTEXITCODE -ne 0) {
            sc.exe create rbminiflt type= filesys start= system error= normal `
                binPath= "\SystemRoot\system32\drivers\rbminiflt.sys" depend= FltMgr | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "  sc create rbminiflt 失败 (exit $LASTEXITCODE), 驱动未注册"
            }
        }
        # Mini-Filter 注册信息 (模拟 INF 的 [HKEY...Services\rbminiflt] 节)
        $svcKey = "HKLM:\SYSTEM\CurrentControlSet\Services\rbminiflt"
        if (-not (Test-Path $svcKey)) { New-Item -Force $svcKey | Out-Null }
        Set-ItemProperty $svcKey Altitude "370030" -Type String
        Set-ItemProperty $svcKey Flags 0 -Type DWord
        Set-ItemProperty $svcKey Group "FSFilter Activity Monitor" -Type String
        # Mini-Filter 实例注册: FltMgr 要求 Services\rbminiflt\Instances\<实例>\
        # 存在 Altitude/Flags, 否则 FltRegisterFilter 返回 0xC0000034
        # (STATUS_OBJECT_NAME_NOT_FOUND), sc start 表现为 exit 2。
        $instKey = "$svcKey\Instances"
        if (-not (Test-Path $instKey)) { New-Item -Force $instKey | Out-Null }
        Set-ItemProperty $instKey "DefaultInstance" "RecycleBin Instance" -Type String
        $instChild = "$instKey\RecycleBin Instance"
        if (-not (Test-Path $instChild)) { New-Item -Force $instChild | Out-Null }
        Set-ItemProperty $instChild Altitude "370030" -Type String
        Set-ItemProperty $instChild Flags 0 -Type DWord
        # Parameters 由 [1/6] 写入; 若服务键被删过导致丢失则补写
        $parKey = "$svcKey\Parameters"
        if (-not (Test-Path $parKey)) { New-Item -Force $parKey | Out-Null }
        Set-ItemProperty $parKey "ProtectedPaths" ([string[]]$NtPaths) -Type MultiString
        Set-ItemProperty $parKey "FailClosed" $FailClosed -Type DWord
        Write-Host "  驱动已按 legacy 方式注册 (Altitude 370030, testsigning)"
    } else {
        Write-Host "  驱动已注册 (Altitude 370030)"
    }
} else {
    Write-Warning "未找到 rbminiflt.inf, 跳过驱动注册"
}

Write-Host "[4/6] 启动驱动"
sc.exe start rbminiflt | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Warning "驱动启动失败 (exit $LASTEXITCODE), 检查签名/测试模式"
}

Write-Host "[5/6] 安装核心服务 rbservice.exe (C, SYSTEM 自启)"
$coreBin = Find-File "rbservice.exe"
if (-not $coreBin) {
    Write-Warning "未找到 rbservice.exe, 请先运行 build_all.cmd Release"
} else {
    Write-Host "  服务二进制: $coreBin"
    sc.exe create RecycleBinSvc binPath= "`"$coreBin`"" type= own start= auto obj= LocalSystem
    sc.exe description RecycleBinSvc "RecycleBin for SMB - 删除拦截核心服务 (落地/配额/还原)"
    # 崩溃后自动重启, 避免服务掉线产生无法落地的孤儿文件
    sc.exe failure RecycleBinSvc reset= 86400 actions= restart/5000/restart/10000/restart/30000
    sc.exe start RecycleBinSvc
}

Write-Host "[6/6] 安装管理 API rbapi.exe (Go, 可选)"
$apiBin = Find-File "rbapi.exe"
if (-not $apiBin) {
    Write-Warning "未找到 rbapi.exe, 跳过 REST API (先运行 build_all.cmd Release, 或 cd service_go; go build -o rbapi.exe .)"
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

# =====================================================================
# [7/6] RB-29: prove the interception path is actually LIVE.
#
# "Driver RUNNING" is NOT proof that deletes are being intercepted.
# On 2026-08-31 a deploy finished at 17:31:53 while the filter was not
# yet filtering the data volume; a real user delete at 17:36:32 went
# straight to DELETE_CLOSE (USN) and the file was lost forever -- with
# no log, no counter and no error anywhere.
#
# So we now prove it end to end: drop a file into a protected path,
# delete it, and require the driver to have STAGED it. Three outcomes:
#   staged            -> protection confirmed
#   probe still there -> delete was refused (fail-closed; data safe,
#                        but the feature is not working)
#   probe gone, not staged -> REAL DELETE. The share is unprotected.
# =====================================================================
Write-Host "[7/6] 拦截链路冒烟验证 (RB-29)"
$smokeOk  = $false
$smokeMsg = "skipped"
$probePath = $null
try {
    if (-not $ProtectedDos -or $ProtectedDos.Count -eq 0) {
        $smokeMsg = "未配置受保护路径"
    } else {
        $shareRoot = $ProtectedDos[0]
        if (-not (Test-Path -LiteralPath $shareRoot)) {
            $smokeMsg = "受保护路径不存在: $shareRoot"
        } else {
            $storeRoot = Join-Path ($shareRoot.Substring(0, 2)) 'RBStore'
            $probeName = 'rb_deploy_probe_{0}.txt' -f (Get-Date -Format 'HHmmss')
            $probePath = Join-Path $shareRoot $probeName

            Set-Content -LiteralPath $probePath -Value 'deploy probe' -Encoding ASCII
            if (-not (Test-Path -LiteralPath $probePath)) {
                $smokeMsg = "无法在 $shareRoot 创建探针文件"
            } else {
                Remove-Item -LiteralPath $probePath -Force
                Start-Sleep -Seconds 3

                $staged = $null
                if (Test-Path -LiteralPath $storeRoot) {
                    $staged = Get-ChildItem -LiteralPath $storeRoot -Recurse -Force `
                                  -Filter ('*' + $probeName) -ErrorAction SilentlyContinue |
                              Select-Object -First 1
                }

                if ($staged) {
                    $smokeOk  = $true
                    $smokeMsg = "拦截生效 - 探针已暂存至 $($staged.FullName)"
                } elseif (Test-Path -LiteralPath $probePath) {
                    $smokeMsg = "删除被拒绝 (fail-closed, 数据未丢, 但功能异常)"
                } else {
                    $smokeMsg = "探针被真删且未暂存 - 驱动未在保护 $shareRoot"
                }
            }
        }
    }
} catch {
    $smokeMsg = "异常: " + $_.Exception.Message
}

if ($smokeOk) {
    Write-Host "  [OK] $smokeMsg" -ForegroundColor Green
} else {
    Write-Host "  ########################################################" -ForegroundColor Red
    Write-Host "  #  警告: 删除拦截未生效" -ForegroundColor Red
    Write-Host "  #  此时删除受保护共享中的文件将【永久丢失】" -ForegroundColor Red
    Write-Host "  #  $smokeMsg" -ForegroundColor Red
    Write-Host "  ########################################################" -ForegroundColor Red
}

# never leave the probe behind in the share
if ($probePath -and (Test-Path -LiteralPath $probePath)) {
    Remove-Item -LiteralPath $probePath -Force -ErrorAction SilentlyContinue
}

Write-Host "部署完成。"
if (-not $smokeOk) {
    Write-Host "(但拦截冒烟验证未通过 - 见上方 [7/6])" -ForegroundColor Red
}
Write-Host ""
Write-Host "验证:"
Write-Host "  sc.exe query RecycleBinSvc"
Write-Host "  sc.exe query RecycleBinApi"
Write-Host "  Invoke-RestMethod http://127.0.0.1:$($UserCfg['RestApiPort'])/health -Headers @{'X-Auth-Token'='$($UserCfg['RestApiToken'])'}"
