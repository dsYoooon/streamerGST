#include <windows.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WorkerProtocol.h"
#include "../GstPlayer/ScreenCaptureServer.h"
#include "../third_party/nlohmann/json.hpp"

namespace {

using json = nlohmann::json;
using namespace GStreamerWorker;

constexpr const wchar_t* kPipeNameW = L"\\\\.\\pipe\\GStreamerWorker";
constexpr const char* kProtocolVersion = "1.0";

struct CommandContext {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    WorkerState state = WorkerState::Idle;
    std::string last_error;
    std::mutex mutex;
};

bool ReadMessage(HANDLE pipe, json& message) {
    DWORD length = 0;
    if (!ReadFile(pipe, &length, sizeof(length), nullptr, nullptr)) {
        return false;
    }

    std::string buffer(length, '\0');
    DWORD total_read = 0;
    while (total_read < length) {
        DWORD bytes_read = 0;
        if (!ReadFile(pipe, buffer.data() + total_read, length - total_read, &bytes_read, nullptr)) {
            return false;
        }
        total_read += bytes_read;
    }

    try {
        message = json::parse(buffer);
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

bool WriteMessage(HANDLE pipe, const json& message) {
    std::string payload = message.dump();
    DWORD length = static_cast<DWORD>(payload.size());
    DWORD written = 0;
    if (!WriteFile(pipe, &length, sizeof(length), &written, nullptr)) {
        return false;
    }
    written = 0;
    if (!WriteFile(pipe, payload.data(), length, &written, nullptr)) {
        return false;
    }
    return written == length;
}

std::vector<GStreamerWrapper::StreamConfigNative> ParseConfigs(const json& payload) {
    std::vector<GStreamerWrapper::StreamConfigNative> configs;
    if (!payload.contains("configs") || !payload["configs"].is_array()) {
        return configs;
    }

    for (const auto& cfg : payload["configs"]) {
        GStreamerWrapper::StreamConfigNative native{};
        native.monitor_index = cfg.value("monitorIndex", 0);
        native.crop_x = cfg.value("cropX", 0);
        native.crop_y = cfg.value("cropY", 0);
        native.crop_w = cfg.value("cropW", 0);
        native.crop_h = cfg.value("cropH", 0);
        native.width = cfg.value("width", 0);
        native.height = cfg.value("height", 0);
        native.framerate = cfg.value("framerate", 0);
        native.bitrate_kbps = cfg.value("bitrateKbps", 0);
        native.keyframe_interval = cfg.value("keyframeInterval", 0);
        native.port = cfg.value("port", 0);
        native.streamIndex = cfg.value("streamIndex", 0);
        native.enable_audio = cfg.value("enableAudio", false);
        native.enable_multicast = cfg.value("enableMulticast", false);
        native.audio_device = cfg.value("audioDevice", "");
        native.enable_hw_accel = cfg.value("enableHardwareAccel", false);
        native.enable_osd = cfg.value("enableOsd", false);
        native.bitrate_control = cfg.value("bitrateControl", "");
        native.profile = cfg.value("profile", "");
        native.overlay_text = cfg.value("osdText", "");
        native.multicast_ip = cfg.value("multiCastIp", "");
        native.multicast_iface = cfg.value("multiCastInterface", "");
        configs.push_back(native);
    }

    return configs;
}

json BuildResponse(const std::string& request_id, const std::string& type, const json& payload = json::object()) {
    json resp;
    resp["protocolVersion"] = kProtocolVersion;
    resp["type"] = type;
    resp["requestId"] = request_id;
    resp["payload"] = payload;
    return resp;
}

json BuildError(const std::string& request_id, const std::string& message) {
    json payload;
    payload["status"] = "error";
    payload["message"] = message;
    return BuildResponse(request_id, "error", payload);
}

bool HandleStart(CommandContext& ctx, const json& message, json& response) {
    const auto& payload = message.value("payload", json::object());
    std::string request_id = message.value("requestId", "");
    std::string server_ip = payload.value("serverIp", "");

    std::lock_guard<std::mutex> lock(ctx.mutex);
    if (ctx.state == WorkerState::Running || ctx.state == WorkerState::Starting) {
        response = BuildError(request_id, "Server already running or starting");
        return false;
    }

    auto configs = ParseConfigs(payload);
    ctx.state = WorkerState::Starting;
    try {
        GStreamerWrapper::RunScreenCaptureRtspServer(server_ip.empty() ? nullptr : server_ip.c_str(),
            configs.data(),
            static_cast<int>(configs.size()));
        ctx.state = WorkerState::Running;
        response = BuildResponse(request_id, "start_server", json{{"status", "ok"}});
        return true;
    }
    catch (const std::exception& ex) {
        ctx.state = WorkerState::Error;
        ctx.last_error = ex.what();
        response = BuildError(request_id, ctx.last_error);
        return false;
    }
}

bool HandleStop(CommandContext& ctx, const json& message, json& response) {
    std::string request_id = message.value("requestId", "");
    std::lock_guard<std::mutex> lock(ctx.mutex);
    if (ctx.state == WorkerState::Idle) {
        response = BuildResponse(request_id, "stop_server", json{{"status", "ok"}, {"message", "Already idle"}});
        return true;
    }

    ctx.state = WorkerState::Stopping;
    try {
        GStreamerWrapper::StopScreenCaptureRtspServer();
        ctx.state = WorkerState::Idle;
        response = BuildResponse(request_id, "stop_server", json{{"status", "ok"}});
        return true;
    }
    catch (const std::exception& ex) {
        ctx.state = WorkerState::Error;
        ctx.last_error = ex.what();
        response = BuildError(request_id, ctx.last_error);
        return false;
    }
}

bool HandleStatus(CommandContext& ctx, const json& message, json& response) {
    std::lock_guard<std::mutex> lock(ctx.mutex);
    std::string request_id = message.value("requestId", "");
    json payload;
    payload["state"] = ToString(ctx.state);
    if (!ctx.last_error.empty()) {
        payload["lastError"] = ctx.last_error;
    }
    response = BuildResponse(request_id, "get_status", payload);
    return true;
}

bool HandlePing(const json& message, json& response) {
    std::string request_id = message.value("requestId", "");
    response = BuildResponse(request_id, "ping", json{{"status", "ok"}});
    return true;
}

void PumpLoop(CommandContext& ctx) {
    while (true) {
        json message;
        if (!ReadMessage(ctx.pipe, message)) {
            break;
        }

        const std::string type = message.value("type", "");
        json response;
        bool ok = false;
        if (type == "ping") {
            ok = HandlePing(message, response);
        }
        else if (type == "start_server") {
            ok = HandleStart(ctx, message, response);
        }
        else if (type == "stop_server") {
            ok = HandleStop(ctx, message, response);
        }
        else if (type == "get_status") {
            ok = HandleStatus(ctx, message, response);
        }
        else if (type == "shutdown") {
            HandleStop(ctx, message, response);
            WriteMessage(ctx.pipe, response);
            break;
        }
        else {
            response = BuildError(message.value("requestId", ""), "Unknown command");
        }

        if (!WriteMessage(ctx.pipe, response)) {
            break;
        }

        if (!ok && ctx.state == WorkerState::Error) {
            // keep loop alive to allow client to query status
        }
    }
}

HANDLE CreatePipeInstance() {
    return CreateNamedPipeW(
        kPipeNameW,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        nullptr);
}

} // namespace

int wmain() {
    HANDLE pipe = CreatePipeInstance();
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to create named pipe" << std::endl;
        return 1;
    }

    if (!ConnectNamedPipe(pipe, nullptr)) {
        if (GetLastError() != ERROR_PIPE_CONNECTED) {
            std::cerr << "Failed to connect named pipe" << std::endl;
            CloseHandle(pipe);
            return 1;
        }
    }

    CommandContext ctx;
    ctx.pipe = pipe;

    PumpLoop(ctx);

    CloseHandle(pipe);
    return 0;
}
