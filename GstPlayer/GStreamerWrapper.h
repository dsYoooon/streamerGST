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

    bool StartPreview(const StreamConfigNative& config, HWND window);
    void StopPreview();

    // 현재 미리보기 윈도우에 대해 render rectangle을 다시 적용
    bool RefreshPreviewOverlay(HWND window);

    void RunScreenCaptureRtspServer(const char* serverIp,
        const StreamConfigNative* configs,
        int count);

    void StopScreenCaptureRtspServer();
}

