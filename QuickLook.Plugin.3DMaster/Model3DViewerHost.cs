using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;

namespace QuickLook.Plugin.ThreeDMaster
{
    public class Model3DViewerHost : HwndHost
    {
        private const int WS_CHILD = 0x40000000;
        private const int WS_VISIBLE = 0x10000000;
        private const int WS_CLIPCHILDREN = 0x02000000;
        private const int WS_CLIPSIBLINGS = 0x04000000;
        private const int WS_POPUP = unchecked((int)0x80000000);
        private const int WS_EX_NOACTIVATE = 0x08000000;
        private const int WS_EX_TOOLWINDOW = 0x00000080;

        private const int GW_CHILD = 5;
        private const uint GA_ROOT = 2;

        private const int WM_KEYDOWN = 0x0100;
        private const int WM_SYSKEYDOWN = 0x0104;

        private const int VK_SPACE = 0x20;
        private const int VK_ESCAPE = 0x1B;
        private const int VK_LEFT = 0x25;
        private const int VK_UP = 0x26;
        private const int VK_RIGHT = 0x27;
        private const int VK_DOWN = 0x28;

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [DllImport("user32.dll", SetLastError = true, EntryPoint = "CreateWindowExW")]
        private static extern IntPtr CreateWindowEx(
            int dwExStyle,
            [MarshalAs(UnmanagedType.LPWStr)] string lpClassName,
            [MarshalAs(UnmanagedType.LPWStr)] string lpWindowName,
            int dwStyle,
            int x, int y, int nWidth, int nHeight,
            IntPtr hWndParent,
            IntPtr hMenu,
            IntPtr hInstance,
            IntPtr lpParam);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool DestroyWindow(IntPtr hwnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetParent(IntPtr hWndChild, IntPtr hWndNewParent);

        [DllImport("user32.dll")]
        private static extern IntPtr GetWindow(IntPtr hWnd, int uCmd);

        [DllImport("user32.dll")]
        private static extern IntPtr GetAncestor(IntPtr hwnd, uint gaFlags);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

        private const uint SWP_NOSIZE = 0x0001;
        private const uint SWP_NOMOVE = 0x0002;
        private const uint SWP_NOZORDER = 0x0004;
        private const uint SWP_NOACTIVATE = 0x0010;
        private const uint SWP_SHOWWINDOW = 0x0040;

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetWindowPos(
            IntPtr hWnd,
            IntPtr hWndInsertAfter,
            int X,
            int Y,
            int cx,
            int cy,
            uint uFlags);

        [DllImport("user32.dll")]
        private static extern IntPtr SendMessage(IntPtr hWnd, int Msg, IntPtr wParam, IntPtr lParam);

        // 宿主进程级隐藏脱离孤岛
        private static readonly object _islandLock = new object();
        private static IntPtr _hiddenIslandHwnd = IntPtr.Zero;

        private static IntPtr GetOrCreateHiddenIsland()
        {
            lock (_islandLock)
            {
                if (_hiddenIslandHwnd != IntPtr.Zero)
                    return _hiddenIslandHwnd;

                _hiddenIslandHwnd = CreateWindowEx(
                    WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                    "STATIC",
                    "3dmaster_HostIsland",
                    WS_POPUP,
                    -32000, -32000, 10, 10,
                    IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);

                return _hiddenIslandHwnd;
            }
        }

        private bool _isDestroying = false;
        private int _lastW = 0;
        private int _lastH = 0;

        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            _isDestroying = false;
            int initialW = 800;
            int initialH = 600;

            if (hwndParent.Handle != IntPtr.Zero && GetClientRect(hwndParent.Handle, out RECT pRect))
            {
                int pw = pRect.Right - pRect.Left;
                int ph = pRect.Bottom - pRect.Top;
                if (pw > 0 && ph > 0)
                {
                    initialW = pw;
                    initialH = ph;
                }
            }

            int style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
            IntPtr hwnd = CreateWindowEx(
                0,
                "STATIC",
                "3dmaster_Container",
                style,
                0, 0,
                initialW,
                initialH,
                hwndParent.Handle,
                IntPtr.Zero,
                IntPtr.Zero,
                IntPtr.Zero);

            if (hwnd != IntPtr.Zero)
            {
                _lastW = initialW;
                _lastH = initialH;
                DaemonClient.SendAttach(hwnd);
            }

            return new HandleRef(this, hwnd);
        }

        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            _isDestroying = true;
            IntPtr container = hwnd.Handle;
            if (container == IntPtr.Zero) return;

            // 1. 同步发出 CLEAR 并等待守护进程将 GL 窗口移至孤岛（最多等 200ms）
            try
            {
                DaemonClient.SendClearWait(200);
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[Model3DViewerHost] SendClearWait exception: {ex.Message}");
            }

            // 2. 宿主侧安全脱离兜底：对容器内尚未脱离的子视口强制 reparent 到宿主隐藏孤岛
            try
            {
                IntPtr island = GetOrCreateHiddenIsland();
                IntPtr child = GetWindow(container, GW_CHILD);
                int guard = 0;
                while (child != IntPtr.Zero && guard++ < 10)
                {
                    SetParent(child, island);
                    child = GetWindow(container, GW_CHILD);
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[Model3DViewerHost] Detach child exception: {ex.Message}");
            }

            // 3. 彻底解耦后再销毁容器窗口，杜绝跨进程白闪与级联杀灭
            DestroyWindow(container);
        }

        protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
        {
            base.OnRenderSizeChanged(sizeInfo);
            if (Handle == IntPtr.Zero || _isDestroying) return;

            // 获取 DPI 缩放并计算物理像素尺寸 (杜绝 WPF DIP 与 Win32 物理像素割裂)
            double dpiX = 1.0;
            double dpiY = 1.0;
            try
            {
                var source = PresentationSource.FromVisual(this);
                if (source?.CompositionTarget != null)
                {
                    dpiX = source.CompositionTarget.TransformToDevice.M11;
                    dpiY = source.CompositionTarget.TransformToDevice.M22;
                }
            }
            catch { }

            int newW = (int)Math.Round(sizeInfo.NewSize.Width * dpiX);
            int newH = (int)Math.Round(sizeInfo.NewSize.Height * dpiY);

            if (newW > 0 && newH > 0 && (newW != _lastW || newH != _lastH))
            {
                _lastW = newW;
                _lastH = newH;
                SetWindowPos(Handle, IntPtr.Zero, 0, 0, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                DaemonClient.SendResize(newW, newH);
            }
        }

        protected override IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
            {
                int vk = wParam.ToInt32();
                if (vk == VK_SPACE || vk == VK_ESCAPE || vk == VK_LEFT || vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN)
                {
                    IntPtr root = GetAncestor(hwnd, GA_ROOT);
                    if (root != IntPtr.Zero)
                    {
                        SendMessage(root, msg, wParam, lParam);
                        handled = true;
                        return IntPtr.Zero;
                    }
                }
            }

            return base.WndProc(hwnd, msg, wParam, lParam, ref handled);
        }

        protected override bool TabIntoCore(TraversalRequest request)
        {
            return false;
        }
    }
}
