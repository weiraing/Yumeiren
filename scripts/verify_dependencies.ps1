# 依赖闭包校验: 校验 dist 中 EXE/DLL 的导入依赖是否全部满足(存在或系统 DLL)
# 用法: powershell -ExecutionPolicy Bypass -File scripts\verify_dependencies.ps1 [-DistDir dist]
param([string]$DistDir = "C:\Users\rain\Documents\ExplorerBg\FolderBgStudio\dist",
      [string]$Objdump = "D:\Develop\Qt\Tools\mingw1310_64\bin\objdump.exe")
$ErrorActionPreference = "Stop"

# Windows 系统目录里的 DLL 视为满足
$sysDir = "$env:SystemRoot\System32"

function Get-Imports([string]$file) {
    $out = & $Objdump -x $file 2>$null
    $names = @()
    foreach ($line in $out) {
        if ($line -match "^\s*DLL Name:\s*(\S+)") { $names += $Matches[1] }
    }
    return $names
}

$missing = @()
$files = @(Get-ChildItem $DistDir -Filter *.exe) + @(Get-ChildItem $DistDir -Filter *.dll) +
         @(Get-ChildItem "$DistDir\platforms" -Filter *.dll) +
         @(Get-ChildItem "$DistDir\imageformats" -Filter *.dll) +
         @(Get-ChildItem "$DistDir\multimedia" -Filter *.dll)
foreach ($f in $files) {
    foreach ($imp in (Get-Imports $f.FullName)) {
        if (Test-Path (Join-Path $DistDir $imp)) { continue }        # dist 内满足
        if ($imp -match '^api-ms-win-') { continue }               # API 集合(系统加载器解析)
        if (Test-Path (Join-Path $sysDir $imp)) { continue }         # 系统目录满足
        $missing += "$($f.Name) -> $imp"
    }
}
if ($missing.Count -gt 0) {
    Write-Host "缺失依赖:"
    $missing | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host ("依赖闭包完整: " + $files.Count + " 个模块全部导入满足")
