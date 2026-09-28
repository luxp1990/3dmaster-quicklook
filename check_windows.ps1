Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public class WinEnum2 {
    public delegate bool EnumWindowDelegate(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr hWndParent, EnumWindowDelegate lpfn, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDesktopWindow();

    [DllImport("user32.dll")]
    public static extern IntPtr GetParent(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", CharSet=CharSet.Auto)]
    public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder title, int size);

    [DllImport("user32.dll", CharSet=CharSet.Auto)]
    public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder name, int size);

    public static void CheckPids(uint[] pids) {
        var pidSet = new HashSet<uint>(pids);
        EnumChildWindows(GetDesktopWindow(), (hWnd, lParam) => {
            uint p;
            GetWindowThreadProcessId(hWnd, out p);
            if (pidSet.Contains(p)) {
                var title = new System.Text.StringBuilder(256);
                var cls = new System.Text.StringBuilder(256);
                GetWindowText(hWnd, title, 256);
                GetClassName(hWnd, cls, 256);
                IntPtr parent = GetParent(hWnd);
                bool vis = IsWindowVisible(hWnd);
                Console.WriteLine(string.Format("PID {0}: HWND=0x{1:X} Vis={2} Parent=0x{3:X} Class={4} Title={5}", p, hWnd.ToInt64(), vis, parent.ToInt64(), cls, title));
            }
            return true;
        }, IntPtr.Zero);
    }
}
'@

$daemon = Get-Process 3dmaster-preview -ErrorAction SilentlyContinue
$ql = Get-Process QuickLook -ErrorAction SilentlyContinue

$pids = @()
if ($daemon) { $pids += [uint32]$daemon.Id }
if ($ql) { $pids += [uint32]$ql.Id }

if ($pids.Count -gt 0) {
    [WinEnum2]::CheckPids($pids)
} else {
    Write-Host "No target processes found."
}
