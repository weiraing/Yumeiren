# 循环友好转码：消除视频壁纸循环边界的单帧跳变。
# 原理：Qt FFmpeg 后端 setLoops(Infinite) 回绕时约有 1 帧的运动跳变(黑帧已由后端
# 循环消除，但跳变在后端内部、应用层无法拦截)。把视频首帧克隆数帧垫到片尾后，
# 接缝两侧为同一画面，跳变在数学上不可见。实测帧差尖峰 27.6 → 0
# (docs/VIDEO_MEDIA_COMPATIBILITY_POLICY.md "循环边界接缝抖动")。
# 依赖 PATH 上的 ffmpeg/ffprobe。输出为仅视频的 .loop.mp4(壁纸默认静音，不带音轨)。
# 用法：powershell -File make_loop_clip.ps1 -InputFile <视频路径> [-PadSec 0.1]
param(
    [Parameter(Mandatory=$true)][string]$InputFile,
    [string]$OutputFile = "",
    [double]$PadSec = 0.1
)
$ErrorActionPreference = "Stop"
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { throw "PATH 上找不到 ffmpeg" }
if (-not (Test-Path $InputFile)) { throw "输入文件不存在: $InputFile" }
if (-not $OutputFile) {
    $OutputFile = [IO.Path]::ChangeExtension($InputFile, ".loop.mp4")
}
# 尾垫帧率取源帧率，保证垫入 3 帧左右
$srcFps = "30"
$fr = & ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 $InputFile
if ($LASTEXITCODE -eq 0 -and $fr -match "^(\d+)/(\d+)$") {
    $srcFps = [string]([math]::Round([double]$Matches[1] / [double]$Matches[2], 3))
}
$tmp = Join-Path $env:TEMP ("loop_frame0_{0}.png" -f $PID)
ffmpeg -v error -y -i $InputFile -frames:v 1 $tmp
if ($LASTEXITCODE -ne 0) { throw "提取首帧失败" }
ffmpeg -v error -y -i $InputFile -loop 1 -framerate $srcFps -t $PadSec -i $tmp `
    -filter_complex "[0:v][1:v]concat=n=2:v=1:a=0[out]" -map "[out]" `
    -c:v libopenh264 -b:v 8M -pix_fmt yuv420p $OutputFile
Remove-Item $tmp -ErrorAction SilentlyContinue
if ($LASTEXITCODE -ne 0) { throw "转码失败" }
Write-Host ("循环友好副本已生成: {0} (片尾 +{1}s 首帧垫)" -f $OutputFile, $PadSec)
