#pragma once

namespace GStreamerWrapper {

// Stream configuration passed from the managed layer. This simple
// structure is shared between the C++ implementation and the C++/CLI
// wrapper.
struct StreamConfigNative {
    int monitor_index;
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    int width;
    int height;
    int framerate;
    int bitrate_kbps;
    int keyframe_interval;
};

// Starts the RTSP screen capture server on a background thread hosting
// `count` streams described by `configs`.
void RunScreenCaptureRtspServer(const char* serverIp,
                                const StreamConfigNative* configs,
                                int count);

// Stops the RTSP screen capture server if it is running. Safe to call
// even when the server is not active.
void StopScreenCaptureRtspServer();
}
