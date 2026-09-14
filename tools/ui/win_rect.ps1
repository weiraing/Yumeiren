# 列出某进程所有可见顶层窗口的屏幕矩形，给 tooltip_shot.ps1 提供悬浮坐标。
# 用法：powershell -File tools\ui\win_rect.ps1 -Pid 1234
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [switch]$All
)

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WinEnum {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left; public int Top; public int Right; public int Bottom; }

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lparam);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] static extern IntPtr GetClassName(IntPtr h, StringBuilder s, int max);

    delegate bool EnumProc(IntPtr h, IntPtr lparam);

    public static bool AllWindows;

    public static string Dump(uint targetPid) {
        var sb = new StringBuilder();
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            if (!AllWindows && !IsWindowVisible(h)) return true;
            Rect r; GetWindowRect(h, out r);
            var t = new StringBuilder(256); GetWindowText(h, t, 256);
            var c = new StringBuilder(256); GetClassName(h, c, 256);
            sb.AppendLine(h + " | " + c + " | vis=" + IsWindowVisible(h) + " | " + t + " | " + r.Left + "," + r.Top + " " + (r.Right - r.Left) + "x" + (r.Bottom - r.Top));
            return true;
        }, IntPtr.Zero);
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $code
[WinEnum]::AllWindows = [bool]$All
[WinEnum]::Dump([uint32]$ProcessId)
