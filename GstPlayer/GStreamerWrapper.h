#pragma once

#include <string>
#include <vector>
#include <Windows.h>

namespace GStreamerWrapper
{
    struct StreamConfigNative
    {
        int monitor_index{};
        int crop_x{};
        int crop_y{};
        int crop_w{};
        int crop_h{};
        int width{};
        int height{};
        int framerate{};
        int bitrate_kbps{};
        int keyframe_interval{};
        int port{};
        int streamIndex{};
        bool enable_audio{};
        bool enable_multicast{};
        std::string audio_device;
        bool enable_hw_accel{};
        bool enable_osd{};
        std::string bitrate_control;
        std::string profile;
        std::string overlay_text;
        std::string multicast_ip;
        std::string multicast_iface;
    };

    void Initialize();
    void Deinitialize();

    bool StartPreview(const StreamConfigNative& config, HWND parent_window);
    void StopPreview();

    // [추가] 자식 윈도우 크기 조절 함수
    void ResizePreview(int w, int h);

    void RunScreenCaptureRtspServer(const char* serverIp,
        const StreamConfigNative* configs,
        int count);

    void StopScreenCaptureRtspServer();
}