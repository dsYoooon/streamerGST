#pragma once

namespace GStreamerWrapper {
// Starts the RTSP screen capture server on a background thread. This
// call returns immediately and does not block the caller's thread.
void RunScreenCaptureRtspServer();

// Stops the RTSP screen capture server if it is running. Safe to call
// even when the server is not active.
void StopScreenCaptureRtspServer();
}
