
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
using Forms = System.Windows.Forms;


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


            private class StreamSetting
            {
                public ComboBox Monitor;
                public TextBox CropX, CropY, CropW, CropH;
                public ComboBox Resolution;
                public TextBox FrameRate, Bitrate, Keyframe;
                public ComboBox BitrateControl;
                public ComboBox Profile;
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
                            if (cfg.MonitorIndex >= 0 && cfg.MonitorIndex < Forms.Screen.AllScreens.Length)
                            {
                                var bounds = Forms.Screen.AllScreens[cfg.MonitorIndex].Bounds;
                                cfg.Width = bounds.Width;
                                cfg.Height = bounds.Height;
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
                if (_configs != null && monitorIndex < _configs.Length)
                {
                    _player?.StartScreenCapture(_configs[monitorIndex]);
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

            setting.Resolution = new ComboBox();
            setting.Resolution.Items.Add("Input");
            setting.Resolution.Items.Add("1080p");
            setting.Resolution.Items.Add("720p");
            root.Children.Add(LabeledControl("Resolution:", setting.Resolution));

            setting.FrameRate = new TextBox { Width = 60 };
            root.Children.Add(LabeledControl("Frame rate:", setting.FrameRate));

            setting.Bitrate = new TextBox { Width = 60 };
            root.Children.Add(LabeledControl("Bitrate:", setting.Bitrate));

            setting.BitrateControl = new ComboBox();
            setting.BitrateControl.Items.Add("CBR");
            setting.BitrateControl.Items.Add("VBR");
            root.Children.Add(LabeledControl("Bitrate Control:", setting.BitrateControl));

            setting.Keyframe = new TextBox { Width = 60 };
            root.Children.Add(LabeledControl("Keyframe Interval:", setting.Keyframe));

            setting.Profile = new ComboBox();
            setting.Profile.Items.Add("main");
            setting.Profile.Items.Add("baseline");
            setting.Profile.Items.Add("high");
            root.Children.Add(LabeledControl("Profile:", setting.Profile));

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