using GStreamerWrapper;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;

namespace GStreamerDotNetTest
{
    /// <summary>
    /// 파이프라인 실행을 별도 프로세스로 분리하여 GStreamer 플러그인 크래시가
    /// 주 프로세스에 전파되지 않도록 한다.
    /// </summary>
    internal static class PipelineWorkerBootstrap
    {
        private const string WorkerFlag = "--pipeline-worker";

        public static bool TryRunWorker(string[] args)
        {
            if (args == null || args.Length < 2 || !string.Equals(args[0], WorkerFlag, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            var payloadPath = args[1];
            if (!File.Exists(payloadPath))
            {
                Console.Error.WriteLine($"[worker] payload file not found: {payloadPath}");
                return true; // UI를 띄우지 않도록 true 반환
            }

            PipelineWorkerPayload payload;
            try
            {
                payload = PipelineWorkerPayload.Load(payloadPath);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[worker] failed to load payload: {ex.Message}");
                return true;
            }

            RunWorker(payload);
            return true;
        }

        private static void RunWorker(PipelineWorkerPayload payload)
        {
            Console.WriteLine("[worker] starting pipeline worker");

            try
            {
                GstPlayer.Initialize();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[worker] GstPlayer initialization failed: {ex.Message}");
                return;
            }

            var configs = payload.ToStreamConfigs();
            var player = new GstPlayer(IntPtr.Zero);

            Console.CancelKeyPress += (s, e) =>
            {
                e.Cancel = true;
                StopPlayer(player);
            };

            try
            {
                player.StartScreenCaptureServer(payload.ServerIp, configs);
                Console.WriteLine("[worker] pipeline started. Waiting for stdin close or Ctrl+C...");
                WaitForExitSignal(player);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[worker] pipeline failed: {ex.Message}");
            }
            finally
            {
                StopPlayer(player);
                GstPlayer.Deinitialize();
            }
        }

        private static void WaitForExitSignal(GstPlayer player)
        {
            // 표준 입력이 닫히거나 "quit" 라인을 받으면 종료한다.
            while (true)
            {
                var line = Console.ReadLine();
                if (line == null || line.Trim().Equals("quit", StringComparison.OrdinalIgnoreCase))
                {
                    break;
                }
            }
            StopPlayer(player);
        }

        private static void StopPlayer(GstPlayer player)
        {
            try
            {
                player?.Stop();
            }
            catch
            {
                // 워커 프로세스가 종료될 것이므로 추가 처리 없음
            }
        }
    }

    /// <summary>
    /// 메인 프로세스와 워커 프로세스 간에 전달되는 스트림 구성 데이터.
    /// </summary>
    [DataContract]
    internal class PipelineWorkerPayload
    {
        [DataMember]
        public string ServerIp { get; set; }

        [DataMember]
        public List<StreamConfigDto> Streams { get; set; } = new List<StreamConfigDto>();

        public static PipelineWorkerPayload FromConfigs(string serverIp, IEnumerable<StreamConfig> configs)
        {
            var payload = new PipelineWorkerPayload { ServerIp = serverIp };
            if (configs != null)
            {
                foreach (var cfg in configs)
                {
                    payload.Streams.Add(StreamConfigDto.FromStreamConfig(cfg));
                }
            }
            return payload;
        }

        public StreamConfig[] ToStreamConfigs()
        {
            var list = new List<StreamConfig>();
            foreach (var dto in Streams)
            {
                list.Add(dto.ToStreamConfig());
            }
            return list.ToArray();
        }

        public void Save(string path)
        {
            using (var fs = File.Create(path))
            {
                var serializer = new DataContractJsonSerializer(typeof(PipelineWorkerPayload));
                serializer.WriteObject(fs, this);
            }
        }

        public static PipelineWorkerPayload Load(string path)
        {
            using (var fs = File.OpenRead(path))
            {
                var serializer = new DataContractJsonSerializer(typeof(PipelineWorkerPayload));
                return (PipelineWorkerPayload)serializer.ReadObject(fs);
            }
        }
    }

    [DataContract]
    internal class StreamConfigDto
    {
        [DataMember] public int MonitorIndex { get; set; }
        [DataMember] public int CropX { get; set; }
        [DataMember] public int CropY { get; set; }
        [DataMember] public int CropW { get; set; }
        [DataMember] public int CropH { get; set; }
        [DataMember] public int Width { get; set; }
        [DataMember] public int Height { get; set; }
        [DataMember] public int Framerate { get; set; }
        [DataMember] public int BitrateKbps { get; set; }
        [DataMember] public int KeyframeInterval { get; set; }
        [DataMember] public int Port { get; set; }
        [DataMember] public int StreamIndex { get; set; }
        [DataMember] public bool EnableAudio { get; set; }
        [DataMember] public string AudioDevice { get; set; }
        [DataMember] public bool EnableHardwareAccel { get; set; }
        [DataMember] public bool EnableOsd { get; set; }
        [DataMember] public bool EnableMultiCast { get; set; }
        [DataMember] public string BitrateControl { get; set; }
        [DataMember] public string Profile { get; set; }
        [DataMember] public string OsdText { get; set; }
        [DataMember] public string MultiCastIP { get; set; }
        [DataMember] public string MultiCastInterface { get; set; }

        public static StreamConfigDto FromStreamConfig(StreamConfig cfg)
        {
            return new StreamConfigDto
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
    /// 메인 프로세스에서 파이프라인 워커를 관리한다.
    /// </summary>
    internal sealed class PipelineProcessManager : IDisposable
    {
        private Process _workerProcess;
        private string _payloadPath;

        public void Start(string serverIp, StreamConfig[] configs)
        {
            Stop();

            var payload = PipelineWorkerPayload.FromConfigs(serverIp, configs);
            _payloadPath = Path.Combine(Path.GetTempPath(), $"gst_worker_{Guid.NewGuid():N}.json");
            payload.Save(_payloadPath);

            var exePath = Process.GetCurrentProcess().MainModule.FileName;
            var psi = new ProcessStartInfo
            {
                FileName = exePath,
                Arguments = $"{WorkerFlag} \"{_payloadPath}\"",
                UseShellExecute = false,
                RedirectStandardInput = true,
                CreateNoWindow = true
            };

            _workerProcess = Process.Start(psi);
        }

        public void Stop()
        {
            try
            {
                if (_workerProcess != null && !_workerProcess.HasExited)
                {
                    try
                    {
                        _workerProcess.StandardInput.WriteLine("quit");
                        if (!_workerProcess.WaitForExit(2000))
                        {
                            _workerProcess.Kill();
                        }
                    }
                    catch
                    {
                        _workerProcess.Kill();
                    }
                }
            }
            finally
            {
                _workerProcess?.Dispose();
                _workerProcess = null;

                if (!string.IsNullOrEmpty(_payloadPath) && File.Exists(_payloadPath))
                {
                    try { File.Delete(_payloadPath); } catch { /* best effort */ }
                    _payloadPath = null;
                }
            }
        }

        public void Dispose()
        {
            Stop();
        }
    }
}
