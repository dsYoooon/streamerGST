// ScreenCaptureServer.cpp (encodebin + per-option HW accel on/off, multi-vendor stable)
// Per-stream RTSP port (server-per-stream) version
// 2025-09-17:
//  - Shared GLib context / thread-pool / session-pool
//  - mtu=1200, queues tuned, h264parse always (stability-first)
//  - **FIX**: Type-safe property setter for enum/string → prevents heap corruption

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/pbutils/encoding-profile.h>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <thread>
#include <cstdio>
#include <mutex>

#include "ScreenCaptureServer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace GStreamerWrapper {

    static std::string g_server_ip;
    static std::vector<StreamConfigNative> g_configs;

    /* ---------- RTSP context: multiple servers (one per stream) ---------- */
    struct ServerEntry {
        GstRTSPServer* server = NULL;
        GstRTSPMountPoints* mounts = NULL;
        guint                source_id = 0;     // attach() id
        int                  service_port = 0;  // listening port (cfg.port)
    };

    struct RtspServerContext {
        GMainLoop* loop = NULL;                 // single mainloop
        GMainContext* main_ctx = NULL;          // shared main context
        GstRTSPThreadPool* thread_pool = NULL;  // shared worker pool
        GstRTSPSessionPool* session_pool = NULL;// shared session pool
        std::vector<ServerEntry> servers;       // N servers
        std::vector<GstRTSPMediaFactory*> factories; // N factories (1:1)
    };

    static RtspServerContext g_ctx;
    static std::thread g_server_thread;
    static std::mutex g_pipeline_build_mutex;

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

    // ========================================================================
    // ===== [추가] 안전한 프로퍼티 세터들 =====
    static gboolean set_property_enum_by_nick(GObject* obj, const char* prop, const char* nick_or_name) {
        if (!obj || !prop || !nick_or_name) return FALSE;
        GParamSpec* pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(obj), prop);
        if (!pspec) return FALSE;
        GType ptype = G_PARAM_SPEC_VALUE_TYPE(pspec);
        if (!g_type_is_a(ptype, G_TYPE_ENUM)) return FALSE;

        GEnumClass* eclass = (GEnumClass*)g_type_class_ref(ptype);
        if (!eclass) return FALSE;

        const GEnumValue* hit = NULL;
        // name 또는 nick 일치 탐색 (대소문자 무시)
        for (int i = 0; i < eclass->n_values; ++i) {
            const GEnumValue* v = &eclass->values[i];
            if ((v->value_name && g_ascii_strcasecmp(v->value_name, nick_or_name) == 0) ||
                (v->value_nick && g_ascii_strcasecmp(v->value_nick, nick_or_name) == 0)) {
                hit = v; break;
            }
        }

        gboolean ok = FALSE;
        if (hit) {
            GValue gv = G_VALUE_INIT;
            g_value_init(&gv, ptype);
            g_value_set_enum(&gv, hit->value);
            g_object_set_property(obj, prop, &gv);
            g_value_unset(&gv);
            ok = TRUE;
        }
        g_type_class_unref(eclass);
        return ok;
    }

    static gboolean set_property_string_or_enum(GObject* obj, const char* prop, const char* val) {
        if (!obj || !prop || !val) return FALSE;
        GParamSpec* pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(obj), prop);
        if (!pspec) return FALSE;
        GType ptype = G_PARAM_SPEC_VALUE_TYPE(pspec);

        if (ptype == G_TYPE_STRING) {
            GValue gv = G_VALUE_INIT;
            g_value_init(&gv, G_TYPE_STRING);
            g_value_set_string(&gv, val);
            g_object_set_property(obj, prop, &gv);
            g_value_unset(&gv);
            return TRUE;
        }
        if (g_type_is_a(ptype, G_TYPE_ENUM)) {
            return set_property_enum_by_nick(obj, prop, val);
        }
        return FALSE; // 다른 타입은 건드리지 않음
    }

    // enum/string 프로퍼티에 대한 안전한 세터 + 별칭 처리
    static gboolean set_str_or_enum_if(GObject* obj, const char* prop, const char* value)
    {
        if (!obj || !prop || !value || !*value) return FALSE;

        if (set_property_string_or_enum(obj, prop, value))
            return TRUE;

        // preset 별칭 (NV 인코더 등)
        if (!g_ascii_strcasecmp(prop, "preset")) {
            if (!g_ascii_strcasecmp(value, "llhq") || !g_ascii_strcasecmp(value, "low-latency-hq"))
                return set_property_string_or_enum(obj, prop, "llhq");
            if (!g_ascii_strcasecmp(value, "llhp") || !g_ascii_strcasecmp(value, "low-latency-hp"))
                return set_property_string_or_enum(obj, prop, "llhp");
        }

        // rate-control / rc-mode → cbr/vbr 정규화
        if (!g_ascii_strcasecmp(prop, "rc-mode") || !g_ascii_strcasecmp(prop, "rate-control")) {
            const char* nick = (g_ascii_strcasecmp(value, "vbr") == 0) ? "vbr" : "cbr";
            if (set_property_string_or_enum(obj, prop, nick))
                return TRUE;
        }

        // usage 별칭 (AMF 등)
        if (!g_ascii_strcasecmp(prop, "usage")) {
            if (!g_ascii_strcasecmp(value, "ultra-low-latency") || !g_ascii_strcasecmp(value, "ultralowlatency"))
                return set_property_string_or_enum(obj, prop, "ultra-low-latency");
        }

        return FALSE;
    }

    // (옵션) enum nick 세팅이 실패하면 숫자 fallback 도 시도하는 헬퍼
    static void set_rc_with_fallback(GObject* obj, const char* prop, const char* rc_nick,
        int vbr_val /*e.g., 0*/, int cbr_val /*e.g., 1*/) {
        if (!set_property_string_or_enum(obj, prop, rc_nick)) {
            // enum nick을 못 찾으면 값으로 시도 (플러그인마다 값이 다를 수 있어 프로젝트별로 맞춰주세요)
            int v = (g_ascii_strcasecmp(rc_nick, "vbr") == 0) ? vbr_val : cbr_val;
            g_object_set(obj, prop, v, NULL);
        }
    }


    static gboolean is_nvidia_system() {
        static gsize init_once = 0;
        static gboolean has_nv = FALSE;
        if (g_once_init_enter(&init_once)) {
            GstElementFactory* fac = gst_element_factory_find("nvh264enc");
            has_nv = (fac != NULL);
            if (fac) {
                gst_object_unref(fac);
            }
            g_once_init_leave(&init_once, 1);
        }
        return has_nv;
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

    typedef struct _MyRtspMedia {
        GstRTSPMedia parent;
    } MyRtspMedia;

    typedef struct _MyRtspMediaClass {
        GstRTSPMediaClass parent_class;
    } MyRtspMediaClass;

    G_DEFINE_TYPE(MyRtspMedia, my_rtsp_media, GST_TYPE_RTSP_MEDIA)

#ifndef MY_TYPE_RTSP_MEDIA
#define MY_TYPE_RTSP_MEDIA (my_rtsp_media_get_type())
#endif

    static gboolean my_rtsp_media_prepare_impl(GstRTSPMedia* media, GstRTSPThread* thread) {
        std::unique_lock<std::mutex> lock(g_pipeline_build_mutex);
        GstRTSPMediaClass* parent_class = GST_RTSP_MEDIA_CLASS(my_rtsp_media_parent_class);
        if (parent_class->prepare) {
            return parent_class->prepare(media, thread);
        }
        return TRUE;
    }

    static gboolean my_rtsp_media_prepare(GstRTSPMedia* media, GstRTSPThread* thread) {
        return my_rtsp_media_prepare_impl(media, thread);
    }

    static gboolean my_rtsp_media_unprepare(GstRTSPMedia* media) {
        std::unique_lock<std::mutex> lock(g_pipeline_build_mutex);
        GstRTSPMediaClass* parent_class = GST_RTSP_MEDIA_CLASS(my_rtsp_media_parent_class);
        if (parent_class->unprepare) {
            return parent_class->unprepare(media);
        }
        return TRUE;
    }

    static void my_rtsp_media_class_init(MyRtspMediaClass* klass) {
        GstRTSPMediaClass* media_class = GST_RTSP_MEDIA_CLASS(klass);
        media_class->prepare = my_rtsp_media_prepare;
        media_class->unprepare = my_rtsp_media_unprepare;
    }

    static void my_rtsp_media_init(MyRtspMedia*) {}

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
        gboolean enable_hw_accel; // 옵션 반영
        gboolean enable_osd;
        gchar* bitrate_control;
        gchar* profile;
        gboolean enable_multicast;
        // overlay
        gchar* overlay_text;
        GstElement* overlay_elem; // weak
    } MyMediaFactory;

    typedef struct _MyMediaFactoryClass {
        GstRTSPMediaFactoryClass parent_class;
    } MyMediaFactoryClass;

    G_DEFINE_TYPE(MyMediaFactory, my_media_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

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

        // 공통: 정수형은 그대로, 문자열/enum은 안전세터로
        set_int_if(elem, "bitrate", mf->v_bitrate_kbps * 1000);
        set_int_if(elem, "key-int-max", mf->keyint);
        set_int_if(elem, "gop-size", mf->keyint);
        if (mf->profile && *mf->profile) {
            set_property_string_or_enum(G_OBJECT(elem), "profile", mf->profile);
        }
        const gchar* rc = mf->bitrate_control ? mf->bitrate_control : "CBR";
        const gchar* rc_nick = (g_ascii_strcasecmp(rc, "VBR") == 0) ? "vbr" : "cbr";

        if (g_strcmp0(fname, "x264enc") == 0) {
            set_int_if(elem, "speed-preset", 1 /*ultrafast*/);
            set_int_if(elem, "tune", 0x00000004 /*zerolatency*/);
            set_bool_if(elem, "byte-stream", TRUE);
            // 일부 빌드에서 kbps 단위일 수 있어 재설정
            set_int_if(elem, "bitrate", mf->v_bitrate_kbps);
            // x264enc 의 profile 은 enum이 아니라 string인 빌드도 있으므로 위에서 이미 안전세터 사용
        }
        else if (g_strcmp0(fname, "qsvh264enc") == 0) {
            set_bool_if(elem, "lowpower", TRUE);
            // rate-control: enum (프로젝트별 빌드 값 상이) → 우선 nick으로, 실패 시 값 fallback(예: VBR=0, CBR=1)
            set_rc_with_fallback(G_OBJECT(elem), "rate-control", rc_nick, /*vbr*/0, /*cbr*/1);
            set_int_if(elem, "target-usage", 3);
            set_int_if(elem, "gop-ref-dist", 1);
        }
        else if (g_strcmp0(fname, "nvh264enc") == 0) {
            // preset/rc-mode 모두 enum인 빌드 존재 → 안전세터
            set_property_string_or_enum(G_OBJECT(elem), "preset", "llhq");
            set_property_string_or_enum(G_OBJECT(elem), "rc-mode", rc_nick);
            set_int_if(elem, "rc-lookahead", 0);
            set_int_if(elem, "b-frames", 0);
        }
        else if (g_strcmp0(fname, "amfenc_h264") == 0) {
            // usage/rate-control 모두 enum → 안전세터
            set_property_string_or_enum(G_OBJECT(elem), "usage", "ultra-low-latency");
            set_property_string_or_enum(G_OBJECT(elem), "rate-control", rc_nick);
            set_int_if(elem, "b-frames", 0);
        }
        else if (g_strcmp0(fname, "d3d11h264enc") == 0) {
            set_int_if(elem, "b-frames", 0);
            // rate-control: enum → 안전세터
            set_property_string_or_enum(G_OBJECT(elem), "rate-control", rc_nick);
        }
    }


    /* ---------- media events ---------- */
    static void on_media_unprepared(GstRTSPMedia* media, gpointer) {
        g_print("[media] unprepared\n");
        if (GstElement* e = gst_rtsp_media_get_element(media)) {
            gst_element_set_state(e, GST_STATE_NULL);
            gst_object_unref(e);
        }
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

    static gboolean link_queue_to_encoder(GstElement* queue, GstElement* upload, GstElement* encoder) {
        if (!queue || !encoder) return FALSE;
        if (upload) {
            if (!link_ok(queue, upload))
                return FALSE;
            if (!link_ok(upload, encoder)) {
                gst_element_unlink(queue, upload);
                return FALSE;
            }
            return TRUE;
        }
        return link_ok(queue, encoder);
    }

    /* ---------- create pipeline ---------- */
    static GstElement* my_media_factory_create_element(GstRTSPMediaFactory* factory, const GstRTSPUrl*) {
        MyMediaFactory* self = (MyMediaFactory*)factory;

        const gint monitor_index = self->monitor_index;
        const gint crop_x = self->crop_x, crop_y = self->crop_y, crop_w = self->crop_w, crop_h = self->crop_h;
        const gint out_w = (self->out_w & ~1), out_h = (self->out_h & ~1), fps = self->fps;
        const gint a_bps = self->a_bitrate_bps;
        const gint keyint = self->keyint > 0 ? self->keyint : fps;

        g_print("[video] HW accel option: %s\n", self->enable_hw_accel ? "ON" : "OFF");

        GstElement* bin = gst_bin_new(NULL);

        /* --- 비디오 소스 & 전처리 (D3D11 경로) --- */
        GstElement* vsrc = gst_element_factory_make("d3d11screencapturesrc", NULL);
        GstElement* vd3d = self->enable_osd ? gst_element_factory_make("d3d11convert", "vd3d11conv") : NULL; // OSD OFF면 제거
        GstElement* tover = self->enable_osd ? gst_element_factory_make("dwritetextoverlay", "overlay") : NULL;
        GstElement* vd3d2 = gst_element_factory_make("d3d11convert", "vd3d11conv2"); // NV12 변환 담당 (단 1회)

        // 폴백용(CPU 경로)
        GstElement* vdown = gst_element_factory_make("d3d11download", "vdown");
        GstElement* vconv = gst_element_factory_make("videoconvert", "vconv");

        GstElement* vq1 = gst_element_factory_make("queue", "vqueue1");

        const gboolean nv_system = self->enable_hw_accel && is_nvidia_system();
        GstElement* vdupload = NULL;
        if (nv_system) {
            vdupload = gst_element_factory_make("d3d12upload", "vd3d12upload");
            if (vdupload)
                g_print("[video] NVIDIA 시스템 감지 → d3d12upload 삽입\n");
            else
                g_warning("NVIDIA 시스템 감지되었지만 d3d12upload 생성 실패, CPU 경로로 폴백합니다.");
        }

        const gboolean try_hw_zero_copy = self->enable_hw_accel && (!nv_system || vdupload != NULL);

        // 인코더: HW on → encodebin, HW off → x264enc
        GstElement* venc = self->enable_hw_accel ?
            gst_element_factory_make("encodebin", "vencbin") :
            gst_element_factory_make("x264enc", "venc");

        GstElement* vparse = gst_element_factory_make("h264parse", "vparse");
        GstElement* vcf = gst_element_factory_make("capsfilter", "vpaycaps");
        GstElement* vq2 = gst_element_factory_make("queue", "vqueue2");
        GstElement* vpay = gst_element_factory_make("rtph264pay", "pay0");

        if (!vsrc || !vd3d2 || !vdown || !vconv || !vq1 || !venc || !vparse || !vcf || !vq2 || !vpay ||
            (self->enable_osd && (!vd3d || !tover))) {
            g_printerr("비디오 요소 생성 실패\n");
            if (bin) gst_object_unref(bin);
            return NULL;
        }

        if (self->enable_osd)
            gst_bin_add_many(GST_BIN(bin), vsrc, vd3d, tover, vd3d2, vdown, vconv, vq1, venc, vparse, vcf, vq2, vpay, NULL);
        else
            gst_bin_add_many(GST_BIN(bin), vsrc, /*vd3d X*/ /*tover X*/ vd3d2, vdown, vconv, vq1, venc, vparse, vcf, vq2, vpay, NULL);

        if (vdupload)
            gst_bin_add(GST_BIN(bin), vdupload);

        // 캡처 기본 설정
        g_object_set(vsrc,
            "monitor-index", monitor_index, "show-cursor", TRUE,
            "crop-x", crop_x, "crop-y", crop_y, "crop-width", crop_w, "crop-height", crop_h,
            "capture-api",1,
            NULL);

        // 1) OSD 경로: BGRA로 변환 후 overlay
        GstElement* prev = vsrc;
        if (self->enable_osd) {
            std::ostringstream css;
            css << "video/x-raw(memory:D3D11Memory),format=BGRA,"
                << "width=" << out_w << ",height=" << out_h
                << ",framerate=" << fps << "/1,pixel-aspect-ratio=1/1";
            if (!link_ok(vsrc, vd3d, gst_caps_from_string(css.str().c_str()))) { gst_object_unref(bin); return NULL; }

            const gchar* txt = (self->overlay_text && *self->overlay_text) ? self->overlay_text : "";
            g_object_set(tover,
                "text", txt,
                "font-family", "Segoe UI",
                "font-size", 20.0,     // gdouble
                "layout-x", 0.03, "layout-y", 0.03, "layout-width", 0.94, "layout-height", 0.94,
                "text-alignment", 0, "paragraph-alignment", 0,
                "foreground-color", 0xFFFFFFFFu, "background-color", 0x00000000u,
                "outline-color", 0x80000000u, "shadow-color", 0x80000000u,
                NULL);

            self->overlay_elem = tover;
            g_object_add_weak_pointer(G_OBJECT(tover), (gpointer*)&self->overlay_elem);
            if (!link_ok(vd3d, tover)) { gst_object_unref(bin); return NULL; }
            prev = tover;
        }

        // (공통) prev → vd3d2 에서 NV12로 1회 변환
        if (!link_ok(prev, vd3d2)) { gst_object_unref(bin); return NULL; }

        // 제로카피용 D3D11Memory NV12 caps
        GstCaps* gpu_nv12_caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "NV12",
            "width", G_TYPE_INT, out_w,
            "height", G_TYPE_INT, out_h,
            "framerate", GST_TYPE_FRACTION, fps, 1,
            "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
        gst_caps_set_features(gpu_nv12_caps, 0, gst_caps_features_new("memory:D3D11Memory", NULL));

        gboolean zero_copy_ok = FALSE;

        // 안정성: HW 가속 시에도 파서 사용 (AU 정렬/헤더 보장)
        const gboolean NO_PARSE_HW = FALSE;

        // HW 가속이면: encodebin + 제로카피 우선 시도
        gboolean attempted_zero_copy = FALSE;
        if (try_hw_zero_copy) {
            attempted_zero_copy = TRUE;
            std::ostringstream pcaps;
            pcaps << "video/x-h264,profile=" << (self->profile ? self->profile : "high");
            GstCaps* h264_caps = gst_caps_from_string(pcaps.str().c_str());
            GstCaps* restr = gst_caps_copy(gpu_nv12_caps); // NV12 + D3D11Memory
            GstEncodingProfile* vprof = (GstEncodingProfile*)
                gst_encoding_video_profile_new(h264_caps, NULL, restr, 1);
            g_object_set(venc, "profile", vprof, NULL);
            gst_encoding_profile_unref(vprof);
            gst_caps_unref(h264_caps);

            // encodebin 내부로 선택된 실제 인코더 튜닝
            g_signal_connect(venc, "element-added", G_CALLBACK(on_encodebin_element_added), self);

            // ★ 제로카피 경로: vd3d2 -> vq1 (D3D11 NV12) -> encodebin
            if (link_ok(vd3d2, vq1, gst_caps_ref(gpu_nv12_caps)) &&
                link_queue_to_encoder(vq1, vdupload, venc)) {
                zero_copy_ok = TRUE;
                g_print("[video] Zero-copy D3D11Memory→%sencodebin 활성화\n",
                    vdupload ? "d3d12upload→" : "");
            }
            else {
                gst_element_unlink(vd3d2, vq1);
            }
        }

        // CPU 폴백 경로 (HW off이거나 zero-copy 실패 시)
        if (!try_hw_zero_copy || !zero_copy_ok) {
            // vd3d2 → vdown(D3D11 NV12) → vconv(CPU NV12) → vq1
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
                if (!link_queue_to_encoder(vq1, vdupload, venc)) { gst_object_unref(bin); return NULL; }
            }
            else {
                // 소프트웨어 x264enc 세팅
                g_object_set(venc,
                    "bitrate", self->v_bitrate_kbps,     // x264: kbps
                    "key-int-max", keyint,
                    "gop-size", keyint,
                    "speed-preset", 1 /*ultrafast*/,
                    "tune", 0x00000004 /*zerolatency*/,
                    "byte-stream", TRUE,
                    NULL);
                set_str_or_enum_if(G_OBJECT(venc), "profile", self->profile ? self->profile : "high");
                if (!link_queue_to_encoder(vq1, NULL, venc)) { gst_object_unref(bin); return NULL; }
            }
        }

        if (attempted_zero_copy && !zero_copy_ok) {
            g_print("[video] Zero-copy 링크 실패 → CPU 폴백으로 전환\n");
        }

        // 파서 경로 (기본)
        gboolean linked_after_enc = FALSE;
        g_object_set(vparse, "config-interval", 1, NULL);
        if (self->enable_hw_accel && NO_PARSE_HW) {
            if (link_ok(venc, vcf)) {
                linked_after_enc = TRUE;
                g_print("[video] h264parse 생략 경로 사용\n");
            }
        }
        if (!linked_after_enc) {
            if (!link_ok(venc, vparse)) { gst_object_unref(bin); return NULL; }
            if (!link_ok(vparse, vcf)) { gst_object_unref(bin); return NULL; }
        }

        // RTP caps & queues
        {
            std::ostringstream paystr;
            paystr << "video/x-h264,stream-format=byte-stream,alignment=au,profile=(string)"
                << (self->profile ? self->profile : "high");
            GstCaps* paycaps = gst_caps_from_string(paystr.str().c_str());
            g_object_set(vcf, "caps", paycaps, NULL);
            gst_caps_unref(paycaps);
        }
        // queue 튜닝: 얕되 약간의 여유
        g_object_set(vq1, "leaky", 2, "max-size-buffers", 15, "max-size-bytes", 0, "max-size-time", (gint64)0, NULL);
        g_object_set(vq2, "leaky", 2, "max-size-buffers", 8, "max-size-bytes", 0, "max-size-time", (gint64)0, NULL);

        // MTU 1200 (조각화 회피)
        g_object_set(vpay, "pt", 96, "config-interval", 1, "mtu", 1200, NULL);

        if (!link_ok(vcf, vq2)) { gst_object_unref(bin); return NULL; }
        if (!link_ok(vq2, vpay)) { gst_object_unref(bin); return NULL; }

        /* --- 오디오 (선택) --- */
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

                g_object_set(aq1, "leaky", 2, "max-size-buffers", 20, "max-size-bytes", 0,
                    "max-size-time", (gint64)0, NULL);
                g_object_set(aq2, "leaky", 2, "max-size-buffers", 12, "max-size-bytes", 0,
                    "max-size-time", (gint64)0, NULL);

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

        if (gpu_nv12_caps) gst_caps_unref(gpu_nv12_caps);
        return bin;
    }

    /* ---------- finalize ---------- */
    static void my_media_factory_constructed(GObject* object) {
        if (G_OBJECT_CLASS(my_media_factory_parent_class)->constructed) {
            G_OBJECT_CLASS(my_media_factory_parent_class)->constructed(object);
        }
        gst_rtsp_media_factory_set_media_gtype(GST_RTSP_MEDIA_FACTORY(object), MY_TYPE_RTSP_MEDIA);
    }
    static void my_media_factory_finalize(GObject* object) {
        MyMediaFactory* self = (MyMediaFactory*)object;
        if (self->overlay_elem) {
            g_object_remove_weak_pointer(G_OBJECT(self->overlay_elem), (gpointer*)&self->overlay_elem);
            self->overlay_elem = NULL;
        }
        if (self->overlay_text) { g_free(self->overlay_text);    self->overlay_text = NULL; }
        if (self->audio_device) { g_free(self->audio_device);    self->audio_device = NULL; }
        if (self->bitrate_control) { g_free(self->bitrate_control); self->bitrate_control = NULL; }
        if (self->profile) { g_free(self->profile);         self->profile = NULL; }
        G_OBJECT_CLASS(my_media_factory_parent_class)->finalize(object);
    }
    static void my_media_factory_class_init(MyMediaFactoryClass* klass) {
        GstRTSPMediaFactoryClass* mklass = GST_RTSP_MEDIA_FACTORY_CLASS(klass);
        mklass->create_element = my_media_factory_create_element;
        GObjectClass* gklass = G_OBJECT_CLASS(klass);
        gklass->constructed = my_media_factory_constructed;
        gklass->finalize = my_media_factory_finalize;
    }
    static void my_media_factory_init(MyMediaFactory* self) {
        self->overlay_text = g_strdup("");
        self->overlay_elem = NULL;
        self->crop_x = self->crop_y = self->crop_w = self->crop_h = 0;
        self->audio_device = NULL;
        self->enable_hw_accel = TRUE;  // 기본값 (구조체에서 덮임)
        self->enable_osd = TRUE;
        self->keyint = 0;
        self->bitrate_control = g_strdup("CBR");
        self->profile = g_strdup("high");
    }

    /* ---------- factory build / per-stream server ---------- */
    static GstRTSPMediaFactory* create_factory_from_config(const StreamConfigNative& cfg, int stream_index_1based) {
        MyMediaFactory* f = (MyMediaFactory*)g_object_new(my_media_factory_get_type(), NULL);

        f->monitor_index = cfg.monitor_index;
        f->crop_x = cfg.crop_x;  f->crop_y = cfg.crop_y;  f->crop_w = cfg.crop_w;  f->crop_h = cfg.crop_h;
        f->out_w = cfg.width > 0 ? cfg.width : 1920;
        f->out_h = cfg.height > 0 ? cfg.height : 1080;
        f->fps = cfg.framerate > 0 ? cfg.framerate : 30;
        f->v_bitrate_kbps = cfg.bitrate_kbps > 0 ? cfg.bitrate_kbps : 8000;
        f->a_bitrate_bps = 128000;
        f->enable_audio = cfg.enable_audio;
        if (!cfg.audio_device.empty()) f->audio_device = g_strdup(cfg.audio_device.c_str());
        f->enable_hw_accel = cfg.enable_hw_accel;
        f->enable_osd = cfg.enable_osd;
        f->keyint = cfg.keyframe_interval > 0 ? cfg.keyframe_interval : f->fps;
        f->enable_multicast = cfg.enable_multicast;
        if (f->bitrate_control) g_free(f->bitrate_control);
        f->bitrate_control = g_strdup(cfg.bitrate_control.empty() ? "CBR" : cfg.bitrate_control.c_str());

        if (f->profile) g_free(f->profile);
        f->profile = g_strdup(cfg.profile.empty() ? "high" : cfg.profile.c_str());

        if (f->overlay_text) g_free(f->overlay_text);
        if (!cfg.overlay_text.empty())
            f->overlay_text = g_strdup(cfg.overlay_text.c_str());
        else {
            std::ostringstream def; def << "Screen " << stream_index_1based;
            f->overlay_text = g_strdup(def.str().c_str());
        }

        gst_rtsp_media_factory_set_shared(GST_RTSP_MEDIA_FACTORY(f), TRUE);
        gst_rtsp_media_factory_set_suspend_mode(GST_RTSP_MEDIA_FACTORY(f), GST_RTSP_SUSPEND_MODE_NONE);
        g_signal_connect(f, "media-configure", G_CALLBACK(on_media_configure), NULL);

        // 서버단 레이턴시(여유)
        gst_rtsp_media_factory_set_latency(GST_RTSP_MEDIA_FACTORY(f), 300);

        // 프로토콜/멀티캐스트 (운영 정책에 따라 조정)
        if (f->enable_multicast) {
            gst_rtsp_media_factory_set_protocols(GST_RTSP_MEDIA_FACTORY(f),
                (GST_RTSP_LOWER_TRANS_UDP_MCAST));
              gst_rtsp_media_factory_set_multicast_iface(GST_RTSP_MEDIA_FACTORY(f), g_server_ip.c_str());
        

            // 멀티캐스트 주소/포트 풀 (예시: 239.255.10.(11+N), base_port=15000+N*20)
        const int base_octet = 11 + stream_index_1based;
        const int base_port = 15000 + stream_index_1based * 20;
        std::ostringstream ip; ip << "239.255.10." << base_octet;
        GstRTSPAddressPool* pool = gst_rtsp_address_pool_new();
        gst_rtsp_address_pool_add_range(pool, ip.str().c_str(), ip.str().c_str(), base_port, base_port + 19, 16);
        gst_rtsp_media_factory_set_address_pool(GST_RTSP_MEDIA_FACTORY(f), pool);
        g_object_unref(pool);
        }
        else {
            gst_rtsp_media_factory_set_protocols(GST_RTSP_MEDIA_FACTORY(f),
               (GstRTSPLowerTrans) (GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));
  
        }
      

        return GST_RTSP_MEDIA_FACTORY(f);
    }

    /* ---------- init / configure / start / cleanup ---------- */
    static void initialize_gstreamer(int* argc, char*** argv, RtspServerContext* ctx) {
        gst_init(argc, argv);
        ctx->loop = g_main_loop_new(NULL, FALSE);
        ctx->main_ctx = g_main_loop_get_context(ctx->loop); // shared context

        // 공유 ThreadPool 생성 (환경에 맞게 8~32)
        ctx->thread_pool = gst_rtsp_thread_pool_new();
        gst_rtsp_thread_pool_set_max_threads(ctx->thread_pool, 32);

        // 공유 SessionPool 생성
        ctx->session_pool = gst_rtsp_session_pool_new();
        gst_rtsp_session_pool_set_max_sessions(ctx->session_pool, 32);
    }

    static void configure_rtsp_server(RtspServerContext* ctx) {
        ctx->factories.reserve(g_configs.size());
        ctx->servers.reserve(g_configs.size());

        for (size_t i = 0; i < g_configs.size(); ++i) {
            const auto& cfg = g_configs[i];

            // 1) 팩토리 생성
            GstRTSPMediaFactory* f = create_factory_from_config(cfg, static_cast<int>(i + 1));
            ctx->factories.push_back(f);

            // 2) 스트림별 서버 생성
            ServerEntry se;
            se.server = gst_rtsp_server_new();
            se.service_port = (cfg.port > 0 ? cfg.port : 10554);

            // 공유 pool 지정
            gst_rtsp_server_set_thread_pool(se.server, ctx->thread_pool);
            gst_rtsp_server_set_session_pool(se.server, ctx->session_pool);

            gst_rtsp_server_set_address(se.server, g_server_ip.c_str());

            char svc[16] = { 0 };
            _snprintf_s(svc, _TRUNCATE, "%d", se.service_port);
            gst_rtsp_server_set_service(se.server, svc);

            g_signal_connect(se.server, "client-connected", G_CALLBACK(client_connected_callback), ctx);

            // 3) 마운트
            se.mounts = gst_rtsp_server_get_mount_points(se.server);
            std::ostringstream mount; mount << "/screen" << (cfg.port - 10553);
            gst_rtsp_mount_points_add_factory(se.mounts, mount.str().c_str(), f);

            ctx->servers.push_back(se);

            g_print("  - mount: rtsp://%s:%d%s\n",
                g_server_ip.c_str(), se.service_port, mount.str().c_str());
        }
    }

    static bool start_rtsp_server(RtspServerContext* ctx) {
        // 모든 서버를 공유 GLib 컨텍스트에 attach
        for (auto& se : ctx->servers) {
            se.source_id = gst_rtsp_server_attach(se.server, ctx->main_ctx);
            if (se.source_id == 0) {
                g_printerr("RTSP 서버 attach 실패 (port=%d)\n", se.service_port);
                return false;
            }
            g_print("RTSP 서버가 시작되었습니다. 예: rtsp://%s:%d/screen1\n",
                g_server_ip.c_str(), se.service_port);
        }
        g_main_loop_run(ctx->loop);
        return true;
    }

    static void cleanup_resources(RtspServerContext* ctx) {
        g_print("5. 리소스 해제...\n");

        // detach
        for (auto& se : ctx->servers) {
            if (se.source_id != 0) {
                g_source_remove(se.source_id);
                se.source_id = 0;
            }
        }
        // mounts/server unref
        for (auto& se : ctx->servers) {
            if (se.mounts) { g_object_unref(se.mounts); se.mounts = NULL; }
            if (se.server) { g_object_unref(se.server); se.server = NULL; }
        }
        ctx->servers.clear();

        // factory unref
        for (auto* f : ctx->factories) {
            if (f) g_object_unref(f);
        }
        ctx->factories.clear();

        // 공유 풀/루프 해제
        if (ctx->thread_pool) { g_object_unref(ctx->thread_pool);  ctx->thread_pool = NULL; }
        if (ctx->session_pool) { g_object_unref(ctx->session_pool); ctx->session_pool = NULL; }
        if (ctx->loop) { g_main_loop_unref(ctx->loop);      ctx->loop = NULL; }
        ctx->main_ctx = NULL;
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

    // g_main_context_invoke()로 호출되어 RTSP 서버 쓰레드에서만 실행됩니다.
    static gboolean stop_server_on_main_loop(gpointer /*user_data*/)
    {
        for (auto& se : g_ctx.servers) {
            if (se.server) {
                gst_rtsp_server_client_filter(se.server, force_client_disconnect, NULL);
            }
        }
        if (g_ctx.session_pool) {
            gst_rtsp_session_pool_cleanup(g_ctx.session_pool);
        }
        if (g_ctx.loop) {
            g_main_loop_quit(g_ctx.loop);
        }

        return G_SOURCE_REMOVE;
    }

    void StopScreenCaptureRtspServer() {
        if (!g_server_thread.joinable()) {
            return;
        }

        if (g_ctx.main_ctx) {
            g_main_context_invoke_full(
                g_ctx.main_ctx,
                G_PRIORITY_HIGH,
                stop_server_on_main_loop,
                NULL,
                NULL);

            g_main_context_wakeup(g_ctx.main_ctx);
        }
        else if (g_ctx.loop) {
            g_main_loop_quit(g_ctx.loop);
        }

        if (g_server_thread.joinable()) {
            g_server_thread.join();
        }
    }

} // namespace GStreamerWrapper
