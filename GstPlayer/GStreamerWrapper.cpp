
#include "GStreamerWrapper.h"
#include "ScreenCaptureServer.h"

#include <vector>
#include <sstream>

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
        StopScreenCaptureRtspServer();
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
    void GstPlayer::StartScreenCapture(StreamConfig config)
    {
        Stop();

        pipeline = gst_pipeline_new("screen-preview");
        GstElement* src = gst_element_factory_make("d3d11screencapturesrc", "src");
        GstElement* conv = gst_element_factory_make("d3d11convert", "conv");
        GstElement* capsfilter = gst_element_factory_make("capsfilter", "caps");
        GstElement* sink = gst_element_factory_make("d3d11videosink", "sink");

        if (!pipeline || !src || !conv || !capsfilter || !sink) {
            g_printerr("파이프라인 생성 실패\n");
            if (pipeline) { gst_object_unref(pipeline); pipeline = nullptr; }
            return;
        }

        g_object_set(src,
            "monitor-index", config.MonitorIndex,
            "show-cursor", TRUE,
            "left", config.CropX,
            "top", config.CropY,
            "width", config.CropW,
            "height", config.CropH,
            NULL);

        std::ostringstream caps_str;
        caps_str << "video/x-raw,format=NV12,width=" << config.Width
                 << ",height=" << config.Height
                 << ",framerate=" << config.Framerate << "/1";
        GstCaps* caps = gst_caps_from_string(caps_str.str().c_str());
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);

        gst_bin_add_many(GST_BIN(pipeline), src, conv, capsfilter, sink, NULL);
        if (!gst_element_link_many(src, conv, capsfilter, sink, NULL)) {
            g_printerr("요소 연결 실패\n");
            gst_object_unref(pipeline);
            pipeline = nullptr;
            return;
        }

        gcHandle = GCHandle::Alloc(this);
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        gst_bus_add_signal_watch(bus);
        g_signal_connect(bus, "message", G_CALLBACK(BusMessageCallback), GCHandle::ToIntPtr(gcHandle).ToPointer());
        gst_bus_set_sync_handler(bus, BusSyncHandler, GCHandle::ToIntPtr(gcHandle).ToPointer(), NULL);
        g_object_unref(bus);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    }

    void GstPlayer::StartScreenCaptureServer(String^ serverIp, array<StreamConfig>^ configs)
    {
        Stop();

        IntPtr ipPtr = Marshal::StringToHGlobalAnsi(serverIp);

        std::vector<StreamConfigNative> nativeConfigs;
        for each (StreamConfig cfg in configs)
        {
            StreamConfigNative ncfg;
            ncfg.monitor_index = cfg.MonitorIndex;
            ncfg.crop_x = cfg.CropX;
            ncfg.crop_y = cfg.CropY;
            ncfg.crop_w = cfg.CropW;
            ncfg.crop_h = cfg.CropH;
            ncfg.width = cfg.Width;
            ncfg.height = cfg.Height;
            ncfg.framerate = cfg.Framerate;
            ncfg.bitrate_kbps = cfg.BitrateKbps;
            ncfg.keyframe_interval = cfg.KeyframeInterval;
            nativeConfigs.push_back(ncfg);
        }

        RunScreenCaptureRtspServer(static_cast<const char*>(ipPtr.ToPointer()),
                                   nativeConfigs.data(),
                                   (int)nativeConfigs.size());
        Marshal::FreeHGlobal(ipPtr);
    }

    void GstPlayer::Stop()
    {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        if (gcHandle.IsAllocated) {
            gcHandle.Free();
        }
        // RTSP 서버가 실행 중이면 중지합니다.
        StopScreenCaptureRtspServer();
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