using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace GStreamerDotNetTest
{
    /// <summary>
    /// GStreamer sink(d3d11videosink 등)에 넘길 자식 HWND를 제공하는 WPF 전용 HwndHost.
    /// WinForms 의존 없음.
    /// </summary>
    public class GstVideoHost : HwndHost
    {
        // ===== Win32 P/Invoke =====
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "CreateWindowExW", SetLastError = true)]
        private static extern IntPtr CreateWindowEx(
            int exStyle,
            string lpszClass, string lpszName, int style,
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

        // ===== State =====
        private IntPtr _hwnd = IntPtr.Zero;

        /// <summary>호스팅되는 자식 HWND (GStreamer window-handle로 전달)</summary>
        public IntPtr Handle => _hwnd;

        /// <summary>자식 HWND가 만들어진 직후 알림</summary>
        public event EventHandler HwndCreated;

        /// <summary>자식 HWND가 파괴된 직후 알림</summary>
        public event EventHandler HwndDestroyed;

        /// <summary>자식 HWND의 크기가 바뀌었을 때 알림 (미리보기 렌더링 영역 보정용)</summary>
        public event EventHandler HwndResized;

        // ===== HwndHost overrides =====
        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            // WPF가 실제 배치 전에 ActualWidth/Height가 0일 수 있으므로 안전값 1로 생성
            int w = Math.Max(1, (int)ActualWidth);
            int h = Math.Max(1, (int)ActualHeight);

            int style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

            // lightweight 자식 윈도우(정적 컨트롤) 생성, GStreamer가 여기에 그리도록 함
            _hwnd = CreateWindowEx(
                0,
                "STATIC",                    // 기본 존재하는 윈도우 클래스
                string.Empty,
                style,
                0, 0, w, h,
                hwndParent.Handle,
                IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);

            if (_hwnd == IntPtr.Zero)
            {
                int err = Marshal.GetLastWin32Error();
                throw new InvalidOperationException($"CreateWindowEx failed: {err}");
            }

            HwndCreated?.Invoke(this, EventArgs.Empty);
            return new HandleRef(this, _hwnd);
        }

        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            try
            {
                if (hwnd.Handle != IntPtr.Zero)
                {
                    DestroyWindow(hwnd.Handle);
                }
            }
            finally
            {
                _hwnd = IntPtr.Zero;
                HwndDestroyed?.Invoke(this, EventArgs.Empty);
            }
        }

        /// <summary>
        /// HwndHost의 배치 경계가 바뀔 때마다 네이티브 자식 창 크기를 즉시 동기화.
        /// </summary>
        protected override void OnWindowPositionChanged(Rect rcBoundingBox)
        {
            base.OnWindowPositionChanged(rcBoundingBox);
            if (_hwnd != IntPtr.Zero)
            {
                int w = Math.Max(1, (int)Math.Round(rcBoundingBox.Width));
                int h = Math.Max(1, (int)Math.Round(rcBoundingBox.Height));
                SetWindowPos(_hwnd, IntPtr.Zero, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                HwndResized?.Invoke(this, EventArgs.Empty);
            }
        }

        /// <summary>
        /// 일부 시나리오에서 레이아웃만 변하고 WindowPositionChanged가 늦게 오는 경우가 있어
        /// 렌더 크기 변경에도 한 번 더 보정.
        /// </summary>
        protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
        {
            base.OnRenderSizeChanged(sizeInfo);
            if (_hwnd != IntPtr.Zero)
            {
                int w = Math.Max(1, (int)Math.Round(sizeInfo.NewSize.Width));
                int h = Math.Max(1, (int)Math.Round(sizeInfo.NewSize.Height));
                SetWindowPos(_hwnd, IntPtr.Zero, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                HwndResized?.Invoke(this, EventArgs.Empty);
            }
        }
    }
}
