#include "ConsumerBin.h"
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <cstdlib>
#include <ctime>
#include <string>
#include <chrono>
#include <thread>
#include <Windows.h>
#include <atomic>
#include <cstdio>

// 전역적으로 FPS 표시 on/off 여부 (true: 표시, false: 미표시)
static std::atomic_bool g_showFps(true);
static bool qcapOk = false;
// FPS 측정을 위한 데이터 구조체 (pad probe callback에서 사용)
struct FpsData {
    GstElement* overlay;     // 텍스트 오버레이 요소
    gint64 lastTime;         // 마지막 업데이트 시간 (마이크로초)
    int frameCount;          // 1초 동안 누적된 프레임 수
};

// pad probe callback: overlay의 src pad를 통해 전달되는 버퍼를 카운트하여 1초마다 FPS를 계산
static GstPadProbeReturn fps_probe_callback(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    FpsData* data = static_cast<FpsData*>(user_data);
    // 버퍼가 전달될 때마다 프레임 카운트 증가
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        data->frameCount++;
        gint64 now = g_get_monotonic_time();
        if (now - data->lastTime >= G_TIME_SPAN_SECOND) { // 1초 경과
            double fps = data->frameCount * 1.0;
            if (g_showFps.load()) {
                gchar* text = g_strdup_printf("FPS: %.1f", fps);
                g_object_set(G_OBJECT(data->overlay), "text", text, NULL);
                g_free(text);
            }
            else {
                // 표시 off인 경우 빈 문자열로 설정
                g_object_set(G_OBJECT(data->overlay), "text", "", NULL);
            }
            data->frameCount = 0;
            data->lastTime = now;
        }
    }
    return GST_PAD_PROBE_OK;
}

// 기존 ConsumerBin 생성에 사용하던 카운터 (ghost pad 생성 및 bin 이름용)
static std::atomic<int> consumerCounter{ 0 };
// 전역 디코더 선택을 위한 index 변수 (전역 index 증가)
static std::atomic<int> globalConsumerDecoderIndex{ 0 };
static void set_queue_limits(GstElement* q) {
    g_object_set(q,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", 350 * GST_MSECOND,
        //"leaky", 1, 
        NULL);
}
ConsumerBin::ConsumerBin(int left, int top, int width, int height, int zorder)
    : consumerBin_(nullptr), windowHandle(NULL),
    _top(top), _left(left), _width(width), _height(height), _zorder(zorder)
{
    windowHandle = CreatePlaybackWindow(left, top, width, height);
    if (!windowHandle) {
        g_printerr("Failed to create playback window.\n");
    }
}
ConsumerBin::ConsumerBin(double rLeft, double rTop, double rWidth, double rHeight, int wallWidth, int wallHeight, int zorder)
    : consumerBin_(nullptr), windowHandle(NULL),
    _top(rTop*wallHeight), _left(rLeft*wallWidth), _width(rWidth*wallWidth), _height(rHeight*wallHeight), _zorder(zorder), 
    _ratioTop(rTop),_ratioLeft(rLeft),_ratioHeight(rHeight),_ratioWidth(rWidth)
{
    windowHandle = CreatePlaybackWindow(_left, _top, _width, _height);
    if (!windowHandle) {
        g_printerr("Failed to create playback window.\n");
    }
}


ConsumerBin::ConsumerBin() : consumerBin_(nullptr), windowHandle(NULL)
{
    // 기본 생성자: 창 생성하지 않음
}

ConsumerBin::~ConsumerBin() {
    Shutdown();
    if (windowHandle) {
        DestroyWindow(windowHandle);
        windowHandle = NULL;
    }
}

bool ConsumerBin::IsSame(double rLeft, double rTop, double rWidth, double rHeight) {
    if (rLeft == _ratioLeft && rTop == _ratioTop && rWidth == _ratioWidth && rHeight == _ratioHeight)
        return true;
    return false;
        
}

HWND ConsumerBin::CreatePlaybackWindow(int left, int top, int width, int height) {
    const wchar_t CLASS_NAME[] = L"GStreamer Playback Window";
    WNDCLASS wc = {};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);
    HWND hwnd = CreateWindowEx(
        WS_EX_TOOLWINDOW,
       // WS_OVERLAPPED,
        CLASS_NAME,
        L"",
        WS_POPUP | WS_VISIBLE ,
        left, top, width, height,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    return hwnd;
}
GstElement* SetDec(int localIndex,const char* name) {
    GstElement* dec = nullptr;
    switch (localIndex % 3) {
    case 0: dec = gst_element_factory_make("nvh264device1dec", name); 
        break;
    case 1: dec = gst_element_factory_make("nvh264dec", name); 
        break;
    case 2: dec = gst_element_factory_make("nvh264device3dec", name); 
        break;
    case 3: dec = gst_element_factory_make("nvh264device2dec", name); 
        break;
    default: dec = gst_element_factory_make("nvh264dec", name); 
        break;
    }
    if (!dec) {
        dec = gst_element_factory_make("d3d11h264dec", name);
    }
    //dec = gst_element_factory_make("d3d11h264dec", name);
    return dec;
}
bool ConsumerBin::Init() {
    int localIndex = globalConsumerDecoderIndex.fetch_add(1);
    std::string binName = "consumer-bin-" + std::to_string(consumerCounter.fetch_add(1));
    //gst_debug_set_default_threshold(GST_LEVEL_INFO);
    consumerBin_ = gst_bin_new(binName.c_str());
    if (!consumerBin_) {
        g_printerr("Failed to create consumer bin.\n");
        return false;
    }
    
    //videotestsrc branch
    //GstElement* vt_src = gst_element_factory_make("videotestsrc", "vt_src");
    GstElement* vt_queue = gst_element_factory_make("queue", "vt_que");

    // ■ RTSP video branch
    GstElement* q_rtsp = gst_element_factory_make("queue", "video_q_rtsp");
    GstElement* parser = gst_element_factory_make("h264parse", "h264_parse");
    GstElement* dec = SetDec(localIndex, "rtsp_h264_dec");
    GstElement* conv = gst_element_factory_make("videoconvert", "vconv");


    // ■ FILE video branch
    GstElement* q_file = gst_element_factory_make("queue", "video_q_file");
    GstElement* parsef = gst_element_factory_make("h264parse", "h264_parse_file");
    GstElement* decf = SetDec(localIndex, "file_h264_dec");
    GstElement* convf = gst_element_factory_make("videoconvert", "vconvf");
    // image file branch
    GstElement* image_queue = gst_element_factory_make("queue", "image_queue");

    // capture branch
    GstElement* qc = gst_element_factory_make("queue", "vq_cap");
    GstElement* vc = gst_element_factory_make("videoconvert", "cap_conv");
    
    
    GstElement* input_selector = gst_element_factory_make("input-selector", "input_selector");

    //downstream
    GstElement* vq_out = gst_element_factory_make("queue", "vq_out");
    GstElement* identity = gst_element_factory_make("identity", "identity");
    GstElement* overlay = gst_element_factory_make("textoverlay", "overlay");
    GstElement* sink = gst_element_factory_make("d3d11videosink", "d3d11videosink");
    //GstElement* sink = gst_element_factory_make("fakesink", "d3d11videosink");
    if (
        //!vt_src ||
        !vt_queue 
        ||!q_rtsp || !parser || !dec || !conv
        || !q_file || !parsef || !decf || !convf
        || !qc || !vc
        || !image_queue
        || !input_selector || !vq_out || !identity || !overlay || !sink 
        )
    {
        g_printerr("consumer_bin 요소 생성 실패\n");
        return false;
    }

    set_queue_limits(qc);
    set_queue_limits(vt_queue);
    set_queue_limits(q_rtsp);
    //set_queue_limits(q_file);
    g_object_set(q_file,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", 2000 * GST_MSECOND,
        "leaky", 1, 
        NULL);
    set_queue_limits(vq_out);
    set_queue_limits(image_queue);
    g_object_set(identity, "sync", false, NULL);
    g_object_set(G_OBJECT(overlay),
        "text", "",    // 초기 텍스트는 빈 문자열
        "color", 0xFFFFFFFF,
        "halignment", 2,
        "valignment", 3,
        NULL);
        
    g_object_set(G_OBJECT(sink),
        "enable-last-sample", false,
        //"force-aspect-ratio", false,
        "sync", TRUE,
        "render-delay", 0,
        "max-lateness", 0,
        NULL);

    gst_bin_add_many(GST_BIN(consumerBin_), input_selector, vq_out, identity, overlay, sink, NULL);
    gst_bin_add_many(GST_BIN(consumerBin_), image_queue
        , q_file, parsef, decf, convf
        , q_rtsp, parser, dec, conv
        //비디오테스트소스 추후
        //, vt_src
        , vt_queue
        , NULL);
    if (qcapOk) {
        gst_bin_add_many(GST_BIN(consumerBin_), qc, vc,  NULL);
    }
    //videotest 추후
    //gst_element_link_many(vt_src, vt_queue, nullptr);


    //gst_element_link_pads(vt_queue, "src", input_selector, "sink_0");
    gst_element_link_many(input_selector, vq_out, identity, 
        overlay, 
        sink, NULL);


    // RTSP video → selector
    gst_element_link_many(q_rtsp,
        parser,
        dec, conv, NULL);
    GstPad* video_rtsp_sel_pad = gst_element_request_pad_simple(input_selector, "sink_%u");
    {
        GstPad* s = gst_element_get_static_pad(conv, "src");
        gst_pad_link(s, video_rtsp_sel_pad);
        gst_object_unref(s);
    }
    {
        // ghost pad for RTSP
        GstPad* p = gst_element_get_static_pad(q_rtsp, "sink");
        gst_element_add_pad(consumerBin_, gst_ghost_pad_new("sink_rtsp_h264", p));
        gst_object_unref(p);
    }
    g_object_set_data(G_OBJECT(consumerBin_), "request_pad_rtsp_h264", video_rtsp_sel_pad);
    //file video
    gst_element_link_many(q_file,
        parsef,
        decf,
        convf,
        NULL);
    GstPad* video_file_sel_pad = gst_element_request_pad_simple(input_selector, "sink_%u");
    {
        GstPad* s = gst_element_get_static_pad(convf, "src");
        gst_pad_link(s, video_file_sel_pad);
        gst_object_unref(s);
    }
    {
        GstPad* p = gst_element_get_static_pad(q_file, "sink");
        gst_element_add_pad(consumerBin_, gst_ghost_pad_new("sink_file_h264", p));
        gst_object_unref(p);
    }
    g_object_set_data(G_OBJECT(consumerBin_), "request_pad_file_h264", video_file_sel_pad);
    // file image
    GstPad* video_image_sel_pad = gst_element_request_pad_simple(input_selector, "sink_%u");
    {
        GstPad* s = gst_element_get_static_pad(image_queue, "src");
        gst_pad_link(s, video_image_sel_pad);
        gst_object_unref(s);
    }
    {
        GstPad* sink = gst_element_get_static_pad(image_queue, "sink");
        gst_element_add_pad(consumerBin_, gst_ghost_pad_new("sink_file_image", sink));
        gst_object_unref(sink);
    }
    g_object_set_data(G_OBJECT(consumerBin_), "request_pad_file_image", video_image_sel_pad);
    GstPad* video_vt_sel_pad = gst_element_request_pad_simple(input_selector, "sink_%u");
    {
        GstPad* s = gst_element_get_static_pad(vt_queue, "src");
        gst_pad_link(s, video_vt_sel_pad);
        gst_object_unref(s);
    }
    {
        GstPad* sink = gst_element_get_static_pad(vt_queue, "sink");
        gst_element_add_pad(consumerBin_, gst_ghost_pad_new("sink_video_test", sink));
        gst_object_unref(sink);
    }
    g_object_set_data(G_OBJECT(consumerBin_), "request_pad_video_test", video_vt_sel_pad);
    //gst_element_set_state(consumerBin_, GST_STATE_READY);
    //capture 
    /*
    gst_element_link_many(qc, vc, NULL);
    GstPad* cap_sel_pad = gst_element_request_pad_simple(input_selector, "sink_%u");
    {
        GstPad* s = gst_element_get_static_pad(vc, "src");
        gst_pad_link(s, cap_sel_pad);
        gst_object_unref(s);
    }
    {
        GstPad* sink = gst_element_get_static_pad(qc, "sink");
        gst_element_add_pad(consumerBin_, gst_ghost_pad_new("sink_capture_card", sink));
        gst_object_unref(sink);
    }
    g_object_set_data(G_OBJECT(consumerBin_), "request_pad_capture_card", cap_sel_pad);
    */


    // --- Step L: Video overlay 설정 (sink에 윈도우 핸들 연결) ---
    if (GST_IS_VIDEO_OVERLAY(sink)) {
        GstVideoOverlay* overlaySink = GST_VIDEO_OVERLAY(sink);
        gst_video_overlay_set_window_handle(overlaySink, reinterpret_cast<guintptr>(windowHandle));
        gst_video_overlay_handle_events(overlaySink, TRUE);
    }
    else {
        g_printerr("[ERROR] The video sink is not a valid overlay sink.\n");
    }
    
    // --- 새로 추가: pad probe를 통한 FPS 측정 ---
    // overlay의 src pad에 pad probe를 등록합니다.
    GstPad* probePad = gst_element_get_static_pad(overlay, "src");
    if (probePad) {
        // FpsData 구조체 초기화
        FpsData* fpsData = new FpsData;
        fpsData->overlay = overlay;
        fpsData->lastTime = g_get_monotonic_time();
        fpsData->frameCount = 0;
        guint probe_id = gst_pad_add_probe(probePad, GST_PAD_PROBE_TYPE_BUFFER, fps_probe_callback, fpsData, NULL);
        // probe_id를 나중에 제거할 수 있도록 저장
        g_object_set_data(
            G_OBJECT(consumerBin_),
            "fps_probe_id",
            GUINT_TO_POINTER(probe_id)
        );
        gst_object_unref(probePad);
        // fpsData 해제는 Shutdown()에서 처리
        g_object_set_data(G_OBJECT(consumerBin_), "fps_data", fpsData);
    }
    else {
        g_printerr("Failed to get overlay src pad for FPS probe.\n");
    }
    //gst_debug_set_default_threshold(GST_LEVEL_ERROR);
    return true;
}

void ConsumerBin::ReconfigureSink() {
     
    GstElement* sink = gst_bin_get_by_name(GST_BIN(consumerBin_), "d3d11videosink");
    if (!sink) return;

    gst_element_set_state(consumerBin_, GST_STATE_NULL);

    //gst_video_overlay_set_window_handle(
    //    GST_VIDEO_OVERLAY(sink),
    //    reinterpret_cast<guintptr>(windowHandle)
    //);
    //RECT rc;
    //GetClientRect((HWND)windowHandle, &rc);
    //int w = rc.right - rc.left;
    //int h = rc.bottom - rc.top;

    //// 5) 렌더 사각형 명시적 설정 (0,0부터 w×h 영역)
    //gst_video_overlay_set_render_rectangle(
    //    GST_VIDEO_OVERLAY(sink),
    //    /*x=*/0, /*y=*/0,
    //    /*w=*/w, /*h=*/h
    //);
    //gst_element_send_event(sink, gst_event_new_reconfigure());
    //
    //gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));

    gst_element_set_state(consumerBin_, GST_STATE_PLAYING);

    gst_object_unref(sink);
}

bool ConsumerBin::Start() {
    if (!consumerBin_) {
        g_printerr("Consumer bin not initialized.\n");
        return false;
    }
    GstStateChangeReturn ret = gst_element_set_state(consumerBin_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to set consumer bin to PLAYING state.\n");
        return false;
    }
    GstState current, pending;
    ret = gst_element_get_state(consumerBin_, &current, &pending, 1 * GST_SECOND);
    g_printerr("Consumer bin state after Start(): %s\n", gst_element_state_get_name(current));
    if (current != GST_STATE_PLAYING) {
        g_printerr("Consumer bin did not reach PLAYING state.\n");
        return false;
    }
    return true;
}

void ConsumerBin::SyncParent() {
    if (consumerBin_) {
        gst_element_sync_state_with_parent(consumerBin_);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ■ ■ ■ ConsumerBin::Shutdown() (전체 재작성) ■ ■ ■
//void ConsumerBin::Shutdown() {
//    gst_debug_set_threshold_for_name("GST_REFCOUNTING", GST_LEVEL_MEMDUMP);
//
//    if (!consumerBin_)
//        return;
//
//    // 1) FPS pad-probe 제거
//    gpointer probePtr = g_object_get_data(G_OBJECT(consumerBin_), "fps_probe_id");
//    if (probePtr) {
//        guint probe_id = GPOINTER_TO_UINT(probePtr);
//        // overlay src pad 가져와서 probe 제거
//        GstElement* overlayElem = gst_bin_get_by_name(GST_BIN(consumerBin_), "overlay");
//        if (overlayElem) {
//            GstPad* pad = gst_element_get_static_pad(overlayElem, "src");
//            if (pad) {
//                gst_pad_remove_probe(pad, probe_id);
//                gst_object_unref(pad);
//            }
//            gst_object_unref(overlayElem);
//        }
//        g_object_set_data(G_OBJECT(consumerBin_), "fps_probe_id", NULL);
//    }
//
//    // 2) fps_data 메모리 해제
//    gpointer fpsDataPtr = g_object_get_data(G_OBJECT(consumerBin_), "fps_data");
//    if (fpsDataPtr) {
//        delete static_cast<FpsData*>(fpsDataPtr);
//        g_object_set_data(G_OBJECT(consumerBin_), "fps_data", NULL);
//    }
//
//    // 3) Input-selector에 요청한 request-pad 반납
//    GstElement* selector = gst_bin_get_by_name(GST_BIN(consumerBin_), "input_selector");
//    if (selector) {
//        const char* keys[] = {
//            "request_pad_rtsp_h264",
//            "request_pad_file_h264",
//            "request_pad_file_image",
//            "request_pad_video_test",
//            // "request_pad_capture_card" // 필요 시 추가
//        };
//        for (auto key : keys) {
//            GstPad* pad = static_cast<GstPad*>(
//                g_object_get_data(G_OBJECT(consumerBin_), key)
//                );
//            if (pad) {
//                gst_element_release_request_pad(selector, pad);
//                gst_object_unref(pad);
//                g_object_set_data(G_OBJECT(consumerBin_), key, NULL);
//            }
//        }
//        gst_object_unref(selector);
//    }
//
//    // 4) ghost pad 제거
//    const char* ghosts[] = {
//        "sink_rtsp_h264",
//        "sink_file_h264",
//        "sink_file_image",
//        "sink_video_test",
//       /// "sink_capture_card"
//    };
//    for (auto name : ghosts) {
//        // 1) 이름으로 pad 객체를 얻어온다
//        GstPad* ghost_pad = gst_element_get_static_pad(consumerBin_, name);
//        if (ghost_pad) {
//            // 2) pad 제거
//            if (!gst_element_remove_pad(consumerBin_, ghost_pad)) {
//                g_printerr("Shutdown: failed to remove ghost pad %s\n", name);
//            }
//            // 3) unref
//            gst_object_unref(ghost_pad);
//        }
//        else {
//            g_printerr("Shutdown: ghost pad '%s' not found\n", name);
//        }
//    }
//
//    // 5) Bin 상태 정리 및 unref
//    gst_element_set_state(consumerBin_, GST_STATE_NULL);
//    gst_object_unref(consumerBin_);
//    consumerBin_ = nullptr;
//
//    // (선택) 디버그 설정 원상복귀
//    // gst_debug_set_default_threshold(GST_LEVEL_DEBUG);
//    gst_debug_set_active(FALSE);
//}
void ConsumerBin::Shutdown() {
    if (!consumerBin_)
        return;

    // 🔍 디버그를 위해 refcount 로그 켜기
    //gst_debug_set_threshold_for_name("GST_REFCOUNTING", GST_LEVEL_MEMDUMP);

    // 1) 완전 정지: PLAYING/PAUSED → NULL
    gst_element_set_state(consumerBin_, GST_STATE_NULL);
    gst_element_get_state(consumerBin_, nullptr, nullptr, GST_CLOCK_TIME_NONE);

    // 2) FPS probe 해제 및 fps_data 삭제
    if (gpointer probePtr = g_object_get_data(G_OBJECT(consumerBin_), "fps_probe_id")) {
        guint probeId = GPOINTER_TO_UINT(probePtr);
        if (GstElement* overlay = gst_bin_get_by_name(GST_BIN(consumerBin_), "overlay")) {
            if (GstPad* p = gst_element_get_static_pad(overlay, "src")) {
                gst_pad_remove_probe(p, probeId);
                gst_object_unref(p);
            }
            gst_object_unref(overlay);
        }
        g_object_set_data(G_OBJECT(consumerBin_), "fps_probe_id", nullptr);
    }
    if (gpointer fpsData = g_object_get_data(G_OBJECT(consumerBin_), "fps_data")) {
        delete static_cast<FpsData*>(fpsData);
        g_object_set_data(G_OBJECT(consumerBin_), "fps_data", nullptr);
    }

    // 3) request-pad 반납 (input_selector)
    if (GstElement* sel = gst_bin_get_by_name(GST_BIN(consumerBin_), "input_selector")) {
        const char* keys[] = {
            "request_pad_rtsp_h264",
            "request_pad_file_h264",
            "request_pad_file_image",
            "request_pad_video_test",
        };
        for (auto key : keys) {
            if (GstPad* req = static_cast<GstPad*>(g_object_get_data(G_OBJECT(consumerBin_), key))) {
                gst_element_release_request_pad(sel, req);
                gst_object_unref(req);
                g_object_set_data(G_OBJECT(consumerBin_), key, nullptr);
            }
        }
        gst_object_unref(sel);
    }

    // 4) ghost-pad 완전 제거 (consumerBin_ 내부)
    {
        GstIterator* it = gst_element_iterate_pads(GST_ELEMENT(consumerBin_));
        GValue val = G_VALUE_INIT;
        while (gst_iterator_next(it, &val) == GST_ITERATOR_OK) {
            GstPad* pad = GST_PAD(g_value_get_object(&val));
            if (GST_IS_GHOST_PAD(pad)) {
                // unlink 되어 있을 수도 있는 peer 해제
                if (GstPad* peer = gst_pad_get_peer(pad)) {
                    gst_pad_unlink(peer, pad);
                    gst_object_unref(peer);
                }
                // bin에서 ghost-pad 제거 + unref
                gst_element_remove_pad(consumerBin_, pad);
                gst_object_unref(pad);
            }
            g_value_reset(&val);
        }
        gst_iterator_free(it);
    }

    // 5) VideoOverlay 이벤트/handle 해제
    if (GstElement* sink = gst_bin_get_by_name(GST_BIN(consumerBin_), "d3d11videosink")) {
        if (GST_IS_VIDEO_OVERLAY(sink)) {
            GstVideoOverlay* vo = GST_VIDEO_OVERLAY(sink);
            gst_video_overlay_handle_events(vo, FALSE);
            gst_video_overlay_set_window_handle(vo, 0);
        }
        gst_object_unref(sink);
    }

    // 6) 남은 자식 요소들(remove + unref)
    {
        GstIterator* it = gst_bin_iterate_elements(GST_BIN(consumerBin_));
        GValue val = G_VALUE_INIT;
        while (gst_iterator_next(it, &val) == GST_ITERATOR_OK) {
            GstElement* child = GST_ELEMENT(g_value_get_object(&val));
            gst_bin_remove(GST_BIN(consumerBin_), child);
            gst_object_unref(child);
            g_value_reset(&val);
        }
        gst_iterator_free(it);
    }

    // 7) (안전 차원) parent에서 완전 분리
    if (GstObject* parent = gst_object_get_parent(GST_OBJECT(consumerBin_))) {
        if (GST_IS_BIN(parent))
            gst_bin_remove(GST_BIN(parent), consumerBin_);
        gst_object_unref(parent);
    }

    // 8) 마지막으로 bin 자신 unref
    gst_object_unref(consumerBin_);
    consumerBin_ = nullptr;
}


bool ConsumerBin::SetWindowHandleOnSink() {
    if (!consumerBin_ || windowHandle == NULL) {
        g_printerr("Consumer bin or window handle is invalid.\n");
        return false;
    }
    GstElement* sinkElem = gst_bin_get_by_name(GST_BIN(consumerBin_), "d3d11videosink");
    if (sinkElem && GST_IS_VIDEO_OVERLAY(sinkElem)) {
        GstVideoOverlay* overlaySink = GST_VIDEO_OVERLAY(sinkElem);
        gst_video_overlay_set_window_handle(overlaySink, reinterpret_cast<guintptr>(windowHandle));
        SetWindowPos(windowHandle, HWND_BOTTOM, 0, 0, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);
        gst_object_unref(sinkElem);
        return true;
    }
    else {
        g_printerr("No video overlay element found in consumer bin.\n");
        if (sinkElem)
            gst_object_unref(sinkElem);
        return false;
    }
}

void ConsumerBin::CheckBin(int idx) {
    if (consumerBin_) {
        GstState currentState, pendingState;
        GstStateChangeReturn ret = gst_element_get_state(consumerBin_, &currentState, &pendingState, 3000);
        g_printerr("[INFO] Player %d : ConsumerBin state: %s (pending: %s, ret: %d)\n",
            idx,
            gst_element_state_get_name(currentState),
            gst_element_state_get_name(pendingState),
            ret);
        GstObject* parentObj = gst_element_get_parent(consumerBin_);
        GstElement* parent = GST_ELEMENT(parentObj);
        if (parent) {
            GstState parState, parPending;
            GstStateChangeReturn parRet = gst_element_get_state(parent, &parState, &parPending, 3000);
            g_printerr("[INFO] Parent state is %s, transitioning to PLAYING...\n",
                gst_element_state_get_name(parState));
            gst_object_unref(parentObj);
        }
        else return;
        GstIterator* it = gst_bin_iterate_elements(GST_BIN(parent));
        GValue item = G_VALUE_INIT;
        gboolean done = FALSE;
        while (!done) {
            switch (gst_iterator_next(it, &item)) {
            case GST_ITERATOR_OK: {
                GstElement* element = GST_ELEMENT(g_value_get_object(&item));
                const gchar* name = gst_element_get_name(element);
                GstState elState, elPendingState;
                GstStateChangeReturn elRet = gst_element_get_state(element, &elState, &elPendingState, 3000);
                g_printerr("[INFO] Player %d : Element '%s' state: %s (pending: %s, ret: %d)\n",
                    idx,
                    name,
                    gst_element_state_get_name(elState),
                    gst_element_state_get_name(elPendingState),
                    elRet);
                g_value_reset(&item);
                break;
            }
            case GST_ITERATOR_DONE:
                done = TRUE;
                break;
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(it);
                break;
            default:
                done = TRUE;
                break;
            }
        }
        gst_iterator_free(it);
        GstIterator* cit = gst_bin_iterate_elements(GST_BIN(consumerBin_));
        GValue citem = G_VALUE_INIT;
        gboolean cdone = FALSE;
        while (!cdone) {
            switch (gst_iterator_next(cit, &citem)) {
            case GST_ITERATOR_OK: {
                GstElement* element = GST_ELEMENT(g_value_get_object(&citem));
                const gchar* name = gst_element_get_name(element);
                GstState elState, elPendingState;
                GstStateChangeReturn elRet = gst_element_get_state(element, &elState, &elPendingState, 3000);
                g_printerr("[INFO] Player %d : Element '%s' state: %s (pending: %s, ret: %d)\n",
                    idx,
                    name,
                    gst_element_state_get_name(elState),
                    gst_element_state_get_name(elPendingState),
                    elRet);
                g_value_reset(&citem);
                break;
            }
            case GST_ITERATOR_DONE:
                cdone = TRUE;
                break;
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(cit);
                break;
            default:
                cdone = TRUE;
                break;
            }
        }
        gst_iterator_free(cit);
    }
}
void ConsumerBin::MoveZorder(bool isTop) {
    if (isTop) {
        SetWindowPos(windowHandle, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE);
    }
    else {
        SetWindowPos(windowHandle, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE);
    }
}
void ConsumerBin::SetWindow(int left, int top, int width, int height) {
    // 1) 윈도우 위치/크기 변경
    SetWindowPos(windowHandle,
        HWND_TOP,
        left, top, width, height,
        SWP_NOZORDER);
    // 2) d3d11videosink 요소 가져오기
    //GstElement* sink = gst_bin_get_by_name(GST_BIN(consumerBin_), "d3d11videosink");
    //if (!sink) return;
    //if (width <= 1 || height <= 1) {
    //    gst_object_unref(sink);
    //    return;
    //}

    //// 3) 윈도우 핸들 다시 설정 → 내부 스왑체인 타겟 재바인딩
    //gst_video_overlay_set_window_handle(
    //    GST_VIDEO_OVERLAY(sink),
    //    reinterpret_cast<guintptr>(windowHandle)
    //);

    //// 4) 재설정 이벤트 전송 → 싱크가 새 창 크기에 맞춰 스왑체인 재생성
    //GstEvent* ev = gst_event_new_reconfigure();
    //gst_element_send_event(sink, ev);

    //gst_object_unref(sink);
}

void ConsumerBin::ShowWin(bool isShowing) {
    if (isShowing) {
        GstElement* sink = gst_bin_get_by_name(GST_BIN(consumerBin_), "d3d11videosink");
        if (!sink) return;


        //// 3) 윈도우 핸들 다시 설정 → 내부 스왑체인 타겟 재바인딩
        //gst_video_overlay_set_window_handle(
        //    GST_VIDEO_OVERLAY(sink),
        //    reinterpret_cast<guintptr>(windowHandle)
        //);

        //// 4) 재설정 이벤트 전송 → 싱크가 새 창 크기에 맞춰 스왑체인 재생성
        //GstEvent* ev = gst_event_new_reconfigure();
        //gst_element_send_event(sink, ev);
        //
        
        gst_object_unref(sink);
        ShowWindow(windowHandle, SW_SHOW);
        // 2) d3d11videosink 요소 가져오기
       
    }
    else
    {
        ShowWindow(windowHandle, SW_HIDE);
        
    }
    
}
void ConsumerBin::ToggleFpsDisplay() {
    g_showFps.store(!g_showFps.load());
    g_printerr("FPS display toggled %s\n", g_showFps.load() ? "ON" : "OFF");
}
