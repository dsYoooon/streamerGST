#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include "../GstPlayer/ScreenCaptureServer.h"

#pragma comment(lib, "gstreamer-1.0.lib")
#pragma comment(lib, "gstvideo-1.0.lib")
#pragma comment(lib, "gobject-2.0.lib")
#pragma comment(lib, "glib-2.0.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    struct PreviewConfig
    {
        int monitor_index = 0;
        int width = 0;
        int height = 0;
        int framerate = 30;
        guintptr window_handle = 0;
    };

    struct WorkerConfig
    {
        std::string mode;
        std::string server_ip;
        PreviewConfig preview;
        std::vector<GStreamerWrapper::StreamConfigNative> streams;
    };

    BOOL WINAPI CtrlHandler(DWORD ctrl)
    {
        if (ctrl == CTRL_C_EVENT || ctrl == CTRL_CLOSE_EVENT)
        {
            GStreamerWrapper::StopScreenCaptureRtspServer();
            gst_deinit();
            return TRUE;
        }
        return FALSE;
    }

    static std::string trim(const std::string &input)
    {
        auto begin = std::find_if_not(input.begin(), input.end(), [](char ch)
                                      { return std::isspace(static_cast<unsigned char>(ch)); });
        auto end = std::find_if_not(input.rbegin(), input.rend(), [](char ch)
                                    { return std::isspace(static_cast<unsigned char>(ch)); })
                       .base();
        if (begin >= end)
            return std::string();
        return std::string(begin, end);
    }

    static bool read_config_file(const std::wstring &path, WorkerConfig &out)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return false;
        }

        std::map<std::string, std::string> current;
        std::string section;
        auto flush_section = [&](const std::string &name)
        {
            if (name.empty())
                return;
            if (name == "preview")
            {
                if (current.count("monitor"))
                    out.preview.monitor_index = std::stoi(current["monitor"]);
                if (current.count("width"))
                    out.preview.width = std::stoi(current["width"]);
                if (current.count("height"))
                    out.preview.height = std::stoi(current["height"]);
                if (current.count("framerate"))
                    out.preview.framerate = std::stoi(current["framerate"]);
                if (current.count("window"))
                    out.preview.window_handle = static_cast<guintptr>(_strtoui64(current["window"].c_str(), nullptr, 10));
            }
            else if (name.rfind("stream", 0) == 0)
            {
                GStreamerWrapper::StreamConfigNative cfg{};
                if (current.count("monitor"))
                    cfg.monitor_index = std::stoi(current["monitor"]);
                if (current.count("crop_x"))
                    cfg.crop_x = std::stoi(current["crop_x"]);
                if (current.count("crop_y"))
                    cfg.crop_y = std::stoi(current["crop_y"]);
                if (current.count("crop_w"))
                    cfg.crop_w = std::stoi(current["crop_w"]);
                if (current.count("crop_h"))
                    cfg.crop_h = std::stoi(current["crop_h"]);
                if (current.count("width"))
                    cfg.width = std::stoi(current["width"]);
                if (current.count("height"))
                    cfg.height = std::stoi(current["height"]);
                if (current.count("framerate"))
                    cfg.framerate = std::stoi(current["framerate"]);
                if (current.count("bitrate"))
                    cfg.bitrate_kbps = std::stoi(current["bitrate"]);
                if (current.count("keyint"))
                    cfg.keyframe_interval = std::stoi(current["keyint"]);
                if (current.count("port"))
                    cfg.port = std::stoi(current["port"]);
                if (current.count("stream_index"))
                    cfg.streamIndex = std::stoi(current["stream_index"]);
                if (current.count("enable_audio"))
                    cfg.enable_audio = (current["enable_audio"] == "1" || current["enable_audio"] == "true");
                if (current.count("enable_multicast"))
                    cfg.enable_multicast = (current["enable_multicast"] == "1" || current["enable_multicast"] == "true");
                if (current.count("audio_device"))
                    cfg.audio_device = current["audio_device"];
                if (current.count("enable_hw_accel"))
                    cfg.enable_hw_accel = (current["enable_hw_accel"] == "1" || current["enable_hw_accel"] == "true");
                if (current.count("enable_osd"))
                    cfg.enable_osd = (current["enable_osd"] == "1" || current["enable_osd"] == "true");
                if (current.count("bitrate_control"))
                    cfg.bitrate_control = current["bitrate_control"];
                if (current.count("profile"))
                    cfg.profile = current["profile"];
                if (current.count("overlay_text"))
                    cfg.overlay_text = current["overlay_text"];
                if (current.count("multicast_ip"))
                    cfg.multicast_ip = current["multicast_ip"];
                if (current.count("multicast_iface"))
                    cfg.multicast_iface = current["multicast_iface"];
                out.streams.push_back(cfg);
            }
            current.clear();
        };

        std::string line;
        while (std::getline(file, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;

            if (line.front() == '[' && line.back() == ']')
            {
                flush_section(section);
                section = line.substr(1, line.size() - 2);
                continue;
            }

            auto pos = line.find('=');
            if (pos == std::string::npos)
                continue;
            std::string key = trim(line.substr(0, pos));
            std::string value = trim(line.substr(pos + 1));
            if (section.empty())
            {
                if (key == "mode")
                    out.mode = value;
                else if (key == "server_ip")
                    out.server_ip = value;
            }
            else
            {
                current[key] = value;
            }
        }
        flush_section(section);

        return !out.mode.empty();
    }

    static gboolean preview_bus_cb(GstBus *, GstMessage *msg, gpointer user_data)
    {
        GMainLoop *loop = static_cast<GMainLoop *>(user_data);
        switch (GST_MESSAGE_TYPE(msg))
        {
        case GST_MESSAGE_ERROR:
        case GST_MESSAGE_EOS:
            g_main_loop_quit(loop);
            break;
        default:
            break;
        }
        return TRUE;
    }

    static GstBusSyncReply preview_sync_handler(GstBus * /*bus*/, GstMessage *msg, gpointer data)
    {
        if (gst_is_video_overlay_prepare_window_handle_message(msg))
        {
            GstVideoOverlay *overlay = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg));
            gst_video_overlay_set_window_handle(overlay, (guintptr)data);
            return GST_BUS_DROP;
        }
        return GST_BUS_PASS;
    }

    static int run_preview(const PreviewConfig &cfg)
    {
        gst_init(nullptr, nullptr);

        GstElement *pipeline = gst_pipeline_new("screen-preview");
        GstElement *src = gst_element_factory_make("d3d11screencapturesrc", "src");
        GstElement *queue = gst_element_factory_make("queue", "que");
        GstElement *conv = gst_element_factory_make("d3d11convert", "conv");
        GstElement *capsfilter = gst_element_factory_make("capsfilter", "caps");
        GstElement *sink = gst_element_factory_make("d3d11videosink", "sink");

        if (!pipeline || !src || !queue || !conv || !capsfilter || !sink)
        {
            g_printerr("파이프라인 생성 실패\n");
            if (pipeline)
                gst_object_unref(pipeline);
            return -1;
        }

        g_object_set(src, "monitor-index", cfg.monitor_index, "show-cursor", TRUE, "capture-api", 1, NULL);
        g_object_set(sink, "force-aspect-ratio", FALSE, NULL);

        std::ostringstream caps_str;
        caps_str << "video/x-raw(memory:D3D11Memory),format=NV12,framerate=" << cfg.framerate << "/1";
        if (cfg.width > 0 && cfg.height > 0)
        {
            caps_str << ",width=" << cfg.width << ",height=" << cfg.height;
        }
        GstCaps *caps = gst_caps_from_string(caps_str.str().c_str());
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);

        gst_bin_add_many(GST_BIN(pipeline), src, queue, conv, capsfilter, sink, NULL);
        if (!gst_element_link_many(src, queue, conv, capsfilter, sink, NULL))
        {
            g_printerr("요소 연결 실패\n");
            gst_object_unref(pipeline);
            return -2;
        }

        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        gst_bus_add_signal_watch(bus);
        g_signal_connect(bus, "message", G_CALLBACK(preview_bus_cb), loop);
        gst_bus_set_sync_handler(bus, preview_sync_handler, (gpointer)cfg.window_handle, NULL);
        g_object_unref(bus);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        g_main_loop_run(loop);

        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        g_main_loop_unref(loop);

        gst_deinit();
        return 0;
    }

    static int run_server(const WorkerConfig &cfg)
    {
        gst_init(nullptr, nullptr);
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        GStreamerWrapper::RunScreenCaptureRtspServer(cfg.server_ip.empty() ? nullptr : cfg.server_ip.c_str(),
                                                     cfg.streams.data(),
                                                     static_cast<int>(cfg.streams.size()));
        // Keep process alive until external termination (Stop) or Ctrl+C
        while (true)
        {
            Sleep(200);
        }
        return 0;
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::wcerr << L"Usage: GstWorker --config <path>\n";
        return 1;
    }

    std::wstring config_path;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--config" && i + 1 < argc)
        {
            config_path = argv[++i];
        }
    }

    if (config_path.empty())
    {
        std::wcerr << L"Missing --config argument\n";
        return 1;
    }

    WorkerConfig config;
    if (!read_config_file(config_path, config))
    {
        std::wcerr << L"Failed to parse config file: " << config_path << L"\n";
        return 1;
    }

    if (config.mode == "preview")
    {
        return run_preview(config.preview);
    }
    if (config.mode == "server")
    {
        return run_server(config);
    }

    std::wcerr << L"Unknown mode: " << std::wstring(config.mode.begin(), config.mode.end()) << L"\n";
    return 1;
}

