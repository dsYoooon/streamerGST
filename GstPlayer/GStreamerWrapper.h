#pragma once

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <vcclr.h>
#include <Windows.h>
using namespace System;
using namespace System::Runtime::InteropServices; // for GCHandle

namespace GStreamerWrapper {

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
        bool EnableAudio;
        String^ AudioDevice;
        bool EnableHardwareAccel;
        bool EnableOsd;
        String^ BitrateControl;
        String^ Profile;
        String^ OsdText;
    };

    public ref class GstPlayer
    {
    private:
        GstElement* pipeline;
        GCHandle gcHandle;

        static void BusMessageCallback(GstBus* bus, GstMessage* msg, gpointer data);
        void HandleBusMessage(GstMessage* msg);

    public:
        HWND videoHwnd;
        GstPlayer(IntPtr windowHandle);
        ~GstPlayer();
        !GstPlayer();

        void StartScreenCapture(StreamConfig config);
        void StartScreenCaptureServer(String^ serverIp, array<StreamConfig>^ configs);
        void Stop();

        static void Initialize();
        static void Deinitialize();
        static array<String^>^ GetAudioDevices();
    };
}