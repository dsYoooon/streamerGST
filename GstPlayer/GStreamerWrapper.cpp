#include "GStreamerWrapper.h"
#include "ScreenCaptureServer.h"
#include "WorkerClient.h"

#include <vector>
#include <string>
#include <sstream>
#include <vcclr.h>
#include <gst/gstdevicemonitor.h>
#include <gst/gstdevice.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Propidl.h>

#pragma comment(lib, "gstreamer-1.0.lib")
#pragma comment(lib, "gstvideo-1.0.lib")
#pragma comment(lib, "gobject-2.0.lib")
#pragma comment(lib, "glib-2.0.lib")
#pragma comment(lib, "ole32.lib")

namespace GStreamerWrapper
{

// 필요한 .NET 네임스페이스 추가
    using namespace System;
    using namespace System::Runtime::InteropServices; // GCHandle을 위해 추가
    using namespace System::Diagnostics;             // Debug 클래스를 위해 추가
    using namespace System::Collections::Generic;
    using namespace System::Text;

    // 기본값: 폴백 OFF (GstDeviceMonitor는 플러그인/드라이버 AV 가능성)
    static bool g_useGstDeviceMonitorFallback = false;

    namespace
    {
        std::string ToUtf8String(String ^value)
        {
            if (String::IsNullOrEmpty(value))
            {
                return std::string();
            }

            array<Byte> ^utf8Bytes = Encoding::UTF8->GetBytes(value);
            std::string result;
            if (utf8Bytes->Length > 0)
            {
                pin_ptr<Byte> pinned = &utf8Bytes[0];
                result.assign(reinterpret_cast<char *>(pinned),
                    reinterpret_cast<char *>(pinned) + utf8Bytes->Length);
            }
            return result;
        }

        static WorkerClient g_worker_client;

        // ------------------------------------------------------------
        // CoreAudio helper: device enumeration (Render/Capture)
        // ------------------------------------------------------------
        static void EnumerateCoreAudio(EDataFlow flow, List<String ^> ^devices)
        {
            if (!devices) return;

            // ✅ CoInitializeEx 처리: S_OK일 때만 CoUninitialize 해야 안전
            HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool shouldCoUninitialize = (initHr == S_OK);

            if (initHr == RPC_E_CHANGED_MODE)
            {
// 이미 다른 모드(STA 등)로 초기화되어 있어도 COM 사용은 가능
                initHr = S_OK;
                shouldCoUninitialize = false;
            }

            if (FAILED(initHr))
                return;

            IMMDeviceEnumerator *enumerator = nullptr;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(hr) && enumerator != nullptr)
            {
                IMMDeviceCollection *collection = nullptr;

                hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
                if (SUCCEEDED(hr) && collection != nullptr)
                {
                    UINT count = 0;
                    if (SUCCEEDED(collection->GetCount(&count)))
                    {
                        for (UINT i = 0; i < count; ++i)
                        {
                            IMMDevice *device = nullptr;
                            if (SUCCEEDED(collection->Item(i, &device)) && device != nullptr)
                            {
                                IPropertyStore *store = nullptr;
                                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr)
                                {
                                    PROPVARIANT friendlyName;
                                    PropVariantInit(&friendlyName);

                                    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &friendlyName)))
                                    {
                                        if (friendlyName.vt == VT_LPWSTR && friendlyName.pwszVal != nullptr)
                                        {
                                            devices->Add(gcnew String(friendlyName.pwszVal));
                                        }
                                    }

                                    PropVariantClear(&friendlyName);
                                    store->Release();
                                }
                                device->Release();
                            }
                        }
                    }
                }

                if (collection)
                {
                    collection->Release();
                    collection = nullptr;
                }
                enumerator->Release();
            }

            if (shouldCoUninitialize)
            {
                CoUninitialize();
            }
        }

        static bool HasActiveCoreAudioEndpoint(EDataFlow flow)
        {
            HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool shouldCoUninitialize = (initHr == S_OK);

            if (initHr == RPC_E_CHANGED_MODE)
            {
                initHr = S_OK;
                shouldCoUninitialize = false;
            }

            if (FAILED(initHr))
                return false;

            bool ok = false;

            IMMDeviceEnumerator *enumerator = nullptr;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(hr) && enumerator != nullptr)
            {
                IMMDeviceCollection *collection = nullptr;
                hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
                if (SUCCEEDED(hr) && collection != nullptr)
                {
                    UINT count = 0;
                    if (SUCCEEDED(collection->GetCount(&count)))
                    {
                        ok = (count > 0);
                    }
                }
                if (collection) collection->Release();
                enumerator->Release();
            }

            if (shouldCoUninitialize)
            {
                CoUninitialize();
            }

            return ok;
        }

        // ------------------------------------------------------------
        // GStreamer device monitor fallback (DANGEROUS)
        // - AV가 발생할 수 있으므로 SEH로 보호
        // ------------------------------------------------------------
        static void EnumerateGstAudioSourceDevicesSafe(List<String ^> ^devices)
        {
            if (!devices) return;

            __try
            {
                GstDeviceMonitor *mon = gst_device_monitor_new();
                if (!mon)
                {
                    return;
                }

                GstCaps *caps = gst_caps_new_empty_simple("audio/x-raw");
                if (!caps)
                {
                    g_warning("Failed to create GstCaps for audio device enumeration");
                    gst_object_unref(mon);
                    return;
                }

                // ✅ 여기서 플러그인/드라이버가 터질 수 있음 → SEH로 보호
                guint filterId = gst_device_monitor_add_filter(mon, "Audio/Source", caps);
                
                if (filterId == 0)
                {
                    g_warning("Failed to add filter to GstDeviceMonitor for audio devices");
                    gst_object_unref(mon);
                    return;
                }

                if (!gst_device_monitor_start(mon))
                {
                    g_warning("Failed to start GstDeviceMonitor for audio devices");
                    gst_object_unref(mon);
                    return;
                }

                GList *devs = gst_device_monitor_get_devices(mon);
                for (GList *l = devs; l; l = l->next)
                {
                    GstDevice *d = GST_DEVICE(l->data);
                    const gchar *name = gst_device_get_display_name(d);
                    if (name)
                    {
                        devices->Add(gcnew String(name));
                    }
                    gst_object_unref(d);
                }

                if (devs)
                {
                    g_list_free(devs);
                }
                gst_caps_unref(caps);
                gst_device_monitor_stop(mon);
                gst_object_unref(mon);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // 플러그인/드라이버 Access Violation 등으로 죽는 것을 방지
                // 여기서는 "폴백 실패"로만 처리
                //Debug::WriteLine("[AudioEnum] GstDeviceMonitor fallback crashed (AV). Ignored.");
            }
        }
    }

    void GstPlayer::SetUseGstDeviceMonitorFallback(bool enable)
    {
        g_useGstDeviceMonitorFallback = enable;
    }

    void GstPlayer::Initialize()
    {
        gst_init(nullptr, nullptr);
        //gst_debug_set_default_threshold(GST_LEVEL_INFO);
        gst_debug_set_default_threshold(GST_LEVEL_WARNING);
        //gst_debug_set_default_threshold(GST_LEVEL_MEMDUMP);
    }

    static GstBusSyncReply BusSyncHandler(GstBus *bus, GstMessage *msg, gpointer data)
    {
        GstPlayer ^player = (GstPlayer ^)GCHandle::FromIntPtr(IntPtr(data)).Target;

        // 'prepare-window-handle' 메시지인지 확인
        if (gst_is_video_overlay_prepare_window_handle_message(msg))
        {
            g_print("prepare-window-handle message received\n");
            GstVideoOverlay *overlay = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg));
            gst_video_overlay_set_window_handle(overlay, (guintptr)player->videoHwnd);

            // 이 메시지는 우리가 처리했으므로 다른 핸들러로 전달하지 않고 버립니다.
            //gst_message_unref(msg);
            return GST_BUS_DROP;
        }

        // 다른 메시지들은 비동기 핸들러가 처리하도록 전달합니다.
        return GST_BUS_PASS;
    }

    void GstPlayer::Deinitialize()
    {
        StopScreenCaptureRtspServer();
        gst_deinit();
    }

    array<String ^> ^GstPlayer::GetAudioDevices()
    {
        // 기존 의미 유지: Capture(마이크) 장치 목록
        List<String ^> ^devices = gcnew List<String ^>();

        // 1) CoreAudio 우선 (안전)
        EnumerateCoreAudio(eCapture, devices);

        // 2) (선택) 폴백 - 기본 OFF, 켜더라도 AV 방어
        if (devices->Count == 0 && g_useGstDeviceMonitorFallback)
        {
            EnumerateGstAudioSourceDevicesSafe(devices);
        }

        return devices->ToArray();
    }

    array<String ^> ^GstPlayer::GetRenderDevices()
    {
        // Render(출력) 장치 목록 (loopback/UI)
        List<String ^> ^devices = gcnew List<String ^>();
        EnumerateCoreAudio(eRender, devices);

        // Render 쪽은 GstDeviceMonitor 폴백이 의미가 애매해서 기본적으로 하지 않음
        // 필요하면 동일하게 옵션으로 추가 가능

        return devices->ToArray();
    }

    bool GstPlayer::HasActiveAudioEndpoint(bool wantRender)
    {
        return HasActiveCoreAudioEndpoint(wantRender ? eRender : eCapture);
    }

    GstPlayer::GstPlayer(IntPtr windowHandle) : pipeline(nullptr)
    {
        videoHwnd = static_cast<HWND>(windowHandle.ToPointer());
    }

    GstPlayer::~GstPlayer()
    {
        this->!GstPlayer();
    }

    GstPlayer::!GstPlayer()
    {
        Stop();
    }

    void GstPlayer::StartScreenCapture(StreamConfig config)
    {
        Stop();

        pipeline = gst_pipeline_new("screen-preview");
        GstElement *src = gst_element_factory_make("d3d11screencapturesrc", "src");
        GstElement *queue = gst_element_factory_make("queue", "que");
        GstElement *conv = gst_element_factory_make("d3d11convert", "conv");
        GstElement *capsfilter = gst_element_factory_make("capsfilter", "caps");
        GstElement *sink = gst_element_factory_make("d3d11videosink", "sink");

        if (!pipeline || !src || !queue || !conv || !capsfilter || !sink)
        {
            g_printerr("파이프라인 생성 실패\n");
            if (pipeline)
            {
                gst_object_unref(pipeline); pipeline = nullptr;
            }
            return;
        }

        g_object_set(src,
            "monitor-index", config.MonitorIndex,
            "show-cursor", TRUE,

            "capture-api", 1,
            NULL);
        g_object_set(sink, "force-aspect-ratio", false, NULL);
        std::ostringstream caps_str;
        caps_str << "video/x-raw(memory:D3D11Memory),format=NV12,framerate=30/1";
        GstCaps *caps = gst_caps_from_string(caps_str.str().c_str());
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);
        //gst_debug_set_default_threshold(GST_LEVEL_INFO);
        gst_bin_add_many(GST_BIN(pipeline), src, queue, conv, capsfilter, sink, NULL);
        if (!gst_element_link_many(src, queue, conv, capsfilter, sink, NULL))
        {
            gst_debug_set_default_threshold(GST_LEVEL_ERROR);
            g_printerr("요소 연결 실패\n");
            gst_object_unref(pipeline);
            pipeline = nullptr;
            return;
        }

        gcHandle = GCHandle::Alloc(this);
        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        gst_bus_add_signal_watch(bus);
        g_signal_connect(bus, "message", G_CALLBACK(BusMessageCallback), GCHandle::ToIntPtr(gcHandle).ToPointer());
        gst_bus_set_sync_handler(bus, BusSyncHandler, GCHandle::ToIntPtr(gcHandle).ToPointer(), NULL);
        g_object_unref(bus);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    }

    void GstPlayer::StartScreenCaptureServer(String ^serverIp, array<StreamConfig> ^configs)
    {
        //Stop();

        std::string serverIpUtf8 = ToUtf8String(serverIp);

        std::vector<StreamConfigNative> nativeConfigs;
        for each (StreamConfig cfg in configs)
        {
            StreamConfigNative ncfg;
            ncfg.monitor_index = cfg.MonitorIndex;
            ncfg.crop_x = cfg.CropX;
            ncfg.crop_y = cfg.CropY;
            ncfg.crop_w = cfg.CropW;
            ncfg.crop_h = cfg.CropH;
            ncfg.width = cfg.Width;
            ncfg.height = cfg.Height;
            ncfg.framerate = cfg.Framerate;
            ncfg.port = cfg.Port;
            ncfg.bitrate_kbps = cfg.BitrateKbps;
            ncfg.keyframe_interval = cfg.KeyframeInterval;
            ncfg.enable_audio = cfg.EnableAudio;
            ncfg.enable_multicast = cfg.EnableMultiCast;
            ncfg.streamIndex = cfg.StreamIndex;
            ncfg.audio_device = ToUtf8String(cfg.AudioDevice);
            ncfg.enable_hw_accel = cfg.EnableHardwareAccel;
            ncfg.enable_osd = cfg.EnableOsd;
            ncfg.profile = ToUtf8String(cfg.Profile);
            ncfg.bitrate_control = ToUtf8String(cfg.BitrateControl);
            ncfg.overlay_text = ToUtf8String(cfg.OsdText);
            ncfg.multicast_ip = ToUtf8String(cfg.MultiCastIP);
            ncfg.multicast_iface = ToUtf8String(cfg.MultiCastInterface);
            nativeConfigs.push_back(ncfg);
        }

        std::string error;
        if (!g_worker_client.StartServer(serverIpUtf8, nativeConfigs, error))
        {
            System::Diagnostics::Debug::WriteLine("start_server failed: " + gcnew String(error.c_str()));
        }
    }

    void GstPlayer::Stop()
    {
        gst_print("GstPlayer::Stop called\n");
        if (pipeline)
        {
            gst_print("GstPlayer::Stop 1\n");
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        gst_print("GstPlayer::Stop 2\n");
        if (gcHandle.IsAllocated)
        {
            gst_print("GstPlayer::Stop 3\n");
            gcHandle.Free();
        }
        // RTSP 서버가 실행 중이면 워커 프로세스에 정지 요청을 보냅니다.
        std::string stopError;
        if (!g_worker_client.StopServer(stopError) && !stopError.empty())
        {
            System::Diagnostics::Debug::WriteLine("stop_server failed: " + gcnew String(stopError.c_str()));
        }
    }

    void GstPlayer::StopPreview()
    {
        if (pipeline)
        {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        if (gcHandle.IsAllocated)
        {
            gcHandle.Free();
        }

    }

    void GstPlayer::BusMessageCallback(GstBus *bus, GstMessage *msg, gpointer data)
    {
        GCHandle gch = GCHandle::FromIntPtr(IntPtr(data));
        GstPlayer ^player = (GstPlayer ^)gch.Target;

        player->HandleBusMessage(msg);

        /*    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS || GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                gch.Free();
            }*/
    }

    void GstPlayer::HandleBusMessage(GstMessage *msg)
    {
        switch (GST_MESSAGE_TYPE(msg))
        {
            case GST_MESSAGE_ERROR:
                {
                    GError *err;
                    gchar *debug;
                    gst_message_parse_error(msg, &err, &debug);

                    g_error_free(err);
                    g_free(debug);
                    Stop();
                    break;
                }
            case GST_MESSAGE_EOS:
                g_print("End-Of-Stream reached.");
                Stop();
                break;
            case GST_MESSAGE_ELEMENT:
                if (gst_is_video_overlay_prepare_window_handle_message(msg))
                {
                    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)), (guintptr)videoHwnd);
                }
                break;
            default:
                break;
        }
    }
}
