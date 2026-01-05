#include "GStreamerWrapper.h"
#include "ScreenCaptureServer.h"

#include <gst/gst.h>
#include <glib.h>

#include <iostream>
#include <sstream>
#include <vector>
#include <string>

using namespace GStreamerWrapper;

namespace
{
    std::vector<StreamConfigNative> g_last_configs;
    std::string g_last_server_ip;
    HWND g_last_preview_hwnd = nullptr;
    int g_last_preview_monitor = 0;

    std::string DecodeBase64(const std::string& value)
    {
        gsize out_len = 0;
        guchar* data = g_base64_decode(value.c_str(), &out_len);
        std::string result;
        if (data && out_len > 0)
        {
            result.assign(reinterpret_cast<char*>(data), reinterpret_cast<char*>(data) + out_len);
        }
        g_free(data);
        return result;
    }

    bool ParseStreamConfigs(std::istream& iss, int count, std::vector<StreamConfigNative>& out)
    {
        out.clear();
        for (int i = 0; i < count; ++i)
        {
            StreamConfigNative cfg{};
            std::string audio_b64, bitrate_b64, profile_b64, osd_b64, mc_ip_b64, mc_iface_b64;
            if (!(iss >> cfg.monitor_index >> cfg.crop_x >> cfg.crop_y >> cfg.crop_w >> cfg.crop_h
                >> cfg.width >> cfg.height >> cfg.framerate >> cfg.bitrate_kbps >> cfg.keyframe_interval
                >> cfg.port >> cfg.streamIndex >> cfg.enable_audio >> cfg.enable_multicast
                >> audio_b64 >> cfg.enable_hw_accel >> cfg.enable_osd >> bitrate_b64 >> profile_b64
                >> osd_b64 >> mc_ip_b64 >> mc_iface_b64))
            {
                return false;
            }

            cfg.audio_device = DecodeBase64(audio_b64);
            cfg.bitrate_control = DecodeBase64(bitrate_b64);
            cfg.profile = DecodeBase64(profile_b64);
            cfg.overlay_text = DecodeBase64(osd_b64);
            cfg.multicast_ip = DecodeBase64(mc_ip_b64);
            cfg.multicast_iface = DecodeBase64(mc_iface_b64);
            out.push_back(cfg);
        }
        return true;
    }

    void StartServer(const std::string& ip, const std::vector<StreamConfigNative>& configs)
    {
        g_last_server_ip = ip;
        g_last_configs = configs;
        RunScreenCaptureRtspServer(ip.empty() ? nullptr : ip.c_str(), configs.data(), static_cast<int>(configs.size()));
    }

    void StartPreviewWithMonitor(HWND hwnd, int monitorIndex)
    {
        g_last_preview_hwnd = hwnd;
        g_last_preview_monitor = monitorIndex;

        StreamConfigNative cfg{};
        if (!g_last_configs.empty())
        {
            cfg = g_last_configs.front();
            for (const auto& c : g_last_configs)
            {
                if (c.monitor_index == monitorIndex)
                {
                    cfg = c;
                    break;
                }
            }
        }
        cfg.monitor_index = monitorIndex;
        StartPreview(cfg, hwnd);
    }
}

int main()
{
    Initialize();

    std::string line;
    while (std::getline(std::cin, line))
    {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "CMD_START_SERVER")
        {
            std::string ip;
            int count = 0;
            iss >> ip >> count;
            std::vector<StreamConfigNative> configs;
            if (!ParseStreamConfigs(iss, count, configs))
            {
                std::cout << "ERROR parsing configs" << std::endl;
                continue;
            }
            StartServer(ip, configs);
            std::cout << "SERVER_STARTED" << std::endl;
        }
        else if (cmd == "CMD_STOP_SERVER")
        {
            StopScreenCaptureRtspServer();
            std::cout << "SERVER_STOPPED" << std::endl;
        }
        else if (cmd == "CMD_START_PREVIEW")
        {
            long long hwnd_val = 0;
            int monitorIdx = 0;
            iss >> hwnd_val >> monitorIdx;
            StartPreviewWithMonitor(reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val)), monitorIdx);
            std::cout << "PREVIEW_STARTED" << std::endl;
        }
        else if (cmd == "CMD_STOP_PREVIEW")
        {
            StopPreview();
            std::cout << "PREVIEW_STOPPED" << std::endl;
        }
        else if (cmd == "CMD_EXIT")
        {
            break;
        }
    }

    Deinitialize();
    return 0;
}

