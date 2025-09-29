
#include "GStreamerWrapper.h"
#include "ScreenCaptureServer.h"

#include <vector>
#include <sstream>
#include <msclr/marshal_cppstd.h>
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

namespace GStreamerWrapper {

    // 필요한 .NET 네임스페이스 추가
    using namespace System;
    using namespace System::Runtime::InteropServices; // GCHandle을 위해 추가
    using namespace System::Diagnostics;             // Debug 클래스를 위해 추가
    using namespace System::Collections::Generic;

    void GstPlayer::Initialize()
    {
        gst_init(nullptr, nullptr);
        //gst_debug_set_default_threshold(GST_LEVEL_INFO);
        //gst_debug_set_default_threshold(GST_LEVEL_WARNING);
    }
    static GstBusSyncReply BusSyncHandler(GstBus* bus, GstMessage* msg, gpointer data)
    {
        GstPlayer^ player = (GstPlayer^)GCHandle::FromIntPtr(IntPtr(data)).Target;

        // 'prepare-window-handle' 메시지인지 확인
        if (gst_is_video_overlay_prepare_window_handle_message(msg)) {
            g_print("prepare-window-handle message received\n");
            GstVideoOverlay* overlay = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg));
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

    array<String^>^ GstPlayer::GetAudioDevices()
    {
        List<String^>^ devices = gcnew List<String^>();

        bool shouldCoUninitialize = false;
        HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(initHr)) {
            shouldCoUninitialize = true;
        }
        else if (initHr == RPC_E_CHANGED_MODE) {
            initHr = S_OK;
        }

        if (SUCCEEDED(initHr)) {
            IMMDeviceEnumerator* enumerator = nullptr;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(hr) && enumerator != nullptr) {
                IMMDeviceCollection* collection = nullptr;
                hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
                if (SUCCEEDED(hr) && collection != nullptr) {
                    UINT count = 0;
                    if (SUCCEEDED(collection->GetCount(&count))) {
                        for (UINT i = 0; i < count; ++i) {
                            IMMDevice* device = nullptr;
                            if (SUCCEEDED(collection->Item(i, &device)) && device != nullptr) {
                                IPropertyStore* store = nullptr;
                                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
                                    PROPVARIANT friendlyName;
                                    PropVariantInit(&friendlyName);
                                    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &friendlyName))) {
                                        if (friendlyName.vt == VT_LPWSTR && friendlyName.pwszVal != nullptr) {
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
                if (collection) {
                    collection->Release();
                    collection = nullptr;
                }
                enumerator->Release();
            }
        }

        if (shouldCoUninitialize) {
            CoUninitialize();
        }

        if (devices->Count > 0) {
            //return devices->ToArray();
        }
        else {
            return devices->ToArray();
        }

        GstDeviceMonitor* mon = gst_device_monitor_new();
        if (!mon) {
            return devices->ToArray();
        }

        GstCaps* caps = gst_caps_new_empty_simple("audio/x-raw");
        if (!caps) {
            g_warning("Failed to create GstCaps for audio device enumeration");
            gst_object_unref(mon);
            return devices->ToArray();
        }

        guint filterId = gst_device_monitor_add_filter(mon, "Audio/Source", caps);
        gst_caps_unref(caps);
        if (filterId == 0) {
            g_warning("Failed to add filter to GstDeviceMonitor for audio devices");
            gst_object_unref(mon);
            return devices->ToArray();
        }

        if (!gst_device_monitor_start(mon)) {
            g_warning("Failed to start GstDeviceMonitor for audio devices");
            gst_object_unref(mon);
            return devices->ToArray();
        }

        GList* devs = gst_device_monitor_get_devices(mon);
        for (GList* l = devs; l; l = l->next) {
            GstDevice* d = GST_DEVICE(l->data);
            const gchar* name = gst_device_get_display_name(d);
            if (name) {
                devices->Add(gcnew String(name));
            }
            gst_object_unref(d);
        }

        if (devs) {
            g_list_free(devs);
        }

        gst_device_monitor_stop(mon);
        gst_object_unref(mon);

        return devices->ToArray();
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
        GstElement* src = gst_element_factory_make("d3d11screencapturesrc", "src");
        GstElement* conv = gst_element_factory_make("d3d11convert", "conv");
        GstElement* capsfilter = gst_element_factory_make("capsfilter", "caps");
        GstElement* sink = gst_element_factory_make("d3d11videosink", "sink");

        if (!pipeline || !src || !conv || !capsfilter || !sink) {
            g_printerr("파이프라인 생성 실패\n");
            if (pipeline) { gst_object_unref(pipeline); pipeline = nullptr; }
            return;
        }

        g_object_set(src,
            "monitor-index", config.MonitorIndex,
            "show-cursor", TRUE,
   
            "capture-api", 1,
            NULL);
        g_object_set(sink, "force-aspect-ratio",false , NULL);
        std::ostringstream caps_str;
        caps_str << "video/x-raw(memory:D3D11Memory),format=NV12,framerate=30/1";
        GstCaps* caps = gst_caps_from_string(caps_str.str().c_str());
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);
        gst_debug_set_default_threshold(GST_LEVEL_INFO);
        gst_bin_add_many(GST_BIN(pipeline), src, conv, capsfilter, sink, NULL);
        if (!gst_element_link_many(src, conv, capsfilter, sink, NULL)) {
            gst_debug_set_default_threshold(GST_LEVEL_ERROR);
            g_printerr("요소 연결 실패\n");
            gst_object_unref(pipeline);
            pipeline = nullptr;
            return;
        }

        gcHandle = GCHandle::Alloc(this);
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        gst_bus_add_signal_watch(bus);
        g_signal_connect(bus, "message", G_CALLBACK(BusMessageCallback), GCHandle::ToIntPtr(gcHandle).ToPointer());
        gst_bus_set_sync_handler(bus, BusSyncHandler, GCHandle::ToIntPtr(gcHandle).ToPointer(), NULL);
        g_object_unref(bus);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    }

    void GstPlayer::StartScreenCaptureServer(String^ serverIp, array<StreamConfig>^ configs)
    {
        Stop();

        IntPtr ipPtr = Marshal::StringToHGlobalAnsi(serverIp);

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
            if (cfg.AudioDevice != nullptr)
                ncfg.audio_device = msclr::interop::marshal_as<std::string>(cfg.AudioDevice);
            ncfg.enable_hw_accel = cfg.EnableHardwareAccel;
            ncfg.enable_osd = cfg.EnableOsd;
            if (cfg.Profile != nullptr)
                ncfg.profile = msclr::interop::marshal_as<std::string>(cfg.Profile);
            if (cfg.BitrateControl != nullptr)
                ncfg.bitrate_control = msclr::interop::marshal_as<std::string>(cfg.BitrateControl);
            if (cfg.OsdText != nullptr)
                 ncfg.overlay_text = msclr::interop::marshal_as<std::string>(cfg.OsdText);
            if (cfg.MultiCastIP != nullptr)
                ncfg.multicast_ip = msclr::interop::marshal_as<std::string>(cfg.MultiCastIP);
            if (cfg.MultiCastInterface != nullptr)
                ncfg.multicast_iface = msclr::interop::marshal_as<std::string>(cfg.MultiCastInterface);
            nativeConfigs.push_back(ncfg);
        }

        RunScreenCaptureRtspServer(static_cast<const char*>(ipPtr.ToPointer()),
                                   nativeConfigs.data(),
                                   (int)nativeConfigs.size());
        Marshal::FreeHGlobal(ipPtr);
    }

    void GstPlayer::Stop()
    {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        if (gcHandle.IsAllocated) {
            gcHandle.Free();
        }
        // RTSP 서버가 실행 중이면 중지합니다.
        StopScreenCaptureRtspServer();
    }

    void GstPlayer::BusMessageCallback(GstBus* bus, GstMessage* msg, gpointer data)
    {
        GCHandle gch = GCHandle::FromIntPtr(IntPtr(data));
        GstPlayer^ player = (GstPlayer^)gch.Target;

        player->HandleBusMessage(msg);

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS || GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gch.Free();
        }
    }

    void GstPlayer::HandleBusMessage(GstMessage* msg)
    {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar* debug;
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
            if (gst_is_video_overlay_prepare_window_handle_message(msg)) {
                gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)), (guintptr)videoHwnd);
            }
            break;
        default:
            break;
        }
    }
}