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
    [string]$Exe = "C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\YumeirenTest.exe"
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
& (Join-Path $PSScriptRoot 'sample.ps1') -DurationSec $DurationSec -IntervalSec $IntervalSec -OutCsv $OutCsv

$p = Get-Process -Name YumeirenTest -ErrorAction SilentlyContinue
if ($p) {
    $p.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 3
    $p = Get-Process -Name YumeirenTest -ErrorAction SilentlyContinue
    if ($p) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
}
Write-Host "scenario done: $OutCsv"
