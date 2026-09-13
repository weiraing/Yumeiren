# 临时: 彻底清场
Stop-Process -Name Yumeiren -Force -ErrorAction SilentlyContinue
Stop-Process -Name YumeirenTest -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Write-Host 'cleaned'
