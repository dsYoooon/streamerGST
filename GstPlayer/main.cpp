// main.cpp 전체 교체 또는 해당 함수 수정

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

    // [수정] __EMPTY__ 토큰 처리 추가
    std::string DecodeBase64(const std::string& value)
    {
        if (value == "__EMPTY__") return "";

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

            // [수정] bool 타입은 직접 읽지 말고 int로 읽어서 변환 (스트림 파싱 안정성)
            int i_audio = 0, i_multicast = 0, i_hw = 0, i_osd = 0;

            if (!(iss >> cfg.monitor_index >> cfg.crop_x >> cfg.crop_y >> cfg.crop_w >> cfg.crop_h
                >> cfg.width >> cfg.height >> cfg.framerate >> cfg.bitrate_kbps >> cfg.keyframe_interval
                >> cfg.port >> cfg.streamIndex
                >> i_audio          // bool -> int
                >> i_multicast      // bool -> int
                >> audio_b64
                >> i_hw             // bool -> int
                >> i_osd            // bool -> int
                >> bitrate_b64 >> profile_b64
                >> osd_b64 >> mc_ip_b64 >> mc_iface_b64))
            {
                // 디버깅을 위해 어디서 실패했는지 출력하고 싶다면:
                // std::cout << "Parse failed at index " << i << std::endl;
                return false;
            }

            // int -> bool 변환
            cfg.enable_audio = (i_audio != 0);
            cfg.enable_multicast = (i_multicast != 0);
            cfg.enable_hw_accel = (i_hw != 0);
            cfg.enable_osd = (i_osd != 0);

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

    // ... 나머지 StartServer, StartPreviewWithMonitor 함수는 기존과 동일 ...
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
        // 기본값 설정 (혹시 config가 없을 경우 안전장치)
        cfg.monitor_index = monitorIndex;
        cfg.framerate = 30;

        if (!g_last_configs.empty())
        {
            // monitorIndex에 맞는 설정 찾기, 없으면 첫 번째 것 사용 (또는 기본값)
            bool found = false;
            for (const auto& c : g_last_configs)
            {
                if (c.monitor_index == monitorIndex)
                {
                    cfg = c;
                    found = true;
                    break;
                }
            }
            if (!found) cfg = g_last_configs.front();
        }

        // Preview용 강제 설정 (필요 시)
        cfg.monitor_index = monitorIndex;

        StartPreview(cfg, hwnd);
    }
}

int main()
{
    // [중요] C++ 표준 입출력 속도 향상 및 버퍼링 이슈 방지
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    Initialize();

    std::string line;
    while (std::getline(std::cin, line))
    {
        //SetConsoleOutputCP(CP_UTF8);
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "CMD_START_SERVER")
        {
            std::string ip;
            int count = 0;
            iss >> ip >> count;

            // ip 문자열의 '_'를 다시 공백으로 복구할 필요가 있다면 여기서 처리
            // (현재 C#에서 Replace(' ', '_') 했으므로 IP에는 영향 없음)

            std::vector<StreamConfigNative> configs;
            if (!ParseStreamConfigs(iss, count, configs))
            {
                std::cout << "[ERROR] Parsing stream configs failed." << std::endl;
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
            if (iss >> hwnd_val >> monitorIdx) {
                StartPreviewWithMonitor(reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_val)), monitorIdx);
                std::cout << "PREVIEW_STARTED" << std::endl;
            }
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
        else
        {
            // 알 수 없는 커맨드 디버깅용
            // std::cout << "UNKNOWN_CMD: " << cmd << std::endl;
        }
    }

    Deinitialize();
    return 0;
}