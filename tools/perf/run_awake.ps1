# Wake the display, hold ES_DISPLAY_REQUIRED while a scenario runs, then release.
# Rationale: unattended long batches can hit the idle display-off timer; a monitor-off
# event pauses the wallpaper pipeline (by design) and would invalidate playback scenarios.
param(
    [Parameter(Mandatory=$true)][string]$PlaylistCsv,
    [Parameter(Mandatory=$true)][string]$OutCsv,
    [int]$DurationSec = 180,
    [string]$AutoStopMs = "0",
    [string]$AutoStartMs = "0",
    [string]$Volume = "0",
    [string]$WasPlaying = "true",
    [string]$PauseFullscreen = "true",
    [string]$TargetFps = "0"
)
$ErrorActionPreference = "Continue"
Add-Type -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
'@ -Name W32Helper -Namespace Win32 | Out-Null

# wake the display if it is off (SC_MONITORPOWER = -1)
[IntPtr]$r = [IntPtr]::Zero
[Win32.W32Helper]::SendMessageTimeout([IntPtr]0xFFFF, 0x0112, [IntPtr]0xF170, [IntPtr](-1), 2, 1000, [ref]$r) | Out-Null
# hold the display on for the duration of the child scenario
[Win32.W32Helper]::SetThreadExecutionState(0x80000000 -bor 0x00000002) | Out-Null # ES_CONTINUOUS|ES_DISPLAY_REQUIRED

try {
    & (Join-Path $PSScriptRoot 'run_scenario.ps1') -PlaylistCsv $PlaylistCsv -WasPlaying $WasPlaying `
        -AutoStopMs $AutoStopMs -AutoStartMs $AutoStartMs -Volume $Volume `
        -PauseFullscreen $PauseFullscreen -TargetFps $TargetFps `
        -DurationSec $DurationSec -OutCsv $OutCsv
} finally {
    [Win32.W32Helper]::SetThreadExecutionState(0x80000000) | Out-Null # clear display requirement
}
Write-Host "awake scenario done: $OutCsv"
