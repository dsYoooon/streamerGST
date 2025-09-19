using System;
using System.Collections.Generic;
using System.Windows;
using System.Diagnostics;
using System.Text.RegularExpressions;
using System.Windows.Controls;
using GStreamerWrapper; // C++/CLI 래퍼 네임스페이스

namespace GStreamerDotNetTest
{
    public partial class MainWindow : Window
    {
        private GstPlayer _player;
        private GstVideoHost _videoHost;
        private StreamConfig[] _configs = new StreamConfig[0];
        private string[] _audioDevices = new string[0];

        // --- StreamSetting 클래스는 변경 없음 ---
        private class StreamSetting
        {
            public ComboBox Monitor;
            public TextBox CropX, CropY, CropW, CropH;
            public ComboBox Resolution;
            public bool UseInputResolution;
            public TextBox FrameRate, Bitrate, Keyframe;
            public ComboBox BitrateControl;
            public ComboBox Profile;
            public ComboBox AudioEnable;
            public ComboBox AudioDevice;
            public ComboBox HwAccel;
            public ComboBox OsdEnable;
            public TabItem Tab;
        }

        private readonly List<StreamSetting> _streamSettings = new List<StreamSetting>();

        public MainWindow()
        {
            InitializeComponent();
        }
        const int defaultStreamCount = 2;
        private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                GstPlayer.Initialize();
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    "GStreamer 초기화 실패: " + ex.Message + "\n\nGStreamer 런타임과 PATH를 확인하세요.",
                    "오류", MessageBoxButton.OK, MessageBoxImage.Error);
                Application.Current.Shutdown();
                return;
            }

            _videoHost = new GstVideoHost();
            videoContainer.Child = _videoHost;
            _player = new GstPlayer(_videoHost.Handle);
            // ★ MTA 백그라운드에서 디바이스 열거
            _audioDevices = await System.Threading.Tasks.Task.Run(() => GstPlayer.GetAudioDevices());

            int monitorCount = DisplayHelper.GetMonitorCount();
            var buttons = new[] { btnM1Play, btnM2Play, btnM3Play, btnM4Play };
            for (int i = 0; i < buttons.Length; i++)
                buttons[i].Visibility = i < monitorCount ? Visibility.Visible : Visibility.Collapsed;

            // =================================================================
            // ★ 수정된 부분 1: 프로그램 시작 시 기본 스트림 4개로 UI 설정
            // =================================================================
            
            Textbox_numofstreamer.Text = defaultStreamCount.ToString();
            UpdateStreamTabs(defaultStreamCount);
            // =================================================================
            startServer(true);
        }

        // ★ 추가된 부분: 기본 설정으로 4개 스트림 구성을 생성하는 함수
        private StreamConfig[] CreateDefaultStreamConfigs(int count)
        {
            var list = new List<StreamConfig>();
            int monitorCount = DisplayHelper.GetMonitorCount();

            for (int i = 0; i < count; i++)
            {
                // 모니터 개수보다 스트림이 많으면 모니터 인덱스를 순환시킵니다.
                int monitorIndex = (monitorCount > 0) ? (i % monitorCount) : 0;

                int width, height;
                // 모니터 크기를 가져오지 못하면 기본 1920x1080으로 설정합니다.
                if (!DisplayHelper.TryGetMonitorSize(monitorIndex, out width, out height))
                {
                    width = 1920;
                    height = 1080;
                }
                if (i == 2 || i == 1)
                {
                    var cfg = new StreamConfig
                    {
                        MonitorIndex = monitorIndex,
                        CropX = 0,
                        CropY = 0,
                        CropW = width,
                        CropH = height,
                        Width = width,
                        Height = height,
                        Framerate = 30,
                        BitrateKbps = 8000,
                        KeyframeInterval = 30,
                        Port = 10554 + i,
                        EnableAudio = false,
                        AudioDevice = (_audioDevices.Length > 0) ? _audioDevices[0] : null,
                        EnableHardwareAccel = true,
                        EnableOsd = true,
                        EnableMultiCast = false,
                        BitrateControl = "CBR",
                        Profile = "high",
                        OsdText = $"Screen {i + 1}"
                    };
                    list.Add(cfg);
                }
                else
                {
                    var cfg = new StreamConfig
                    {
                        MonitorIndex = monitorIndex,
                        CropX = 0,
                        CropY = 0,
                        CropW = width,
                        CropH = height,
                        Width = width,
                        Height = height,
                        Framerate = 30,
                        BitrateKbps = 8000,
                        KeyframeInterval = 30,
                        Port = 10554 + i,
                        EnableAudio = false,
                        AudioDevice = (_audioDevices.Length > 0) ? _audioDevices[0] : null,
                        EnableHardwareAccel = false,
                        EnableOsd = true,
                        BitrateControl = "CBR",
                        EnableMultiCast = false,
                        Profile = "baseline",
                        OsdText = $"Screen {i + 1}"
                    };
                    list.Add(cfg);
                }
                    
                //if(i==2 || i==1)
                //if (i != 2)
                    
            }
            return list.ToArray();
        }
        private void startServer(bool isFirst)
        {
            string serverIp = (Textbox_serverIP.Text ?? "").Trim();

            // =================================================================
            // ★ 수정된 부분 2: UI 설정 대신 기본 설정 값 사용
            // =================================================================
            if (isFirst) _configs = CreateDefaultStreamConfigs(defaultStreamCount);
            
            // =================================================================

            if (_player != null)
                _player.StartScreenCaptureServer(serverIp, _configs);
        }
        private void btnPlay_Click(object sender, RoutedEventArgs e)
        {
            startServer(false);
        }

        // --- 이하 나머지 코드는 변경 없습니다. ---

        private StreamConfig[] CollectStreamConfigs()
        {
            var list = new List<StreamConfig>();
            int idx = 0;
            foreach (var s in _streamSettings)
            {
                var cfg = new StreamConfig();

                int mi;
                int.TryParse(s.Monitor.Text, out mi);
                cfg.MonitorIndex = mi;

                int cx, cy, cw, ch;
                int.TryParse(s.CropX.Text, out cx);
                int.TryParse(s.CropY.Text, out cy);
                int.TryParse(s.CropW.Text, out cw);
                int.TryParse(s.CropH.Text, out ch);
                cfg.CropX = cx; cfg.CropY = cy; cfg.CropW = cw; cfg.CropH = ch;

                bool useInput = s.UseInputResolution;

                if (useInput)
                {
                    if (cw > 0 && ch > 0)
                    {
                        cfg.Width = cw;
                        cfg.Height = ch;
                    }
                    else
                    {
                        int mw, mh;
                        if (DisplayHelper.TryGetMonitorSize(cfg.MonitorIndex, out mw, out mh))
                        {
                            cfg.CropX = 0; cfg.CropY = 0;
                            cfg.CropW = mw; cfg.CropH = mh;
                            cfg.Width = mw; cfg.Height = mh;
                        }
                        else
                        {
                            cfg.Width = 0; cfg.Height = 0;
                        }
                    }
                }
                else
                {
                    string res = s.Resolution.SelectedItem as string;
                    if (res == "1080p") { cfg.Width = 1920; cfg.Height = 1080; }
                    else if (res == "720p") { cfg.Width = 1280; cfg.Height = 720; }
                    else { cfg.Width = 0; cfg.Height = 0; }
                }

                int fr, br, ki;
                int.TryParse(s.FrameRate.Text, out fr);
                int.TryParse(s.Bitrate.Text, out br);
                int.TryParse(s.Keyframe.Text, out ki);
                cfg.Framerate = fr;
                cfg.BitrateKbps = br;
                cfg.KeyframeInterval = ki;
                cfg.Port = 10554 + idx++;
                cfg.BitrateControl = s.BitrateControl.SelectedItem as string;
                cfg.Profile = s.Profile.SelectedItem as string;

                cfg.EnableAudio = s.AudioEnable.SelectedIndex == 0;
                cfg.AudioDevice = s.AudioDevice.SelectedItem as string;

                cfg.EnableHardwareAccel = s.HwAccel.SelectedIndex == 0;
                cfg.EnableOsd = s.OsdEnable.SelectedIndex == 0;

                list.Add(cfg);
            }

            return list.ToArray();
        }

        private void btnStop_Click(object sender, RoutedEventArgs e)
        {
            if (_player != null) _player.Stop();
        }

        private void MainWindow_Closing(object sender, System.ComponentModel.CancelEventArgs e)
        {
            if (_player != null) _player.Stop();
            GstPlayer.Deinitialize();
        }

        private void btnMonitorPlay_Click(object sender, RoutedEventArgs e)
        {
            var btn = sender as Button;
            if (btn == null) return;

            var match = Regex.Match(btn.Name, @"M(\d+)Play", RegexOptions.IgnoreCase);
            int monitorIndex = 0;
            if (match.Success)
            {
                int idx = int.Parse(match.Groups[1].Value);
                monitorIndex = Math.Max(0, idx - 1);
            }

            int width, height;
            if (DisplayHelper.TryGetMonitorSize(monitorIndex, out width, out height))
            {
                var cfg = new StreamConfig
                {
                    MonitorIndex = monitorIndex,
                    CropX = 0,
                    CropY = 0,
                    CropW = width,
                    CropH = height,
                    Width = width,
                    Height = height,
                    Framerate = 30,
                    BitrateKbps = 4000,
                    KeyframeInterval = 1,
                    EnableAudio = true,
                    EnableHardwareAccel = true,
                    EnableOsd = true,
                    BitrateControl = "CBR",
                    Profile = "high",
                };
                if (_player != null) _player.StartScreenCapture(cfg);
            }
        }

        private void Textbox_numofstreamer_TextChanged(object sender, TextChangedEventArgs e)
        {
            int count;
            if (int.TryParse(Textbox_numofstreamer.Text, out count) && count >= 0)
            {
                UpdateStreamTabs(count);
            }
        }

        private void UpdateStreamTabs(int count)
        {
            StreamSettingsTab.Items.Clear();
            _streamSettings.Clear();
            for (int i = 0; i < count; i++)
            {
                var setting = CreateStreamSettingTab(i);
                _streamSettings.Add(setting);
                StreamSettingsTab.Items.Add(setting.Tab);
            }
        }

        private StreamSetting CreateStreamSettingTab(int index)
        {
            var setting = new StreamSetting();
            var tab = new TabItem { Header = "stream" + (index + 1) };
            var root = new StackPanel { Margin = new Thickness(10) };

            setting.Monitor = new ComboBox();
            for (int i = 0; i < 4; i++) setting.Monitor.Items.Add(i.ToString());
            root.Children.Add(LabeledControl("Monitor Index:", setting.Monitor));

            setting.CropX = new TextBox { Width = 40 };
            setting.CropY = new TextBox { Width = 40 };
            setting.CropW = new TextBox { Width = 60 };
            setting.CropH = new TextBox { Width = 60 };
            var cropPanel = new StackPanel { Orientation = Orientation.Horizontal };
            cropPanel.Children.Add(new Label { Content = "X" }); cropPanel.Children.Add(setting.CropX);
            cropPanel.Children.Add(new Label { Content = "Y" }); cropPanel.Children.Add(setting.CropY);
            cropPanel.Children.Add(new Label { Content = "W" }); cropPanel.Children.Add(setting.CropW);
            cropPanel.Children.Add(new Label { Content = "H" }); cropPanel.Children.Add(setting.CropH);
            root.Children.Add(LabeledControl("Crop:", cropPanel));

            setting.Monitor.SelectionChanged += (s, e) =>
            {
                string txt = setting.Monitor.SelectedItem as string;
                int idx;
                if (!string.IsNullOrEmpty(txt) && int.TryParse(txt, out idx))
                {
                    int mw, mh;
                    if (DisplayHelper.TryGetMonitorSize(idx, out mw, out mh))
                    {
                        setting.CropX.Text = "0";
                        setting.CropY.Text = "0";
                        setting.CropW.Text = mw.ToString();
                        setting.CropH.Text = mh.ToString();
                    }
                }
            };
            setting.Monitor.SelectedIndex = 0;

            setting.Resolution = new ComboBox();
            setting.Resolution.Items.Add("Input");
            setting.Resolution.Items.Add("1080p");
            setting.Resolution.Items.Add("720p");
            setting.Resolution.SelectedIndex = 0;
            setting.UseInputResolution = true;
            setting.Resolution.SelectionChanged += (s, e) =>
            {
                var sel = setting.Resolution.SelectedItem as string;
                setting.UseInputResolution = (sel == "Input");
            };
            root.Children.Add(LabeledControl("Resolution:", setting.Resolution));

            var hint = new TextBlock
            {
                Text = "Tip: Resolution=Input이면 Crop W/H가 출력해상도가 됩니다.",
                Margin = new Thickness(0, 2, 0, 6),
                Opacity = 0.7
            };
            root.Children.Add(hint);

            setting.FrameRate = new TextBox { Width = 60, Text = "30" };
            root.Children.Add(LabeledControl("Frame rate:", setting.FrameRate));

            setting.Bitrate = new TextBox { Width = 80, Text = "4000" };
            root.Children.Add(LabeledControl("Bitrate (kbps):", setting.Bitrate));

            setting.BitrateControl = new ComboBox();
            setting.BitrateControl.Items.Add("CBR");
            setting.BitrateControl.Items.Add("VBR");
            setting.BitrateControl.SelectedIndex = 0;
            root.Children.Add(LabeledControl("Bitrate Control:", setting.BitrateControl));

            setting.Keyframe = new TextBox { Width = 60, Text = "1" };
            root.Children.Add(LabeledControl("Keyframe Interval:", setting.Keyframe));

            setting.Profile = new ComboBox();
            setting.Profile.Items.Add("main");
            setting.Profile.Items.Add("baseline");
            setting.Profile.Items.Add("high");
            setting.Profile.SelectedItem = "baseline";
            root.Children.Add(LabeledControl("Profile:", setting.Profile));

            setting.AudioEnable = new ComboBox();
            setting.AudioEnable.Items.Add("Use");
            setting.AudioEnable.Items.Add("Don't use");
            setting.AudioEnable.SelectedIndex = 0;
            root.Children.Add(LabeledControl("Audio Stream:", setting.AudioEnable));

            setting.AudioDevice = new ComboBox();
            foreach (var dev in _audioDevices) setting.AudioDevice.Items.Add(dev);
            if (setting.AudioDevice.Items.Count > 0) setting.AudioDevice.SelectedIndex = 0;
            root.Children.Add(LabeledControl("Audio Device:", setting.AudioDevice));

            setting.HwAccel = new ComboBox();
            setting.HwAccel.Items.Add("Use");
            setting.HwAccel.Items.Add("Don't use");
            setting.HwAccel.SelectedIndex = 0;
            root.Children.Add(LabeledControl("HW Acceleration:", setting.HwAccel));

            setting.OsdEnable = new ComboBox();
            setting.OsdEnable.Items.Add("Use");
            setting.OsdEnable.Items.Add("Don't use");
            setting.OsdEnable.SelectedIndex = 0;
            root.Children.Add(LabeledControl("OSD:", setting.OsdEnable));

            tab.Content = root;
            setting.Tab = tab;
            return setting;
        }

        private FrameworkElement LabeledControl(string label, UIElement control)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 2, 0, 2) };
            panel.Children.Add(new Label { Content = label, Width = 140 });
            panel.Children.Add(control);
            return panel;
        }

        private void BtnSaveSetting_Click(object sender, RoutedEventArgs e)
        {
            _configs = CollectStreamConfigs();
        }
    }
}