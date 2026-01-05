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
            // 파이프라인을 별도 프로세스에서 실행하도록 요청된 경우 UI를 건너뜁니다.
            if (PipelineWorkerMode.TryRun(e.Args))
            {
                Shutdown();
                return;
            }

            base.OnStartup(e);
        }

    }
}
