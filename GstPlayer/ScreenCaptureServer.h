#pragma once

namespace GStreamerWrapper {
// Starts the RTSP screen capture server on a background thread. This
// call returns immediately and does not block the caller's thread.
//
// `serverIp` is the IP address the server will bind to. It was
// previously hard coded as a macro inside the implementation but is now
// provided dynamically from the .NET side so that applications can
// configure the address at run time.
void RunScreenCaptureRtspServer(const char* serverIp);

// Stops the RTSP screen capture server if it is running. Safe to call
// even when the server is not active.
void StopScreenCaptureRtspServer();
}
