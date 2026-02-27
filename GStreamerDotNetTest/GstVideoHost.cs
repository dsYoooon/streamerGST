using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace GStreamerDotNetTest
{
    public class GstVideoHost : HwndHost
    {
        // ===== Win32 P/Invoke =====
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "CreateWindowExW", SetLastError = true)]
        private static extern IntPtr CreateWindowEx(
            int exStyle, string lpszClass, string lpszName, int style,
            int x, int y, int width, int height,
            IntPtr hwndParent, IntPtr hMenu, IntPtr hInst, IntPtr pvParam);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyWindow(IntPtr hwnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetWindowPos(
            IntPtr hWnd, IntPtr hWndInsertAfter,
            int X, int Y, int cx, int cy, uint uFlags);

        // ===== Styles / Flags =====
        private const int WS_CHILD = 0x40000000;
        private const int WS_VISIBLE = 0x10000000;
        private const int WS_CLIPSIBLINGS = 0x04000000;
        private const int WS_CLIPCHILDREN = 0x02000000;

        private const uint SWP_NOZORDER = 0x0004;
        private const uint SWP_NOACTIVATE = 0x0010;

        private IntPtr _hwnd = IntPtr.Zero;
        public IntPtr Handle => _hwnd;

        // [변경] 크기 변경 시 폭/높이를 인자로 전달하는 이벤트
        public event Action<int, int> WindowResized;

        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            int w = Math.Max(1, (int)ActualWidth);
            int h = Math.Max(1, (int)ActualHeight);

            // C# 쪽은 단순히 자리를 차지하는 부모 윈도우(Static)만 생성
            _hwnd = CreateWindowEx(
                0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                0, 0, w, h,
                hwndParent.Handle,
                IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);

            if (_hwnd == IntPtr.Zero)
                throw new InvalidOperationException($"CreateWindowEx failed: {Marshal.GetLastWin32Error()}");

            return new HandleRef(this, _hwnd);
        }

        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            if (hwnd.Handle != IntPtr.Zero)
                DestroyWindow(hwnd.Handle);
            _hwnd = IntPtr.Zero;
        }

        protected override void OnWindowPositionChanged(Rect rcBoundingBox)
        {
            base.OnWindowPositionChanged(rcBoundingBox);
            UpdateWindowSize(rcBoundingBox.Width, rcBoundingBox.Height);
        }

        protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
        {
            base.OnRenderSizeChanged(sizeInfo);
            UpdateWindowSize(sizeInfo.NewSize.Width, sizeInfo.NewSize.Height);
        }

        private void UpdateWindowSize(double width, double height)
        {
            if (_hwnd != IntPtr.Zero)
            {
                int w = Math.Max(1, (int)Math.Round(width));
                int h = Math.Max(1, (int)Math.Round(height));
                SetWindowPos(_hwnd, IntPtr.Zero, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

                // [중요] 리사이즈 이벤트 발생 -> 메인 윈도우에서 잡아서 C++로 전송
                WindowResized?.Invoke(w, h);
            }
        }
    }
}