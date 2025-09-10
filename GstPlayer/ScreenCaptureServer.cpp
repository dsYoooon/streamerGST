#define TextOveray 1

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <thread>

#include "ScreenCaptureServer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace GStreamerWrapper {

// Server IP address supplied from the .NET layer. Previously this value
// was specified via the `SIP` macro making it impossible to configure at
// run time.  It is now stored in a global that is set when the server is
// started.
static std::string g_server_ip;
static std::vector<StreamConfigNative> g_configs;

// ===== 모니터 개수 탐지 =====
static BOOL CALLBACK CountMonitorsProc(HMONITOR, HDC, LPRECT, LPARAM lParam) {
    int* c = reinterpret_cast<int*>(lParam); (*c)++; return TRUE;
}
static int DetectMonitorCount() {
    int c = 0; EnumDisplayMonitors(NULL, NULL, CountMonitorsProc, reinterpret_cast<LPARAM>(&c));
    return c > 0 ? c : 1;
}

// ===== RTSP 컨텍스트 =====
struct RtspServerContext {
    GMainLoop* loop = NULL;
    GstRTSPServer* server = NULL;
    GstRTSPMountPoints* mounts = NULL;
    guint server_source_id = 0; // attach된 서버 소스 ID
    std::vector<GstRTSPMediaFactory*> factories; // 정리용 보관
};

// 전역 컨텍스트와 실행 스레드를 보관하여 비동기 실행 및 중지를 지원합니다.
static RtspServerContext g_ctx;
static std::thread g_server_thread;

// 전방 선언
static void SetOverlayText(RtspServerContext* ctx, int screen_index, const char* text);
// ===== [ADD] 공용 프로퍼티 세터/인코더 선택 유틸 =====
static inline gboolean has_prop(GstElement* e, const char* name) {
    return e && g_object_class_find_property(G_OBJECT_GET_CLASS(e), name) != nullptr;
}
static inline void set_int_if(GstElement* e, const char* name, int v) {
    if (has_prop(e, name)) g_object_set(e, name, v, NULL);
}
static inline void set_bool_if(GstElement* e, const char* name, gboolean v) {
    if (has_prop(e, name)) g_object_set(e, name, v, NULL);
}
static inline void set_string_if(GstElement* e, const char* name, const char* v) {
    if (has_prop(e, name)) g_object_set(e, name, v, NULL);
}

// H.264 하드웨어 인코더 후보들 중 "등록되어 있고(rank가 가장 높은)" 요소를 선택
// ※ 필요시 후보 순서는 자유롭게 추가/조정 가능
static const char* pick_best_hw_h264_encoder() {
    struct Cand { const char* name; };
    static const Cand candidates[] = {
        {"d3d11h264enc"}, // DX11 기반
        {"qsvh264enc"},   // Intel QuickSync
        {"nvh264enc"},    // NVIDIA NVENC
        {"amfh264enc"},   // AMD AMF
        {"mfh264enc"},    // Media Foundation H264
    };
    const char* best = nullptr;
    guint best_rank = 0;
    for (const auto& c : candidates) {
        GstElementFactory* f = gst_element_factory_find(c.name);
        if (!f) continue;
        guint r = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(f));
        if (!best || r > best_rank) {
            best = c.name;
            best_rank = r;
        }
        gst_object_unref(f);
    }
    return best; // nullptr이면 사용 가능한 HW 인코더 없음
}

// 인코더 공통 튜닝(존재하는 프로퍼티만 안전하게 세팅)
// - v_kbps: kbps 기준
// - keyint: 키프레임 간격 (GOP)
// - fps   : 참고용(일부 인코더는 keyint=fps 형태로 사용)
static void tune_h264_encoder(GstElement* enc, int v_kbps, int keyint, int /*fps*/) {
    if (!enc) return;

    // 비트레이트 (kbps/bps 혼용 대비: 여러 이름을 시도)
    set_int_if(enc, "bitrate", v_kbps);                // 대부분 kbps
    set_int_if(enc, "max-bitrate", v_kbps);
    set_int_if(enc, "target-bitrate", v_kbps * 1000);  // 일부 bps

    // 키프레임 간격/GOP
    set_int_if(enc, "key-int-max", keyint); // x264enc
    set_int_if(enc, "gop-size", keyint); // qsv/nvenc/d3d11 등
    set_int_if(enc, "gop", keyint); // mf/amf 등

    // 저지연/레이트컨트롤(있는 경우에만)
    set_string_if(enc, "tune", "zerolatency"); // x264enc
    set_bool_if(enc, "low-latency", TRUE);   // 일부 HW 인코더
    set_string_if(enc, "rate-control", "cbr"); // qsv/nvenc/d3d11 일부
    set_string_if(enc, "rc-mode", "cbr"); // amf 등

    // H.264 스트림 포맷 관련
    set_bool_if(enc, "byte-stream", TRUE);   // x264enc 등에만 적용됨
    set_string_if(enc, "speed-preset", "ultrafast"); // x264enc
    set_string_if(enc, "preset", "ultrafast"); // 일부 인코더는 preset 사용
}

// ===== 클라이언트 종료/로그 =====
static void client_closed_callback(GstRTSPClient* client, gpointer user_data) {
    (void)user_data;
    GstRTSPConnection* c = gst_rtsp_client_get_connection(client);
    const gchar* ip = c ? gst_rtsp_connection_get_ip(c) : "unknown";
    g_print("<<< 클라이언트 종료: %s\n", ip ? ip : "unknown");
    if (c) g_object_unref(c);
}

static void client_connected_callback(GstRTSPServer*, GstRTSPClient* client, gpointer user_data) {
    (void)user_data;
    GstRTSPConnection* c = gst_rtsp_client_get_connection(client);
    if (c) {
        const gchar* ip = gst_rtsp_connection_get_ip(c);
        g_print(">>> 새로운 클라이언트: %s\n", ip ? ip : "unknown");
        g_object_unref(c);
    }
    g_signal_connect(client, "closed", G_CALLBACK(client_closed_callback), user_data);
}

static GstRTSPFilterResult force_client_disconnect(GstRTSPServer*, GstRTSPClient* client, gpointer) {
    gst_rtsp_client_close(client);
    return GST_RTSP_FILTER_REMOVE;
}

// ====== 커스텀 Factory ======
typedef struct _MyMediaFactory {
    GstRTSPMediaFactory parent;
    int monitor_index;
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    int out_w, out_h;
    int fps;
    int v_bitrate_kbps;
    int a_bitrate_bps;
    int keyint;  // [ADD] 키프레임 간격(GOP) - 전달값 없으면 fps 사용
    gboolean enable_audio;
    gchar* audio_device;
    gboolean use_hw_accel;

#ifdef TextOveray
    // 텍스트 오버레이 상태 (오버레이가 켜진 빌드에서만 포함)
    gchar* overlay_text;
    GstElement* overlay_elem;  // weak
#endif
} MyMediaFactory;

typedef struct _MyMediaFactoryClass {
    GstRTSPMediaFactoryClass parent_class;
} MyMediaFactoryClass;

G_DEFINE_TYPE(MyMediaFactory, my_media_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

static GstCaps* caps_from(const std::string& s) { return gst_caps_from_string(s.c_str()); }
static gboolean link_ok(GstElement* a, GstElement* b, GstCaps* caps = NULL) {
    gboolean ok = caps ? gst_element_link_filtered(a, b, caps) : gst_element_link(a, b);
    if (!ok) g_printerr("link failed between %s -> %s\n", GST_ELEMENT_NAME(a), GST_ELEMENT_NAME(b));
    if (caps) gst_caps_unref(caps);
    return ok;
}

// --- (선택) 수명주기 로그
static void on_media_unprepared(GstRTSPMedia* media, gpointer) {
    g_print("[media] unprepared\n");
    GstElement* e = gst_rtsp_media_get_element(media);
    if (e) { gst_element_set_state(e, GST_STATE_NULL); gst_object_unref(e); }
}
static void on_media_prepared(GstRTSPMedia*, gpointer) {
    g_print("[media] prepared\n");
}
static void on_media_configure(GstRTSPMediaFactory*, GstRTSPMedia* media, gpointer) {
    g_signal_connect(media, "prepared", G_CALLBACK(on_media_prepared), NULL);
    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), NULL);
}

static GstElement* my_media_factory_create_element(GstRTSPMediaFactory* factory, const GstRTSPUrl*) {
    MyMediaFactory* self = (MyMediaFactory*)factory;

    const gint monitor_index = self->monitor_index;
    const gint crop_x = self->crop_x;
    const gint crop_y = self->crop_y;
    const gint crop_w = self->crop_w;
    const gint crop_h = self->crop_h;
    const gint out_w = (self->out_w & ~1);
    const gint out_h = (self->out_h & ~1);
    const gint fps = self->fps;
    const gint v_kbps = self->v_bitrate_kbps;
    const gint a_bps = self->a_bitrate_bps;

    GstElement* bin = gst_bin_new(NULL);

    // ---- 비디오 ----
    GstElement* vsrc = gst_element_factory_make("d3d11screencapturesrc", NULL);
    GstElement* vd3d = gst_element_factory_make("d3d11convert", "vd3d11conv");
#ifdef TextOveray
    GstElement* tover = gst_element_factory_make("dwritetextoverlay", "overlay");
#endif
    GstElement* vd3d2 = gst_element_factory_make("d3d11convert", "vd3d11conv2");
    GstElement* vdown = NULL; // CPU path when using non-D3D11 encoder
    GstElement* vq1 = gst_element_factory_make("queue", "vqueue1");
    // [CHANGE] 요구사항: HW ON이면 x264 제외 HW 인코더 중 랭크 최상 선택, HW OFF면 x264 고정
    const char* enc_name = nullptr;
    if (self->use_hw_accel) {
        enc_name = pick_best_hw_h264_encoder(); // 없으면 nullptr
        if (!enc_name) {
            g_printerr("[warn] 사용 가능한 하드웨어 H.264 인코더가 없어 x264enc로 대체합니다.\n");
            enc_name = "x264enc";
        }
        else {
            g_print("[info] 선택된 하드웨어 H.264 인코더: %s\n", enc_name);
        }
    }
    else {
        enc_name = "x264enc";
    }
    GstElement* venc = gst_element_factory_make(enc_name, "venc");
    if (!venc) {
        g_printerr("인코더(%s) 생성 실패\n", enc_name);
        if (bin) gst_object_unref(bin);
        return NULL;
    }
    const gboolean use_d3d11_encoder = g_str_has_prefix(enc_name, "d3d11");
    if (!use_d3d11_encoder) {
        vdown = gst_element_factory_make("d3d11download", "vdown");
    }
    GstElement* vparse = gst_element_factory_make("h264parse", "vparse");
    GstElement* vcf = gst_element_factory_make("capsfilter", "vpaycaps");
    GstElement* vq2 = gst_element_factory_make("queue", "vqueue2");
    GstElement* vpay = gst_element_factory_make("rtph264pay", "pay0");

#ifndef TextOveray
    if (!vsrc || !vd3d || !vd3d2 || (!use_d3d11_encoder && !vdown) || !vq1 || !venc || !vparse || !vcf || !vq2 || !vpay) {
#else
    if (!vsrc || !vd3d || !tover || !vd3d2 || (!use_d3d11_encoder && !vdown) || !vq1 || !venc || !vparse || !vcf || !vq2 || !vpay) {
#endif
        g_printerr("비디오 요소 생성 실패\n");
        if (bin) gst_object_unref(bin);
        return NULL;
    }

#ifndef TextOveray
    if (use_d3d11_encoder)
        gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, vd3d2, vq1, venc, vparse, vcf, vq2, vpay, NULL);
    else
        gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, vd3d2, vdown, vq1, venc, vparse, vcf, vq2, vpay, NULL);
#else
    if (use_d3d11_encoder)
        gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, tover, vd3d2, vq1, venc, vparse, vcf, vq2, vpay, NULL);
    else
        gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, tover, vd3d2, vdown, vq1, venc, vparse, vcf, vq2, vpay, NULL);
#endif

    g_object_set(vsrc,
        "monitor-index", monitor_index,
        "show-cursor", TRUE,
        "crop-x", crop_x,
        "crop-y", crop_y,
        "crop-width", crop_w,
        "crop-height", crop_h,
        NULL);

    // GPU caps and overlay
    {
        std::ostringstream css_gpu_rgba;
        css_gpu_rgba << "video/x-raw(memory:D3D11Memory),format=BGRA," <<
            "width=" << out_w << ",height=" << out_h <<
            ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1";
        if (!link_ok(vsrc, vd3d, caps_from(css_gpu_rgba.str()))) return bin;
    }

#ifdef TextOveray
    // 텍스트 오버레이
    {
        const gchar* txt = (self->overlay_text && *self->overlay_text) ? self->overlay_text : "";
        g_object_set(tover,
            "text", txt,
            "font-desc", "Segoe UI 11",
            "halignment", 0,
            "valignment", 2,
            "xpad", 8, "ypad", 8,
            "shaded-background", TRUE,
            "draw-shadow", TRUE,
            NULL);
        self->overlay_elem = tover;
        g_object_add_weak_pointer(G_OBJECT(tover), (gpointer*)&self->overlay_elem);
    }

    if (!link_ok(vd3d, tover)) return bin;
    if (!link_ok(tover, vd3d2)) return bin;
#else
    if (!link_ok(vd3d, vd3d2)) return bin;
#endif

    {
        std::ostringstream css_gpu_nv12;
        css_gpu_nv12 << "video/x-raw(memory:D3D11Memory),format=NV12," <<
            "width=" << out_w << ",height=" << out_h <<
            ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1";
        if (use_d3d11_encoder) {
            if (!link_ok(vd3d2, vq1, caps_from(css_gpu_nv12.str()))) return bin;
        } else {
            if (!link_ok(vd3d2, vdown, caps_from(css_gpu_nv12.str()))) return bin;
            if (!link_ok(vdown, vq1)) return bin;
        }
    }

    static const char* kH264Profile = "high";
    static const char* kH264Level = "5";

    // [CHANGE] 공통 튜닝(있는 프로퍼티만 세팅) + x264 특성은 자동으로 감지되어 적용
    const int keyint = (self->keyint > 0) ? self->keyint : fps;
    tune_h264_encoder(venc, v_kbps, keyint, fps);

    // (선택) x264 전용 추가 옵션을 더 주고 싶다면 has_prop로 감싸서 세팅
    if (g_strcmp0(enc_name, "x264enc") == 0) {
        // 예: 프로파일/레벨/HRD/버퍼 등
        set_string_if(venc, "option-string", ("level=" + std::string(kH264Level) +
            ":vbv-maxrate=" + std::to_string(v_kbps) +
            ":vbv-bufsize=" + std::to_string(v_kbps * 2)).c_str());
        set_int_if(venc, "nal-hrd", 2);
        set_int_if(venc, "vbv-buf-capacity", 1000);
        set_int_if(venc, "b-adapt", 0);
        set_int_if(venc, "bframes", 0);
        set_int_if(venc, "ref", 1);
        // speed-preset, tune, byte-stream 등은 tune_h264_encoder에서 이미 안전 적용
    }


    {
        std::ostringstream css;
        css << "video/x-h264,stream-format=byte-stream,alignment=au," <<
            "profile=(string)" << kH264Profile;
        GstCaps* paycaps = gst_caps_from_string(css.str().c_str());
        g_object_set(vcf, "caps", paycaps, NULL);
        gst_caps_unref(paycaps);
    }

    g_object_set(vq1, "leaky", 2, "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", 0, NULL);
    g_object_set(vq2, "leaky", 2, "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", 0, NULL);

    g_object_set(vparse, "config-interval", 1, NULL);
    g_object_set(vpay, "pt", 96, "config-interval", 1, "mtu", 1200, NULL);
    if (!link_ok(vq1, venc)) return bin;

    if (!link_ok(venc, vparse) ||
        !link_ok(vparse, vcf) || !link_ok(vcf, vq2) || !link_ok(vq2, vpay))
        return bin;


    if (self->enable_audio) {
        GstElement* asrc = gst_element_factory_make("wasapisrc", "asrc");
        GstElement* aq1 = gst_element_factory_make("queue", "aqueue1");
        GstElement* aconv = gst_element_factory_make("audioconvert", NULL);
        GstElement* ares = gst_element_factory_make("audioresample", NULL);
        GstElement* acapsf = gst_element_factory_make("capsfilter", "acaps");
        GstElement* arate = gst_element_factory_make("audiorate", NULL);
        GstElement* aq2 = gst_element_factory_make("queue", "aqueue2");
        GstElement* aenc = gst_element_factory_make("opusenc", "aenc");
        GstElement* apay = gst_element_factory_make("rtpopuspay", "pay1");

        if (!asrc || !aq1 || !aconv || !ares || !acapsf || !arate || !aq2 || !aenc || !apay) {
            g_warning("오디오 요소 생성 실패, 비디오만 스트리밍합니다.");
        }
        else {
            gst_bin_add_many(GST_BIN(bin), asrc, aq1, aconv, ares, acapsf, arate, aq2, aenc, apay, NULL);

            g_object_set(asrc, "loopback", TRUE, "do-timestamp", TRUE, NULL);
            if (self->audio_device)
                g_object_set(asrc, "device-name", self->audio_device, NULL);
            g_object_set(aq1, "leaky", 2, "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", (gint64)200000000, NULL);
            g_object_set(aq2, "leaky", 2, "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", (gint64)200000000, NULL);

            GstCaps* a_caps = gst_caps_new_simple("audio/x-raw",
                "format", G_TYPE_STRING, "S16LE",
                "rate", G_TYPE_INT, 48000,
                "channels", G_TYPE_INT, 2, NULL);
            g_object_set(acapsf, "caps", a_caps, NULL);
            gst_caps_unref(a_caps);

            g_object_set(aenc, "bitrate", a_bps, "frame-size", 20, NULL);
            g_object_set(apay, "pt", 97, NULL);

            if (!link_ok(asrc, aq1) || !link_ok(aq1, aconv) || !link_ok(aconv, ares) ||
                !link_ok(ares, acapsf) || !link_ok(acapsf, arate) || !link_ok(arate, aq2) ||
                !link_ok(aq2, aenc) || !link_ok(aenc, apay)) {
                g_warning("오디오 파이프라인 연결 실패, 비디오만 스트리밍합니다.");
            }
        }
    }

    return bin;
}

static void my_media_factory_finalize(GObject * object) {
    MyMediaFactory* self = (MyMediaFactory*)object;
#ifdef TextOveray
    if (self->overlay_elem) {
        g_object_remove_weak_pointer(G_OBJECT(self->overlay_elem), (gpointer*)&self->overlay_elem);
        self->overlay_elem = NULL;
    }
    if (self->overlay_text) {
        g_free(self->overlay_text);
        self->overlay_text = NULL;
    }
#endif
    if (self->audio_device) {
        g_free(self->audio_device);
        self->audio_device = NULL;
    }
    G_OBJECT_CLASS(my_media_factory_parent_class)->finalize(object);
}

static void my_media_factory_class_init(MyMediaFactoryClass * klass) {
    GstRTSPMediaFactoryClass* mklass = GST_RTSP_MEDIA_FACTORY_CLASS(klass);
    mklass->create_element = my_media_factory_create_element;

    GObjectClass* gklass = G_OBJECT_CLASS(klass);
    gklass->finalize = my_media_factory_finalize;
}
static void my_media_factory_init(MyMediaFactory * self) {
#ifdef TextOveray
    self->overlay_text = g_strdup("");
    self->overlay_elem = NULL;
#endif
    self->crop_x = 0;
    self->crop_y = 0;
    self->crop_w = 0;
    self->crop_h = 0;
    self->audio_device = NULL;
    self->use_hw_accel = FALSE;
    self->keyint = 0; // [ADD]
}

static GstRTSPMediaFactory* create_factory_from_config(const StreamConfigNative& cfg,
                                                      int stream_index) {
    MyMediaFactory* f = (MyMediaFactory*)g_object_new(my_media_factory_get_type(), NULL);
    f->monitor_index = cfg.monitor_index;
    f->crop_x = cfg.crop_x;
    f->crop_y = cfg.crop_y;
    f->crop_w = cfg.crop_w;
    f->crop_h = cfg.crop_h;
    f->out_w = cfg.width > 0 ? cfg.width : 1920;
    f->out_h = cfg.height > 0 ? cfg.height : 1200;
    f->fps = cfg.framerate > 0 ? cfg.framerate : 30;
    f->v_bitrate_kbps = cfg.bitrate_kbps > 0 ? cfg.bitrate_kbps : 8000;
    f->a_bitrate_bps = 128000;
    f->enable_audio = cfg.enable_audio;
    if (!cfg.audio_device.empty())
        f->audio_device = g_strdup(cfg.audio_device.c_str());
    f->use_hw_accel = cfg.enable_hw_accel;
	f->keyint = cfg.keyframe_interval > 0 ? cfg.keyframe_interval : f->fps; // [ADD]


#ifdef TextOveray
    if (f->overlay_text) g_free(f->overlay_text);
    std::ostringstream def; def << "Screen " << stream_index;
    f->overlay_text = g_strdup(def.str().c_str());
#endif

    gst_rtsp_media_factory_set_shared(GST_RTSP_MEDIA_FACTORY(f), TRUE);
    gst_rtsp_media_factory_set_suspend_mode(GST_RTSP_MEDIA_FACTORY(f), GST_RTSP_SUSPEND_MODE_NONE);
    g_signal_connect(f, "media-configure", G_CALLBACK(on_media_configure), NULL);

    gst_rtsp_media_factory_set_protocols(GST_RTSP_MEDIA_FACTORY(f), GST_RTSP_LOWER_TRANS_UDP_MCAST);
    gst_rtsp_media_factory_set_multicast_iface(GST_RTSP_MEDIA_FACTORY(f), g_server_ip.c_str());

    // Ports and multicast IPs previously depended on the monitor index. This
    // caused collisions when multiple streams originated from the same monitor.
    // Instead, derive them from the sequential stream index so that every
    // stream gets a unique range regardless of monitor configuration.
    const int base_octet = 11 + stream_index;
    const int base_port = 15000 + stream_index * 20;
    std::ostringstream ip; ip << "239.255.10." << base_octet;
    GstRTSPAddressPool* pool = gst_rtsp_address_pool_new();
    gst_rtsp_address_pool_add_range(pool, ip.str().c_str(), ip.str().c_str(), base_port, base_port + 19, 16);
    gst_rtsp_media_factory_set_address_pool(GST_RTSP_MEDIA_FACTORY(f), pool);
    g_object_unref(pool);

    return GST_RTSP_MEDIA_FACTORY(f);
}

static void SetOverlayText(RtspServerContext * ctx, int screen_index, const char* text) {
    if (!ctx) return;
    if (screen_index < 0 || screen_index >= (int)ctx->factories.size()) return;

    MyMediaFactory* f = (MyMediaFactory*)ctx->factories[screen_index];
    if (!f) return;

#ifdef TextOveray
    if (f->overlay_text) g_free(f->overlay_text);
    f->overlay_text = g_strdup(text ? text : "");
    if (f->overlay_elem) {
        g_object_set(f->overlay_elem, "text", f->overlay_text, NULL);
    }
#else
    (void)text;
#endif
}

static void initialize_gstreamer(int* argc, char*** argv, RtspServerContext * ctx) {
    gst_init(argc, argv);
    ctx->loop = g_main_loop_new(NULL, FALSE);
    ctx->server = gst_rtsp_server_new();
    gst_rtsp_server_set_address(ctx->server, g_server_ip.c_str());
    gst_rtsp_server_set_service(ctx->server, "10554");

    g_object_set(G_OBJECT(ctx->server), "session-timeout", 2, NULL);
    {
        GstRTSPSessionPool* pool = gst_rtsp_server_get_session_pool(ctx->server);
        if (pool) {
            gst_rtsp_session_pool_set_max_sessions(pool, 8);
            g_object_unref(pool);
        }
    }

    g_signal_connect(ctx->server, "client-connected", G_CALLBACK(client_connected_callback), ctx);
    ctx->mounts = gst_rtsp_server_get_mount_points(ctx->server);
}

static void configure_rtsp_server(RtspServerContext * ctx) {
    ctx->factories.reserve(g_configs.size());
    for (size_t i = 0; i < g_configs.size(); ++i) {
        GstRTSPMediaFactory* f = create_factory_from_config(g_configs[i], static_cast<int>(i));
        ctx->factories.push_back(f);
        std::ostringstream mount; mount << "/screen" << (i + 1);
        gst_rtsp_mount_points_add_factory(ctx->mounts, mount.str().c_str(), f);
        g_print("  - mount: rtsp://%s:10554%s\n", g_server_ip.c_str(), mount.str().c_str());
    }
}

static bool start_rtsp_server(RtspServerContext * ctx) {
    ctx->server_source_id = gst_rtsp_server_attach(ctx->server, NULL);
    if (ctx->server_source_id == 0) {
        g_printerr("RTSP 서버 attach 실패\n");
        return false;
    }
    g_print("RTSP 서버가 시작되었습니다. 예: rtsp://%s:10554/screen1\n", g_server_ip.c_str());
    g_main_loop_run(ctx->loop);
    return true;
}

static void cleanup_resources(RtspServerContext * ctx) {
    g_print("5. 리소스 해제...\n");
    if (ctx->server_source_id != 0) {
        g_source_remove(ctx->server_source_id);
        ctx->server_source_id = 0;
    }
    if (ctx->mounts)  g_object_unref(ctx->mounts);
    for (auto* f : ctx->factories) { if (f) g_object_unref(f); }
    if (ctx->server)  g_object_unref(ctx->server);
    if (ctx->loop)    g_main_loop_unref(ctx->loop);
    // reset pointers to avoid double free on repeated start/stop
    ctx->loop = NULL;
    ctx->server = NULL;
    ctx->mounts = NULL;
    ctx->factories.clear();
}

// 서버를 백그라운드 스레드에서 실행하도록 수정. `serverIp`는 서버가
// 바인딩할 주소이며 전역 변수로 보관되어 이후 GStreamer 초기화 시
// 사용된다.
void RunScreenCaptureRtspServer(const char* serverIp,
                                const StreamConfigNative* configs,
                                int count) {
    g_configs.clear();
    if (configs && count > 0) {
        g_configs.assign(configs, configs + count);
    }

    if (serverIp) {
        g_server_ip = serverIp;
    } else {
        g_server_ip.clear();
    }

    if (g_server_thread.joinable()) {
        g_printerr("RTSP server already running\n");
        return;
    }
    g_server_thread = std::thread([]() {
        int argc = 0; char** argv = NULL;
        initialize_gstreamer(&argc, &argv, &g_ctx);
        configure_rtsp_server(&g_ctx);
        start_rtsp_server(&g_ctx); // 내부에서 g_main_loop_run 수행
        cleanup_resources(&g_ctx);
        g_print("서버 종료\n");
    });
}

void StopScreenCaptureRtspServer() {
    if (g_ctx.server) {
        gst_rtsp_server_client_filter(g_ctx.server, force_client_disconnect, NULL);
        GstRTSPSessionPool* pool = gst_rtsp_server_get_session_pool(g_ctx.server);
        if (pool) {
            gst_rtsp_session_pool_cleanup(pool);
            g_object_unref(pool);
        }
    }
    if (g_ctx.loop) {
        g_main_loop_quit(g_ctx.loop);
        GMainContext* ctx = g_main_loop_get_context(g_ctx.loop);
        if (ctx) {
            g_main_context_wakeup(ctx);
        }
    }
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
}

} // namespace GStreamerWrapper
