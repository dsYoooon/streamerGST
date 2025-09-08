
#include "GStreamerWrapper.h"
#include "ScreenCaptureServer.h"

// 필요한 .NET 네임스페이스 추가
using namespace System::Runtime::InteropServices; // GCHandle을 위해 추가
using namespace System::Diagnostics;             // Debug 클래스를 위해 추가

#pragma comment(lib, "gstreamer-1.0.lib")
#pragma comment(lib, "gstvideo-1.0.lib")
#pragma comment(lib, "gobject-2.0.lib")
#pragma comment(lib, "glib-2.0.lib")

namespace GStreamerWrapper {

    void GstPlayer::Initialize()
    {
        gst_init(nullptr, nullptr);
    }
    static GstBusSyncReply BusSyncHandler(GstBus* bus, GstMessage* msg, gpointer data)
    {
        GstPlayer^ player = (GstPlayer^)GCHandle::FromIntPtr(IntPtr(data)).Target;

        // 'prepare-window-handle' 메시지인지 확인
        if (gst_is_video_overlay_prepare_window_handle_message(msg)) {
            g_print("prepare-window-handle message received\n");
            GstVideoOverlay* overlay = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg));
            gst_video_overlay_set_window_handle(overlay, (guintptr)player->videoHwnd);

            // 이 메시지는 우리가 처리했으므로 다른 핸들러로 전달하지 않고 버립니다.
            gst_message_unref(msg);
            return GST_BUS_DROP;
        }

        // 다른 메시지들은 비동기 핸들러가 처리하도록 전달합니다.
        return GST_BUS_PASS;
    }

    void GstPlayer::Deinitialize()
    {
        gst_deinit();
    }

    GstPlayer::GstPlayer(IntPtr windowHandle) : pipeline(nullptr)
    {
        videoHwnd = static_cast<HWND>(windowHandle.ToPointer());
    }

    GstPlayer::~GstPlayer()
    {
        this->!GstPlayer();
    }

    GstPlayer::!GstPlayer()
    {
        Stop();
    }
    void GstPlayer::StartScreenCaptureServer()
    {
        // 기존 파이프라인 정지 후 새로운 RTSP 서버 실행
        Stop();
        RunScreenCaptureRtspServer();
    }

    void GstPlayer::Stop()
    {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }

    void GstPlayer::BusMessageCallback(GstBus* bus, GstMessage* msg, gpointer data)
    {
        GCHandle gch = GCHandle::FromIntPtr(IntPtr(data));
        GstPlayer^ player = (GstPlayer^)gch.Target;

        player->HandleBusMessage(msg);

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS || GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gch.Free();
        }
    }

    void GstPlayer::HandleBusMessage(GstMessage* msg)
    {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;
            gst_message_parse_error(msg, &err, &debug);
           
            g_error_free(err);
            g_free(debug);
            Stop();
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End-Of-Stream reached.");
            Stop();
            break;
        case GST_MESSAGE_ELEMENT:
            if (gst_is_video_overlay_prepare_window_handle_message(msg)) {
                gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)), (guintptr)videoHwnd);
            }
            break;
        default:
            break;
        }
    }
}