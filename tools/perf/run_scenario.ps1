# Phase2 baseline scenario runner: seeds HKCU video/* settings the same way the app
# stores them, launches the unelevated YumeirenTest build, samples metrics, closes app.
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
    [string]$Volume = "0",        # HKCU video/volume (0 = app disables the audio track)
    [string]$ProbeStage = "",     # YUMEIREN_PROBE_STAGE: B/C/D/E (empty = normal run)
    [int]$AutoStopMs = 0,         # YUMEIREN_AUTO_STOP_MS: one-shot stopAll() after N ms
    [int]$AutoStartMs = 0         # YUMEIREN_AUTO_START_MS: one-shot startPlaying() after N ms
)
$ErrorActionPreference = "Continue"
$vk = 'HKCU:\Software\Yumeiren\Yumeiren\video'
$playlist = $PlaylistCsv -split ';'
New-Item -Path $vk -Force | Out-Null
Set-ItemProperty -Path $vk -Name playlist -Value ([string[]]$playlist) -Type MultiString
Set-ItemProperty -Path $vk -Name wasPlaying -Value $WasPlaying -Type String
Set-ItemProperty -Path $vk -Name targetFps -Value $TargetFps -Type String
Set-ItemProperty -Path $vk -Name autoLoop -Value $AutoLoop -Type String
Set-ItemProperty -Path $vk -Name pauseFullscreen -Value $PauseFullscreen -Type String
Set-ItemProperty -Path $vk -Name volume -Value $Volume -Type String

# probe/auto hooks are consumed by the app on startup; clear leftovers first
Remove-Item Env:YUMEIREN_PROBE_STAGE -ErrorAction SilentlyContinue
Remove-Item Env:YUMEIREN_AUTO_STOP_MS -ErrorAction SilentlyContinue
Remove-Item Env:YUMEIREN_AUTO_START_MS -ErrorAction SilentlyContinue
if ($ProbeStage) { $env:YUMEIREN_PROBE_STAGE = $ProbeStage }
if ($AutoStopMs -gt 0) { $env:YUMEIREN_AUTO_STOP_MS = [string]$AutoStopMs }
if ($AutoStartMs -gt 0) { $env:YUMEIREN_AUTO_START_MS = [string]$AutoStartMs }

# ensure no stale instance
$old = Get-Process -Name YumeirenTest -ErrorAction SilentlyContinue
if ($old) {
    $old | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Seconds 2
    Get-Process -Name YumeirenTest -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe)
Start-Sleep -Seconds $InitWaitSec
& (Join-Path $PSScriptRoot 'sample2.ps1') -DurationSec $DurationSec -IntervalSec $IntervalSec -OutCsv $OutCsv

$p = Get-Process -Name YumeirenTest -ErrorAction SilentlyContinue
if ($p) {
    $p.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 3
    $p = Get-Process -Name YumeirenTest -ErrorAction SilentlyContinue
    if ($p) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
}
Write-Host "scenario done: $OutCsv"
