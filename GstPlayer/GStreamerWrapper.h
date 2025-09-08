#pragma once

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <vcclr.h>
#include <Windows.h>
using namespace System;

namespace GStreamerWrapper {

    public ref class GstPlayer
    {
    private:
        GstElement* pipeline;
        //HWND videoHwnd; // 창 핸들(HWND)을 직접 저장

        // C#에서 호출할 수 없도록 private으로 선언
        static void BusMessageCallback(GstBus* bus, GstMessage* msg, gpointer data);
        void HandleBusMessage(GstMessage* msg);

    public:
        HWND videoHwnd;
        // 생성자가 Control 대신 IntPtr을 받도록 수정
        GstPlayer(IntPtr windowHandle);
        // 소멸자 (IDisposable 패턴을 위해 필요)
        ~GstPlayer();
        // C++ CLI에서는 !GstPlayer() 형태의 Finalizer도 정의해주는 것이 좋습니다.
        !GstPlayer();

        void StartScreenCaptureServer();
        void Stop();

        static void Initialize();
        static void Deinitialize();
    };
}