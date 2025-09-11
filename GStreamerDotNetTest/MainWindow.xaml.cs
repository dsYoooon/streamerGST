
using System;
using System.Collections.Generic;
using System.Windows;


using System.Runtime.InteropServices;
using System.Diagnostics;
using System.Linq;
using System.Threading.Tasks;
using System.Threading;
using System.Reflection;

using GStreamerWrapper; // C++/CLI 래퍼 네임스페이스
using System.IO;
using System.Security.Policy;
using System.Text.RegularExpressions;
using System.Windows.Controls;


namespace GStreamerDotNetTest
{
        /// <summary>
        /// Interaction logic for MainWindow.xaml
        /// </summary>
        public partial class MainWindow : Window
        {
            private GstPlayer _player;
            private GstVideoHost _videoHost;
            private StreamConfig[] _configs = Array.Empty<StreamConfig>();
            private string[] _audioDevices = Array.Empty<string>();


            private class StreamSetting
            {
                public ComboBox Monitor;
                public TextBox CropX, CropY, CropW, CropH;
                public ComboBox Resolution;
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

            private void MainWindow_Loaded(object sender, RoutedEventArgs e)
            {
                try
                {
                    // GStreamer 초기화
                    GstPlayer.Initialize();
                }
                catch (Exception ex)
                {
                    MessageBox.Show($"GStreamer 초기화 실패: {ex.Message}\n\nGStreamer 런타임이 설치되었는지, PATH 환경변수가 올바르게 설정되었는지 확인하세요.", "오류", MessageBoxButton.OK, MessageBoxImage.Error);
                    Application.Current.Shutdown();
                    return;
                }

                // 네이티브 창을 호스팅할 HwndHost 생성
                _videoHost = new GstVideoHost();
                videoContainer.Child = _videoHost;

                // HwndHost가 제공하는 창 핸들(Handle)을 GstPlayer에 전달
                _player = new GstPlayer(_videoHost.Handle);
                _audioDevices = GstPlayer.GetAudioDevices();

                int monitorCount = DisplayHelper.GetMonitorCount();
                var buttons = new[] { btnM1Play, btnM2Play, btnM3Play, btnM4Play };
                for (int i = 0; i < buttons.Length; i++)
                {
                    buttons[i].Visibility = i < monitorCount ? Visibility.Visible : Visibility.Collapsed;
                }
            }

            private StreamConfig[] CollectStreamConfigs()
            {
                var list = new List<StreamConfig>();
                foreach (var s in _streamSettings)
                {
                    var cfg = new StreamConfig();

                    int.TryParse(s.Monitor.Text, out cfg.MonitorIndex);
                    int.TryParse(s.CropX.Text, out cfg.CropX);
                    int.TryParse(s.CropY.Text, out cfg.CropY);
                    int.TryParse(s.CropW.Text, out cfg.CropW);
                    int.TryParse(s.CropH.Text, out cfg.CropH);
                    switch (s.Resolution.SelectedItem as string)
                    {
                        case "1080p": cfg.Width = 1920; cfg.Height = 1080; break;
                        case "720p": cfg.Width = 1280; cfg.Height = 720; break;
                        case "Input":
                        default:
                            if (DisplayHelper.TryGetMonitorSize(cfg.MonitorIndex, out var w, out var h))
                            {
                                cfg.Width = cfg.CropW;
                                cfg.Height = cfg.CropH;
                        }
                            else
                            {
                                cfg.Width = 0;
                                cfg.Height = 0;
                            }
                 
                        break;
                    }
                    int.TryParse(s.FrameRate.Text, out cfg.Framerate);
                    int.TryParse(s.Bitrate.Text, out cfg.BitrateKbps);
                    int.TryParse(s.Keyframe.Text, out cfg.KeyframeInterval);
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

            private void btnPlay_Click(object sender, RoutedEventArgs e)
            {
                string serverIp = Textbox_serverIP.Text;
                _configs = CollectStreamConfigs();
                _player?.StartScreenCaptureServer(serverIp, _configs);
            }

            private void btnStop_Click(object sender, RoutedEventArgs e)
            {
                _player?.Stop();
            }

            private void MainWindow_Closing(object sender, System.ComponentModel.CancelEventArgs e)
            {
                // 애플리케이션 종료 시 리소스 정리
                _player?.Stop();
                // GstPlayer가 IDisposable을 구현했다면 _player.Dispose() 호출

                GstPlayer.Deinitialize();
            }

        private void btnMonitorPlay_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn)
            {
                var match = Regex.Match(btn.Name, @"M(\d+)Play", RegexOptions.IgnoreCase);
                int monitorIndex = 0;
                if (match.Success)
                {
                    int idx = int.Parse(match.Groups[1].Value);
                    monitorIndex = Math.Max(0, idx - 1);
                }

                if (DisplayHelper.TryGetMonitorSize(monitorIndex, out int width, out int height))
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
                        KeyframeInterval = 1
                    };
                    _player?.StartScreenCapture(cfg);
                }
            }
        }

        private void Textbox_numofstreamer_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (int.TryParse(Textbox_numofstreamer.Text, out int count) && count >= 0)
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
            var tab = new TabItem { Header = $"stream{index + 1}" };
            var root = new StackPanel { Margin = new Thickness(10) };

            setting.Monitor = new ComboBox();
            for (int i = 0; i < 4; i++) setting.Monitor.Items.Add(i.ToString());
            root.Children.Add(LabeledControl("Monitor Index:", setting.Monitor));

            setting.CropX = new TextBox { Width = 40 };
            setting.CropY = new TextBox { Width = 40 };
            setting.CropW = new TextBox { Width = 40 };
            setting.CropH = new TextBox { Width = 40 };
            var cropPanel = new StackPanel { Orientation = Orientation.Horizontal };
            cropPanel.Children.Add(new Label { Content = "X" }); cropPanel.Children.Add(setting.CropX);
            cropPanel.Children.Add(new Label { Content = "Y" }); cropPanel.Children.Add(setting.CropY);
            cropPanel.Children.Add(new Label { Content = "W" }); cropPanel.Children.Add(setting.CropW);
            cropPanel.Children.Add(new Label { Content = "H" }); cropPanel.Children.Add(setting.CropH);
            root.Children.Add(LabeledControl("Crop:", cropPanel));

            setting.Monitor.SelectionChanged += (s, e) =>
            {
                if (setting.Monitor.SelectedItem is string txt &&
                    int.TryParse(txt, out int idx) &&
                    DisplayHelper.TryGetMonitorSize(idx, out int mw, out int mh))
                {
                    setting.CropX.Text = "0";
                    setting.CropY.Text = "0";
                    setting.CropW.Text = mw.ToString();
                    setting.CropH.Text = mh.ToString();
                }
            };
            setting.Monitor.SelectedIndex = 0;

            setting.Resolution = new ComboBox();
            setting.Resolution.Items.Add("Input");
            setting.Resolution.Items.Add("1080p");
            setting.Resolution.Items.Add("720p");
            setting.Resolution.SelectedIndex = 0;
            root.Children.Add(LabeledControl("Resolution:", setting.Resolution));

            setting.FrameRate = new TextBox { Width = 60, Text = "30" };
            root.Children.Add(LabeledControl("Frame rate:", setting.FrameRate));

            setting.Bitrate = new TextBox { Width = 60, Text = "4000" };
            root.Children.Add(LabeledControl("Bitrate:", setting.Bitrate));

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
            panel.Children.Add(new Label { Content = label, Width = 120 });
            panel.Children.Add(control);
            return panel;
        }

            private void BtnSaveSetting_Click(object sender, RoutedEventArgs e)
            {
                _configs = CollectStreamConfigs();
            }
    }
}