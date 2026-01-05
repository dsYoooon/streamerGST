#include "GStreamerWrapper.h"
#include "ScreenCaptureServer.h"

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <glib.h>
#include <sstream>

namespace GStreamerWrapper
{
    namespace
    {
        GstElement* g_preview_pipeline = nullptr;
        GstBus* g_preview_bus = nullptr;

        void StopPreviewInternal()
        {
            if (g_preview_pipeline)
            {
                gst_element_set_state(g_preview_pipeline, GST_STATE_NULL);
                gst_object_unref(g_preview_pipeline);
                g_preview_pipeline = nullptr;
            }

            if (g_preview_bus)
            {
                gst_object_unref(g_preview_bus);
                g_preview_bus = nullptr;
            }
        }

        void ApplyOverlayHandle(GstElement* sink, HWND window)
        {
            if (!sink || !window)
                return;

            if (GST_IS_VIDEO_OVERLAY(sink))
            {
                gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), (guintptr)window);
            }
        }
    }

    void Initialize()
    {
        static bool initialized = false;
        if (!initialized)
        {
            gst_init(nullptr, nullptr);
            initialized = true;
        }
    }

    void Deinitialize()
    {
        StopPreviewInternal();
        StopScreenCaptureRtspServer();
    }

    bool StartPreview(const StreamConfigNative& config, HWND window)
    {
        StopPreviewInternal();

        GstElement* pipeline = gst_pipeline_new("screen-preview");
        GstElement* src = gst_element_factory_make("d3d11screencapturesrc", "src");
        GstElement* queue = gst_element_factory_make("queue", "queue");
        GstElement* conv = gst_element_factory_make("d3d11convert", "conv");
        GstElement* capsfilter = gst_element_factory_make("capsfilter", "caps");
        GstElement* sink = gst_element_factory_make("d3d11videosink", "sink");

        if (!pipeline || !src || !queue || !conv || !capsfilter || !sink)
        {
            StopPreviewInternal();
            return false;
        }

        g_object_set(src,
            "monitor-index", config.monitor_index,
            "show-cursor", TRUE,
            "capture-api", 1,
            NULL);
        g_object_set(sink, "force-aspect-ratio", FALSE, NULL);

        std::ostringstream caps_str;
        caps_str << "video/x-raw(memory:D3D11Memory),format=NV12,framerate="
                 << (config.framerate > 0 ? config.framerate : 30) << "/1";
        GstCaps* caps = gst_caps_from_string(caps_str.str().c_str());
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);

        gst_bin_add_many(GST_BIN(pipeline), src, queue, conv, capsfilter, sink, NULL);
        if (!gst_element_link_many(src, queue, conv, capsfilter, sink, NULL))
        {
            gst_object_unref(pipeline);
            return false;
        }

        g_preview_pipeline = pipeline;
        g_preview_bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));

        ApplyOverlayHandle(sink, window);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        return true;
    }

    void StopPreview()
    {
        StopPreviewInternal();
    }
}

