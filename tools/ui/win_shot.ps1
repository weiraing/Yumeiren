# 抓取指定 HWND 的窗口区域：先恢复+前置，再重新读取矩形，保证截图与矩形一一对应。
# 用法：powershell -File tools\ui\win_shot.ps1 -Hwnd 723270 -Out C:\tmp\win.png
param(
    [Parameter(Mandatory = $true)][int]$Hwnd,
    [string]$Out = "$env:TEMP\win_shot.png",
    [switch]$Restore
)
Add-Type -AssemblyName System.Drawing

$preAware = @'
using System;
using System.Runtime.InteropServices;
public static class PreDpi {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
Add-Type -TypeDefinition $preAware
# 必须在任何取坐标/截图之前声明 DPI 感知：否则 user32 给的是 96dpi 虚化坐标，
# 而 CopyFromScreen 抓的是物理像素，两者差一个缩放系数，截图会整体偏移。
[void][PreDpi]::SetProcessDPIAware()

$code = @'
using System;
using System.Runtime.InteropServices;
public static class WinOps {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public static string RectOf(IntPtr h) {
        Rect r; GetWindowRect(h, out r);
        return r.Left + "," + r.Top + " " + (r.Right - r.Left) + "x" + (r.Bottom - r.Top) + " iconic=" + IsIconic(h);
    }
}
'@
Add-Type -TypeDefinition $code

$h = [IntPtr]$Hwnd
if ($Restore -or [WinOps]::IsIconic($h)) {
    [WinOps]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
    Start-Sleep -Milliseconds 250
}
[WinOps]::BringWindowToTop($h) | Out-Null
[WinOps]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500

$rectStr = [WinOps]::RectOf($h)
$parts = $rectStr -split ' '
$lt = $parts[0] -split ','
$sz = $parts[1] -split 'x'
$left = [int]$lt[0]; $top = [int]$lt[1]; $w = [int]$sz[0]; $ht = [int]$sz[1]
Write-Host "rect $rectStr"

$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($left, $top, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $Out"
