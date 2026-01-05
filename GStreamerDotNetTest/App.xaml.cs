using System;
using System.Collections.Generic;
using System.Configuration;
using System.Data;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows;

namespace GStreamerDotNetTest
{
    /// <summary>
    /// App.xaml에 대한 상호 작용 논리
    /// </summary>
    public partial class App : Application
    {
        protected override void OnStartup(StartupEventArgs e)
        {
            // 파이프라인 워커 모드는 UI 없이 별도의 프로세스에서 파이프라인만 실행한다.
            if (PipelineWorkerBootstrap.TryRunWorker(e.Args))
            {
                Shutdown();
                return;
            }

            base.OnStartup(e);
        }
    }
}
