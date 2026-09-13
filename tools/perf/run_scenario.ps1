# Phase2 baseline scenario runner: seeds the app's unified config (config/.ini),
# launches the unelevated YumeirenTest build, samples metrics, closes app.
param(
    # paths separated by ';' (bash-friendly; -File mode cannot bind arrays)
    [Parameter(Mandatory=$true)][string]$PlaylistCsv,
    [string]$WasPlaying = "true",
    [string]$TargetFps = "0",
    [string]$AutoLoop = "true",
    [string]$PauseFullscreen = "true",
    [int]$DurationSec = 60,
    [int]$IntervalSec = 5,
    [int]$InitWaitSec = 4,
    [Parameter(Mandatory=$true)][string]$OutCsv,
    [string]$Exe = "C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\YumeirenTest.exe",
    # --- Phase4 memory-attribution additions ---
    [string]$Volume = "0",        # video/volume (0 = app disables the audio track)
    [string]$ProbeStage = "",     # YUMEIREN_PROBE_STAGE: B/C/D/E (empty = normal run)
    [int]$AutoStopMs = 0,         # YUMEIREN_AUTO_STOP_MS: one-shot stopAll() after N ms
    [int]$AutoStartMs = 0,        # YUMEIREN_AUTO_START_MS: one-shot startPlaying() after N ms
    [string]$ProcName = "YumeirenTest",  # sampler target process name (SinkProbe A/B)
    [string]$AppArgs = ""         # extra command line passed to $Exe (SinkProbe media/duration)
)
$ErrorActionPreference = "Continue"

# 应用配置已迁移至 <运行目录>/config/.ini(统一配置中心): 场景播种直接写 INI 对应键。
function Set-IniValue {
    param([string]$Path, [string]$Section, [string]$Key, [string]$Value)
    $out = New-Object System.Collections.Generic.List[string]
    $inSec = $false; $done = $false
    if (Test-Path $Path) { $lines = @(Get-Content $Path) } else { $lines = @() }
    foreach ($l in $lines) {
        if ($l -match "^\s*\[(.+)\]\s*$") {
            if ($inSec -and -not $done) { $out.Add("$Key=$Value"); $done = $true }
            $inSec = ($Matches[1] -ieq $Section)
            $out.Add($l); continue
        }
        if ($inSec -and $l -match "^\s*" + [regex]::Escape($Key) + "=") {
            if (-not $done) { $out.Add("$Key=$Value"); $done = $true }
            continue
        }
        $out.Add($l)
    }
    if ($inSec -and -not $done) { $out.Add("$Key=$Value") }
    if (-not ($out | Where-Object { $_ -match "^\s*\[" })) {
        $out.Add("[$Section]"); $out.Add("$Key=$Value")
    }
    [System.IO.File]::WriteAllLines($Path, [string[]]$out.ToArray())
}

$playlist = $PlaylistCsv -split ';'
$cfgPath = 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\config\.ini'
New-Item -Path (Split-Path $cfgPath) -Force | Out-Null
if (-not (Test-Path $cfgPath)) { [System.IO.File]::WriteAllLines($cfgPath, @('[Video]')) }
# QSettings 的 QStringList 序列化为逗号+空格分隔
Set-IniValue -Path $cfgPath -Section 'Video' -Key 'playlist' -Value ($playlist -join ', ')
Set-IniValue -Path $cfgPath -Section 'Video' -Key 'wasPlaying' -Value $WasPlaying
Set-IniValue -Path $cfgPath -Section 'Video' -Key 'targetFps' -Value $TargetFps
Set-IniValue -Path $cfgPath -Section 'Video' -Key 'autoLoop' -Value $AutoLoop
Set-IniValue -Path $cfgPath -Section 'Video' -Key 'pauseFullscreen' -Value $PauseFullscreen
Set-IniValue -Path $cfgPath -Section 'Video' -Key 'volume' -Value $Volume

# probe/auto hooks are consumed by the app on startup; clear leftovers first
Remove-Item Env:YUMEIREN_PROBE_STAGE -ErrorAction SilentlyContinue
Remove-Item Env:YUMEIREN_AUTO_STOP_MS -ErrorAction SilentlyContinue
Remove-Item Env:YUMEIREN_AUTO_START_MS -ErrorAction SilentlyContinue
if ($ProbeStage) { $env:YUMEIREN_PROBE_STAGE = $ProbeStage }
if ($AutoStopMs -gt 0) { $env:YUMEIREN_AUTO_STOP_MS = [string]$AutoStopMs }
if ($AutoStartMs -gt 0) { $env:YUMEIREN_AUTO_START_MS = [string]$AutoStartMs }

# ensure no stale instance
$old = Get-Process -Name $ProcName -ErrorAction SilentlyContinue
if ($old) {
    $old | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Seconds 2
    Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

if ($AppArgs) {
    Start-Process -FilePath $Exe -ArgumentList $AppArgs -WorkingDirectory (Split-Path $Exe)
} else {
    Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe)
}
Start-Sleep -Seconds $InitWaitSec
& (Join-Path $PSScriptRoot 'sample2.ps1') -ProcName $ProcName -DurationSec $DurationSec -IntervalSec $IntervalSec -OutCsv $OutCsv

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue
if ($p) {
    $p.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 3
    $p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue
    if ($p) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
}
Write-Host "scenario done: $OutCsv"
