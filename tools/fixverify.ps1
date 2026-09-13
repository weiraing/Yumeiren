# Self-contained elevated verification (ASCII only - no Chinese literals):
# 1) kill all instances  2) force wasPlaying=true (playlist untouched)
# 3) clear logs  4) start YumeirenTest (auto-restores user playlist)
# 5) restart explorer (= what uninstall does)  6) recovery window
Stop-Process -Name YumeirenTest -Force -ErrorAction SilentlyContinue
Stop-Process -Name Yumeiren -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$vk = 'HKCU:\Software\Yumeiren\Yumeiren\video'
Set-ItemProperty -Path $vk -Name wasPlaying -Value 'true' -Type String

Remove-Item "$env:LOCALAPPDATA\Yumeiren\logs\*" -Force -ErrorAction SilentlyContinue

Start-Process 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\YumeirenTest.exe'
Start-Sleep -Seconds 10

Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) {
    Start-Process explorer.exe
}
Start-Sleep -Seconds 25
Write-Host 'verify done'
