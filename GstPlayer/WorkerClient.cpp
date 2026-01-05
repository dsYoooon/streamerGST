#include "WorkerClient.h"

#include <filesystem>
#include <memory>
#include <sstream>
#include <string_view>

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\GStreamerWorker";
constexpr char kProtocolVersion[] = "1.0";

bool WriteMessage(HANDLE pipe, const nlohmann::json& message, std::string& error) {
    std::string payload = message.dump();
    DWORD length = static_cast<DWORD>(payload.size());
    DWORD written = 0;
    if (!WriteFile(pipe, &length, sizeof(length), &written, nullptr)) {
        error = "Failed to write length to pipe";
        return false;
    }
    written = 0;
    if (!WriteFile(pipe, payload.data(), length, &written, nullptr)) {
        error = "Failed to write payload to pipe";
        return false;
    }
    if (written != length) {
        error = "Incomplete pipe write";
        return false;
    }
    return true;
}

bool ReadMessage(HANDLE pipe, nlohmann::json& message, std::string& error) {
    DWORD length = 0;
    if (!ReadFile(pipe, &length, sizeof(length), nullptr, nullptr)) {
        error = "Failed to read length from pipe";
        return false;
    }

    std::string buffer(length, '\0');
    DWORD total_read = 0;
    while (total_read < length) {
        DWORD bytes_read = 0;
        if (!ReadFile(pipe, buffer.data() + total_read, length - total_read, &bytes_read, nullptr)) {
            error = "Failed to read payload from pipe";
            return false;
        }
        total_read += bytes_read;
    }

    try {
        message = nlohmann::json::parse(buffer);
    }
    catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }

    return true;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size_needed);
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size_needed, nullptr, nullptr);
    return result;
}

} // namespace

namespace GStreamerWrapper {

WorkerClient::WorkerClient() {
    InitializeCriticalSection(&lock_);
}

WorkerClient::~WorkerClient() {
    Shutdown();
    DeleteCriticalSection(&lock_);
}

bool WorkerClient::IsProcessAlive() const {
    if (process_handle_ == INVALID_HANDLE_VALUE || process_handle_ == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_handle_, &exit_code)) {
        return false;
    }
    return exit_code == STILL_ACTIVE;
}

std::wstring WorkerClient::ResolveWorkerPath() const {
    wchar_t module_path[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    std::filesystem::path base = std::filesystem::path(module_path).parent_path();
    std::filesystem::path candidate = base / L"GStreamerWorker.exe";
    if (std::filesystem::exists(candidate)) {
        return candidate.wstring();
    }
    candidate = base / L".." / L"GStreamerWorker" / L"GStreamerWorker.exe";
    if (std::filesystem::exists(candidate)) {
        return candidate.lexically_normal().wstring();
    }
    return L"GStreamerWorker.exe";
}

std::string WorkerClient::NextRequestId() {
    std::ostringstream oss;
    oss << request_counter_++;
    return oss.str();
}

bool WorkerClient::EnsureWorkerProcess(std::string& error) {
    if (IsProcessAlive()) {
        return true;
    }

    if (process_handle_ != INVALID_HANDLE_VALUE && process_handle_ != nullptr) {
        CloseHandle(process_handle_);
        process_handle_ = INVALID_HANDLE_VALUE;
    }

    std::wstring worker_path = ResolveWorkerPath();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring cmd_line = worker_path;

    if (!CreateProcessW(worker_path.c_str(), cmd_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        error = "Failed to spawn worker process";
        return false;
    }

    CloseHandle(pi.hThread);
    process_handle_ = pi.hProcess;
    return true;
}

bool WorkerClient::ConnectPipe(std::string& error) {
    if (pipe_handle_ != INVALID_HANDLE_VALUE && pipe_handle_ != nullptr) {
        return true;
    }

    DWORD wait_ms = 5000;
    if (!WaitNamedPipeW(kPipeName, wait_ms)) {
        error = "Worker pipe not available";
        return false;
    }

    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = "Failed to open worker pipe";
        return false;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    pipe_handle_ = pipe;
    return true;
}

bool WorkerClient::SendRequest(const nlohmann::json& request,
                               nlohmann::json& response,
                               std::string& error,
                               DWORD /*timeout_ms*/) {
    if (pipe_handle_ == INVALID_HANDLE_VALUE || pipe_handle_ == nullptr) {
        error = "Pipe not connected";
        return false;
    }

    if (!WriteMessage(pipe_handle_, request, error)) {
        return false;
    }
    if (!ReadMessage(pipe_handle_, response, error)) {
        return false;
    }
    return true;
}

bool WorkerClient::Ping(std::string& error) {
    EnterCriticalSection(&lock_);
    auto exit = std::unique_ptr<void, void(*)(void*)>(nullptr, [&](void*) { LeaveCriticalSection(&lock_); });

    if (!EnsureWorkerProcess(error)) {
        return false;
    }
    if (!ConnectPipe(error)) {
        return false;
    }

    nlohmann::json request;
    request["protocolVersion"] = kProtocolVersion;
    request["type"] = "ping";
    request["requestId"] = NextRequestId();
    request["payload"] = nlohmann::json::object();

    nlohmann::json response;
    if (!SendRequest(request, response, error)) {
        return false;
    }
    return response.value("payload", nlohmann::json::object()).value("status", "") == "ok";
}

bool WorkerClient::StartServer(const std::string& server_ip,
                               const std::vector<StreamConfigNative>& configs,
                               std::string& error) {
    EnterCriticalSection(&lock_);
    auto exit = std::unique_ptr<void, void(*)(void*)>(nullptr, [&](void*) { LeaveCriticalSection(&lock_); });

    if (!EnsureWorkerProcess(error)) {
        return false;
    }
    if (!ConnectPipe(error)) {
        return false;
    }

    nlohmann::json payload;
    payload["serverIp"] = server_ip;
    nlohmann::json cfg_array = nlohmann::json::array();
    for (const auto& cfg : configs) {
        nlohmann::json item;
        item["monitorIndex"] = cfg.monitor_index;
        item["cropX"] = cfg.crop_x;
        item["cropY"] = cfg.crop_y;
        item["cropW"] = cfg.crop_w;
        item["cropH"] = cfg.crop_h;
        item["width"] = cfg.width;
        item["height"] = cfg.height;
        item["framerate"] = cfg.framerate;
        item["bitrateKbps"] = cfg.bitrate_kbps;
        item["keyframeInterval"] = cfg.keyframe_interval;
        item["port"] = cfg.port;
        item["streamIndex"] = cfg.streamIndex;
        item["enableAudio"] = cfg.enable_audio;
        item["enableMulticast"] = cfg.enable_multicast;
        item["audioDevice"] = cfg.audio_device;
        item["enableHardwareAccel"] = cfg.enable_hw_accel;
        item["enableOsd"] = cfg.enable_osd;
        item["bitrateControl"] = cfg.bitrate_control;
        item["profile"] = cfg.profile;
        item["osdText"] = cfg.overlay_text;
        item["multiCastIp"] = cfg.multicast_ip;
        item["multiCastInterface"] = cfg.multicast_iface;
        cfg_array.push_back(item);
    }
    payload["configs"] = cfg_array;

    nlohmann::json request;
    request["protocolVersion"] = kProtocolVersion;
    request["type"] = "start_server";
    request["requestId"] = NextRequestId();
    request["payload"] = payload;

    nlohmann::json response;
    if (!SendRequest(request, response, error)) {
        return false;
    }

    auto status = response.value("payload", nlohmann::json::object()).value("status", "");
    if (status != "ok") {
        error = response.value("payload", nlohmann::json::object()).value("message", "start_server failed");
        return false;
    }
    return true;
}

bool WorkerClient::StopServer(std::string& error) {
    EnterCriticalSection(&lock_);
    auto exit = std::unique_ptr<void, void(*)(void*)>(nullptr, [&](void*) { LeaveCriticalSection(&lock_); });

    if (!IsProcessAlive()) {
        return true;
    }
    if (!ConnectPipe(error)) {
        return false;
    }

    nlohmann::json request;
    request["protocolVersion"] = kProtocolVersion;
    request["type"] = "stop_server";
    request["requestId"] = NextRequestId();
    request["payload"] = nlohmann::json::object();

    nlohmann::json response;
    if (!SendRequest(request, response, error)) {
        return false;
    }
    return true;
}

void WorkerClient::Shutdown() {
    EnterCriticalSection(&lock_);
    auto exit = std::unique_ptr<void, void(*)(void*)>(nullptr, [&](void*) { LeaveCriticalSection(&lock_); });

    if (pipe_handle_ != INVALID_HANDLE_VALUE && pipe_handle_ != nullptr) {
        nlohmann::json request;
        request["protocolVersion"] = kProtocolVersion;
        request["type"] = "shutdown";
        request["requestId"] = NextRequestId();
        request["payload"] = nlohmann::json::object();
        std::string error;
        nlohmann::json response;
        SendRequest(request, response, error, 2000);
        CloseHandle(pipe_handle_);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }

    if (process_handle_ != INVALID_HANDLE_VALUE && process_handle_ != nullptr) {
        WaitForSingleObject(process_handle_, 2000);
        CloseHandle(process_handle_);
        process_handle_ = INVALID_HANDLE_VALUE;
    }
}

} // namespace GStreamerWrapper
