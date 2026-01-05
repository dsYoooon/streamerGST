using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace GStreamerDotNetTest
{
    public class StreamConfig
    {
        public int MonitorIndex { get; set; }
        public int CropX { get; set; }
        public int CropY { get; set; }
        public int CropW { get; set; }
        public int CropH { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }
        public int Framerate { get; set; }
        public int BitrateKbps { get; set; }
        public int KeyframeInterval { get; set; }
        public int Port { get; set; }
        public int StreamIndex { get; set; }
        public bool EnableAudio { get; set; }
        public bool EnableMultiCast { get; set; }
        public string AudioDevice { get; set; }
        public bool EnableHardwareAccel { get; set; }
        public bool EnableOsd { get; set; }
        public string BitrateControl { get; set; }
        public string Profile { get; set; }
        public string OsdText { get; set; }
        public string MultiCastIP { get; set; }
        public string MultiCastInterface { get; set; }
    }

    public class GstProcessManager : IDisposable
    {
        private Process _process;
        private readonly object _sync = new object();
        private StreamConfig[] _lastConfigs = Array.Empty<StreamConfig>();
        private string _lastServerIp = string.Empty;
        private IntPtr _lastPreviewHandle = IntPtr.Zero;
        private int _lastPreviewMonitor = 0;
        private StreamConfig _lastPreviewConfig = null;
        private bool _disposed;

        public event Action<string> OutputReceived;

        public string ExecutablePath { get; set; }

        public GstProcessManager(string executablePath)
        {
            ExecutablePath = executablePath;
        }

        public void Start()
        {
            lock (_sync)
            {
                if (_process != null && !_process.HasExited)
                    return;

                var psi = new ProcessStartInfo
                {
                    FileName = ExecutablePath,
                    RedirectStandardInput = true,
                    RedirectStandardOutput = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    // [추가] 서브프로세스의 출력을 UTF-8로 읽도록 설정
                    //StandardOutputEncoding = Encoding.Default
                };

                _process = new Process { StartInfo = psi, EnableRaisingEvents = true };
                _process.Exited += OnProcessExited;
                _process.OutputDataReceived += (s, e) =>
                {
                    if (e.Data != null)
                        OutputReceived?.Invoke(e.Data);
                };

                _process.Start();
                _process.BeginOutputReadLine();

                if (_lastConfigs.Length > 0)
                {
                    SendStartServer(_lastServerIp, _lastConfigs);
                }
                if (_lastPreviewHandle != IntPtr.Zero)
                {
                    SendStartPreview(_lastPreviewHandle, _lastPreviewMonitor, _lastPreviewConfig);
                }
            }
        }

        public void Stop()
        {
            lock (_sync)
            {
                try
                {
                    _process?.StandardInput.WriteLine("CMD_EXIT");
                }
                catch { }
                finally
                {
                    _process?.Kill();
                    _process?.Dispose();
                    _process = null;
                }
            }
        }

        public void Restart()
        {
            Stop();
            Start();
        }

        public void SendStartServer(string serverIp, StreamConfig[] configs)
        {
            if (configs == null) configs = Array.Empty<StreamConfig>();
            _lastConfigs = configs.ToArray();
            _lastServerIp = serverIp ?? string.Empty;
            var cmd = new StringBuilder();
            cmd.Append("CMD_START_SERVER ");
            cmd.Append(_lastServerIp.Replace(' ', '_')); // IP는 공백 없음
            cmd.Append(' ');
            cmd.Append(_lastConfigs.Length);

            foreach (var c in _lastConfigs)
            {
                cmd.Append(' ');
                AppendConfig(cmd, c);
            }

            SendCommand(cmd.ToString());
        }

        public void SendStopServer()
        {
            SendCommand("CMD_STOP_SERVER");
        }

        public void SendStartPreview(IntPtr hwnd, int monitorIndex, StreamConfig previewConfig = null)
        {
            _lastPreviewHandle = hwnd;
            _lastPreviewMonitor = monitorIndex;
            _lastPreviewConfig = previewConfig;

            var cmd = new StringBuilder();
            cmd.AppendFormat("CMD_START_PREVIEW {0} {1}", hwnd.ToInt64(), monitorIndex);

            if (previewConfig != null)
            {
                cmd.Append(" CFG ");
                AppendConfig(cmd, previewConfig);
            }

            SendCommand(cmd.ToString());
        }

        public void SendStopPreview()
        {
            _lastPreviewHandle = IntPtr.Zero;
            SendCommand("CMD_STOP_PREVIEW");
        }

        public void SendUpdatePreviewRectangle(IntPtr hwnd)
        {
            _lastPreviewHandle = hwnd;
            if (hwnd == IntPtr.Zero)
                return;

            SendCommand($"CMD_UPDATE_PREVIEW_RECT {hwnd.ToInt64()}");
        }

        private void SendCommand(string command)
        {
            lock (_sync)
            {
                if (_process == null || _process.HasExited)
                    return;
                _process.StandardInput.WriteLine(command);
                _process.StandardInput.Flush();
            }
        }

        private void OnProcessExited(object sender, EventArgs e)
        {
            if (_disposed) return;
            Task.Delay(500).ContinueWith(_ => Start());
        }

        // GstProcessManager.cs 맨 아래 부분

        private static string Encode(string value)
        {
            // [수정] 빈 값일 경우 Base64 변환 시 ""가 되므로, 공백 파싱이 밀리는 문제 해결을 위해 명시적 토큰 사용
            if (string.IsNullOrEmpty(value)) return "__EMPTY__";
            return Convert.ToBase64String(Encoding.UTF8.GetBytes(value));
        }

        private static void AppendConfig(StringBuilder cmd, StreamConfig c)
        {
            cmd.AppendFormat("{0} {1} {2} {3} {4} {5} {6} {7} {8} {9} {10} {11} {12} {13} {14} {15} {16} {17} {18} {19} {20} {21}",
                c.MonitorIndex,
                c.CropX,
                c.CropY,
                c.CropW,
                c.CropH,
                c.Width,
                c.Height,
                c.Framerate,
                c.BitrateKbps,
                c.KeyframeInterval,
                c.Port,
                c.StreamIndex,
                c.EnableAudio ? 1 : 0,
                c.EnableMultiCast ? 1 : 0,
                Encode(c.AudioDevice),
                c.EnableHardwareAccel ? 1 : 0,
                c.EnableOsd ? 1 : 0,
                Encode(c.BitrateControl),
                Encode(c.Profile),
                Encode(c.OsdText),
                Encode(c.MultiCastIP),
                Encode(c.MultiCastInterface));
        }

        public void Dispose()
        {
            _disposed = true;
            Stop();
        }
    }
}

