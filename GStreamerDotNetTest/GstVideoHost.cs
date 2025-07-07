using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace GStreamerDotNetTest
{
    public class GstVideoHost : HwndHost
    {
        // Win32 API 함수 임포트
        [DllImport("user32.dll", EntryPoint = "CreateWindowEx", CharSet = CharSet.Unicode)]
        internal static extern IntPtr CreateWindowEx(int dwExStyle, string lpszClassName, string lpszWindowName, int style, int x, int y, int width, int height, IntPtr hwndParent, IntPtr hMenu, IntPtr hInst, [MarshalAs(UnmanagedType.AsAny)] object pvParam);

        [DllImport("user32.dll", EntryPoint = "DestroyWindow", CharSet = CharSet.Unicode)]
        internal static extern bool DestroyWindow(IntPtr hwnd);

        // 창 스타일 상수
        internal const int WS_CHILD = 0x40000000;
        internal const int WS_VISIBLE = 0x10000000;

        private IntPtr _hwnd = IntPtr.Zero;

        public GstVideoHost()
        {
        }

        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            _hwnd = CreateWindowEx(0, "static", "", WS_CHILD | WS_VISIBLE,
                                   0, 0, (int)this.ActualWidth, (int)this.ActualHeight,
                                   hwndParent.Handle, IntPtr.Zero, IntPtr.Zero, 0);

            return new HandleRef(this, _hwnd);
        }

        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            DestroyWindow(hwnd.Handle);
        }

        // 창 크기가 변경될 때 네이티브 창 크기도 조절
        protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
        {
            base.OnRenderSizeChanged(sizeInfo);
            // 이 부분은 추가적인 Win32 API 호출(SetWindowPos)로 구현할 수 있으나
            // 간단한 예제에서는 생략합니다.
        }
    }
}