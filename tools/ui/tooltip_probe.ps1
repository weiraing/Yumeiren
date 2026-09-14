# 悬浮提示视觉验证助手(调试用)。
#
# 思路：Qt 的提示框是一个独立顶层窗口，不需要知道控件坐标 —— 依次把真实光标停到候选点，
# 每次对比该进程新出现的顶层窗口即可抓到提示框，再按它自己的矩形截图。
# 候选点用目标窗口矩形内的“逻辑像素”偏移表示(与截图一致，脚本按 DPI 倍数换算成物理坐标)，
# 因此同一份候选表在 100%/150% 缩放下都能用。
#
# 用法：
#   powershell -File tools\ui\tooltip_probe.ps1 -ProcessId 20944 -Hwnd 723270 `
#     -Points "625,1063|600,655|120,400" -Out tip.png
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][int]$Hwnd,
    [Parameter(Mandatory = $true)][string]$Points,
    [string]$Out = "$env:TEMP\tooltip_probe.png",
    [int]$WaitMs = 1400,
    [int]$Margin = 55
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$code = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class WinOps {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Pt { public int X; public int Y; }
    delegate bool EnumProc(IntPtr h, IntPtr lparam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Pt p);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);

    static uint s_pid;
    static List<IntPtr> s_list;

    public static string RectOf(IntPtr h) {
        Rect r; GetWindowRect(h, out r);
        return r.Left + "," + r.Top + " " + (r.Right - r.Left) + "x" + (r.Bottom - r.Top);
    }
    public static int[] RectArr(IntPtr h) {
        Rect r; GetWindowRect(h, out r);
        return new int[] { r.Left, r.Top, r.Right, r.Bottom };
    }
    public static IntPtr RootAt(int x, int y) {
        var p = new Pt { X = x, Y = y };
        return GetAncestor(WindowFromPoint(p), 2);
    }
    public static List<IntPtr> TopWindows(uint pid) {
        s_pid = pid; s_list = new List<IntPtr>();
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == s_pid && IsWindowVisible(h)) s_list.Add(h);
            return true;
        }, IntPtr.Zero);
        return s_list;
    }
    public static IntPtr FirstNew(List<IntPtr> baseSet, uint pid) {
        foreach (var h in TopWindows(pid)) if (!baseSet.Contains(h)) return h;
        return IntPtr.Zero;
    }
    // 系统会拒绝后台进程抢焦点，补一次 ALT 按键事件即可放行
    public static bool ForceForeground(IntPtr h) {
        for (int i = 0; i < 4; i++) {
            keybd_event(0x12, 0, 0, IntPtr.Zero);
            keybd_event(0x12, 0, 2, IntPtr.Zero);
            ShowWindow(h, 9);
            BringWindowToTop(h);
            SetForegroundWindow(h);
            System.Threading.Thread.Sleep(320);
            var rc = RectArr(h);
            if (RootAt(rc[0] + 30, rc[1] + 30) == h) return true;
        }
        return false;
    }
}
'@
Add-Type -TypeDefinition $code
[WinOps]::SetProcessDPIAware() | Out-Null

$hwndI = [IntPtr]$Hwnd
$rect = [WinOps]::RectArr($hwndI)
$originX = $rect[0]
$originY = $rect[1]
Write-Host ("target $hwndI rect=" + [WinOps]::RectOf($hwndI))
if (-not [WinOps]::ForceForeground($hwndI)) { Write-Host "WARN: 置前失败，命中检测会跳过不可见点" }
$rect = [WinOps]::RectArr($hwndI)
$originX = $rect[0]
$originY = $rect[1]
Write-Host ("rect=" + ($rect -join '/') + " 候选点数=" + $Points.Count)

$bmp = New-Object System.Drawing.Bitmap(2, 2)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dpiX = $g.DpiX
$g.Dispose()
$bmp.Dispose()
$factor = $dpiX / 96.0
Write-Host ("capture dpi=$dpiX factor=$factor")

# 候选点是窗口内物理像素偏移；换算成屏幕绝对坐标后逐个停留试探。
$base = [WinOps]::TopWindows([uint32]$ProcessId)
$pointList = $Points -split ';'
foreach ($pt in $pointList) {
    if (-not $pt) { continue }
    $pair = $pt -split ','
    $X = $originX + [int]$pair[0]
    $Y = $originY + [int]$pair[1]
    if (-not [WinOps]::ForceForeground($hwndI)) { Write-Host "WARN: $X,$Y 置前失败" }
    if ([WinOps]::RootAt($X, $Y) -ne $hwndI) { Write-Host "SKIP $X,$Y 非最前"; continue }

    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point($X, ($Y - 14))
    Start-Sleep -Milliseconds 120
    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point($X, $Y)
    Start-Sleep -Milliseconds $WaitMs

    $found = [WinOps]::FirstNew($base, [uint32]$ProcessId)
    if ($found -eq [IntPtr]::Zero) { Write-Host "no tooltip at $X,$Y"; continue }

    $tr = [WinOps]::RectArr($found)
    $left = [Math]::Max(0, $tr[0] - $Margin)
    $top = [Math]::Max(0, $tr[1] - $Margin)
    $w = [Math]::Min(2560, $tr[2] + $Margin) - $left
    $h = [Math]::Min(1600, $tr[3] + $Margin) - $top
    $dir = Split-Path $Out -Parent
    $leaf = [System.IO.Path]::GetFileNameWithoutExtension((Split-Path $Out -Leaf))
    $shot = Join-Path $dir ($leaf + "_" + $found + ".png")
    $img = New-Object System.Drawing.Bitmap($w, $h)
    $gr = [System.Drawing.Graphics]::FromImage($img)
    $gr.CopyFromScreen($left, $top, 0, 0, $img.Size)
    $gr.Dispose()
    $img.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Write-Host ("HIT hwnd=$found tiprect=" + ($tr -join '/') + " -> $shot")
}
