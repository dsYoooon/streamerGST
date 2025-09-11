// ScreenCaptureServer.cpp (encodebin + per-option HW accel on/off, multi-vendor stable)

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/pbutils/encoding-profile.h>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <thread>

#include "ScreenCaptureServer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace GStreamerWrapper {

    static std::string g_server_ip;
    static std::vector<StreamConfigNative> g_configs;

    struct RtspServerContext {
        GMainLoop* loop = NULL;
        GstRTSPServer* server = NULL;
        GstRTSPMountPoints* mounts = NULL;
        guint server_source_id = 0;
        std::vector<GstRTSPMediaFactory*> factories;
    };

    static RtspServerContext g_ctx;
    static std::thread g_server_thread;

    /* ---------- small utils ---------- */
    static gboolean element_has_property(GstElement* e, const char* name) {
        if (!e || !name) return FALSE;
        GObjectClass* klass = G_OBJECT_GET_CLASS(e);
        return g_object_class_find_property(klass, name) != NULL;
    }
    static void set_int_if(GstElement* e, const char* name, int v) {
        if (element_has_property(e, name)) g_object_set(e, name, v, NULL);
    }
    static void set_bool_if(GstElement* e, const char* name, gboolean v) {
        if (element_has_property(e, name)) g_object_set(e, name, v, NULL);
    }
    static void set_str_if(GstElement* e, const char* name, const char* v) {
        if (element_has_property(e, name)) g_object_set(e, name, v, NULL);
    }

    /* ---------- rank control: enable_hw ? HW↑ : SW↑ ---------- */
    static void set_encoder_ranks_by_option(bool enable_hw) {
        GstRegistry* reg = gst_registry_get();
        auto set_rank = [&](const char* name, gint rank) {
            if (GstPluginFeature* f = gst_registry_find_feature(reg, name, GST_TYPE_ELEMENT_FACTORY)) {
                gst_plugin_feature_set_rank(f, rank);
                gst_object_unref(f);
            }
            };

        if (enable_hw) {
            // 하드웨어 인코더 우선
            set_rank("nvh264enc", GST_RANK_PRIMARY + 220);
            set_rank("amfenc_h264", GST_RANK_PRIMARY + 210);
            set_rank("qsvh264enc", GST_RANK_PRIMARY + 200);
            set_rank("d3d11h264enc", GST_RANK_PRIMARY + 190);
            // 소프트웨어는 낮춤
            set_rank("x264enc", GST_RANK_SECONDARY);
        }
        else {
            // 소프트웨어 인코더 우선 (HW는 뒤로)
            set_rank("x264enc", GST_RANK_PRIMARY + 200);
            set_rank("nvh264enc", GST_RANK_SECONDARY);
            set_rank("amfenc_h264", GST_RANK_SECONDARY);
            set_rank("qsvh264enc", GST_RANK_SECONDARY);
            set_rank("d3d11h264enc", GST_RANK_SECONDARY);
        }
    }

    /* ---------- RTSP client callbacks (절대 g_object_unref 연결X) ---------- */
    static void client_closed_callback(GstRTSPClient* client, gpointer) {
        GstRTSPConnection* c = gst_rtsp_client_get_connection(client);
        const gchar* ip = c ? gst_rtsp_connection_get_ip(c) : "unknown";
        g_print("<<< 클라이언트 종료: %s\n", ip ? ip : "unknown");
    }
    static void client_connected_callback(GstRTSPServer*, GstRTSPClient* client, gpointer user_data) {
        (void)user_data;
        GstRTSPConnection* c = gst_rtsp_client_get_connection(client);
        if (c) {
            const gchar* ip = gst_rtsp_connection_get_ip(c);
            g_print(">>> 새로운 클라이언트: %s\n", ip ? ip : "unknown");
        }
        g_signal_connect(client, "closed", G_CALLBACK(client_closed_callback), user_data);
    }

    static GstRTSPFilterResult force_client_disconnect(GstRTSPServer*, GstRTSPClient* client, gpointer) {
        gst_rtsp_client_close(client);
        return GST_RTSP_FILTER_REMOVE;
    }

    /* ---------- custom factory ---------- */
    typedef struct _MyMediaFactory {
        GstRTSPMediaFactory parent;

        // config
        int monitor_index;
        int crop_x, crop_y, crop_w, crop_h;
        int out_w, out_h;
        int fps;
        int v_bitrate_kbps;
        int a_bitrate_bps;
        int keyint;
        gboolean enable_audio;
        gchar* audio_device;
        gboolean enable_hw_accel; // ★ 옵션 반영
        gboolean enable_osd;

        // overlay
        gchar* overlay_text;
        GstElement* overlay_elem; // weak
    } MyMediaFactory;

    typedef struct _MyMediaFactoryClass {
        GstRTSPMediaFactoryClass parent_class;
    } MyMediaFactoryClass;

    G_DEFINE_TYPE(MyMediaFactory, my_media_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

        /* ---------- encodebin element-added: vendor별 저지연 튜닝 ---------- */
        static void on_encodebin_element_added(GstElement* encodebin, GstElement* elem, gpointer user_data) {
        (void)encodebin;
        MyMediaFactory* mf = (MyMediaFactory*)user_data;

        const gchar* fname = NULL;
        if (GstElementFactory* fac = gst_element_get_factory(elem)) {
            fname = GST_OBJECT_NAME(fac);
        }
        if (!fname) return;

        g_print("[encodebin] selected: %s (bitrate=%dk, keyint=%d)\n",
            fname, mf->v_bitrate_kbps, mf->keyint);

        // 공통 시도
        set_int_if(elem, "bitrate", mf->v_bitrate_kbps * 1000);
        set_int_if(elem, "key-int-max", mf->keyint);
        set_int_if(elem, "gop-size", mf->keyint);

        if (g_strcmp0(fname, "x264enc") == 0) {
            set_int_if(elem, "speed-preset", 1 /*ultrafast*/);
            set_int_if(elem, "tune", 0x00000004 /*zerolatency*/);
            set_bool_if(elem, "byte-stream", TRUE);
            // 일부 빌드에서 kbps 단위
            set_int_if(elem, "bitrate", mf->v_bitrate_kbps);
        }
        else if (g_strcmp0(fname, "qsvh264enc") == 0) {
            set_bool_if(elem, "lowpower", TRUE);
            set_int_if(elem, "rate-control", 1 /*CBR*/);
            set_int_if(elem, "target-usage", 3);
            set_int_if(elem, "gop-ref-dist", 1);
        }
        else if (g_strcmp0(fname, "nvh264enc") == 0) {
            set_str_if(elem, "preset", "llhq");
            set_str_if(elem, "rc-mode", "cbr");
            set_int_if(elem, "rc-lookahead", 0);
            set_int_if(elem, "b-frames", 0);
        }
        else if (g_strcmp0(fname, "amfenc_h264") == 0) {
            set_str_if(elem, "usage", "ultra-low-latency"); // 없으면 무시됨
            set_int_if(elem, "rate-control", 1 /*CBR*/);
            set_int_if(elem, "b-frames", 0);
        }
        else if (g_strcmp0(fname, "d3d11h264enc") == 0) {
            set_int_if(elem, "b-frames", 0);
        }
    }

    /* ---------- media events ---------- */
    static void on_media_unprepared(GstRTSPMedia* media, gpointer) {
        g_print("[media] unprepared\n");
        if (GstElement* e = gst_rtsp_media_get_element(media)) { gst_element_set_state(e, GST_STATE_NULL); gst_object_unref(e); }
    }
    static void on_media_prepared(GstRTSPMedia*, gpointer) { g_print("[media] prepared\n"); }
    static void on_media_configure(GstRTSPMediaFactory*, GstRTSPMedia* media, gpointer) {
        g_signal_connect(media, "prepared", G_CALLBACK(on_media_prepared), NULL);
        g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), NULL);
    }

    /* ---------- helpers ---------- */
    static gboolean link_ok(GstElement* a, GstElement* b, GstCaps* caps = NULL) {
        gboolean ok = caps ? gst_element_link_filtered(a, b, caps) : gst_element_link(a, b);
        if (caps) gst_caps_unref(caps);
        return ok;
    }

    /* ---------- create pipeline ---------- */
    static GstElement* my_media_factory_create_element(GstRTSPMediaFactory* factory, const GstRTSPUrl*) {
        MyMediaFactory* self = (MyMediaFactory*)factory;
        //gst_debug_set_default_threshold(GST_LEVEL_INFO);

        const gint monitor_index = self->monitor_index;
        const gint crop_x = self->crop_x, crop_y = self->crop_y, crop_w = self->crop_w, crop_h = self->crop_h;
        const gint out_w = (self->out_w & ~1), out_h = (self->out_h & ~1), fps = self->fps;
        const gint a_bps = self->a_bitrate_bps;
        const gint keyint = self->keyint > 0 ? self->keyint : fps;

        // 옵션에 따라 인코더 랭크 구성
        set_encoder_ranks_by_option(self->enable_hw_accel);
        g_print("[video] HW accel option: %s\n", self->enable_hw_accel ? "ON" : "OFF");

        GstElement* bin = gst_bin_new(NULL);

        /* --- 비디오 소스 & 전처리 (D3D11 경로) --- */
        GstElement* vsrc = gst_element_factory_make("d3d11screencapturesrc", NULL);
        GstElement* vd3d = gst_element_factory_make("d3d11convert", "vd3d11conv");
        GstElement* tover = self->enable_osd ? gst_element_factory_make("dwritetextoverlay", "overlay") : NULL;
        GstElement* vd3d2 = gst_element_factory_make("d3d11convert", "vd3d11conv2");

        // 폴백용(CPU 경로)
        GstElement* vdown = gst_element_factory_make("d3d11download", "vdown");
        GstElement* vconv = gst_element_factory_make("videoconvert", "vconv");

        GstElement* vq1 = gst_element_factory_make("queue", "vqueue1");

        // 인코더: HW on → encodebin, HW off → x264enc
        GstElement* venc = self->enable_hw_accel ?
            gst_element_factory_make("encodebin", "vencbin") :
            gst_element_factory_make("x264enc", "venc");

        GstElement* vparse = gst_element_factory_make("h264parse", "vparse");
        GstElement* vcf = gst_element_factory_make("capsfilter", "vpaycaps");
        GstElement* vq2 = gst_element_factory_make("queue", "vqueue2");
        GstElement* vpay = gst_element_factory_make("rtph264pay", "pay0");

        if (!vsrc || !vd3d || !vd3d2 || !vq1 || !venc || !vparse || !vcf || !vq2 || !vpay ||
            (self->enable_osd && !tover) ||
            (!self->enable_hw_accel && (!vdown || !vconv))   // SW 경로에선 필요
            ) {
            g_printerr("비디오 요소 생성 실패\n");
            if (bin) gst_object_unref(bin);
            return NULL;
        }

        if (self->enable_osd)
            gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, tover, vd3d2, vdown, vconv, vq1, venc, vparse, vcf, vq2, vpay, NULL);
        else
            gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, vd3d2, vdown, vconv, vq1, venc, vparse, vcf, vq2, vpay, NULL);

        // 캡처 기본 설정
        g_object_set(vsrc,
            "monitor-index", monitor_index, "show-cursor", TRUE,
            "crop-x", crop_x, "crop-y", crop_y, "crop-width", crop_w, "crop-height", crop_h, NULL);

        // 1) GPU BGRA at out size/fps
        {
            std::ostringstream css;
            css << "video/x-raw(memory:D3D11Memory),format=BGRA,"
                << "width=" << out_w << ",height=" << out_h
                << ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1";
            if (!link_ok(vsrc, vd3d, gst_caps_from_string(css.str().c_str()))) { gst_object_unref(bin); return NULL; }
        }

        // 2) 텍스트 오버레이 (옵션)
        GstElement* prev = vd3d;
        if (self->enable_osd) {
            const gchar* txt = (self->overlay_text && *self->overlay_text) ? self->overlay_text : "";
            g_object_set(tover, "text", txt, "font-desc", "Segoe UI 11",
                "halignment", 0, "valignment", 2,
                "xpad", 8, "ypad", 8,
                "shaded-background", TRUE, "draw-shadow", TRUE, NULL);
            self->overlay_elem = tover;
            g_object_add_weak_pointer(G_OBJECT(tover), (gpointer*)&self->overlay_elem);
            if (!link_ok(vd3d, tover)) { gst_object_unref(bin); return NULL; }
            prev = tover;
        }

        // 3) GPU NV12 (D3D11Memory)
        if (!link_ok(prev, vd3d2)) { gst_object_unref(bin); return NULL; }
        // 제로카피를 위한 D3D11Memory NV12 caps
        GstCaps* gpu_nv12_caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "NV12",
            "width", G_TYPE_INT, out_w,
            "height", G_TYPE_INT, out_h,
            "framerate", GST_TYPE_FRACTION, fps, 1,
            "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
        GstCapsFeatures* d3d11_feat = gst_caps_features_new("memory:D3D11Memory", NULL);
        gst_caps_set_features(gpu_nv12_caps, 0, d3d11_feat);

        gboolean zero_copy_ok = FALSE;

        // 4) 인코더 설정 (+ HW일 때 restriction을 D3D11Memory로 줘서 제로카피 유도)
        if (self->enable_hw_accel) {
            // encodebin profile + restriction (D3D11 NV12)
            GstCaps* h264_caps = gst_caps_from_string("video/x-h264,profile=high");
            GstCaps* restr = gst_caps_copy(gpu_nv12_caps); // NV12 + D3D11Memory
            GstEncodingProfile* vprof = (GstEncodingProfile*)
                gst_encoding_video_profile_new(h264_caps, NULL, restr, 1);
            g_object_set(venc, "profile", vprof, NULL);
            gst_encoding_profile_unref(vprof);
            gst_caps_unref(h264_caps);
            // element-added 튜닝 콜백
            g_signal_connect(venc, "element-added", G_CALLBACK(on_encodebin_element_added), self);

            // ★ 제로카피 경로 먼저 시도: vd3d2 -> vq1 (D3D11 NV12) -> encodebin
            if (gst_element_link_filtered(vd3d2, vq1, gst_caps_ref(gpu_nv12_caps)) &&
                gst_element_link(vq1, venc)) {
                zero_copy_ok = TRUE;
                g_print("[video] Zero-copy D3D11Memory→encodebin 활성화\n");
            }
            else {
                g_print("[video] Zero-copy 링크 실패 → CPU 폴백으로 전환\n");
            }
        }

        // 5) CPU 폴백 경로 (HW off 이거나 zero-copy 실패 시)
        if (!self->enable_hw_accel || !zero_copy_ok) {
            // vd3d2 → vdown (GPU NV12 그대로) → vconv → vq1 (CPU NV12) → 인코더
            {
                std::ostringstream gpu_css;
                gpu_css << "video/x-raw(memory:D3D11Memory),format=NV12,"
                    << "width=" << out_w << ",height=" << out_h
                    << ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1";
                if (!link_ok(vd3d2, vdown, gst_caps_from_string(gpu_css.str().c_str()))) { gst_object_unref(bin); return NULL; }
            }
            {
                std::ostringstream cpu_css;
                cpu_css << "video/x-raw,format=NV12,"
                    << "width=" << out_w << ",height=" << out_h
                    << ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1";
                if (!link_ok(vdown, vconv)) { gst_object_unref(bin); return NULL; }
                if (!link_ok(vconv, vq1, gst_caps_from_string(cpu_css.str().c_str()))) { gst_object_unref(bin); return NULL; }
            }

            if (self->enable_hw_accel) {
                // encodebin은 profile은 이미 설정됨. vq1 → venc 링크만 하면 됨.
                if (!gst_element_link(vq1, venc)) { gst_object_unref(bin); return NULL; }
            }
            else {
                // 소프트웨어 x264enc 세팅
                g_object_set(venc,
                    "bitrate", self->v_bitrate_kbps,
                    "key-int-max", keyint,
                    "gop-size", keyint,
                    "speed-preset", 1 /*ultrafast*/,
                    "tune", 0x00000004 /*zerolatency*/,
                    "byte-stream", TRUE,
                    NULL);
                if (!gst_element_link(vq1, venc)) { gst_object_unref(bin); return NULL; }
            }
        }

        // 6) parse → caps → queue → pay
        g_object_set(vparse, "config-interval", 1, NULL);
        {
            GstCaps* paycaps = gst_caps_from_string(
                "video/x-h264,stream-format=byte-stream,alignment=au,profile=(string)high");
            g_object_set(vcf, "caps", paycaps, NULL);
            gst_caps_unref(paycaps);
        }
        // vq1은 드롭하지 않음, vq2는 payloader 보호용으로 살짝 leaky
        g_object_set(vq1, "leaky", 0, "max-size-buffers", 0, "max-size-bytes", 0, "max-size-time", (gint64)0, NULL);
        g_object_set(vq2, "leaky", 2, "max-size-buffers", 2, "max-size-bytes", 0, "max-size-time", (gint64)0, NULL);
        g_object_set(vpay, "pt", 96, "config-interval", 1, "mtu", 1200, NULL);

        if (!link_ok(venc, vparse)) { gst_object_unref(bin); return NULL; }
        if (!link_ok(vparse, vcf)) { gst_object_unref(bin); return NULL; }
        if (!link_ok(vcf, vq2)) { gst_object_unref(bin); return NULL; }
        if (!link_ok(vq2, vpay)) { gst_object_unref(bin); return NULL; }

        /* --- 오디오 (끊김 해결 튜닝 반영) --- */
        if (self->enable_audio) {
            GstElement* asrc = gst_element_factory_make("wasapi2src", "asrc");
            if (!asrc) asrc = gst_element_factory_make("wasapisrc", "asrc"); // 폴백
            GstElement* aq1 = gst_element_factory_make("queue", "aqueue1");
            GstElement* aconv = gst_element_factory_make("audioconvert", NULL);
            GstElement* ares = gst_element_factory_make("audioresample", NULL);
            GstElement* acaps = gst_element_factory_make("capsfilter", "acaps");
            GstElement* aq2 = gst_element_factory_make("queue", "aqueue2");
            GstElement* aenc = gst_element_factory_make("opusenc", "aenc");
            GstElement* apay = gst_element_factory_make("rtpopuspay", "pay1");

            if (!asrc || !aq1 || !aconv || !ares || !acaps || !aq2 || !aenc || !apay) {
                g_warning("오디오 요소 생성 실패, 비디오만 스트리밍합니다.");
            }
            else {
                gst_bin_add_many(GST_BIN(bin), asrc, aq1, aconv, ares, acaps, aq2, aenc, apay, NULL);

                g_object_set(asrc,
                    "loopback", TRUE,
                    "do-timestamp", TRUE,
                    "loopback-silence-on-device-mute", TRUE,
                    NULL);

                // 큐 버퍼(끊김 방지): non-leaky + 충분한 시간 버퍼
                g_object_set(aq1, "leaky", 0, "max-size-buffers", 0, "max-size-bytes", 0,
                    "max-size-time", (gint64)500000000, NULL);
                g_object_set(aq2, "leaky", 0, "max-size-buffers", 0, "max-size-bytes", 0,
                    "max-size-time", (gint64)500000000, NULL);

                GstCaps* a_caps = gst_caps_new_simple("audio/x-raw",
                    "format", G_TYPE_STRING, "S16LE",
                    "rate", G_TYPE_INT, 48000,
                    "channels", G_TYPE_INT, 2, NULL);
                g_object_set(acaps, "caps", a_caps, NULL);
                gst_caps_unref(a_caps);

                g_object_set(aenc,
                    "bitrate", a_bps,
                    "frame-size", 40,
                    "inband-fec", TRUE,
                    "packet-loss-percentage", 10,
                    "complexity", 5, NULL);
                g_object_set(apay, "pt", 97, NULL);

                if (!gst_element_link_many(asrc, aq1, aconv, ares, acaps, aq2, aenc, apay, NULL)) {
                    g_warning("오디오 파이프라인 연결 실패, 비디오만 스트리밍합니다.");
                }
            }
        }

        // 정리
        if (gpu_nv12_caps) gst_caps_unref(gpu_nv12_caps);
        return bin;
    }


    /* ---------- finalize ---------- */
    static void my_media_factory_finalize(GObject* object) {
        MyMediaFactory* self = (MyMediaFactory*)object;
        if (self->overlay_elem) {
            g_object_remove_weak_pointer(G_OBJECT(self->overlay_elem), (gpointer*)&self->overlay_elem);
            self->overlay_elem = NULL;
        }
        if (self->overlay_text) { g_free(self->overlay_text); self->overlay_text = NULL; }
        if (self->audio_device) { g_free(self->audio_device); self->audio_device = NULL; }
        G_OBJECT_CLASS(my_media_factory_parent_class)->finalize(object);
    }
    static void my_media_factory_class_init(MyMediaFactoryClass* klass) {
        GstRTSPMediaFactoryClass* mklass = GST_RTSP_MEDIA_FACTORY_CLASS(klass);
        mklass->create_element = my_media_factory_create_element;
        GObjectClass* gklass = G_OBJECT_CLASS(klass);
        gklass->finalize = my_media_factory_finalize;
    }
    static void my_media_factory_init(MyMediaFactory* self) {
        self->overlay_text = g_strdup("");
        self->overlay_elem = NULL;
        self->crop_x = self->crop_y = self->crop_w = self->crop_h = 0;
        self->audio_device = NULL;
        self->enable_hw_accel = TRUE; // 기본값 (구조체에서 덮임)
        self->enable_osd = TRUE;
        self->keyint = 0;
    }

    /* ---------- factory build / server ---------- */
    static GstRTSPMediaFactory* create_factory_from_config(const StreamConfigNative& cfg, int stream_index) {
        MyMediaFactory* f = (MyMediaFactory*)g_object_new(my_media_factory_get_type(), NULL);

        f->monitor_index = cfg.monitor_index;
        f->crop_x = cfg.crop_x; f->crop_y = cfg.crop_y; f->crop_w = cfg.crop_w; f->crop_h = cfg.crop_h;
        f->out_w = cfg.width > 0 ? cfg.width : 1920;
        f->out_h = cfg.height > 0 ? cfg.height : 1080;
        f->fps = cfg.framerate > 0 ? cfg.framerate : 30;
        f->v_bitrate_kbps = cfg.bitrate_kbps > 0 ? cfg.bitrate_kbps : 8000;
        f->a_bitrate_bps = 128000;
        f->enable_audio = cfg.enable_audio;
        if (!cfg.audio_device.empty()) f->audio_device = g_strdup(cfg.audio_device.c_str());
        f->enable_hw_accel = cfg.enable_hw_accel;               // ★ 구조체 옵션 반영
        f->enable_osd = cfg.enable_osd;
        f->keyint = cfg.keyframe_interval > 0 ? cfg.keyframe_interval : f->fps;

        if (f->overlay_text) g_free(f->overlay_text);
        std::ostringstream def; def << "Screen " << stream_index;
        f->overlay_text = g_strdup(def.str().c_str());

        gst_rtsp_media_factory_set_shared(GST_RTSP_MEDIA_FACTORY(f), TRUE);
        gst_rtsp_media_factory_set_suspend_mode(GST_RTSP_MEDIA_FACTORY(f), GST_RTSP_SUSPEND_MODE_NONE);
        g_signal_connect(f, "media-configure", G_CALLBACK(on_media_configure), NULL);

        // 서버단 레이턴시(여유): 필요 시 300→400~600 조정
        gst_rtsp_media_factory_set_latency(GST_RTSP_MEDIA_FACTORY(f), 300);

        // 프로토콜: 디버깅 편의상 TCP|UDP_MCAST 모두 허용(운영에 맞춰 조정)
        gst_rtsp_media_factory_set_protocols(GST_RTSP_MEDIA_FACTORY(f),
            ( GST_RTSP_LOWER_TRANS_UDP_MCAST));
        gst_rtsp_media_factory_set_multicast_iface(GST_RTSP_MEDIA_FACTORY(f), g_server_ip.c_str());

        const int base_octet = 11 + stream_index;
        const int base_port = 15000 + stream_index * 20;
        std::ostringstream ip; ip << "239.255.10." << base_octet;
        GstRTSPAddressPool* pool = gst_rtsp_address_pool_new();
        gst_rtsp_address_pool_add_range(pool, ip.str().c_str(), ip.str().c_str(), base_port, base_port + 19, 16);
        gst_rtsp_media_factory_set_address_pool(GST_RTSP_MEDIA_FACTORY(f), pool);
        g_object_unref(pool);

        return GST_RTSP_MEDIA_FACTORY(f);
    }

    static void initialize_gstreamer(int* argc, char*** argv, RtspServerContext* ctx) {
        gst_init(argc, argv);
        ctx->loop = g_main_loop_new(NULL, FALSE);
        ctx->server = gst_rtsp_server_new();
        gst_rtsp_server_set_address(ctx->server, g_server_ip.c_str());
        gst_rtsp_server_set_service(ctx->server, "10554");

        if (GstRTSPSessionPool* pool = gst_rtsp_server_get_session_pool(ctx->server)) {
            gst_rtsp_session_pool_set_max_sessions(pool, 8);
            g_object_unref(pool);
        }

        g_signal_connect(ctx->server, "client-connected", G_CALLBACK(client_connected_callback), ctx);
        ctx->mounts = gst_rtsp_server_get_mount_points(ctx->server);
    }

    static void configure_rtsp_server(RtspServerContext* ctx) {
        ctx->factories.reserve(g_configs.size());
        for (size_t i = 0; i < g_configs.size(); ++i) {
            GstRTSPMediaFactory* f = create_factory_from_config(g_configs[i], static_cast<int>(i));
            ctx->factories.push_back(f);
            std::ostringstream mount; mount << "/screen" << (i + 1);
            gst_rtsp_mount_points_add_factory(ctx->mounts, mount.str().c_str(), f);
            g_print("  - mount: rtsp://%s:10554%s\n", g_server_ip.c_str(), mount.str().c_str());
        }
    }

    static bool start_rtsp_server(RtspServerContext* ctx) {
        ctx->server_source_id = gst_rtsp_server_attach(ctx->server, NULL);
        if (ctx->server_source_id == 0) { g_printerr("RTSP 서버 attach 실패\n"); return false; }
        g_print("RTSP 서버가 시작되었습니다. 예: rtsp://%s:10554/screen1\n", g_server_ip.c_str());
        g_main_loop_run(ctx->loop);
        return true;
    }

    static void cleanup_resources(RtspServerContext* ctx) {
        g_print("5. 리소스 해제...\n");
        if (ctx->server_source_id != 0) { g_source_remove(ctx->server_source_id); ctx->server_source_id = 0; }
        if (ctx->mounts)  g_object_unref(ctx->mounts);
        for (auto* f : ctx->factories) { if (f) g_object_unref(f); }
        if (ctx->server)  g_object_unref(ctx->server);
        if (ctx->loop)    g_main_loop_unref(ctx->loop);
        ctx->loop = NULL; ctx->server = NULL; ctx->mounts = NULL; ctx->factories.clear();
    }

    /* ---------- API ---------- */
    void RunScreenCaptureRtspServer(const char* serverIp, const StreamConfigNative* configs, int count) {
        g_configs.clear();
        if (configs && count > 0) g_configs.assign(configs, configs + count);
        g_server_ip = serverIp ? serverIp : "";

        if (g_server_thread.joinable()) { g_printerr("RTSP server already running\n"); return; }

        g_server_thread = std::thread([]() {
            int argc = 0; char** argv = NULL;
            initialize_gstreamer(&argc, &argv, &g_ctx);
            configure_rtsp_server(&g_ctx);
            start_rtsp_server(&g_ctx);
            cleanup_resources(&g_ctx);
            g_print("서버 종료\n");
            });
    }

    void StopScreenCaptureRtspServer() {
        if (g_ctx.server) {
            gst_rtsp_server_client_filter(g_ctx.server, force_client_disconnect, NULL);
            if (GstRTSPSessionPool* pool = gst_rtsp_server_get_session_pool(g_ctx.server)) {
                gst_rtsp_session_pool_cleanup(pool);
                g_object_unref(pool);
            }
        }
        if (g_ctx.loop) {
            g_main_loop_quit(g_ctx.loop);
            if (GMainContext* c = g_main_loop_get_context(g_ctx.loop)) g_main_context_wakeup(c);
        }
        if (g_server_thread.joinable()) g_server_thread.join();
    }

} // namespace GStreamerWrapper
