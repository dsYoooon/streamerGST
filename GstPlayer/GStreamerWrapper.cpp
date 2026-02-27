#include "GStreamerWrapper.h"
#include "ScreenCaptureServer.h"

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <glib.h>
#include <sstream>
#include <thread>
#include <atomic>
#include <tchar.h>

namespace GStreamerWrapper
{
    namespace
    {
        GstElement* g_preview_pipeline = nullptr;

        // 윈도우 관리용 전역 변수
        std::thread g_window_thread;
        std::atomic<HWND> g_child_hwnd{ nullptr }; // GStreamer가 그릴 실제 윈도우 (자식)
        std::atomic<bool> g_stop_window_thread{ false };

        // 윈도우 프로시저 (메시지 처리)
        LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }

        // 별도 스레드에서 윈도우 생성 및 메시지 루프 실행
        void WindowThreadFunc(HWND parent_hwnd) {
            // 1. 윈도우 클래스 등록 (유니크한 이름 사용)
            WNDCLASSEX wc = { 0 };
            wc.cbSize = sizeof(WNDCLASSEX);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = PreviewWndProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = _T("GstPreviewClass_Sub");
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            RegisterClassEx(&wc);

            // 2. 자식 윈도우 생성 (부모: C#에서 받은 핸들)
            // WS_CHILD 스타일 필수
            HWND child = CreateWindowEx(
                0, _T("GstPreviewClass_Sub"), _T("GstChild"),
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                0, 0, 800, 600, // 초기 크기 (나중에 ResizePreview로 조절됨)
                parent_hwnd, NULL, GetModuleHandle(NULL), NULL
            );

            g_child_hwnd.store(child);

            // 3. 메시지 루프 (d3d11videosink가 이 루프에 의존하여 화면을 갱신함)
            MSG msg;
            while (!g_stop_window_thread.load() && GetMessage(&msg, NULL, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            // 종료 처리
            if (child) DestroyWindow(child);
            UnregisterClass(_T("GstPreviewClass_Sub"), GetModuleHandle(NULL));
            g_child_hwnd.store(nullptr);
        }

        void StopPreviewInternal()
        {
            // 1. 파이프라인 정지
            if (g_preview_pipeline) {
                gst_element_set_state(g_preview_pipeline, GST_STATE_NULL);
                gst_object_unref(g_preview_pipeline);
                g_preview_pipeline = nullptr;
            }

            // 2. 윈도우 스레드 종료
            if (g_window_thread.joinable()) {
                g_stop_window_thread.store(true);
                // 메시지 루프 깨우기 (GetMessage 탈출)
                HWND hwnd = g_child_hwnd.load();
                if (hwnd) PostMessage(hwnd, WM_NULL, 0, 0);
                g_window_thread.join();
            }
            g_stop_window_thread.store(false);
            g_child_hwnd.store(nullptr);
        }
    }

    void Initialize() {
        static bool initialized = false;
        if (!initialized) {
            gst_init(nullptr, nullptr);
            initialized = true;
        }
    }

    void Deinitialize() {
        StopPreviewInternal();
        StopScreenCaptureRtspServer();
    }

    // [추가] 외부에서 호출할 리사이즈 함수 (C# -> Main -> 여기)
    void ResizePreview(int w, int h) {
        HWND hwnd = g_child_hwnd.load();
        if (hwnd) {
            // SetWindowPos는 다른 스레드에서 호출해도 안전함 (OS가 메시지로 처리)
            SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        }
    }

    bool StartPreview(const StreamConfigNative& config, HWND parent_window)
    {
        StopPreviewInternal();

        // 1. UI 스레드 시작 (C# 윈도우 안에 자식 윈도우 생성)
        g_stop_window_thread.store(false);
        g_window_thread = std::thread(WindowThreadFunc, parent_window);

        // 윈도우가 생성될 때까지 잠시 대기
        int retries = 50;
        while (retries-- > 0 && g_child_hwnd.load() == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        HWND render_target = g_child_hwnd.load();
        if (!render_target) {
            g_print("[ERROR] Failed to create preview child window\n");
            return false;
        }

        // 2. 파이프라인 생성
        GstElement* pipeline = gst_pipeline_new("screen-preview");
        GstElement* src = gst_element_factory_make("d3d11screencapturesrc", "src");
        GstElement* queue = gst_element_factory_make("queue", "queue");
        GstElement* conv = gst_element_factory_make("d3d11convert", "conv");
        GstElement* capsfilter = gst_element_factory_make("capsfilter", "caps");
        GstElement* sink = gst_element_factory_make("d3d11videosink", "sink");

        if (!pipeline || !src || !queue || !conv || !capsfilter || !sink) {
            if (pipeline) gst_object_unref(pipeline);
            return false;
        }

        g_object_set(src, "monitor-index", config.monitor_index, "show-cursor", TRUE, NULL);

        // [중요] 자식 윈도우에 꽉 차게 그리기 위해 force-aspect-ratio 끔
        g_object_set(sink, "force-aspect-ratio", FALSE, NULL);

        std::ostringstream caps_str;
        caps_str << "video/x-raw(memory:D3D11Memory),format=NV12,framerate="
            << (config.framerate > 0 ? config.framerate : 30) << "/1";
        GstCaps* caps = gst_caps_from_string(caps_str.str().c_str());
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);

        gst_bin_add_many(GST_BIN(pipeline), src, queue, conv, capsfilter, sink, NULL);
        if (!gst_element_link_many(src, queue, conv, capsfilter, sink, NULL)) {
            gst_object_unref(pipeline);
            return false;
        }

        // 3. 오버레이 설정 (우리가 만든 자식 윈도우 핸들 사용)
        GstVideoOverlay* overlay = GST_VIDEO_OVERLAY(sink);
        gst_video_overlay_set_window_handle(overlay, (guintptr)render_target);

        g_preview_pipeline = pipeline;
        gst_element_set_state(pipeline, GST_STATE_PLAYING);

        return true;
    }

    void StopPreview() {
        StopPreviewInternal();
    }
}