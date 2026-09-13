# 临时: 关闭正式版, 启动可点击的测试实例(非提权)
Stop-Process -Name Yumeiren -Force -ErrorAction SilentlyContinue
Stop-Process -Name YumeirenTest -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Start-Process 'C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build\YumeirenTest.exe'
