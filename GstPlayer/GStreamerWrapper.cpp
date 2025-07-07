
#include "GStreamerWrapper.h"

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

    void GstPlayer::Play(String^ rtspUrl)
    {
        if (pipeline) {
            Stop();
        }

        // 이제 GCHandle이 정상적으로 인식됩니다.
        GCHandle gch = GCHandle::Alloc(this);

        char* nativeUrl = (char*)Marshal::StringToHGlobalAnsi(rtspUrl).ToPointer();
        gchar* pipeline_str = g_strdup_printf("playbin uri=%s", nativeUrl);
        pipeline = gst_parse_launch(pipeline_str, NULL);
        g_free(pipeline_str);
        Marshal::FreeHGlobal(IntPtr(nativeUrl));

        if (!pipeline) {
            // 이제 Debug::WriteLine이 정상적으로 인식됩니다.
            g_print("Failed to create GStreamer pipeline.");
            GCHandle::FromIntPtr(GCHandle::ToIntPtr(gch)).Free();
            return;
        }

        GstBus* bus = gst_element_get_bus(pipeline);
        gst_bus_add_watch(bus, (GstBusFunc)BusMessageCallback, (gpointer)GCHandle::ToIntPtr(gch).ToPointer());
        gst_object_unref(bus);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
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