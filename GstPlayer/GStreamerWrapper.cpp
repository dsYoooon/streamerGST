
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
    // rtspsrc의 'pad-added' 시그널에 대한 콜백
 // rtspsrc는 스트림 정보를 받아야 패드를 생성하므로, 동적으로 연결해야 합니다.
    static void OnPadAdded(GstElement* src, GstPad* new_pad, gpointer data) {
        g_print("Received new pad '%s' from '%s':\n", GST_PAD_NAME(new_pad), GST_ELEMENT_NAME(src));

        GstElement* depay = (GstElement*)data;
        GstPad* sink_pad = gst_element_get_static_pad(depay, "sink");
        if (gst_pad_is_linked(sink_pad)) {
            g_print("We are already linked. Ignoring.\n");
            gst_object_unref(sink_pad);
            return;
        }

        GstCaps* new_pad_caps = gst_pad_get_current_caps(new_pad);
        const GstStructure* new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
        const gchar* new_pad_type = gst_structure_get_name(new_pad_struct);

        // H.264 비디오 스트림일 때만 depayloader와 연결합니다.
        if (!g_str_has_prefix(new_pad_type, "application/x-rtp")) {
            g_print("It has type '%s' which is not raw video. Ignoring.\n", new_pad_type);
            if (new_pad_caps != NULL)
                gst_caps_unref(new_pad_caps);
            gst_object_unref(sink_pad);
            return;
        }

        GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
        if (GST_PAD_LINK_FAILED(ret)) {
            g_print("Type is '%s' but link failed.\n", new_pad_type);
        }
        else {
            g_print("Link succeeded (type '%s').\n", new_pad_type);
        }

    exit:
        if (new_pad_caps != NULL)
            gst_caps_unref(new_pad_caps);
        gst_object_unref(sink_pad);
    }


    void GstPlayer::Play(String^ rtspUrl) {
        if (pipeline) {
            Stop();
        }

        GCHandle gch = GCHandle::Alloc(this);
        char* nativeUrl = (char*)Marshal::StringToHGlobalAnsi(rtspUrl).ToPointer();

        // 1. 파이프라인과 엘리먼트들을 수동으로 생성
        pipeline = gst_pipeline_new("rtsp-pipeline");
        GstElement* source = gst_element_factory_make("rtspsrc", "source");
        GstElement* depay = gst_element_factory_make("rtph264depay", "depay");
        GstElement* parse = gst_element_factory_make("h264parse", "parse");
        GstElement* decoder = gst_element_factory_make("d3d11h264dec", "decoder"); // d3d11h264dec 또는 avdec_h264 사용
        //GstElement* convert = gst_element_factory_make("videoconvert", "convert");
        GstElement* sink = gst_element_factory_make("d3d11videosink", "sink");

        if (!pipeline || !source || !depay || !parse || !decoder ||  !sink) {
            g_printerr("Not all elements could be created.\n");
            Marshal::FreeHGlobal(IntPtr(nativeUrl));
            gch.Free();
            return;
        }

        // 2. 파이프라인에 엘리먼트들 추가
        gst_bin_add_many(GST_BIN(pipeline), source, depay, parse, decoder,  sink, NULL);

        // 3. 엘리먼트들을 연결 (rtspsrc 제외)
        if (!gst_element_link_many(depay, parse, decoder,  sink, NULL)) {
            g_printerr("Elements could not be linked.\n");
            gst_object_unref(pipeline);
            Marshal::FreeHGlobal(IntPtr(nativeUrl));
            gch.Free();
            return;
        }

        // 4. rtspsrc 프로퍼티 설정 (핵심: latency=0)
        g_object_set(source, "location", nativeUrl, "latency", 0, NULL);
        Marshal::FreeHGlobal(IntPtr(nativeUrl));

        // 5. rtspsrc의 pad-added 시그널에 콜백 연결
        g_signal_connect(source, "pad-added", G_CALLBACK(OnPadAdded), depay);

        //렌더링시 원본비율 유지 true, 무시 false
        g_object_set(sink, "force-aspect-ratio", false, NULL);
        
        // 6. 버스 핸들러 설정
        GstBus* bus = gst_element_get_bus(pipeline);
        gst_bus_set_sync_handler(bus, (GstBusSyncHandler)BusSyncHandler, (gpointer)GCHandle::ToIntPtr(gch).ToPointer(), NULL);
        gst_object_unref(bus);

        // 7. 파이프라인 재생
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            g_printerr("Unable to set the pipeline to the playing state.\n");
        }
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