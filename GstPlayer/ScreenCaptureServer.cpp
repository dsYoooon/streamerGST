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
    int out_w, out_h;
    int fps;
    int v_bitrate_kbps;
    int a_bitrate_bps;
    gboolean enable_audio;

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
    const gint out_w = (self->out_w & ~1);
    const gint out_h = (self->out_h & ~1);
    const gint fps = self->fps;
    const gint v_kbps = self->v_bitrate_kbps;
    const gint a_bps = self->a_bitrate_bps;

    GstElement* bin = gst_bin_new(NULL);

    // ---- 비디오 ----
    GstElement* vsrc = gst_element_factory_make("d3d11screencapturesrc", NULL);
    GstElement* vd3d = gst_element_factory_make("d3d11convert", "vd3d11conv");
    GstElement* vdown = gst_element_factory_make("d3d11download", "vdown");     // GPU->CPU
    GstElement* vconv = gst_element_factory_make("videoconvert", "vconv");      // CPU colorspace

#ifdef TextOveray
    GstElement* tover = gst_element_factory_make("textoverlay", "overlay");
    GstElement* vconv2 = gst_element_factory_make("videoconvert", "vconv2");     // 호환성
#endif

    GstElement* vq1 = gst_element_factory_make("queue", "vqueue1");
    GstElement* venc = gst_element_factory_make("x264enc", "venc");
    GstElement* vparse = gst_element_factory_make("h264parse", "vparse");
    GstElement* vcf = gst_element_factory_make("capsfilter", "vpaycaps");
    GstElement* vq2 = gst_element_factory_make("queue", "vqueue2");
    GstElement* vpay = gst_element_factory_make("rtph264pay", "pay0");

#ifndef TextOveray
    if (!vsrc || !vd3d || !vdown || !vconv || !vq1 || !venc || !vparse || !vcf || !vq2 || !vpay) {
#else
    if (!vsrc || !vd3d || !vdown || !vconv || !tover || !vconv2 || !vq1 || !venc || !vparse || !vcf || !vq2 || !vpay) {
#endif
        g_printerr("비디오 요소 생성 실패\n");
        if (bin) gst_object_unref(bin);
        return NULL;
    }

#ifndef TextOveray
    gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, vdown, vconv, vq1, venc, vparse, vcf, vq2, vpay, NULL);
#else
    gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, vdown, vconv, tover, vconv2, vq1, venc, vparse, vcf, vq2, vpay, NULL);
#endif

    g_object_set(vsrc, "monitor-index", monitor_index, "show-cursor", TRUE, NULL);

    // GPU caps
    {
        std::ostringstream css_gpu;
        css_gpu << "video/x-raw(memory:D3D11Memory),format=NV12," <<
            "width=" << out_w << ",height=" << out_h <<
            ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1";
        if (!link_ok(vsrc, vd3d)) return bin;
        if (!link_ok(vd3d, vdown, caps_from(css_gpu.str()))) return bin;
    }

    // CPU caps
    {
        std::ostringstream css_cpu;
        css_cpu << "video/x-raw,format=NV12," <<
            "width=" << out_w << ",height=" << out_h <<
            ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1";
        if (!link_ok(vdown, vconv)) return bin;
#ifndef TextOveray
        if (!link_ok(vconv, vq1, caps_from(css_cpu.str()))) return bin;
#else
        if (!link_ok(vconv, tover, caps_from(css_cpu.str()))) return bin;
#endif
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

    if (!link_ok(tover, vconv2)) return bin;
    if (!link_ok(vconv2, vq1)) return bin;
#endif

    static const char* kH264Profile = "high";
    static const char* kH264Level = "5";

    g_object_set(venc,
        "bitrate", v_kbps,
        "key-int-max", fps,
        "b-adapt", 0,
        "bframes", 0,
        "ref", 1,
        "byte-stream", TRUE,
        "speed-preset", 1,
        "tune", 0x04,
        NULL);

    {
        std::ostringstream opt;
        opt << "level=" << kH264Level << ":vbv-maxrate=" << v_kbps << ":vbv-bufsize=" << (v_kbps * 2);
        g_object_set(venc, "option-string", opt.str().c_str(), NULL);
        g_object_set(venc, "nal-hrd", 2, "vbv-buf-capacity", 1000, NULL);
    }

    if (!strcmp(kH264Profile, "baseline") || !strcmp(kH264Profile, "constrained-baseline")) {
        g_object_set(venc, "cabac", FALSE, "dct8x8", FALSE, "bframes", 0, "ref", 1, NULL);
    }
    else if (!strcmp(kH264Profile, "main")) {
        g_object_set(venc, "cabac", TRUE, "dct8x8", FALSE, NULL);
    }
    else if (!strcmp(kH264Profile, "high")) {
        g_object_set(venc, "cabac", TRUE, "dct8x8", TRUE, NULL);
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

    if (!link_ok(vq1, venc) || !link_ok(venc, vparse) ||
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
}

static GstRTSPMediaFactory* create_factory_from_config(const StreamConfigNative& cfg) {
    const bool ENABLE_AUDIO = true;

    MyMediaFactory* f = (MyMediaFactory*)g_object_new(my_media_factory_get_type(), NULL);
    f->monitor_index = cfg.monitor_index;
    f->out_w = cfg.width > 0 ? cfg.width : 1920;
    f->out_h = cfg.height > 0 ? cfg.height : 1200;
    f->fps = cfg.framerate > 0 ? cfg.framerate : 30;
    f->v_bitrate_kbps = cfg.bitrate_kbps > 0 ? cfg.bitrate_kbps : 8000;
    f->a_bitrate_bps = 128000;
    f->enable_audio = ENABLE_AUDIO;

#ifdef TextOveray
    if (f->overlay_text) g_free(f->overlay_text);
    std::ostringstream def; def << "Screen " << (f->monitor_index + 1);
    f->overlay_text = g_strdup(def.str().c_str());
#endif

    gst_rtsp_media_factory_set_shared(GST_RTSP_MEDIA_FACTORY(f), TRUE);
    gst_rtsp_media_factory_set_suspend_mode(GST_RTSP_MEDIA_FACTORY(f), GST_RTSP_SUSPEND_MODE_NONE);
    g_signal_connect(f, "media-configure", G_CALLBACK(on_media_configure), NULL);

    gst_rtsp_media_factory_set_protocols(GST_RTSP_MEDIA_FACTORY(f), GST_RTSP_LOWER_TRANS_UDP_MCAST);
    gst_rtsp_media_factory_set_multicast_iface(GST_RTSP_MEDIA_FACTORY(f), g_server_ip.c_str());

    const int base_octet = 11 + f->monitor_index;
    const int base_port = 15000 + f->monitor_index * 20;
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
        GstRTSPMediaFactory* f = create_factory_from_config(g_configs[i]);
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
