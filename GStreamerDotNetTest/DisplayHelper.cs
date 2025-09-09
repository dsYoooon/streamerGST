using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace GStreamerDotNetTest
{
    internal static class DisplayHelper
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        private delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdcMonitor, ref RECT rect, IntPtr data);

        [DllImport("user32.dll")]
        private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr lprcClip, MonitorEnumProc callback, IntPtr data);

        public static bool TryGetMonitorSize(int index, out int width, out int height)
        {
            var rects = new List<RECT>();
            EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero,
                (IntPtr hMonitor, IntPtr hdc, ref RECT rect, IntPtr data) =>
                {
                    rects.Add(rect);
                    return true;
                }, IntPtr.Zero);

            if (index >= 0 && index < rects.Count)
            {
                var r = rects[index];
                width = r.Right - r.Left;
                height = r.Bottom - r.Top;
                return true;
            }

            width = height = 0;
            return false;
        }
    }
}

