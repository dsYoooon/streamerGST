
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

            private void btnPlay_Click(object sender, RoutedEventArgs e)
            {
                _player?.StartScreenCaptureServer();
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
                _player?.StartScreenCapture(monitorIndex);
            }
        }
    }
}