<#
    publish.ps1 - 把 iidxfps 一键发布到 GitHub

    用法（在仓库根目录执行）:
        powershell -ExecutionPolicy Bypass -File .\docs\publish.ps1
        powershell -ExecutionPolicy Bypass -File .\docs\publish.ps1 -Check      # 只做检查，不登录不推送
        powershell -ExecutionPolicy Bypass -File .\docs\publish.ps1 -Repo myname -Private

    参数:
        -Repo <名字>        仓库名，默认 iidxfps
        -Tag  <tag>         release 的 tag，默认 v1.0.0
        -Private            建私有仓库（默认公开）
        -Check              只做环境/内容检查，不登录、不推送
        -SkipRelease        只推仓库，不建 release
        -Proxy <url>        代理地址，例如 http://127.0.0.1:10808
                            不传时会自动读取 Windows 系统代理设置（国内直连 github.com 通常不通）
        -Git  <path>        指定 git.exe
        -Gh   <path>        指定 gh.exe
        -Zig  <path>        指定 zig.exe（用于编译）

    git / gh / zig 的查找顺序：参数 -> PATH -> <仓库>\tools -> <仓库>\..\tools
    没有的话脚本会打印下载地址（都是免安装 zip）:
        git : https://github.com/git-for-windows/git/releases   (MinGit-*-64-bit.zip)
        gh  : https://github.com/cli/cli/releases               (gh_*_windows_amd64.zip)
        zig : https://ziglang.org/download/                     (zig-x86_64-windows-*.zip)
#>
[CmdletBinding()]
param(
    [string]$Repo = 'iidxfps',
    [string]$Tag = 'v1.0.0',
    [switch]$Private,
    [switch]$Check,
    [switch]$SkipRelease,
    [string]$Proxy = '',
    [string]$Git = '',
    [string]$Gh = '',
    [string]$Zig = ''
)

$ErrorActionPreference = 'Stop'

function Info($m) { Write-Host "[*] $m" }
function Ok($m)   { Write-Host "[ok] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "[x] $m" -ForegroundColor Red; exit 1 }

try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# ---------------------------------------------------------------- 仓库根
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root 'build.bat'))) { Die "找不到仓库根（应该在 <仓库>\docs\publish.ps1）" }
Set-Location $root
Info "仓库根: $root"

# ---------------------------------------------------------------- 找 git / gh
function Find-Tool([string]$explicit, [string]$name, [string[]]$rel) {
    if ($explicit -and (Test-Path $explicit)) { return (Resolve-Path $explicit).Path }
    $onPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($r in $rel) {
        $p = Join-Path $root $r
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    return ''
}

$gitExe = Find-Tool $Git 'git.exe' @('tools\mingit\cmd\git.exe', '..\tools\mingit\cmd\git.exe')
$ghExe  = Find-Tool $Gh  'gh.exe'  @('tools\gh\bin\gh.exe', '..\tools\gh\bin\gh.exe')

# zig 可能在 tools\zig\zig.exe，也可能在 tools\zig\<版本>\zig.exe
$zigExe = Find-Tool $Zig 'zig.exe' @('tools\zig.exe', 'tools\zig\zig.exe', '..\tools\zig.exe', '..\tools\zig\zig.exe')
if (-not $zigExe) {
    foreach ($base in @((Join-Path $root 'tools'), (Join-Path (Split-Path -Parent $root) 'tools'))) {
        if (Test-Path $base) {
            $found = Get-ChildItem $base -Recurse -Depth 2 -Filter 'zig.exe' -ErrorAction SilentlyContinue |
                     Select-Object -First 1
            if ($found) { $zigExe = $found.FullName; break }
        }
    }
}
if ($zigExe) { $env:ZIG = $zigExe; Info "zig : $zigExe  ($(& $zigExe version))" }
else { Warn "找不到 zig.exe，编译步骤会失败（下载: https://ziglang.org/download/ ，或用 -Zig <path>）" }

if (-not $gitExe) {
    Warn "找不到 git.exe"
    Write-Host "     下载 MinGit（免安装）: https://github.com/git-for-windows/git/releases  ->  MinGit-*-64-bit.zip"
    Write-Host "     解压到 <仓库>\tools\mingit\ 或 <仓库>\..\tools\mingit\ ，或用 -Git <path> 指定"
}
if (-not $ghExe) {
    Warn "找不到 gh.exe"
    Write-Host "     下载 GitHub CLI（免安装）: https://github.com/cli/cli/releases  ->  gh_*_windows_amd64.zip"
    Write-Host "     解压到 <仓库>\tools\gh\ 或 <仓库>\..\tools\gh\ ，或用 -Gh <path> 指定"
}
if ($gitExe) { Info "git : $gitExe  ($(& $gitExe --version))" }
if ($ghExe)  { Info "gh  : $ghExe  ($((& $ghExe --version) -split "`n" | Select-Object -First 1))" }
if (-not $gitExe -or -not $ghExe) { if (-not $Check) { Die "缺少 git 或 gh，无法继续" } }

# ---------------------------------------------------------------- 初始化 git（刚解压出来的目录还没有 .git）
if ($gitExe -and -not (Test-Path (Join-Path $root '.git'))) {
    & $gitExe init -q -b main
    Info "已初始化本地 git 仓库"
}

# ---------------------------------------------------------------- 代理
if (-not $Proxy) {
    try {
        $reg = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction Stop
        if ($reg.ProxyEnable -eq 1 -and $reg.ProxyServer) {
            $server = $reg.ProxyServer
            if ($server -notmatch '^\w+://') { $server = "http://$server" }
            $Proxy = $server
            Info "从系统代理设置读到: $Proxy"
        }
    } catch { }
}
if ($Proxy) {
    $env:HTTP_PROXY = $Proxy
    $env:HTTPS_PROXY = $Proxy
    Info "已为 git / gh 设置代理: $Proxy（Go/C 程序不读系统代理，必须用环境变量）"
}

# ---------------------------------------------------------------- 检查仓库内容
Info "检查将要提交的内容 ..."
$staged = & $gitExe status --porcelain
if ($staged -match 'iidxfps\.dll|build/|dist/|_report\.txt|\.pdb') {
    Warn "git 看到了一些不该提交的文件:"
    $staged | Where-Object { $_ -match 'iidxfps\.dll|build/|dist/|_report\.txt|\.pdb' } | ForEach-Object { Write-Host "     $_" }
    Die "请先检查 .gitignore（正常情况下这些会被忽略）"
}
$files = (& $gitExe ls-files --others --exclude-standard --cached) | Sort-Object -Unique
if (-not $files) { $files = & $gitExe ls-files }
Info ("待提交文件 {0} 个" -f ($files | Measure-Object).Count)
$files | ForEach-Object { Write-Host "     $_" }

$sensitive = $files | Where-Object { $_ -match '^iidxfps\.ini$|_report\.txt$' }
if ($sensitive) { Warn "注意：以下文件看起来是运行期配置/报告，确认里面没有个人信息: $($sensitive -join ', ')" }

# 敏感串扫描（PCBID / 卡号 / 本机路径）；跳过本脚本自己，否则会匹配到下面的正则本身
$pat = '(?i)01A68379407A51B3B649|0120100000C25930A173|-p\s+[0-9A-F]{16,}|C:\\Users\\|E:\\IIDX'
$hit = $false
foreach ($f in $files) {
    if (-not (Test-Path $f)) { continue }
    if ($f -match 'publish\.ps1$') { continue }
    $m = Select-String -Path $f -Pattern $pat -ErrorAction SilentlyContinue
    if ($m) { $hit = $true; $m | ForEach-Object { Warn "敏感串? $($_.Path):$($_.LineNumber): $($_.Line.Trim())" } }
}
if (-not $hit) { Ok "敏感串扫描通过（没有 PCBID / 卡号 / 本机用户路径）" }

# ---------------------------------------------------------------- 产物 & release 包
if ($Check -and -not (Test-Path 'build\iidxfps.dll')) {
    Info "-Check 模式：尝试编译一次以确认能出活 ..."
    & cmd /c build.bat 2>&1 | Select-Object -Last 3
}

$zip = Join-Path (Split-Path -Parent $root) "iidxfps_$Tag.zip"
if (-not $Check) {
    if (-not (Test-Path 'build\iidxfps.dll')) {
        Info "编译 iidxfps.dll ..."
        & cmd /c build.bat | Out-Host
        if (-not (Test-Path 'build\iidxfps.dll')) { Die "编译失败" }
    }
    if (-not $SkipRelease) {
        Info "打包 release 附件 -> $zip"
        $stage = Join-Path $env:TEMP "iidxfps_release_$Tag"
        Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        Copy-Item 'build\iidxfps.dll' $stage
        Copy-Item 'docs\iidxfps.ini' (Join-Path $stage 'iidxfps.ini')
        Copy-Item 'docs\player-guide.zh-CN.txt' (Join-Path $stage '玩家说明.txt')
        Copy-Item 'docs\how-it-works.zh-CN.md' (Join-Path $stage '原理与逆向笔记.md')
        Remove-Item $zip -Force -ErrorAction SilentlyContinue
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
        Ok "附件: $zip ($((Get-Item $zip).Length) 字节)"
    }
}

if ($Check) {
    Ok "-Check 完成：环境与内容都没问题，去掉 -Check 就会真正发布"
    exit 0
}

# ---------------------------------------------------------------- 登录
& $ghExe auth status *> $null
if ($LASTEXITCODE -ne 0) {
    Info "需要先登录 GitHub：浏览器里输入 gh 给的一次性代码即可"
    & $ghExe auth login --hostname github.com --git-protocol https --web
    if ($LASTEXITCODE -ne 0) { Die "登录失败（代码过期就重跑一次本脚本）" }
}
$login = (& $ghExe api user --jq .login).Trim()
$mail = "$login@users.noreply.github.com"
Ok "已登录: $login"

# ---------------------------------------------------------------- 建仓库 + 提交 + 推送
$visibility = if ($Private) { '--private' } else { '--public' }
$desc = 'spice2x module that locks beatmania IIDX to its native 60/120 fps using QueryPerformanceCounter, with monitor-check refresh spoofing. Version independent, no bm2dx.dll patching.'

if (-not (Test-Path '.git')) { & $gitExe init -q -b main | Out-Null }
& $gitExe config user.name  $login
& $gitExe config user.email $mail
& $gitExe add -A

$hasCommit = $true
& $gitExe rev-parse --verify HEAD 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { $hasCommit = $false }

if ($hasCommit) {
    $dirty = & $gitExe status --porcelain
    if ($dirty) { & $gitExe commit -q -m "Update iidxfps" ; Info "已追加提交" }
} else {
    & $gitExe commit -q -m "iidxfps $Tag : lock beatmania IIDX to its native 60/120 fps (spice2x module)"
    Ok "已创建首个提交（作者: $login <$mail>）"
}
& $gitExe branch -M main

& $ghExe repo view "$login/$Repo" *> $null
$repoExists = ($LASTEXITCODE -eq 0)
if (-not $repoExists) {
    Info "创建仓库 $login/$Repo ($visibility) ..."
    & $ghExe repo create $Repo $visibility --description $desc --disable-wiki
    if ($LASTEXITCODE -ne 0) { Die "创建仓库失败" }
} else {
    Info "仓库 $login/$Repo 已存在，直接推送"
}
$remotes = & $gitExe remote
if (-not $remotes) { & $gitExe remote add origin "https://github.com/$login/$Repo.git" }
& $ghExe auth setup-git
Info "推送 main ..."
& $gitExe push -u origin main
if ($LASTEXITCODE -ne 0) { Die "推送失败" }
Ok "仓库: https://github.com/$login/$Repo"

# ---------------------------------------------------------------- release
if (-not $SkipRelease) {
    if (-not (Test-Path $zip)) { Die "找不到 release 附件: $zip" }
    & $ghExe release view $Tag --repo "$login/$Repo" *> $null
    if ($LASTEXITCODE -eq 0) {
        Warn "release $Tag 已存在，改为上传/覆盖附件"
        & $ghExe release upload $Tag $zip --clobber --repo "$login/$Repo"
    } else {
        $notes = @"
首个公开版本。

* 用 QueryPerformanceCounter 把游戏主循环锁到机台原生帧率（TDJ 120 / LDJ 60），不再跟着显示器刷新率跑
* 用 QPC 重写 timeGetTime / GetTickCount(64)
* 伪造 EnumDisplaySettings 上报的刷新率（Monitor Check / 适配器帧率）
* 运行时自动选择限速点；刷效率本来就正确时完全不介入
* iidxfps_report.txt 自包含状态报告，方便反馈问题

安装：把 iidxfps.dll 和 iidxfps.ini 放进游戏 contents 目录，启动参数加 `-k iidxfps.dll`。
有问题的玩家请发游戏目录里的 iidxfps_report.txt。
"@
        & $ghExe release create $Tag $zip --repo "$login/$Repo" `
            --title "iidxfps $Tag" --notes $notes
        if ($LASTEXITCODE -ne 0) { Die "创建 release 失败" }
    }
    Ok "Release: https://github.com/$login/$Repo/releases/tag/$Tag"
}

Write-Host ''
Ok "全部完成"
Info "仓库   : https://github.com/$login/$Repo"
if (-not $SkipRelease) { Info "Release: https://github.com/$login/$Repo/releases/tag/$Tag" }
Info "CI     : https://github.com/$login/$Repo/actions"
