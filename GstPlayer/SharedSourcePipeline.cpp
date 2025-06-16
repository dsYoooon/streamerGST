
#include "SharedSourcePipeline.h"
#include <gst/gst.h>
//#include <atomic>
#include <mutex>
#include <thread>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <gst/app/gstappsrc.h>
#include <gst/video/videooverlay.h>
#include <Windows.h>


// 전역 GLib 메인 루프 관련
static std::thread glibMainLoopThread;
static std::atomic<bool> glibMainLoopStarted{ false };
static std::once_flag glibMainLoopFlag;
static std::atomic<int> idxCounter{ 0 };
static GMainContext* global_context;
static GMainLoop* global_loop;
static HWND CreatePlaybackWindow(int left, int top, int width, int height) {
    const wchar_t CLASS_NAME[] = L"GStreamer Playback Window";
    static bool registered = false;
    if (!registered) {
        WNDCLASS wc = {};
        wc.lpfnWndProc = DefWindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = CLASS_NAME;
        RegisterClass(&wc);
        registered = true;
    }
    return CreateWindowEx(
        WS_EX_TOOLWINDOW,
        CLASS_NAME,
        L"",
        WS_POPUP | WS_VISIBLE,
        left, top, width, height,
        NULL, NULL,
        GetModuleHandle(NULL),
        NULL
    );
}
struct StateChange {
    GstElement* pipeline;
    GstState     target_state;
};

static gboolean _idle_change_state(gpointer user_data) {
    auto* d = static_cast<StateChange*>(user_data);
    gst_element_set_state(d->pipeline, d->target_state);
    delete d;
    return G_SOURCE_REMOVE;
}
void schedule_set_state(GstElement* pipe, GstState state) {
    auto* data = new StateChange{ pipe, state };
    // global_context 위에서 실행돼야 하므로 g_main_context_invoke 사용
    g_main_context_invoke(global_context, _idle_change_state, data);
}
bool Wait_for_state(GstElement* element, GstState desiredState, guint timeout_ms) {
    GstState current, pending;
    GstClockTime timeout = timeout_ms * GST_MSECOND;
    GstStateChangeReturn ret = gst_element_get_state(element, &current, &pending, timeout);
    return (current == desiredState);
}
GstElement* SetDeco(int localIndex, const char* name) {
    GstElement* dec = nullptr;
    
    /*switch (localIndex % 4) {
    case 3: dec = gst_element_factory_make("nvh264device1dec", name);
        break;
    case 0: dec = gst_element_factory_make("d3d11h264dec", name);
        break;
    case 1: dec = gst_element_factory_make("d3d11h264dec", name);
        break;
    case 2: dec = gst_element_factory_make("nvh264device1dec", name);
        break;
    default: dec = gst_element_factory_make("nvh264dec", name);
        break;
    }
    if (!dec) {
        dec = gst_element_factory_make("d3d11h264dec", name);
    }*/
    //dec = gst_element_factory_make("nvh264dec", name);
    dec = gst_element_factory_make("nvh264device1dec", name);
    //dec = gst_element_factory_make("d3d11h264dec", name);
    return dec;
}
static void set_queue_limits(GstElement* q) {
    g_object_set(q,
        "max-size-buffers", 30,
        "max-size-bytes", 0,
        "max-size-time", 0,
        "leaky", 2,
        NULL);
}
SharedSourcePipeline::SharedSourcePipeline(const std::string& rtspUrl)
    : rtspUrl_(rtspUrl),
    pipeline_(nullptr),
    rtspsrc_(nullptr),
    depay_(nullptr),
    parse_(nullptr),
    tee_(nullptr),
    qcapDevice_(nullptr)
{
    static bool gstInitialized = false;
    if (!gstInitialized) {
        
        //g_slice_set_config(G_SLICE_CONFIG_ALWAYS_MALLOC, TRUE);
        gst_init(nullptr, nullptr);
        gst_debug_set_threshold_for_name("QOS", GST_LEVEL_MEMDUMP);
//        gst_debug_set_default_threshold(GST_LEVEL_INFO);
        GstRegistry* reg = gst_registry_get();
        gst_registry_scan_path(
            reg,
            "C:\\gstreamer\\1.0\\msvc_x86_64\\lib\\gstreamer-1.0"
        );

        gstInitialized = true;
    }
}
static void on_decode_pad_added(GstElement* dec, GstPad* pad, gpointer user_data)
{
    GstElement* conv = GST_ELEMENT(user_data);
    GstPad* sink = gst_element_get_static_pad(conv, "sink");
    if (sink && !GST_PAD_IS_LINKED(sink))
        gst_pad_link(pad, sink);
    if (sink) gst_object_unref(sink);
}
//-----------------------------------------------------------------------------
// ensure_dynamic_audio_chain
//    Builds: queue → [rtpjitterbuffer] → valve → decodebin → convert → resample → volume → autoaudiosink
static void ensure_dynamic_audio_chain(
    GstPipeline* pipeline,
    GstPad* pad,
    const char* id,
    bool use_jitter
) {
    //return;
    // 1) 중복 생성 방지
    std::string flagKey = "audio-created-";
    flagKey += id;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pipeline), flagKey.c_str())))
        return;

    // 2) 네이밍 헬퍼
    auto name = [&](const char* elem) {
        return std::string(elem) + "_" + id;
        };

    // 3) 요소 생성
    GstElement* queue = gst_element_factory_make("queue", name("queue").c_str());
    GstElement* jitter = use_jitter
        ? gst_element_factory_make("rtpjitterbuffer", name("jitter").c_str())
        : nullptr;
    GstElement* valve = gst_element_factory_make("valve", "valve");
    GstElement* decode = gst_element_factory_make("decodebin", name("decode").c_str());
    GstElement* convert = gst_element_factory_make("audioconvert", name("convert").c_str());
    GstElement* resample = gst_element_factory_make("audioresample", name("resample").c_str());
    GstElement* volume = gst_element_factory_make("volume", "volume");
    GstElement* sink = gst_element_factory_make("autoaudiosink", name("sink").c_str());

    if (!queue || !valve || !decode || !convert || !resample || !volume || !sink
        || (use_jitter && !jitter)) {
        g_printerr("ensure_dynamic_audio_chain: element creation failed for id='%s'\n", id);
        return;
    }
    //g_print("\n%s\n", flagKey.find("file"));
    
    /*if (flagKey.find("file") != std::string::npos) {
        g_object_set(queue,
            "max-size-buffers", 0,
            "max-size-bytes", 0,
            "max-size-time", 2000 * GST_MSECOND,
            "leaky", 2,
            NULL);
    }
    else {
        set_queue_limits(queue);
    }*/
    set_queue_limits(queue);
  
    //set_queue_limits(queue);
    if (use_jitter) {
        gst_bin_add_many(GST_BIN(pipeline),
            queue,
            jitter,
            valve,
            decode, convert, resample, volume,
            sink,
            NULL
        );
    }
    else
    {
        gst_bin_add_many(GST_BIN(pipeline),
            queue,
            valve,
            decode, convert, resample, volume,
            sink,
            NULL
        );
    }
    // 4) 파이프라인에 추가

    if (jitter)
        g_object_set(jitter, "latency", 50, NULL);

    // 5) 부모 상태와 동기화
    for (auto e : { queue, jitter, valve, decode, convert, resample, volume, sink })
        if (e) gst_element_sync_state_with_parent(e);

    // 6) 링크 구성
    // pad → queue
    {
        GstPad* qsink = gst_element_get_static_pad(queue, "sink");
        gst_pad_link(pad, qsink);
        gst_object_unref(qsink);
    }
    // queue → [jitter] → valve
    if (jitter)
        gst_element_link_many(queue, jitter, valve, NULL);
    else
        gst_element_link(queue, valve);

    // valve → decodebin
    gst_element_link(valve, decode);

    // decodebin → convert → resample → volume → sink
    g_signal_connect(decode, "pad-added",
        G_CALLBACK(on_decode_pad_added), convert);
    gst_element_link_many(convert, resample, volume, sink, NULL);

    // 7) 초기 상태: mute(=drop) 시켜 놓기
    g_object_set(valve, "drop", false, NULL);

    // 8) 생성 플래그 기록
    g_object_set_data(G_OBJECT(pipeline),
        flagKey.c_str(),
        GINT_TO_POINTER(TRUE));

    g_print("Audio chain [%s] created (valve-based)\n", id);
}



//------------------------------------------------------------------------------
// QCAP 비디오 콜백
QRETURN WINAPI on_video_preview_callback(
    PVOID, double dSampleTime, BYTE* pFrame, ULONG len, PVOID user_data)
{
    auto data = static_cast<CaptureData*>(user_data);
    if (!data || !pFrame || len == 0) return QCAP_RT_OK;
    // 인스턴스별 lastSampleTime 사용
    //double deltaMs = (dSampleTime - data->lastSampleTime) * 1000.0;
    //data->lastSampleTime = dSampleTime;
    
    // ① 전체 시작
    //gint64 t0 = g_get_monotonic_time();

    // ② 버퍼풀에서 버퍼 획득
    //gint64 t1;
    GstBuffer* buf = nullptr;
    if (gst_buffer_pool_acquire_buffer(data->videoPool, &buf, NULL) != GST_FLOW_OK)
        return QCAP_RT_FAIL;
    //t1 = g_get_monotonic_time();

    // ③ memcpy
    GstMapInfo info;
    gst_buffer_map(buf, &info, GST_MAP_WRITE);
    memcpy(info.data, pFrame, std::min<ULONG>(len, info.size));
    gst_buffer_unmap(buf, &info);
    //gint64 t2 = g_get_monotonic_time();

    // ④ push_buffer
    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(dSampleTime * GST_SECOND, 1, 1);
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, 30);
    GstFlowReturn ret = gst_app_src_push_buffer(data->videoSrc, buf);
    //gint64 t3 = g_get_monotonic_time();

    //g_print("[Timing] pool_acquire=%lldµs, memcpy=%lldµs, push=%lldµs, total=%lldµs, interval: %.2f ms\n",    t1 - t0, t2 - t1, t3 - t2, t3 - t0, deltaMs);
    

    if (ret != GST_FLOW_OK)
        g_printerr("[AppSrc] push failed: %s\n", gst_flow_get_name(ret));

    return QCAP_RT_OK;
}


//------------------------------------------------------------------------------
// QCAP 오디오 콜백
QRETURN WINAPI on_audio_preview_callback(
    PVOID, double dSampleTime, BYTE* pAudio, ULONG len, PVOID user_data)
{
    auto data = static_cast<CaptureData*>(user_data);
    if (!data || !pAudio || len == 0) return QCAP_RT_OK;

    GstBuffer* buf = nullptr;
    if (gst_buffer_pool_acquire_buffer(data->audioPool, &buf, NULL) != GST_FLOW_OK)
        return QCAP_RT_FAIL;

    GstMapInfo info;
    gst_buffer_map(buf, &info, GST_MAP_WRITE);
    memcpy(info.data, pAudio, std::min<ULONG>(len, info.size));
    gst_buffer_unmap(buf, &info);

    gst_buffer_resize(buf, 0, len);
    guint64 pts = gst_util_uint64_scale(dSampleTime * GST_SECOND, 1, 1);
    guint64 dur = gst_util_uint64_scale(len / 4, GST_SECOND, 48000);
    GST_BUFFER_PTS(buf) = pts;
    GST_BUFFER_DURATION(buf) = dur;

    if (gst_app_src_push_buffer(data->audioSrc, buf) != GST_FLOW_OK) {
        g_printerr("audio push-buffer failed\n");

    }
    return QCAP_RT_OK;
}

static void on_pad_added_rtsp(GstElement* src, GstPad* new_pad, gpointer data) {
    GstPipeline* pipe = GST_PIPELINE(data);
    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
    if (!caps) return;
    GstStructure* s = gst_caps_get_structure(caps, 0);
    const gchar* name = gst_structure_get_name(s);
    GstElement* target = nullptr;

    if (g_str_has_prefix(name, "application/x-rtp")) {
        const GValue* mv = gst_structure_get_value(s, "media");
        if (mv && g_strcmp0(g_value_get_string(mv), "video") == 0)
            target = gst_bin_get_by_name(GST_BIN(pipe), "video_q_rtsp");
        else
            ensure_dynamic_audio_chain(pipe, new_pad, "rtsp", true);
    }
    else if (g_str_has_prefix(name, "video/"))
        target = gst_bin_get_by_name(GST_BIN(pipe), "video_q_rtsp");
    else if (g_str_has_prefix(name, "audio/"))
        ensure_dynamic_audio_chain(pipe, new_pad, "rtsp", true);


    if (target) {
        GstPad* sink = gst_element_get_static_pad(target, "sink");
        if (sink && !gst_pad_is_linked(sink))
            gst_pad_link(new_pad, sink);
        gst_object_unref(sink);
        gst_object_unref(target);
    }
    gst_caps_unref(caps);
}
static void on_pad_added_video_file(GstElement* src, GstPad* pad, gpointer user_data) {
    GstElement* pipeline = static_cast<GstElement*>(user_data);

    GstElement* qv = gst_bin_get_by_name(GST_BIN(pipeline), "video_queue_file");
    GstElement* parse = gst_bin_get_by_name(GST_BIN(pipeline), "h264_parse");

    if (!qv || !parse) {
        g_printerr("Failed to find elements for pad linking.\n");
        return;
    }

    GstPad* sinkpad = gst_element_get_static_pad(qv, "sink");
    if (gst_pad_is_linked(sinkpad)) {
        g_print("Pad already linked, skipping.\n");
        gst_object_unref(sinkpad);
        gst_object_unref(qv);
        gst_object_unref(parse);
        return;
    }

    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link qtdemux pad to queue.\n");
    }
    else {
        g_print("Linked qtdemux pad to video queue.\n");

        // queue → parse → dec → conv
        if (!gst_element_link_many(qv, parse, NULL)) {
            g_printerr("Failed to link queue → parse.\n");
        }
        else {
            GstElement* dec = gst_bin_get_by_name(GST_BIN(pipeline), "h264_decode");
            GstElement* conv = gst_bin_get_by_name(GST_BIN(pipeline), "convert");

            if (!gst_element_link_many(parse, dec, conv, NULL)) {
                g_printerr("Failed to link parse → dec → convert.\n");
            }
            else {
                g_print("Linked video decode branch successfully.\n");
            }

            gst_object_unref(dec);
            gst_object_unref(conv);
        }
    }

    gst_object_unref(sinkpad);
    gst_object_unref(qv);
    gst_object_unref(parse);
}

SharedSourcePipeline::~SharedSourcePipeline() {

    Shutdown();
}
// 버스 콜백 함수: EOS 메시지를 감지하여 루프 재생을 위한 seek 호출
static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer data) {
    GstElement* pipeline = GST_ELEMENT(data);
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
    {


        g_print("EOS detected, looping video playback...\n");
        GstState currentState, pendingState;
        GstStateChangeReturn ret = gst_element_get_state(pipeline, &currentState, &pendingState, 1000);
        if (currentState != GST_STATE_PLAYING) {

            g_printerr("[INFO] Pipeline state: %s (pending: %s, ret: %d)\n",

                gst_element_state_get_name(currentState),
                gst_element_state_get_name(pendingState),
                ret);
        }
        //gst_debug_set_default_threshold(GST_LEVEL_INFO);
        gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
            (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
        gst_debug_set_default_threshold(GST_LEVEL_ERROR);
     
        break;
    }
    default:
        break;
    }
    return TRUE;
}
 //GLib 메인 루프를 별도 스레드에서 실행 (RTSP 파이프라인 전용)
void StartGLibMainLoop() {
    std::call_once(glibMainLoopFlag, []() {
        g_print("    startglmainloop      %d", idxCounter.load());
    /*if (!glibMainLoopStarted.load()) {
        glibMainLoopStarted = true;*/
        global_context = g_main_context_new();
        global_loop = g_main_loop_new(global_context, FALSE);
        //GMainLoop* global_loop = g_main_loop_new(global_context, FALSE);
        glibMainLoopThread = std::thread([]() {
            
            g_main_context_push_thread_default(global_context);
            g_main_loop_run(global_loop);
            g_main_loop_unref(global_loop);
            g_main_context_unref(global_context);
            });
        });
}

// rtspsrc의 동적 pad가 생성되면 depay의 sink pad에 연결하는 콜백 (RTSP branch 전용)
void SharedSourcePipeline::on_pad_added(GstElement* src, GstPad* new_pad, gpointer user_data) {
    GstElement* depay = static_cast<GstElement*>(user_data);
    GstPad* sink_pad = gst_element_get_static_pad(depay, "sink");
    if (sink_pad) {
        GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
        if (ret != GST_PAD_LINK_OK) {
            g_printerr("Failed to link rtspsrc pad to depay sink pad.\n");
        }
        gst_object_unref(sink_pad);
    }
}

bool SharedSourcePipeline::Init() {
    g_printerr("%s\n", rtspUrl_.c_str());
    pipeline_ = gst_pipeline_new(("pipeline-" + rtspUrl_).c_str());
    if (!pipeline_) {
        g_printerr("Failed to create shared source pipeline.\n");
        return false;
    }

    bool isFake = (rtspUrl_.find("fakesrc://") == 0);
    bool isImage = (rtspUrl_.find("image://") == 0);
    bool isVideo = (rtspUrl_.find("video://") == 0);
    bool isCapture = (rtspUrl_.find("capture://") == 0);
    bool success = false;
    // branchOut will hold the last element of the branch that must be linked to tee.
    GstElement* branchOut = nullptr;

    if (isFake) {
        success = CreateFakesrcBranch(&branchOut);
    }
    else if (isImage) {
        success = CreateImageBranch(&branchOut);
    }
    else if (isVideo) {
        success = CreateVideoBranch(&branchOut);
    }
    else if (isCapture) {

        // ① SDK DLL 로드
        hQ = LoadLibraryA("QCAP.X64.dll");
        if (!hQ) {
            g_printerr("Failed to load QCAP SDK DLL\n");
            return false;

        }
        // ② 함수 주소 가져오기
        fQ_CREATE = (PFN_QCAP_CREATE)GetProcAddress(hQ, "QCAP_CREATE");
        fQ_VID = (PFN_QCAP_REGISTER_VIDEO_PREVIEW_CALLBACK)GetProcAddress(hQ, "QCAP_REGISTER_VIDEO_PREVIEW_CALLBACK");
        fQ_AUD = (PFN_QCAP_REGISTER_AUDIO_PREVIEW_CALLBACK)GetProcAddress(hQ, "QCAP_REGISTER_AUDIO_PREVIEW_CALLBACK");
        fQ_RUN = (PFN_QCAP_RUN)GetProcAddress(hQ, "QCAP_RUN");
        fQ_STOP = (PFN_QCAP_STOP)GetProcAddress(hQ, "QCAP_STOP");
        fQ_DESTROY = (PFN_QCAP_DESTROY)GetProcAddress(hQ, "QCAP_DESTROY");
        if (!fQ_CREATE || !fQ_VID || !fQ_AUD || !fQ_RUN || !fQ_STOP || !fQ_DESTROY) {
            g_printerr("QCAP SDK symbols missing\n");
            return false;

        }
        //success = CreateCaptureBranch(&branchOut);
        success = CreateCaptureBranch(&branchOut, false);
    }
    else {
        success = CreateRTSPBranch(&branchOut);
    }
    if (!success || branchOut == nullptr)
        return false;

    // 공통 티와 fakesink 생성 및 파이프라인에 추가
    if (isFake || isCapture) {
        tee_ = gst_element_factory_make("tee", "tee_video_test");
    }
    else if (isImage) {
        tee_ = gst_element_factory_make("tee", "tee_file_image");
    }
    else if (isVideo) { // 동영상 파일 및 RTSP
        tee_ = gst_element_factory_make("tee", "tee_file_h264");
    }
    else { // 동영상 파일 및 RTSP
        tee_ = gst_element_factory_make("tee", "tee_rtsp_h264");
    }
    if (!tee_) {
        g_printerr("Failed to create tee element.\n");
        return false;
    }

    GstElement* fakesink = gst_element_factory_make("fakesink", "fakesink");
    if (!fakesink) {
        g_printerr("Failed to create fakesink.\n");
        return false;
    }

    g_object_set(fakesink, "async", false, NULL);
    // 티와 fakesink 연결 (request pad 방식)


    if (isVideo) {
        GstElement* que = gst_element_factory_make("queue", "sync_q");
        //set_queue_limits(que);
        //g_object_set(sync_identity, "sync", TRUE, NULL);
        gst_bin_add_many(GST_BIN(pipeline_), tee_, que, fakesink, NULL);
        g_object_set(fakesink, "sync", TRUE, NULL);
        // branch의 출력 요소(branchOut)를 common tee에 연결
        if (!gst_element_link_many(branchOut, tee_, que, fakesink, NULL)) {
            g_printerr("Failed to link branch output to tee.\n");
            return false;
        }
 
    }
    else if (isFake) {
        gst_bin_add_many(GST_BIN(pipeline_), tee_, fakesink, NULL);
        if (gst_element_link(branchOut, tee_)) {
            GstPad* tee_src = gst_element_request_pad_simple(tee_, "src_%u");
            GstPad* fsink_pad = gst_element_get_static_pad(fakesink, "sink");
            if (gst_pad_link(tee_src, fsink_pad) != GST_PAD_LINK_OK)
                g_printerr("Failed to link tee to fakesink in videotest pipeline\n");
            gst_object_unref(tee_src);
            gst_object_unref(fsink_pad);
        }

    }
    else {
        gst_bin_add_many(GST_BIN(pipeline_), tee_, fakesink, NULL);
        if (!gst_element_link_many(branchOut, tee_, fakesink, NULL)) {
            g_printerr("Failed to link branch output to tee.\n");
            return false;
        }
 
        //mainCont = g_main_context_new();
        //mainLoop_ = g_main_loop_new(mainCont,FALSE);
        //mainLoopThread = std::thread([this]() {
        //            // 이 스레드에서만 유효하도록 컨텍스트 바인딩
        //        g_main_context_push_thread_default(mainCont);
        //    g_print("Pipeline %s main loop start\n", rtspUrl_.c_str());
        //    g_main_loop_run(mainLoop_);
        //    g_print("Pipeline %s main loop exit\n", rtspUrl_.c_str());
        //    g_main_context_pop_thread_default(mainCont);
        //            // 리소스 정리
        //        g_main_loop_unref(mainLoop_);
        //    g_main_context_unref(mainCont);
        //    });

        
    }
    //GstBus* bus = gst_element_get_bus(pipeline_);
    //gst_bus_add_watch(bus, bus_callback, pipeline_);
    //gst_object_unref(bus);
    StartGLibMainLoop();
    g_main_context_push_thread_default(global_context);
    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_add_watch(bus, bus_callback, pipeline_);
    gst_object_unref(bus);
    g_main_context_pop_thread_default(global_context);
    /*  SetPlay();
      if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
          g_printerr("Failed to set pipeline to PLAYING state.\n");
          return false;
      }*/
    schedule_set_state(pipeline_, GST_STATE_PLAYING);
    //gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    const guint pollInterval = 67;
    guint elapsed = 0;
    while (!Wait_for_state(pipeline_, GST_STATE_PLAYING, pollInterval) && elapsed < 10000) {
        elapsed += pollInterval;
    }
    g_printerr("wait during : %d\n", elapsed);
    //gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    return true;
}
bool SharedSourcePipeline::CreateFakesrcBranch(GstElement** branchOut) {
    // 구성: videotestsrc → videoconvert
    GstElement* source = gst_element_factory_make("videotestsrc", "video_source");
    if (!source) {
        g_printerr("Failed to create videotestsrc.\n");
        return false;
    }
    g_object_set(G_OBJECT(source), "pattern", 1, "background-color", 0, "is-live", TRUE, NULL);

    GstElement* convert = gst_element_factory_make("videoconvert", "video_convert");
    if (!convert) {
        g_printerr("Failed to create videoconvert.\n");
        return false;
    }


    GstElement* capsfilter = gst_element_factory_make("capsfilter", "fakesrc_caps");
    GstCaps* caps = gst_caps_from_string("video/x-raw, format=NV12");
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);


    gst_bin_add_many(GST_BIN(pipeline_), source, capsfilter, convert, NULL);
    if (!gst_element_link_many(source, capsfilter, convert, NULL)) {
        g_printerr("Failed to link videotestsrc -> videoconvert.\n");
        return false;
    }
    // branch output은 videoconvert
    *branchOut = convert;
    return true;
}
bool SharedSourcePipeline::CreateImageBranch(GstElement** branchOut) {
    std::string filePath = rtspUrl_.substr(std::string("image://").length());
    GstElement* source = gst_element_factory_make("filesrc", "image_source");
    if (!source) {
        g_printerr("Failed to create filesrc for image.\n");
        return false;
    }
    g_object_set(G_OBJECT(source), "location", filePath.c_str(), NULL);

    GstElement* decodebin = gst_element_factory_make("decodebin", "image_decodebin");
    if (!decodebin) {
        g_printerr("Failed to create decodebin for image.\n");
        return false;
    }
    GstElement* imagefreeze = gst_element_factory_make("imagefreeze", "image_freeze");
    if (!imagefreeze) {
        g_printerr("Failed to create imagefreeze for image.\n");
        return false;
    }
    gst_bin_add_many(GST_BIN(pipeline_), source, decodebin, imagefreeze, NULL);
    if (!gst_element_link(source, decodebin)) {
        g_printerr("Failed to link filesrc to decodebin for image.\n");
        return false;
    }
    // decodebin의 동적 패드를 imagefreeze에 연결
    g_signal_connect(decodebin, "pad-added", G_CALLBACK(+[](GstElement* src, GstPad* new_pad, gpointer data) {
        GstElement* imagefreeze = static_cast<GstElement*>(data);
        GstPad* sink_pad = gst_element_get_static_pad(imagefreeze, "sink");
        if (sink_pad) {
            gst_pad_link(new_pad, sink_pad);
            gst_object_unref(sink_pad);
        }
        }), imagefreeze);
    // branch output은 imagefreeze
    *branchOut = imagefreeze;
    return true;
}

bool SharedSourcePipeline::CreateCaptureBranch(GstElement** branchOut, bool useAudio) {
    // 1) 비디오 appsrc 생성 및 설정
    GstElement* vsrc = gst_element_factory_make("appsrc", "capture_vsrc");
    if (!vsrc) {
        g_printerr("Capture branch: Failed to create video appsrc.\n");
        return false;
    }
    const int VW = 1920, VH = 1080, VFPS = 60;
    GstCaps* vcaps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        "width", G_TYPE_INT, VW,
        "height", G_TYPE_INT, VH,
        "framerate", GST_TYPE_FRACTION, VFPS, 1,
        NULL);

    if (!captureData_)
        captureData_ = new CaptureData();
    captureData_->videoSrc = GST_APP_SRC(vsrc);

    // 비디오 버퍼풀 설정
    captureData_->videoPool = gst_buffer_pool_new();
    {
        GstCaps* pc = gst_caps_copy(vcaps);
        GstStructure* cfg = gst_buffer_pool_get_config(captureData_->videoPool);
        gst_buffer_pool_config_set_params(cfg, pc, VW * VH * 2, 1, 20);
        gst_buffer_pool_set_config(captureData_->videoPool, cfg);
        gst_buffer_pool_set_active(captureData_->videoPool, TRUE);
        gst_caps_unref(pc);
    }

    gst_app_src_set_caps(GST_APP_SRC(vsrc), vcaps);
    gst_caps_unref(vcaps);
    g_object_set(G_OBJECT(vsrc),
        "stream-type", GST_APP_STREAM_TYPE_STREAM,
        "format", GST_FORMAT_TIME,
        "do-timestamp", TRUE,
        //"is-live" , TRUE,
        NULL);

    // 2) 비디오 downstream: queue → videoconvert
    GstElement* vqueue = gst_element_factory_make("queue", "capture_vqueue");
    GstElement* vconvert = gst_element_factory_make("videoconvert", "capture_vconvert");
    //GstElement* vconvert = gst_element_factory_make("d3d11convert", "capture_vconvert");
    if (!vqueue || !vconvert) {
        g_printerr("Capture branch: Failed to create video downstream elements.\n");
        return false;
    }
    set_queue_limits(vqueue);
    

    gst_bin_add_many(GST_BIN(pipeline_), vsrc, 
        vqueue,
         vconvert,
        NULL);
    if (!gst_element_link_many(vsrc,
         vqueue, 
 vconvert, 
        NULL)) {
        g_printerr("Capture branch: Failed to link video chain.\n");
        return false;
    }

    // 비디오 요소 상태 동기화
    for (auto e : { vsrc ,vqueue,vconvert }) {
        gst_element_sync_state_with_parent(e);
    }

    // 3) QCAP 디바이스 생성 및 **비디오 콜백** 등록 (항상)
    //    URL에서 deviceName, channel 파싱
    std::string spec = rtspUrl_.substr(strlen("capture://"));
    std::string deviceName;
    int channel = 0;
    size_t commaPos = spec.find(',');
    if (commaPos != std::string::npos) {
        deviceName = spec.substr(0, commaPos);
        try { channel = std::stoi(spec.substr(commaPos + 1)); }
        catch (...) {
            g_printerr("Capture branch: Invalid channel in URL, defaulting to 0.\n");
            channel = 0;
        }
    }
    else {
        deviceName = spec;
    }
    g_printerr("Capture branch: Using device '%s' channel %d.\n",
        deviceName.c_str(), channel);
    
    // QCAP 생성
    if (fQ_CREATE((CHAR*)deviceName.c_str(), channel, NULL,
        &qcapDevice_, TRUE, FALSE)) {
        g_printerr("Capture branch: QCAP_CREATE failed.\n");
        return false;
    }

    // 비디오 프레임 콜백
    fQ_VID(qcapDevice_,
        on_video_preview_callback, captureData_);

    // QCAP 비디오 출력 포맷
    QCAP_SET_VIDEO_DEFAULT_OUTPUT_FORMAT(
        qcapDevice_,
        QCAP_COLORSPACE_TYPE_NV12,
        VW, VH, FALSE, double(VFPS)
    );

    // QCAP 실행
    

    // 4) 오디오 체인 + 콜백: useAudio == true 일 때만
    if (useAudio) {
        // 4-1) 오디오 appsrc 생성 및 caps/풀 설정
        GstCaps* acaps = gst_caps_from_string(
            "audio/x-raw,format=S16LE,channels=2,rate=48000,layout=interleaved");
        GstElement* asrc = gst_element_factory_make("appsrc", "capture_asrc");
        if (!asrc) {
            g_printerr("Capture branch: Failed to create audio appsrc.\n");
            return false;
        }
        captureData_->audioSrc = GST_APP_SRC(asrc);

        captureData_->audioPool = gst_buffer_pool_new();
        {
            GstCaps* pc = gst_caps_copy(acaps);
            GstStructure* cfg = gst_buffer_pool_get_config(captureData_->audioPool);
            gst_buffer_pool_config_set_params(cfg, pc, 48000 * 4 / 2, 1, 5);
            gst_buffer_pool_set_config(captureData_->audioPool, cfg);
            gst_buffer_pool_set_active(captureData_->audioPool, TRUE);
            gst_caps_unref(pc);
        }

        gst_app_src_set_caps(GST_APP_SRC(asrc), acaps);
        gst_caps_unref(acaps);
        g_object_set(G_OBJECT(asrc),
            "stream-type", GST_APP_STREAM_TYPE_STREAM,
            "format", GST_FORMAT_TIME,
            "do-timestamp", TRUE,
            NULL);

        // 4-2) 오디오 downstream: queue→convert→identity→volume→resample→sink
        GstElement* aque = gst_element_factory_make("queue", "capture_aqueue");
        GstElement* acon = gst_element_factory_make("audioconvert", "capture_aconvert");
        GstElement* aident = gst_element_factory_make("identity", "capture_aident");
        GstElement* vol = gst_element_factory_make("volume", "capture_avolume");
        GstElement* ares = gst_element_factory_make("audioresample", "capture_aresample");
        GstElement* asnk = gst_element_factory_make("autoaudiosink", "capture_asink");
        if (!aque || !acon || !aident || !vol || !ares || !asnk) {
            g_printerr("Capture branch: Failed to create audio downstream elements.\n");
            return false;
        }
        //set_queue_limits(aque);
        g_object_set(aque,
            /* 최대 버퍼 개수를 600으로 제한 */
            "max-size-buffers", 5,
            /* 최대 바이트 수를 57 600 000으로 제한 */
            "max-size-bytes", 0,
            /* 시간 기반 제한은 사용하지 않음 */
            "max-size-time", 0,
            /* 넘칠 때 맨 앞 버퍼부터 드랍 */
            "leaky", 2,
            NULL
        );

        gst_bin_add_many(GST_BIN(pipeline_),
            asrc, aque, acon, aident, vol, ares, asnk, NULL);
        if (!gst_element_link_many(asrc, aque, acon, aident, vol, ares, asnk, NULL)) {
            g_printerr("Capture branch: Failed to link audio chain.\n");
            return false;
        }

        for (auto e : { asrc, aque, acon, aident, vol, ares, asnk }) {
            gst_element_sync_state_with_parent(e);
        }

        // identity 핸드오프 시그널 활성화
        g_object_set(aident, "signal-handoffs", TRUE, NULL);

        // 4-3) 오디오 프레임 콜백
        fQ_AUD(qcapDevice_, on_audio_preview_callback, captureData_);
    }
    fQ_RUN(qcapDevice_);
    // 5) 브랜치 아웃풋(비디오 convert) 반환
    *branchOut = vconvert;
    return true;
}



bool SharedSourcePipeline::CreateVideoBranch(GstElement** branchOut) {
    std::string filePath = rtspUrl_.substr(std::string("video://").length());
    GstElement* source = gst_element_factory_make("filesrc", "video_source");
    if (!source) {
        g_printerr("Failed to create filesrc for video.\n");
        return false;
    }
    g_object_set(G_OBJECT(source), "location", filePath.c_str(), "is-live",true, NULL);

    GstElement* demux = gst_element_factory_make("qtdemux", "video_demux");
    if (!demux) {
        g_printerr("Failed to create qtdemux for video.\n");
        return false;
    }

    GstElement* qv_f = gst_element_factory_make("queue", "video_queue_file");
    GstElement* parse = gst_element_factory_make("h264parse", "h264_parse");
    GstElement* dec = gst_element_factory_make("nvh264dec", "h264_decode");
    GstElement* conv = gst_element_factory_make("videoconvert", "convert");
    g_object_set(qv_f,
        "max-size-buffers", 20,
        "max-size-bytes", 0,
        "max-size-time", 0,
        "leaky", 2,
        NULL);

    gst_bin_add_many(GST_BIN(pipeline_), source, demux, qv_f, parse, dec, conv, NULL);

    if (!gst_element_link(source, demux)) {
        g_printerr("Failed to link filesrc -> demux for video.\n");
        return false;
    }



    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added_video_file), pipeline_);

    // branch output은 identity (tee 연결은 여기서 나가야 함)
    *branchOut = conv;
    return true;
}

bool SharedSourcePipeline::CreateRTSPBranch(GstElement** branchOut) {
    rtspsrc_ = gst_element_factory_make("rtspsrc", "rtsp_source");
    GstElement* qv = gst_element_factory_make("queue", "video_q_rtsp");
    GstElement* depay = gst_element_factory_make("rtph264depay", "h264_depay");
    GstElement* parse = gst_element_factory_make("h264parse", "h264_parse");
    GstElement* qv1 = gst_element_factory_make("queue", "video_q_rtsp1");
    sourceIdx = idxCounter.load();
    GstElement* dec = SetDeco(idxCounter.fetch_add(1), "dec");
    
        //gst_element_factory_make("nvh264dec", "h264_decode");
    selector_ = gst_element_factory_make("output-selector", "selector_rtsp_h264");
    GstElement* que = gst_element_factory_make("queue", "fakeque");
    GstElement* qued = gst_element_factory_make("queue", "decque");
    GstElement* preFake = gst_element_factory_make("fakesink", "pre_fakesink");
    GstElement* conv = gst_element_factory_make("videoconvert", "convert");
    GstElement* rate = gst_element_factory_make("videorate", "rate");
    GstElement* caps = gst_element_factory_make("capsfilter", "caps");
    if (!rtspsrc_ || !depay || !parse || !qv || !qv1 || !dec || !conv) {
        g_printerr("Failed to create one or more RTSP branch elements.\n");
        return false;
    }
    GstCaps* fps_caps = gst_caps_from_string("video/x-raw,framerate=60/1");
    g_object_set(caps, "caps", fps_caps, NULL);
    g_object_set(parse, "config-interval", 1, NULL);
    gst_caps_unref(fps_caps);
    set_queue_limits(qv);
    set_queue_limits(qv1);
    set_queue_limits(que);
    set_queue_limits(qued);
    gst_bin_add_many(GST_BIN(pipeline_), rtspsrc_, qv, depay, parse, qv1, selector_, que,qued,  dec, rate, caps, conv, preFake, NULL);
    g_object_set(G_OBJECT(rtspsrc_), "location", rtspUrl_.c_str(), "latency", 34,
        //"drop-on-latency", TRUE,
        NULL);
    //g_object_set(G_OBJECT(dec), "qos", false, NULL);
    if (!gst_element_link_many(qv, depay, parse, selector_, NULL)) {
        g_printerr("Failed to link RTSP branch: depay -> parse.\n");
        return false;
    }
    //gst_element_link(que, preFake);
    if (!gst_element_link_many(qued, dec, 
        //rate, caps, 
        NULL)) {
        g_printerr("Failed to link decode chain.\n");
        return false;
    }
    selectorDecodePad_ = gst_element_request_pad_simple(selector_, "src_%u");
    selectorFakePad_ = gst_element_request_pad_simple(selector_, "src_%u");
    GstPad* decSink = gst_element_get_static_pad(qued, "sink");
    GstPad* fakeSink = gst_element_get_static_pad(preFake, "sink");
    g_object_set(preFake, "async", false,  NULL);
    gst_pad_link(selectorDecodePad_, decSink);
    gst_pad_link(selectorFakePad_, fakeSink);
    gst_object_unref(decSink);
    gst_object_unref(fakeSink);
    g_object_set(selector_, "active-pad", selectorFakePad_, NULL);
    
    
    // rtspsrc의 동적 패드를 depay에 연결
    g_signal_connect(rtspsrc_, "pad-added", G_CALLBACK(on_pad_added_rtsp), pipeline_);
    // branch output은 parse
    *branchOut = dec;
    return true;
}


GstPad* SharedSourcePipeline::GetRequestPad() {
    if (!tee_) {
        g_printerr("Tee element is not available.\n");
        return nullptr;
    }
    GstPad* pad = gst_element_get_request_pad(tee_, "src_%u");
    if (!pad) {
        g_printerr("Failed to obtain request pad from tee.\n");
    }
    return pad;
}

void SharedSourcePipeline::SetReady() {
    gst_element_set_state(pipeline_, GST_STATE_READY);
}



void SharedSourcePipeline::SetPlay() {
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    gst_element_set_state(tee_, GST_STATE_PLAYING);
    const guint pollInterval = 67;
    guint elapsed = 0;
    while (!Wait_for_state(pipeline_, GST_STATE_PLAYING, pollInterval) && elapsed < 1000) {
        elapsed += pollInterval;
    }
    g_printerr("wait during : %d\n", elapsed);
}

void SharedSourcePipeline::CheckPipeline() {
    if (!pipeline_) {
        return;
    }
    bool needRestart = false;
    GstState currentState, pendingState;
    GstStateChangeReturn ret = gst_element_get_state(pipeline_, &currentState, &pendingState, 1000);
    if (currentState != GST_STATE_PLAYING) {
        needRestart = true;
        g_printerr("[INFO] Player %s : Pipeline state: %s (pending: %s, ret: %d)\n",
            rtspUrl_.c_str(),
            gst_element_state_get_name(currentState),
            gst_element_state_get_name(pendingState),
            ret);
    }
    GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipeline_));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            GstElement* element = GST_ELEMENT(g_value_get_object(&item));
            const gchar* name = gst_element_get_name(element);
            GstState elState, elPendingState;
            GstStateChangeReturn elRet = gst_element_get_state(element, &elState, &elPendingState, 1000);
            {
                needRestart = true;
                g_printerr("[INFO] Player %s : Element '%s' state: %s (pending: %s, ret: %d)\n",
                    rtspUrl_.c_str(),
                    name,
                    gst_element_state_get_name(elState),
                    gst_element_state_get_name(elPendingState),
                    elRet);
            }
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
}

void SharedSourcePipeline::Shutdown() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
        // 메인루프 스레드 종료 대기
        if (mainLoop_) {
       g_main_loop_quit(mainLoop_);
        if (mainLoopThread.joinable())
             mainLoopThread.join();
                // mainLoop_와 mainContext_는 루프 스레드에서 unref 처리됨
            mainLoop_ = nullptr;
        mainCont = nullptr;
        
    }
    if (qcapDevice_) {
        fQ_STOP(qcapDevice_);
        fQ_DESTROY(qcapDevice_);
        qcapDevice_ = nullptr;

    }
    if (captureData_) {
        if (captureData_->videoPool) {
            gst_buffer_pool_set_active(captureData_->videoPool, FALSE);
            gst_object_unref(captureData_->videoPool);
            captureData_->videoPool = nullptr;
        }
        if (captureData_->audioPool) {
            gst_buffer_pool_set_active(captureData_->audioPool, FALSE);
            gst_object_unref(captureData_->audioPool);
            captureData_->audioPool = nullptr;
        }
        captureData_->videoSrc = nullptr;
        captureData_->audioSrc = nullptr;
        delete captureData_;
        captureData_ = nullptr;
    }
    if (hQ) {
        FreeLibrary(hQ);
        hQ = nullptr;
    }
}
//-----------------------------------------------------------------------------
// SharedSourcePipeline::ToggleMute
//    Toggle the valve’s drop property for the given chain id.
void SharedSourcePipeline::ToggleMute() {
    // build name and lookup

    GstElement* valve = gst_bin_get_by_name(GST_BIN(pipeline_), "valve");
    if (!valve) {
        g_printerr("ToggleMute: valve  not found\n");
        return;
    }

    // flip state and apply
    isMute = !isMute;
    g_object_set(valve, "drop", isMute, NULL);
    gst_object_unref(valve);

    g_print("Audio chain is now %s\n",
        isMute ? "MUTED" : "UNMUTED");
}
void SharedSourcePipeline::SetVolume(double vol) {
    // build name and lookup

    GstElement* volume = gst_bin_get_by_name(GST_BIN(pipeline_), "volume");
    if (!volume) {
        g_printerr("ToggleMute: valve  not found\n");
        return;
    }

    // flip state and apply
//    isMute = !isMute;
    g_object_set(volume, "volume", vol, NULL);
    gst_object_unref(volume);


}

void SharedSourcePipeline::ActivateDecodePad() {
    if (selector_ && selectorDecodePad_)
        g_object_set(selector_, "active-pad", selectorDecodePad_, NULL);
}

void SharedSourcePipeline::ActivateFakePad() {
    if (selector_ && selectorFakePad_)
        g_object_set(selector_, "active-pad", selectorFakePad_, NULL);
}