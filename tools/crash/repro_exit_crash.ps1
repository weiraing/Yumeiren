# Qt6Widgets 退出崩溃复现器(任务书 5.2 退出场景)
#
# 假设：崩溃只发生在“退出时视频管线仍存活”的路径上 —— 函数内静态单例的析构由
# CRT atexit 链在 main() 返回之后触发，此时 ~QApplication 已执行完，
# QCoreApplication::self 归零；VideoWallpaper::~VideoWallpaper -> stopAll ->
# teardownOutputs -> fbswin::unmountWindow(QWidget*) 仍调用 QWidget 接口，
# 落到 Qt6Widgets 内部对 qApp 的空指针虚调用 -> c0000005。
#
# 两组对照用同一二进制：
#   默认        = “播放中退出”(预期 c0000005 + 按 PID 命名的 WER dump)
#   -NoPlayback = “从未起播退出”(预期干净退出)
#
# 判据一律不依赖系统时钟(本机时钟出现过非单调跳变)：
#   存活 = PID；崩溃 = dump 文件名内嵌 PID + 进程退出码；起播 = 日志内容 play index=。
param(
    [string]$Exe = "C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\YumeirenTest.exe",
    [string]$Playlist = "C:/Users/rain/Documents/ExplorerBg/FolderBgStudio/build/media/video/inv/inv_1080p30.mp4",
    [int]$PlayWaitSec = 15,
    [string]$DumpDir = "C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\tools\dumps",
    [switch]$NoPlayback
)
$ErrorActionPreference = "Continue"
$root = Split-Path $Exe
$cfgPath = Join-Path $root "config\.ini"
$diagLog = "$env:LOCALAPPDATA\Yumeiren\logs\videowallpaper.log"
$exeLeaf = Split-Path $Exe -Leaf
$procName = $exeLeaf -replace '\.exe$', ''
$seeder = Join-Path $PSScriptRoot "seed_run_config.py"

function Get-LogTail {
    param([string]$Path, [long]$From)
    if (-not (Test-Path $Path)) { return @() }
    $len = (Get-Item $Path).Length
    if ($len -le $From) { return @() }
    $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek($From, 'Begin') | Out-Null
        $sr = New-Object System.IO.StreamReader($fs)
        return @($sr.ReadToEnd() -split "`n" | ForEach-Object { $_.TrimEnd("`r") })
    } finally { $fs.Dispose() }
}

# 前置条件：不允许有残留实例(会被单实例守卫挡掉，测不到退出路径)
$stale = @(Get-Process -Name $procName -ErrorAction SilentlyContinue)
if ($stale.Count) {
    Write-Host "ABORT: 已有 $($stale.Count) 个 $procName 实例在运行(PID $(($stale.Id) -join ','))，先退出再测"
    exit 3
}

# 播种配置：由 python 负责 UTF-8 编码与正斜杠路径，避免 PowerShell 把反斜杠当转义符吃掉
$play = if ($NoPlayback) { 'false' } else { 'true' }
& python $seeder $cfgPath --playlist $Playlist --was-playing $play
if ($LASTEXITCODE -ne 0) { Write-Host "ABORT: 配置播种失败"; exit 4 }

$logMark = if (Test-Path $diagLog) { (Get-Item $diagLog).Length } else { 0 }
$p = Start-Process -FilePath $Exe -WorkingDirectory $root -PassThru
Start-Sleep -Seconds $PlayWaitSec

$running = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
$alive = [bool]$running
$wsMb = if ($running) { [math]::Round($running.WorkingSet64 / 1MB, 1) } else { -1 }
$midLog = @(Get-LogTail $diagLog $logMark |
    Where-Object { $_ -match 'play index|mediaStatus=|teardown|管线|唤起|配置读取' })
$started = [bool](@($midLog | Where-Object { $_ -match 'session=\d+ play index' }).Count)

# 退出：只走“关闭主窗口”这条正常路径(closeEvent -> 保存配置 -> quit)
$closed = $false
$hung = $false
if ($alive) {
    $p.Refresh()
    $closed = $p.CloseMainWindow()
    if (-not $p.WaitForExit(25000)) {
        # Kill 之后 ExitCode 恒为 0(TerminateProcess 传 0)，必须单独标记，
        # 否则“退出卡死被强杀”会被误读成“干净退出”。
        $hung = $true
        Stop-Process -Id $p.Id -Force
        Start-Sleep -Seconds 2
    }
}
$code = $p.ExitCode
$hex = '0x{0:X8}' -f $code

# WER 全量转储落盘需要时间，按 PID 轮询最多 60s
$newDumps = @()
for ($i = 0; $i -lt 20; $i++) {
    $newDumps = @(Get-ChildItem $DumpDir -Filter "$exeLeaf.*.dmp" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name.EndsWith(".$($p.Id).dmp") })
    if ($newDumps.Count -or ($code -eq 0 -and $i -ge 5)) { break }
    Start-Sleep -Seconds 3
}
$tail = @(Get-LogTail $diagLog $logMark)

Write-Host ""
Write-Host "== repro result (playback at exit: $(-not $NoPlayback)) =="
Write-Host "pid                        : $($p.Id)"
Write-Host "closed-window-requested    : $closed"
Write-Host "hung-25s-after-close       : $hung"
Write-Host "working-set-MB-before-close: $wsMb"
Write-Host "playback-started           : $started"
Write-Host "exit-code                  : $hex ($code)"
Write-Host "new-dumps (pid matched)    : $($newDumps.Count)"
$newDumps | ForEach-Object {
    Write-Host "  dump: $($_.Name) $([math]::Round($_.Length / 1MB))MB"
}
Write-Host "diag-log-lines             : $($tail.Count)"
$tail | Select-Object -Last 16 | ForEach-Object { Write-Host "  $_" }
Write-Host "--- 关键日志(起播/卸载) ---"
$tail | Where-Object { $_ -match 'play index|teardown|stopAll|媒体错误' } |
    Select-Object -First 10 | ForEach-Object { Write-Host "  $_" }

if ($NoPlayback) {
    if ($code -eq 0 -and $newDumps.Count -eq 0) {
        Write-Host "VERDICT: 对照组(未起播)干净退出，无转储"; exit 0
    }
    Write-Host "VERDICT: 对照组异常(退出码 $hex / 转储 $($newDumps.Count))，前提被打破"; exit 1
}
if (-not $started) {
    Write-Host "VERDICT: INCONCLUSIVE —— 未观察到起播，退出码不可用于判断崩溃假设"
    exit 2
}
if ($hex -eq '0xC0000005' -or $newDumps.Count) {
    Write-Host "VERDICT: REPRODUCED —— 播放中退出触发访问违例(Qt6Widgets 路径)"; exit 1
}
if ($code -eq 0) { Write-Host "VERDICT: NOT REPRODUCED —— 播放中退出仍为干净退出"; exit 0 }
Write-Host "VERDICT: 非零退出码 $hex"; exit 2
