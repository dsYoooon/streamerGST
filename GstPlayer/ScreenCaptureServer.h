#pragma once

#include <string>
#include "GStreamerWrapper.h"

namespace GStreamerWrapper {

// Starts the RTSP screen capture server on a background thread hosting
// `count` streams described by `configs`.
void RunScreenCaptureRtspServer(const char* serverIp,
                                const StreamConfigNative* configs,
                                int count);

// Stops the RTSP screen capture server if it is running. Safe to call
// even when the server is not active.
void StopScreenCaptureRtspServer();
}
