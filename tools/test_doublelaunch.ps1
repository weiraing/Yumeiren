# 临时调试助手(不提交)：提权环境下执行双启动流程, 诊断日志记录唤起结果
# 用法: Start-Process powershell -Verb RunAs -ArgumentList '-ExecutionPolicy Bypass -File <this>'
param([switch]$LaunchPair, [switch]$RebuildPair)
if ($RebuildPair) {
    Stop-Process -Name Yumeiren -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    & 'D:\Program Files\JetBrains\CLion 2025.2.6\bin\cmake\win\x64\bin\cmake.exe' --build 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build' --target Yumeiren
    Start-Sleep -Seconds 1
    Remove-Item "$env:LOCALAPPDATA\Yumeiren\logs\guard_*.log" -ErrorAction SilentlyContinue
    Remove-Item "$env:LOCALAPPDATA\Yumeiren\logs\videowallpaper.log" -ErrorAction SilentlyContinue
    Start-Process 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\Yumeiren.exe'
    Start-Sleep -Seconds 8
    Start-Process 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\Yumeiren.exe'
    Start-Sleep -Seconds 5
    Write-Host 'rebuild+pair done'
    exit
}
if (-not $LaunchPair) {
    Stop-Process -Name Yumeiren -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    Write-Host "killed"
    exit
}
Remove-Item "$env:LOCALAPPDATA\Yumeiren\logs\videowallpaper.log" -ErrorAction SilentlyContinue
Start-Process 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\Yumeiren.exe'
Start-Sleep -Seconds 8
Start-Process 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\Yumeiren.exe'
Start-Sleep -Seconds 5
Write-Host "pair done"
