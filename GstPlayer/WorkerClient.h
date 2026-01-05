#pragma once

#include <string>
#include <vector>
#define NOMINMAX
#include <windows.h>

#include "ScreenCaptureServer.h"
#include "../third_party/nlohmann/json.hpp"

namespace GStreamerWrapper {

class WorkerClient {
public:
    WorkerClient();
    ~WorkerClient();

    bool StartServer(const std::string& server_ip,
                     const std::vector<StreamConfigNative>& configs,
                     std::string& error);

    bool StopServer(std::string& error);
    bool Ping(std::string& error);
    void Shutdown();

private:
    bool EnsureWorkerProcess(std::string& error);
    bool ConnectPipe(std::string& error);
    bool SendRequest(const nlohmann::json& request,
                     nlohmann::json& response,
                     std::string& error,
                     DWORD timeout_ms = 5000);
    bool IsProcessAlive() const;
    std::wstring ResolveWorkerPath() const;
    std::string NextRequestId();

    HANDLE process_handle_ = INVALID_HANDLE_VALUE;
    HANDLE pipe_handle_ = INVALID_HANDLE_VALUE;
    mutable CRITICAL_SECTION lock_;
    unsigned int request_counter_ = 1;
};

} // namespace GStreamerWrapper
