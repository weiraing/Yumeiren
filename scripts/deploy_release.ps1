# Release 部署脚本: 从 build 组装最小可运行发布包(dist/)
# 白名单 = PE 导入闭包分析(见 verify_dependencies.ps1) + 插件裁剪决策
# 用法: powershell -ExecutionPolicy Bypass -File scripts\deploy_release.ps1
param(
    [string]$BuildDir = "C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\build",
    [string]$DistDir = "C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\dist"
)
$ErrorActionPreference = "Stop"

# 1) 清理旧 dist
if (Test-Path $DistDir) { Remove-Item $DistDir -Recurse -Force }
New-Item -ItemType Directory -Path $DistDir | Out-Null
New-Item -ItemType Directory -Path "$DistDir\config" | Out-Null

# 2) 白名单: EXE + 必需 Qt DLL + FFmpeg 运行库 + MinGW 运行时 + D3D 着色器编译器
$rootFiles = @(
    "Yumeiren.exe",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
    "Qt6Multimedia.dll", "Qt6MultimediaWidgets.dll", "Qt6Network.dll",
    "avcodec-61.dll", "avformat-61.dll", "avutil-59.dll",
    "swscale-8.dll", "swresample-5.dll",
    "D3Dcompiler_47.dll",
    "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"
)
foreach ($f in $rootFiles) {
    $src = Join-Path $BuildDir $f
    if (-not (Test-Path $src)) { throw "构建目录缺少 $f" }
    Copy-Item $src $DistDir
}

# 3) 插件白名单(裁剪决策: 删 windowsmediaplugin=冗余后端/Qt6Svg+qsvg=无SVG资源/tls=无网络功能/styles=Fusion内置)
Copy-Item "$BuildDir\platforms" "$DistDir\platforms" -Recurse
Copy-Item "$BuildDir\imageformats" "$DistDir\imageformats" -Recurse
Remove-Item "$DistDir\imageformats\qsvg.dll" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "$DistDir\multimedia" -Force | Out-Null
Copy-Item "$BuildDir\multimedia\ffmpegmediaplugin.dll" "$DistDir\multimedia\" -Force

# 4) 首次运行空 config(应用自动创建 config/.ini)
Write-Host "dist assembled: $DistDir"
