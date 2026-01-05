#include "GStreamerWrapper.h"
#include "ScreenCaptureServer.h"

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
    using namespace System::IO;

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

    void GstPlayer::StopWorker()
    {
        try
        {
            if (workerProcess != nullptr && !workerProcess->HasExited)
            {
                workerProcess->Kill();
                workerProcess->WaitForExit(2000);
            }
        }
        catch (Exception ^ex)
        {
            Debug::WriteLine("[GstWorker] failed to stop: " + ex->Message);
        }

        workerProcess = nullptr;

        try
        {
            if (!String::IsNullOrEmpty(workerConfigPath) && File::Exists(workerConfigPath))
            {
                File::Delete(workerConfigPath);
            }
        }
        catch (Exception ^ex)
        {
            Debug::WriteLine("[GstWorker] failed to cleanup config: " + ex->Message);
        }

        workerConfigPath = nullptr;
    }

    void GstPlayer::LaunchWorkerWithConfig(String ^configContent)
    {
        StopWorker();

        workerConfigPath = Path::Combine(Path::GetTempPath(),
            String::Format("gstworker_{0}.cfg", Guid::NewGuid().ToString("N")));
        File::WriteAllText(workerConfigPath, configContent, Encoding::UTF8);

        String ^exePath = Path::Combine(AppDomain::CurrentDomain->BaseDirectory, "GstWorker.exe");
        if (!File::Exists(exePath))
        {
            throw gcnew FileNotFoundException("GstWorker.exe not found", exePath);
        }

        ProcessStartInfo ^psi = gcnew ProcessStartInfo();
        psi->FileName = exePath;
        psi->Arguments = String::Format("--config \"{0}\"", workerConfigPath);
        psi->UseShellExecute = false;
        psi->CreateNoWindow = true;

        workerProcess = Process::Start(psi);
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
        StopWorker();
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

        StringBuilder ^sb = gcnew StringBuilder();
        sb->AppendLine("mode=preview");
        sb->AppendLine("[preview]");
        sb->AppendLine(String::Format("monitor={0}", config.MonitorIndex));
        if (config.Width > 0 && config.Height > 0)
        {
            sb->AppendLine(String::Format("width={0}", config.Width));
            sb->AppendLine(String::Format("height={0}", config.Height));
        }
        sb->AppendLine(String::Format("framerate={0}", config.Framerate > 0 ? config.Framerate : 30));
        sb->AppendLine(String::Format("window={0}", (Int64)videoHwnd));

        LaunchWorkerWithConfig(sb->ToString());
    }

    void GstPlayer::StartScreenCaptureServer(String ^serverIp, array<StreamConfig> ^configs)
    {
        StopWorker();

        StringBuilder ^sb = gcnew StringBuilder();
        sb->AppendLine("mode=server");
        if (!String::IsNullOrEmpty(serverIp))
        {
            sb->AppendLine(String::Format("server_ip={0}", serverIp));
        }

        int idx = 0;
        for each (StreamConfig cfg in configs)
        {
            sb->AppendLine(String::Format("[stream{0}]", idx++));
            sb->AppendLine(String::Format("monitor={0}", cfg.MonitorIndex));
            sb->AppendLine(String::Format("crop_x={0}", cfg.CropX));
            sb->AppendLine(String::Format("crop_y={0}", cfg.CropY));
            sb->AppendLine(String::Format("crop_w={0}", cfg.CropW));
            sb->AppendLine(String::Format("crop_h={0}", cfg.CropH));
            sb->AppendLine(String::Format("width={0}", cfg.Width));
            sb->AppendLine(String::Format("height={0}", cfg.Height));
            sb->AppendLine(String::Format("framerate={0}", cfg.Framerate));
            sb->AppendLine(String::Format("bitrate={0}", cfg.BitrateKbps));
            sb->AppendLine(String::Format("keyint={0}", cfg.KeyframeInterval));
            sb->AppendLine(String::Format("port={0}", cfg.Port));
            sb->AppendLine(String::Format("stream_index={0}", cfg.StreamIndex));
            sb->AppendLine(String::Format("enable_audio={0}", cfg.EnableAudio ? "1" : "0"));
            sb->AppendLine(String::Format("enable_multicast={0}", cfg.EnableMultiCast ? "1" : "0"));
            if (!String::IsNullOrEmpty(cfg.AudioDevice)) sb->AppendLine(String::Format("audio_device={0}", cfg.AudioDevice));
            sb->AppendLine(String::Format("enable_hw_accel={0}", cfg.EnableHardwareAccel ? "1" : "0"));
            sb->AppendLine(String::Format("enable_osd={0}", cfg.EnableOsd ? "1" : "0"));
            if (!String::IsNullOrEmpty(cfg.BitrateControl)) sb->AppendLine(String::Format("bitrate_control={0}", cfg.BitrateControl));
            if (!String::IsNullOrEmpty(cfg.Profile)) sb->AppendLine(String::Format("profile={0}", cfg.Profile));
            if (!String::IsNullOrEmpty(cfg.OsdText)) sb->AppendLine(String::Format("overlay_text={0}", cfg.OsdText));
            if (!String::IsNullOrEmpty(cfg.MultiCastIP)) sb->AppendLine(String::Format("multicast_ip={0}", cfg.MultiCastIP));
            if (!String::IsNullOrEmpty(cfg.MultiCastInterface)) sb->AppendLine(String::Format("multicast_iface={0}", cfg.MultiCastInterface));
        }

        LaunchWorkerWithConfig(sb->ToString());
    }

    void GstPlayer::Stop()
    {
        StopWorker();
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
        // RTSP 서버가 실행 중이면 중지합니다.
        StopScreenCaptureRtspServer();
    }

    void GstPlayer::StopPreview()
    {
        StopWorker();
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
