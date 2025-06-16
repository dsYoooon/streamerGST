//#define play
#define freeR
using System;
using System.Collections.Generic;
using System.Windows;


using System.Runtime.InteropServices;
using System.Diagnostics;
using System.Linq;
using System.Threading.Tasks;
using System.Threading;
using System.Reflection;

using GStreamerWrapperCLI;
using System.IO;
using System.Security.Policy;
using System.Text.RegularExpressions;
namespace GStreamerDotNetTest
{

    public static class cb
    {
        //static int width = 960;
        //static int height = 540;
        static int width = 1920;
        static int height = 1080;
        private static List<ConsumerBinWrapper> consumerBins = new List<ConsumerBinWrapper>();
        public static async Task init(int cnt)
        {
           await initCunsumerbin(cnt);
            Task.Delay(500);
        }
        public static void setConsumerbin(int idx, string src)
        {
            ConsumerBinWrapper cbw = consumerBins.FirstOrDefault(bin => bin== consumerBins[idx]);
            Debug.WriteLine("before atach" + src + " " + idx);
            //SetWindow(idx, !src.Contains("fakesrc://"));
            //Task.Run(() =>
            //{
                //if (SourceManagerWrapper.IsConsumerBinAttached(src,cbw.GetBin()))
                //{

                //}
                //else
                {
                 
                //SourceManagerWrapper.setInfo();
                // detach from fakesrc using raw branch (false)
                Task.Delay(34);
                SourceManagerWrapper.AutoDetachConsumerBin(cbw.GetBin());
                    int retry = 0;


                // attach to actual RTSP URL using RTSP branch (true) 
                //consumerBins[idx].ShowWin(!src.Contains("fakesrc://"));

                //consumerBins[idx].ReconfigureSink();
                if (src.Contains("fakesrc"))
                {

                }
                else
                {
                        while (!SourceManagerWrapper.AttachConsumerBin(src, cbw.GetBin(), true) && retry++ < 3)
                        {
                            Debug.WriteLine("FAIL {0} attach", idx);
                            Task.Delay(34);
                        }

                    }






                }

           // });
             
            
            //SourceManagerWrapper.CheckTee(src);


        }
    
        public static void allFree(int cnt)
        {
            for (int i = cnt  -1; i >= 0 ; i--)
            {
                SourceManagerWrapper.DetachConsumerBin(consumerBins[i].GetBin());
                var cb = consumerBins[i];
                cb.Dispose();
                consumerBins.RemoveAt(i);
                cb = null;

            }
        }
        public static int getCnt()
        {
            return consumerBins.Count;
        } 
        public static void SetWindow(int idx, bool isStop)
        { 
            //consumerBins[idx].ShowWin(isStop); 
            if (isStop)
            {
                //consumerBins[idx].ReconfigureSink();
                //consumerBins[idx].SetWindow((idx / 4) * 200, (idx % 4) * 200, 200, 200);

            }
            else 
            {
                //consumerBins[idx].SetWindow((idx / 2) * 960,(idx % 2) * 540,960,   540);

            }
            


        }
        public static void SetWindow( int idx, int x, int y, int w, int h) 
        {
              
              
                consumerBins[idx].SetWindow(x, y, w,h);
            consumerBins[idx].ReconfigureSink();
             
              
        } 
        public static void NewConsumer(string src, int x,int y, int w, int h)
        {
            SourceManagerWrapper.GetOrCreateSource(src);
        
            ConsumerBinWrapper consumer = new ConsumerBinWrapper(


                   x,
                   y,
                   w,h, 0);
     
            if (consumer.Init())
            {
                consumerBins.Add(consumer);
            }
      
            consumer.ShowWin(true); 
            SourceManagerWrapper.AttachConsumerBin("fakesrc://", consumer.GetBin(), true);

            //SourceManagerWrapper.AutoDetachConsumerBin(consumer.GetBin());
            //SourceManagerWrapper.AttachConsumerBin(src, consumer.GetBin(), true);
        }
        public static void checktee(int idx)
        {
            consumerBins[idx].CheckBin(idx);
        }
        private static async Task initCunsumerbin(int _totlaProcessCount)
        {
            //// 1. 기존 순차 코드
            //for (var i = 0; i < _totlaProcessCount; i++)
            //{
            //    ConsumerBinWrapper consumer = new ConsumerBinWrapper(


            //        (i / 4) * width, (i % 4) * height, width,
            //       height, i);
            //    if (consumer.Init())
            //    {
            //        consumerBins.Add(consumer);
            //    }
            //    await Task.Delay(17);
            //    SourceManagerWrapper.AttachConsumerBin("fakesrc://", consumerBins[i].GetBin(), false);

            //}
            // 2. 제미니 제안 병렬 코드
            var initTasks = new List<Task<ConsumerBinWrapper>>(); // 생성된 consumer를 반환하는 Task

            for (var i = 0; i < _totlaProcessCount; i++)
            {
                int idx = i; // 클로저를 위한 변수 복사
                //initTasks.Add(Task.Run(() => // 백그라운드 스레드에서 실행 (CPU 바운드 작업에 적합)
                //{
                //    // 생성 및 Init()은 별도의 스레드에서 실행될 수 있도록 Task.Run으로 감쌉니다.
                //    // ConsumerBinWrapper 생성은 비교적 빠르지만, Init()이 느릴 수 있습니다.
                //    ConsumerBinWrapper consumer = new ConsumerBinWrapper(
                //        (idx / 4) * width, (idx % 4) * height, width,
                //        height, idx);

                //    if (consumer.Init()) // Init()이 내부적으로 GStreamer 파이프라인을 초기화한다면
                //    {
                //        return consumer; // 성공적으로 초기화된 consumer 반환
                //    }
                //    return null; // 실패 시 null 반환
                //}));
                ConsumerBinWrapper consumer = new ConsumerBinWrapper(
                        ((idx+0) / 4 ) * width, (idx % 4) * height, width,
                        height, idx);

                consumer.Init(); // Init()이 내부적으로 GStreamer 파이프라인을 초기화한다면
                consumerBins.Add(consumer);
            }

            // 모든 Init 작업이 완료될 때까지 비동기적으로 대기
            var initializedConsumers = await Task.WhenAll(initTasks);

            foreach (var consumer in initializedConsumers)
            {
                if (consumer != null)
                {
                    consumerBins.Add(consumer);
                }
            }
            Debug.WriteLine($"ConsumerBins initialized. Total: {consumerBins.Count}");
            await Task.Delay(17);
            var attachTasks = new List<Task>();

            for (int i = 0; i < consumerBins.Count; i++) // 초기화 성공한 consumerBins만 순회
            {
                int idx = i; // 클로저를 위한 변수 복사 (for 루프 변수 캡처 주의)
                             // consumerBins[idx] 접근 시 인덱스 오류 방지
                ConsumerBinWrapper currentConsumer = consumerBins[idx];
                //SourceManagerWrapper.set
                attachTasks.Add(Task.Run(() =>
                {
                     
                    SourceManagerWrapper.AttachConsumerBin("fakesrc://", currentConsumer.GetBin(), false);
                    
                }));
                //currentConsumer.ShowWin(false);
            }
            await Task.WhenAll(attachTasks);

        }
    }
    public partial class MainWindow : Window
    {
        [DllImport("user32.dll")]
        private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint dwFlags);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFOEX lpmi);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);

        private const uint MONITOR_DEFAULTTONEAREST = 0x00000002;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        struct MONITORINFOEX
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string szDevice;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        struct DISPLAY_DEVICE
        {
            public uint cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceString;
            public uint StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceID;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceKey;
        }

        private CancellationTokenSource changeUrlCts;


        int _idx1 = 0, _idx2 = 0;
        int Idx0 { get { return _idx1; } }
        int Idx1 { get { return _idx1++; } }
        //private List<GStreamerPlayer> _players = new List<GStreamerPlayer>();
        string[] nUrls =
        {

            "rtsp://192.168.10.22:10554/screen1",
            "rtsp://192.168.10.23:10554/screen1",
            "rtsp://192.168.10.24:10554/screen1",
            "rtsp://192.168.10.25:10554/screen1",
            "rtsp://192.168.10.26:10554/screen1",
            "rtsp://192.168.10.27:10554/screen1",
        };
        string[] urls = {
            //"video://z:/file/17.mp4",
            //"image://Z:/file/1.jpg",
            //"rtsp://192.168.10.21:10554/screen1",
            
       //"capture://SC0710 PCI,0",
       //"capture://SC0710 PCI,4",
       //"capture://SC0710 PCI,5",
       //"capture://SC0710 PCI,6",
       //"capture://SC0710 PCI,7",
       
       //"capture://SC0710 PCI,2",



       //     "capture://SC0710 PCI,8",
       //     "capture://SC0710 PCI,10",
       //     "capture://SC0710 PCI,1",
       //       "capture://SC0710 PCI,3",
       //   "capture://SC0710 PCI,5",
       //     "capture://SC0710 PCI,6",
       //     "capture://SC0710 PCI,7",
       //     "capture://SC0710 PCI,9",
       //     "capture://SC0710 PCI,11",
       //     "capture://SC0710 PCI,4",
            //"rtsp://192.168.10.21:10554/screen1",
            //"rtsp://192.168.10.22:10554/screen1",
            //"rtsp://192.168.10.23:10554/screen1",
            //"rtsp://192.168.10.24:10554/screen1",
            //"rtsp://192.168.10.25:10554/screen1",
            //"rtsp://192.168.10.26:10554/screen1",
            //"rtsp://192.168.10.27:10554/screen1",
            //////"rtsp://admin:admin@192.168.10.125:554/hdmi1",
            ////  "rtsp://admin:admin@192.168.10.122:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.123:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.124:554/hdmi1",

                        "rtsp://admin:opticis031!@192.168.10.66/Streaming/Channels/101",
            "rtsp://admin:opticis031!@192.168.10.69/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.70/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.72/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.75/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.76/Streaming/Channels/101",

            //   "capture://SC0710 PCI,4",
            //"capture://SC0710 PCI,5",
            //"capture://SC0710 PCI,6",
            //"capture://SC0710 PCI,7",
            //"capture://SC0710 PCI,8",
            //"capture://SC0710 PCI,10",

            //"capture://SC0710 PCI,0",
            //"capture://SC0710 PCI,1",


            //"capture://SC0710 PCI,9",
            //"capture://SC0710 PCI,11",
            //"capture://SC0710 PCI,2",
            //"capture://SC0710 PCI,3",
            //"capture://SC0710 PCI,5",
            //"capture://SC0710 PCI,6",
            //"capture://SC0710 PCI,7",
            //"capture://SC0710 PCI,8",


            //"video://z:/file/14.mp4",
            //"video://z:/file/15.mp4",
            //"video://z:/file/16.mp4",
            //"video://z:/file/17.mp4",
            
            //"rtsp://192.168.10.23:10554/screen1",
            //"rtsp://192.168.10.24:10554/screen1",
            //"rtsp://192.168.10.25:10554/screen1",
            //"rtsp://192.168.10.26:10554/screen1",
            //"rtsp://192.168.10.27:10554/screen1",
            //"capture://SC0710 PCI,0",
            //"capture://SC0710 PCI,1",
            //"capture://SC0710 PCI,2",
            //"capture://SC0710 PCI,3",
            ////"capture://SC0710 PCI,4",
            //"capture://SC0710 PCI,5",
            //"capture://SC0710 PCI,6",
            //"capture://SC0710 PCI,7",
            //"capture://SC0710 PCI,8",
            ////"rtsp://admin:admin@192.168.10.121:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.122:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.123:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.124:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.125:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.126:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.127:554/hdmi1",
            //"rtsp://admin:opticis031!@192.168.10.66/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.69/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.70/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.72/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.75/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.76/Streaming/Channels/101",



            //"capture://SC0710 PCI,9", 
            //"image://C:/shared/file/1.jpg",
            //"rtsp://192.168.10.21:10554/screen1", 
            //"rtsp://admin:opticis031!@192.168.10.69/Streaming/Channels/101",
             
            //"capture://SC0710 PCI,1",
            //"capture://SC0710 PCI,2",// 11.3기가
            //"capture://SC0710 PCI,3",
            //"rtsp://192.168.10.23:10554/screen1",
            //"rtsp://192.168.10.23:10554/screen1",
            //"rtsp://192.168.10.24:10554/screen1",
            //"rtsp://192.168.10.25:10554/screen1",
            //"rtsp://192.168.10.26:10554/screen1",
            //"rtsp://192.168.10.27:10554/screen1",
            //"rtsp://admin:opticis031!@192.168.10.70/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.72/Streaming/Channels/101",



            ////"rtsp://192.168.10.105:10554/screen1",  
            
            ////"video://C:/shared/file/14.mp4",
            ////"video://C:/shared/file/15.mp4",
            ////"video://C:/shared/file/16.mp4",
            ////"video://C:/shared/file/17.mp4",
            ////////"rtsp://admin:opticis031!@192.168.10.63/hdmi1",
            //////"capture://SC0710 PCI,0",

            //"video://C:/shared/file/jw.mp4",
            //"video://C:/Users/marsr/source/repos/jw.mp4",
            //"video://C:/Users/marsr/source/repos/14.mp4",
            //"video://C:/Users/marsr/source/repos/15.mp4",
            
            //////////"video://C:/Users/marsr/source/repos/jw.mp4",

            //"rtsp://192.168.10.21:10554/screen1",
            //"rtsp://192.168.10.22:10554/screen1",
            //"rtsp://192.168.10.23:10554/screen1",
            //"rtsp://192.168.10.24:10554/screen1",
            //"rtsp://192.168.10.25:10554/screen1",
            //"rtsp://192.168.10.26:10554/screen1",
            //"rtsp://192.168.10.27:10554/screen1",
            
            //////101 105
            //////131 138
            //////ip 63,66,69,68,70
            //"rtsp://admin:admin!@192.168.10.135/hdmi1",
            //"rtsp://admin:opticis031!@192.168.10.69/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.70/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.72/Streaming/Channels/101",
            ////"rtsp://admin:opticis031!@192.168.10.66/Streaming/Channels/101",
            //"rtsp://admin:opticis031!@192.168.10.75/Streaming/Channels/101",
            //"rtsp://admin:admin@192.168.10.132:554/hdmi1",
            //"rtsp://admin:admin@192.168.10.133:554/hdmi1",

               
            //////"rtsp://admin:opticis031!@192.168.10.132/hdmi1",
            //////"rtsp://admin:opticis031!@192.168.10.133/hdmi1", 
            //////"rtsp://admin:opticis031!@192.168.10.134/hdmi1", 
                        
            //"image://C:/shared/file/1.jpg",
        };
        int dd = 0;
        int cnt = 2;
        int shiftnum = 0;
        private Timer _timer;
        bool isStart = false;
        bool isStop = false;
        public MainWindow()
        {
            InitializeComponent();
            Loaded += MainWindow_Loaded1;
            //_timer = new Timer(me, null, 0, 10000);
        }
     
        // MainWindow_Loaded1: SSP 및 ConsumerBin 초기화 후 fakesrc://에 attach
        private async void MainWindow_Loaded1(object sender, RoutedEventArgs e)
        {
            Stopwatch stopwatch = new Stopwatch();
            stopwatch.Start();
            SourceManagerWrapper.GetOrCreateSource("fakesrc://");
            //SourceManagerWrapper.GetOrCreateSource(urls[0]);
            // 1. 모든 URL에 대한 SSP(Shared Source Pipeline) 병렬 생성
            var sspTasks = new List<Task>();
            for (int i = Idx1; i < urls.Length; i++)
            {
                int idx = i;
                sspTasks.Add(Task.Run(() =>
                {
                    // SSP 생성 (SSP는 내부적으로 RTSP 혹은 fakesrc:// 구분)
                    IntPtr sspPipelinePtr = SourceManagerWrapper.GetOrCreateSource(urls[idx]);
                    Debug.WriteLine("[DEBUG] SSP created for: {0}", urls[idx]);
                    //SourceManagerWrapper.toggleMute(urls[idx]);

                }));
                await Task.Delay(34);
            }
            await Task.WhenAll(sspTasks);
            //SourceManagerWrapper.GetOrCreateSource(urls[0]);
            // 1. 모든 URL에 대한 SSP(Shared Source Pipeline) 병렬 생성
            //var sspTasks = new List<Task>();
            //for (int i = Idx1; i < urls.Length; i++)
            //{
            //    int idx = i;
            //    sspTasks.Add(Task.Run(() =>
            //    {
            //        // SSP 생성 (SSP는 내부적으로 RTSP 혹은 fakesrc:// 구분)
            //        //IntPtr sspPipelinePtr = SourceManagerWrapper.GetOrCreateSource(urls[idx]);
            //        //Debug.WriteLine("[DEBUG] SSP created for: {0}", urls[idx]);
            //        //SourceManagerWrapper.toggleMute(urls[idx]);

            //    }));
            //    cb.NewConsumer(urls[idx], (idx / 2) * 960, (idx % 2) * 540,

            //       960, 
            //       540); 
            //    //await Task.Delay(3367);
            //    cb.SetWindow(idx, true);
            //}  
            //await Task.WhenAll(sspTasks);
            Debug.WriteLine("[DEBUG] create end ssp");
            // fakesrc SSP 생성
            //SourceManagerWrapper.GetOrCreateSource("fakesrc://");
            //SourceManagerWrapper.GetOrCreateSource("fakesrc://1");
            //await Task.Delay(50);
            await cb.init(cnt);
            Debug.WriteLine("fake attach end");


            stopwatch.Stop();
            //SourceManagerWrapper.setInfo();
            //await cb.init(cnt);

            //SourceManagerWrapper.setWarning();
            // Debug.WriteLine($"[DEBUG] StartPlay() 전체 실행 시간: {stopwatch.ElapsedMilliseconds} ms");
        }

        // 예시: 다른 방식으로 플레이어 생성

        private async void init()
        {
            Stopwatch stopwatch = new Stopwatch();
            stopwatch.Start();

            // 1. 모든 URL에 대한 SSP(Shared Source Pipeline) 병렬 생성
            var sspTasks = new List<Task>();
            for (int i = 0; i < urls.Length; i++)
            {
                int idx = i;
                sspTasks.Add(Task.Run(() =>
                {
                    // SSP 생성 (SSP는 내부적으로 RTSP 혹은 fakesrc:// 구분)
                    //IntPtr sspPipelinePtr = SourceManagerWrapper.GetOrCreateSource(urls[idx]);
                    //Debug.WriteLine("[DEBUG] SSP created for: {0}", urls[idx]);
                    //SourceManagerWrapper.toggleMute(urls[idx]);

                }));
                cb.NewConsumer(urls[idx], (idx / 2) * 960, (idx % 2) * 540,

                   960,
                   540);
                //await Task.Delay(3367);

            }
            await Task.WhenAll(sspTasks);
            Debug.WriteLine("[DEBUG] create end ssp");
            // fakesrc SSP 생성
            //SourceManagerWrapper.GetOrCreateSource("fakesrc://");
            //SourceManagerWrapper.GetOrCreateSource("fakesrc://1");
            await Task.Delay(50);
            // await cb.init(cnt);
            Debug.WriteLine("fake attach end");


            stopwatch.Stop();
            Debug.WriteLine($"[DEBUG] StartPlay() 전체 실행 시간: {stopwatch.ElapsedMilliseconds} ms");
        }
        private void ShowGpu()
        {
            IntPtr hwnd = Process.GetCurrentProcess().MainWindowHandle;
            IntPtr hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (hMonitor == IntPtr.Zero)
            {
                Console.WriteLine("Failed to get monitor handle.");
                return;
            }
            MONITORINFOEX monitorInfo = new MONITORINFOEX();
            monitorInfo.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
            if (!GetMonitorInfo(hMonitor, ref monitorInfo))
            {
                Console.WriteLine("Failed to get monitor info.");
                return;
            }
            Console.WriteLine($"Monitor Name: {monitorInfo.szDevice}");
            DISPLAY_DEVICE displayDevice = new DISPLAY_DEVICE();
            displayDevice.cb = (uint)Marshal.SizeOf(typeof(DISPLAY_DEVICE));
            int gpuIndex = -1;
            int adapterIndex = 0;
            while (EnumDisplayDevices(null, (uint)adapterIndex, ref displayDevice, 0))
            {
                Console.WriteLine($"Adapter {adapterIndex}: {displayDevice.DeviceString}");
                Console.WriteLine($"DeviceName: {displayDevice.DeviceName}");
                if (monitorInfo.szDevice == displayDevice.DeviceName)
                {
                    gpuIndex = adapterIndex;
                    break;
                }
                adapterIndex++;
            }
            if (gpuIndex >= 0)
            {
                Console.WriteLine($"Monitor is connected to GPU Index: {gpuIndex}");
            }
            else
            {
                Console.WriteLine("Failed to determine GPU index.");
            }
        }

        // 예시: 특정 스트림으로 ConsumerBin을 attach 후 스트림 재생



        // 버튼 클릭: detach 후 RTSP 분기로 attach 전환
        private async void StartButton_Click(object sender, RoutedEventArgs e)
        {
            isStart = true;
            var attachTasks = new List<Task>();
            if (cb.getCnt() == 0)
            {
                int cc = cb.getCnt();
                cb.NewConsumer(urls[Idx1 % urls.Length], (cc / 4) * 1920, (cc % 4) * 1080, 1920, 1080);
            }

            else
            {
                SourceManagerWrapper.setWarning();

                for (int i = 0; i < cb.getCnt(); i++)
                {
                    int idx = i;

                    attachTasks.Add(Task.Run(() =>
                    {
                        //SourceManagerWrapper.GetOrCreateSource(urls[idx % urls.Length]);
                        //cb.SetWindow(idx + Idx0 + shiftnum, false);

                        cb.setConsumerbin(idx, urls[(idx + dd) % urls.Length]);



                    }));
                    await Task.Delay(1);

                }
                await Task.WhenAll(attachTasks);
            }

            //for (int i = 0; i < cb.getCnt(); i++)
            //{
            //    int idx = i;

            //    attachTasks.Add(Task.Run(() =>
            //    {
            //        cb.SetWindow(idx, true);


            //    }));
            //    await Task.Delay(1);

            //}
            //await Task.WhenAll(attachTasks);
            //SourceManagerWrapper.setInfo();
            //init();
            //for (int i = 0; i < urls.Length; i++)
            //{
            //    cb.SetWindow(i, false);
            //}
            //try
            //{
            //    var attachTasks = new List<Task>();
            //    //Parallel.For(0, cnt, i =>
            //    //{
            //    //    //cb.SetWindow(i + Idx0 + shiftnum, false);
            //    //    cb.setConsumerbin(i, urls[(i + Idx0 + shiftnum) % urls.Length]);


            //    //});
            //    //for (int i = 0; i < cnt; i++)
            //    //{
            //    //    int idx = i;

            //    //    attachTasks.Add(Task.Run(() =>
            //    //    {
            //    //        cb.SetWindow(idx + Idx0 + shiftnum, false);



            //    //    }));
            //    //    await Task.Delay(34);
            //    //}
            //    //await Task.WhenAll(attachTasks);
            //    for (int i = 0; i < cnt; i++)
            //    {
            //        int idx = i;

            //        attachTasks.Add(Task.Run(() =>
            //        {


            //            cb.setConsumerbin(idx, urls[(idx + Idx0 + shiftnum) % urls.Length]);

            //        }));
            //        await Task.Delay(34);
            //    }
            //    await Task.WhenAll(attachTasks);
            //}
            //catch (Exception ex)
            //{
            //    MessageBox.Show("플레이 중 오류 발생: " + ex.Message);
            //}

            dd +=1;
        }

        // 버튼 클릭: RTSP 분기로 detach 후 fakesrc(raw branch)로 재attach 전환
        private async void StopButton_Click(object sender, RoutedEventArgs e)
        {
            //for (int i = 0; i < cnt; i++)
            //{
            //    cb.SetWindow(i, false);
            //}
            isStop = true;
            var dettachTasks = new List<Task>();
            for (int i = 0; i < cnt; i++)
            {
                int idx = i;
                dettachTasks.Add(Task.Run(() =>
                {
                    //  cb.SetWindow(idx, true);
                    cb.setConsumerbin(idx, "fakesrc://");


                }));
                cb.SetWindow(i, false);
                await Task.Delay(1);

            }
            await Task.WhenAll(dettachTasks);


        }

        private async void MoveButton_Click(object sender, RoutedEventArgs e)
        {
            if (Idx0 > nUrls.Length)
            {
                return;
            }
            SourceManagerWrapper.setInfo();
            int x, y, w, h = 0;
            x = int.Parse(XInput.Text);
            y = int.Parse(YInput.Text);
            w = int.Parse(WidthInput.Text);
            h = int.Parse(HeightInput.Text);
            cb.NewConsumer("fakesrc://", x, y,

                   w,
                   h);
        }


        private async void Button_Click(object sender, RoutedEventArgs e)
        {
            int count = cb.getCnt();
            var detachTask = Task.Run(() =>
            {
                Parallel.ForEach(Enumerable.Range(0, count), i =>
                {
                    int idx = i;
                    cb.checktee(idx);
                    //consumerBins[idx].CheckBin(idx); 
                });
            });
            detachTask.Wait();
            SourceManagerWrapper.CheckTee(urls[0]);
            //SourceManagerWrapper.CheckTee("fakesrc://");
        }


        private async void CustomStreamButton_Click(object sender, RoutedEventArgs e)
        {


            var attachTasks = new List<Task>();
#if play 
            for (int i = 0; i < urls.Length; i++)
            {
            int a = Idx1;
                attachTasks.Add(Task.Run(() =>
                {
                    // force PLAYING
                    SourceManagerWrapper.SetPlay(urls[idx]);
                      }));
                
            }
            await Task.WhenAll(attachTasks);
#endif
#if freeR
            cb.allFree(cb.getCnt());
#endif



            GC.Collect();
            GC.WaitForPendingFinalizers();





        }
        private async void Button_Null_Click(object sender, RoutedEventArgs e)
        {
            SourceManagerWrapper.SetNull(urls[0]);
        }
        private async void Button_Ready_Click(object sender, RoutedEventArgs e)
        {
            SourceManagerWrapper.SetReady(urls[0]);
        }
        private async void Button_Play_Click(object sender, RoutedEventArgs e)
        {
            SourceManagerWrapper.SetPlay(urls[0]);
        }
    }
}
