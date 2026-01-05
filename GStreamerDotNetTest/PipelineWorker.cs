using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Xml.Serialization;
using GStreamerWrapper;

namespace GStreamerDotNetTest
{
    /// <summary>
    /// DTO used to persist the pipeline configuration to disk so that a
    /// separate worker process can bootstrap the GStreamer pipeline without
    /// touching the main UI process.
    /// </summary>
    [Serializable]
    public class PipelineWorkerSettings
    {
        public string ServerIp { get; set; } = string.Empty;
        public List<StreamConfigContract> Streams { get; set; } = new();

        public static PipelineWorkerSettings FromConfigs(string serverIp, IEnumerable<StreamConfig> configs)
        {
            return new PipelineWorkerSettings
            {
                ServerIp = serverIp ?? string.Empty,
                Streams = configs.Select(StreamConfigContract.FromStreamConfig).ToList()
            };
        }

        public StreamConfig[] ToStreamConfigs()
        {
            return Streams.Select(s => s.ToStreamConfig()).ToArray();
        }
    }

    /// <summary>
    /// Serializable representation of <see cref="StreamConfig"/>. The native
    /// struct lives in the C++/CLI wrapper and cannot be attributed directly
    /// for XML serialization, so this adapter mirrors the fields we need.
    /// </summary>
    [Serializable]
    public class StreamConfigContract
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
        public string AudioDevice { get; set; }
        public bool EnableHardwareAccel { get; set; }
        public bool EnableOsd { get; set; }
        public bool EnableMultiCast { get; set; }
        public string BitrateControl { get; set; }
        public string Profile { get; set; }
        public string OsdText { get; set; }
        public string MultiCastIP { get; set; }
        public string MultiCastInterface { get; set; }

        public static StreamConfigContract FromStreamConfig(StreamConfig cfg)
        {
            return new StreamConfigContract
            {
                MonitorIndex = cfg.MonitorIndex,
                CropX = cfg.CropX,
                CropY = cfg.CropY,
                CropW = cfg.CropW,
                CropH = cfg.CropH,
                Width = cfg.Width,
                Height = cfg.Height,
                Framerate = cfg.Framerate,
                BitrateKbps = cfg.BitrateKbps,
                KeyframeInterval = cfg.KeyframeInterval,
                Port = cfg.Port,
                StreamIndex = cfg.StreamIndex,
                EnableAudio = cfg.EnableAudio,
                AudioDevice = cfg.AudioDevice,
                EnableHardwareAccel = cfg.EnableHardwareAccel,
                EnableOsd = cfg.EnableOsd,
                EnableMultiCast = cfg.EnableMultiCast,
                BitrateControl = cfg.BitrateControl,
                Profile = cfg.Profile,
                OsdText = cfg.OsdText,
                MultiCastIP = cfg.MultiCastIP,
                MultiCastInterface = cfg.MultiCastInterface
            };
        }

        public StreamConfig ToStreamConfig()
        {
            return new StreamConfig
            {
                MonitorIndex = MonitorIndex,
                CropX = CropX,
                CropY = CropY,
                CropW = CropW,
                CropH = CropH,
                Width = Width,
                Height = Height,
                Framerate = Framerate,
                BitrateKbps = BitrateKbps,
                KeyframeInterval = KeyframeInterval,
                Port = Port,
                StreamIndex = StreamIndex,
                EnableAudio = EnableAudio,
                AudioDevice = AudioDevice,
                EnableHardwareAccel = EnableHardwareAccel,
                EnableOsd = EnableOsd,
                EnableMultiCast = EnableMultiCast,
                BitrateControl = BitrateControl,
                Profile = Profile,
                OsdText = OsdText,
                MultiCastIP = MultiCastIP,
                MultiCastInterface = MultiCastInterface
            };
        }
    }

    /// <summary>
    /// Launches and controls the external worker process that hosts the
    /// GStreamer pipeline. When qsvh264enc (or any plugin) crashes, only this
    /// process should terminate rather than the main WPF UI.
    /// </summary>
    public class PipelineWorkerLauncher : IDisposable
    {
        private Process _workerProcess;

        public bool IsRunning => _workerProcess != null && !_workerProcess.HasExited;

        public void Start(string serverIp, StreamConfig[] configs)
        {
            Stop();

            var settings = PipelineWorkerSettings.FromConfigs(serverIp, configs);
            string configPath = WriteSettingsToTempFile(settings);

            var currentExe = Process.GetCurrentProcess().MainModule?.FileName;
            if (string.IsNullOrWhiteSpace(currentExe))
            {
                throw new InvalidOperationException("현재 실행 파일 경로를 확인할 수 없습니다.");
            }

            var psi = new ProcessStartInfo
            {
                FileName = currentExe,
                Arguments = $"--pipeline-worker \"{configPath}\"",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = false,
                RedirectStandardError = false
            };

            _workerProcess = Process.Start(psi);
        }

        public void Stop()
        {
            try
            {
                if (_workerProcess != null && !_workerProcess.HasExited)
                {
                    _workerProcess.Kill(true);
                    _workerProcess.WaitForExit(2000);
                }
            }
            catch
            {
                // best effort; failures here should not crash the UI
            }
            finally
            {
                _workerProcess?.Dispose();
                _workerProcess = null;
            }
        }

        private static string WriteSettingsToTempFile(PipelineWorkerSettings settings)
        {
            string tempPath = Path.Combine(Path.GetTempPath(), $"gst-pipeline-{Guid.NewGuid():N}.xml");
            using (var stream = File.Create(tempPath))
            {
                var serializer = new XmlSerializer(typeof(PipelineWorkerSettings));
                serializer.Serialize(stream, settings);
            }
            return tempPath;
        }

        public void Dispose()
        {
            Stop();
        }
    }

    /// <summary>
    /// Entry point for the worker process. This runs without the WPF UI and
    /// hosts the RTSP server until the process is terminated by the parent.
    /// </summary>
    public static class PipelineWorkerMode
    {
        public static bool TryRun(string[] args)
        {
            if (args.Length >= 2 && string.Equals(args[0], "--pipeline-worker", StringComparison.OrdinalIgnoreCase))
            {
                Run(args[1]);
                return true;
            }
            return false;
        }

        private static void Run(string settingsPath)
        {
            if (!File.Exists(settingsPath))
            {
                return;
            }

            PipelineWorkerSettings settings;
            using (var stream = File.OpenRead(settingsPath))
            {
                var serializer = new XmlSerializer(typeof(PipelineWorkerSettings));
                settings = serializer.Deserialize(stream) as PipelineWorkerSettings;
            }

            if (settings == null)
            {
                return;
            }

            GstPlayer.Initialize();

            using (var player = new GstPlayer(IntPtr.Zero))
            {
                player.StartScreenCaptureServer(settings.ServerIp, settings.ToStreamConfigs());

                using (var quitEvent = new ManualResetEvent(false))
                {
                    Console.CancelKeyPress += (s, e) =>
                    {
                        quitEvent.Set();
                        e.Cancel = true;
                    };

                    AppDomain.CurrentDomain.ProcessExit += (s, e) => quitEvent.Set();

                    quitEvent.WaitOne();
                }

                player.Stop();
            }

            GstPlayer.Deinitialize();

            try
            {
                File.Delete(settingsPath);
            }
            catch
            {
                // ignore cleanup errors
            }
        }
    }
}

