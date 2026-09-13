# Self-contained elevated verification v3: explorer restart + pixel proof
# 1) kill all  2) wasPlaying=true  3) clear logs  4) start YumeirenTest
# 5) restart explorer  6) recovery window  7) show desktop  8) capture 2 frames
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
Start-Sleep -Seconds 20

$sh = New-Object -ComObject Shell.Application
$sh.ToggleDesktop()
Start-Sleep -Seconds 2
$out = 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\tools'
ffmpeg -v error -y -f gdigrab -framerate 30 -i desktop -frames:v 1 "$out\recovery_a.png"
Start-Sleep -Milliseconds 1200
ffmpeg -v error -y -f gdigrab -framerate 30 -i desktop -frames:v 1 "$out\recovery_b.png"
Write-Host 'verify done'
