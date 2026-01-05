#pragma once

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <vcclr.h>
#include <Windows.h>

namespace GStreamerWrapper
{

    using namespace System;
    using namespace System::Runtime::InteropServices; // for GCHandle

    public value struct StreamConfig
    {
        int MonitorIndex;
        int CropX;
        int CropY;
        int CropW;
        int CropH;
        int Width;
        int Height;
        int Framerate;
        int BitrateKbps;
        int KeyframeInterval;
        int Port;
        int StreamIndex;
        bool EnableAudio;
        String ^AudioDevice;
        bool EnableHardwareAccel;
        bool EnableOsd;
        bool EnableMultiCast;
        String ^BitrateControl;
        String ^Profile;
        String ^OsdText;
        String ^MultiCastIP;
        String ^MultiCastInterface;
    };

    public ref class GstPlayer
    {
    private:
        GstElement *pipeline;
        GCHandle gcHandle;
        System::Diagnostics::Process ^workerProcess;
        System::String ^workerConfigPath;

        static void BusMessageCallback(GstBus *bus, GstMessage *msg, gpointer data);
        void HandleBusMessage(GstMessage *msg);
        void StopWorker();
        void LaunchWorkerWithConfig(System::String ^configContent);

    public:
        HWND videoHwnd;
        GstPlayer(IntPtr windowHandle);
        ~GstPlayer();
        !GstPlayer();

        void StartScreenCapture(StreamConfig config);
        void StartScreenCaptureServer(String ^serverIp, array<StreamConfig> ^configs);
        void Stop();
        void StopPreview();
        static void Initialize();
        static void Deinitialize();

        // 기존 API 유지: Capture(마이크) 장치 이름 목록
        static array<String ^> ^GetAudioDevices();

        // 추가: Render(출력) 장치 이름 목록 (loopback/UI용)
        static array<String ^> ^GetRenderDevices();

        // 추가: Render/Capture ACTIVE 존재 여부 (오디오 체인 생성 여부 판단용)
        static bool HasActiveAudioEndpoint(bool wantRender);

        // (선택) 위험한 GstDeviceMonitor 폴백을 켤지 여부 (기본 false 권장)
        static void SetUseGstDeviceMonitorFallback(bool enable);
    };
}
