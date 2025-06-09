#pragma once
#include <msclr/marshal_cppstd.h>
#include "SourceManager.h"
#include <mutex> // C++11 이상 표준 라이브러리
using namespace System;

namespace GStreamerWrapperCLI {

    public ref class SourceManagerWrapper {
    public:
        static IntPtr GetOrCreateSource(String^ rtspUrl) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            return IntPtr(pipeline);
        }
        // SSP의 bin 반환
        static IntPtr GetBin(String^ rtspUrl) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            return IntPtr(pipeline->GetPipeline());
        }
        static void ReleaseSource(String^ rtspUrl) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SourceManager::Instance().ReleaseSource(url);
        }
        static void CheckTee(String^ rtspUrl) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            if (pipeline) {
                pipeline->CheckPipeline();
            }
        }

        static IntPtr GetRequestPad(String^ rtspUrl) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);

            GstPad* pad = pipeline->GetRequestPad();
            return IntPtr(pad);
        }
        static void SetReady(String^ rtspUrl) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            
            if (!pipeline) {
                g_printerr("Failed to get SSP for URL: %s\n", url.c_str());
                return;
            }
            GstElement* sspPipeline = pipeline->GetPipeline();
            if (!sspPipeline) {
                g_printerr("SSP pipeline is null for URL: %s\n", url.c_str());
                return;
            }
            
            gst_element_set_state(sspPipeline, GST_STATE_READY);
            
                //pipeline->SetReady();
        }
        static void SetPlay(String^ rtspUrl) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            
            if (!pipeline) {
                g_printerr("Failed to get SSP for URL: %s\n", url.c_str());
                return ;
            }
            GstElement* sspPipeline = pipeline->GetPipeline();
            if (!sspPipeline) {
                g_printerr("SSP pipeline is null for URL: %s\n", url.c_str());
                return ;
            }
            
            gst_element_set_state(sspPipeline, GST_STATE_PLAYING);
            
                //pipeline->SetPlay();
        }
        static void setInfo() {
            gst_debug_set_default_threshold(GST_LEVEL_INFO);
        } 
        static void setWarning() {
            gst_debug_set_default_threshold(GST_LEVEL_WARNING);
        }
        // 새롭게 수정된 AttachConsumerBin: sinkBinPtr는 새 sink bin (Consumer bin)이고, useRTSPBranch에 따라 RTSP용 또는 VideoTest용 분기를 선택함.
        static bool AttachConsumerBin(String^ rtspUrl, IntPtr sinkBinPtr, bool useRTSPBranch) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            if (!pipeline) {
                g_printerr("Failed to get SSP for URL: %s\n", url.c_str());
                return false;
            }
            GstElement* sspPipeline = pipeline->GetPipeline();
            if (!sspPipeline) {
                g_printerr("SSP pipeline is null for URL: %s\n", url.c_str());
                return false;
            }
            GstElement* sinkBin = static_cast<GstElement*>(sinkBinPtr.ToPointer());
            if (!sinkBin) {
                g_printerr("Sink bin pointer is null.\n");
                return false;
            }
            //GstElement* sink = gst_bin_get_by_name(GST_BIN(sinkBin), "d3d11videosink");
            g_printerr("attach for URL: %s\n", url.c_str());
            // URL을 통한 소스타입 판단
            bool isRtsp = (url.find("rtsp://") != std::string::npos);
            bool isVideo = (url.find("video://") != std::string::npos);
            bool isFake = (url.find("fakesrc://") != std::string::npos);
            bool isImage = (url.find("image://") != std::string::npos);
            bool isCapture = (url.find("capture://") 
                != std::string::npos);
            //if (!isFake)
                //gst_element_set_state(sinkBin, GST_STATE_NULL);
            //const char* teeName = nullptr;
            //const char* branchSinkName = nullptr;
            //const char* inName = nullptr;
            const gchar* tee_name = nullptr;
            const gchar* ghost_pad = nullptr;

            const gchar* sel = nullptr;
            if (isRtsp ) {
             
                tee_name = "tee_rtsp_h264";
                ghost_pad = "sink_rtsp_h264";
                sel = "request_pad_rtsp_h264";
            }
            else if (isVideo) {
                //gst_element_set_state(sinkBin, GST_STATE_READY);
                tee_name = "tee_file_h264";
                ghost_pad = "sink_file_h264";
                sel = "request_pad_file_h264";
            }
                
            else if ( isImage) {
                tee_name = "tee_file_image";
                ghost_pad = "sink_file_image";
                sel = "request_pad_file_image";
            }
                
            else {
                tee_name = "tee_video_test"; // 기본값
                ghost_pad = "sink_video_test";
                sel = "request_pad_video_test";
            }
           

            // sinkBin이 파이프라인에 추가되지 않았다면 추가
            const gchar* sinkBinName = gst_element_get_name(sinkBin);
            g_print("%s", sinkBinName);
            if (!gst_bin_get_by_name(GST_BIN(sspPipeline), sinkBinName)) {
                gst_bin_add(GST_BIN(sspPipeline), sinkBin);
                gst_element_sync_state_with_parent(sinkBin);
            }
            //gst_debug_set_default_threshold(GST_LEVEL_INFO);
            // 사용 분기에 따라 tee 이름 결정 
            GstElement* tee = gst_bin_get_by_name(GST_BIN(sspPipeline), tee_name);
            if (!tee) {
                g_printerr("AttachConsumerBin: Unable to find tee element in pipeline\n");
                return false;
            }

            // AttachConsumerBin 함수 내 이전 티 패드 해제 부분 (URL 기준 판별)
            GstPad* old_pad = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), "last_tee_pad");
            if (old_pad) {
                // URL에 "video://" 또는 "rtsp://"가 포함된 경우에만 동적 패드로 간주하고 해제
                if (url.find("video://") != std::string::npos || url.find("rtsp://") != std::string::npos) {
                    g_print("AttachConsumerBin: Releasing previous tee pad\n");
                    gst_element_release_request_pad(tee, old_pad);
                }
                g_object_set_data(G_OBJECT(sinkBin), "last_tee_pad", NULL);
                //gst_object_unref(old_pad);
            }
            
            // 새로운 tee 패드 요청
            GstPad* tee_req = gst_element_request_pad_simple(tee, "src_%u");
            if (!tee_req) {
                g_printerr("AttachConsumerBin: Failed to request pad from tee\n");
                gst_object_unref(tee);
                return false;
            }
            g_object_set_data(G_OBJECT(sinkBin), "last_tee_pad", tee_req);
            // 6) teeReq → consumerBin ghost-pad 직접 연결
            GstPad* sinkPad = gst_element_get_static_pad(sinkBin, ghost_pad);
            if (!sinkPad) {
                gst_element_release_request_pad(tee, tee_req);
                gst_object_unref(tee);
                return false;
            }
            if (gst_pad_link(tee_req, sinkPad) != GST_PAD_LINK_OK) {
                gst_object_unref(sinkPad);
                gst_element_release_request_pad(tee, tee_req);
                gst_object_unref(tee);
                return false;
            }
            gst_object_unref(sinkPad);

            // input-selector에서 활성화할 패드 설정 (여기서는 기존 코드 그대로)
            GstElement* input_selector = gst_bin_get_by_name(GST_BIN(sinkBin), "input_selector");
            if (input_selector) {
                GstPad* active_in = (GstPad*)g_object_get_data(G_OBJECT(sinkBin),sel);
                if (active_in) {
                    g_object_set(input_selector, "active-pad", active_in, NULL);
                }
                gst_object_unref(input_selector);
            }
            //if(!isFake)
            gst_element_set_state(sspPipeline, GST_STATE_PLAYING);
            gst_element_sync_state_with_parent(sinkBin);
   
            g_print("AttachConsumerBin: Consumer bin successfully attached to %s pipeline.\n",
                useRTSPBranch ? "RTSP" : "VideoTest");
            gst_object_unref(tee);
            //gst_object_unref(sink);
            if (isVideo) {
                //Sleep(100);
                gst_element_seek_simple(GST_ELEMENT(sspPipeline), GST_FORMAT_TIME,  (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
                
            }
            //gst_object_unref(sink);
           // gst_object_unref(sinkBin);

      
         
            return true;
        }
        static bool IsConsumerBinAttached(String^ rtspUrl, IntPtr sinkBinPtr) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            if (!pipeline) {
                return false;
            }
            GstElement* sspPipeline = pipeline->GetPipeline();
            GstElement* sinkBin = static_cast<GstElement*>(sinkBinPtr.ToPointer());
            if (!sspPipeline || !sinkBin) {
                return false;
            }
            
            // sinkBin이 이미 sspPipeline에 존재하는지 이름으로 확인합니다.
            const gchar* sinkBinName = gst_element_get_name(sinkBin);
            GstElement* found = gst_bin_get_by_name(GST_BIN(sspPipeline), sinkBinName);
            bool result = (found != NULL);
            if (found) {
                gst_object_unref(found);
            }
            //gst_object_unref(sinkBin);
            return result;
        }
        static void print_parent_pipeline_name(IntPtr sinkBinPtr) {
            GstElement* bin = static_cast<GstElement*>(sinkBinPtr.ToPointer());
            if (!bin) return;
            GstObject* parent = gst_object_get_parent(GST_OBJECT(bin));
            if (!parent) {
                g_print("[%s] has no parent.\n", gst_element_get_name(bin));
                return;
            }
            // 부모가 GstElement인지 확인 후 이름 출력
            const gchar* parent_name = gst_object_get_name(parent);
            g_print("Element '%s' parent pipeline/bin: '%s'\n",
                gst_element_get_name(bin), parent_name);
            gst_object_unref(parent);
        }

        static bool DetachConsumerBin(IntPtr sinkBinPtr) {
            GstElement* sinkBin = static_cast<GstElement*>(sinkBinPtr.ToPointer());
            if (!sinkBin) {
                g_printerr("AutoDetachConsumerBin: Sink bin is null.\n");
                return false;
            }
            //gst_debug_set_threshold_for_name("GST_REFCOUNTING", GST_LEVEL_MEMDUMP);
            //gst_element_set_state(sinkBin, GST_STATE_NULL);



            // tee request-pad 해제

            g_object_set_data(G_OBJECT(sinkBin), "last_tee_pad", nullptr);

            // input-selector 비활성화

            {
                const char* ghost_names[] = {
                    "sink_rtsp_h264",
                    "sink_file_h264",
                    "sink_file_image",
                    "sink_video_test"
                };
                for (auto name : ghost_names) {
                    if (GstPad* ghostPad = gst_element_get_static_pad(sinkBin, name)) {
                        gst_element_remove_pad(sinkBin, ghostPad);
                        gst_object_unref(ghostPad);
                    }
                }
            }
            // flush events & bin 제거

            gst_object_unref(sinkBin);
            g_print("AutoDetachConsumerBin: Successfully detached sinkbin from pipeline.\n");
            return true;
        }
        static bool AutoDetachConsumerBin(IntPtr sinkBinPtr) {
            GstElement* sinkBin = static_cast<GstElement*>(sinkBinPtr.ToPointer());
            if (!sinkBin) {
                g_printerr("AutoDetachConsumerBin: Sink bin is null.\n");
                return false;
            } 
            //gst_debug_set_threshold_for_name("GST_REFCOUNTING", GST_LEVEL_MEMDUMP);
            //gst_element_set_state(sinkBin, GST_STATE_NULL);
            GstObject* parentObj = gst_element_get_parent(sinkBin);
            if (!parentObj) {
                g_printerr("AutoDetachConsumerBin: Sink bin has no parent.\n");
                return false;
            }

            GstBin* sspPipeline = GST_BIN(GST_ELEMENT(parentObj));
            GstElement* volume = gst_bin_get_by_name(GST_BIN(sspPipeline), "volume");
            if (!volume) {
                g_printerr("ToggleMute: valve  not found\n");
                
            }
            else
            {
                g_object_set(volume, "volume", 0, NULL);
                gst_object_unref(volume);
            }

            // flip state and apply
        //    isMute = !isMute;
            
            // ghost-pad unlink (위와 동일)
            GstPad* branch_sink = nullptr;
            if ((branch_sink = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), "sink_rtsp_h264")) ||
                (branch_sink = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), "sink_file_h264")) ||
                (branch_sink = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), "sink_file_image")) ||
                (branch_sink = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), "sink_capture_card")) ||
                (branch_sink = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), "sink_video_test"))) {
                if (GstPad* peer = gst_pad_get_peer(branch_sink)) {
                    gst_pad_unlink(peer, branch_sink);
                    gst_object_unref(peer);
                }
                //gst_object_unref(branch_sink);
            }

            // tee request-pad 해제
            if (GstPad* teePad = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), "last_tee_pad")) {
                GstElement* tee = gst_bin_get_by_name(sspPipeline, "tee_rtsp_h264");
                if (!tee) tee = gst_bin_get_by_name(sspPipeline, "tee_file_h264");
                if (!tee) tee = gst_bin_get_by_name(sspPipeline, "tee_file_image");
                if (!tee) tee = gst_bin_get_by_name(sspPipeline, "tee_capture_card");
                if (!tee) tee = gst_bin_get_by_name(sspPipeline, "tee_video_test");
                if (tee) {
                    gst_element_release_request_pad(tee, teePad);
                    gst_object_unref(tee);
                }
                g_object_set_data(G_OBJECT(sinkBin), "last_tee_pad", nullptr);
                //gst_object_unref(teePad);
            }

            // input-selector 비활성화
            if (GstElement* selector = gst_bin_get_by_name(GST_BIN(sinkBin), "input_selector")) { 
                g_object_set(selector, "active-pad", NULL, NULL);
                gst_object_unref(selector);
            }
          
            // flush events & bin 제거
            gst_element_send_event(sinkBin, gst_event_new_flush_start());
            gst_element_send_event(sinkBin, gst_event_new_flush_stop(FALSE));
            if (!gst_bin_remove(sspPipeline, sinkBin)) {
                g_printerr("AutoDetachConsumerBin: Failed to remove sinkbin from pipeline\n");
                gst_object_unref(parentObj);
                return false;
            }



            //gst_object_unref(parentObj);
            //gst_object_unref(sinkBin);
            g_print("AutoDetachConsumerBin: Successfully detached sinkbin from pipeline.\n");
            return true;
        } 
        static void toggleMute(String^ rtspUrl) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            if (pipeline)
                pipeline->ToggleMute();
        } 
        static void SetVolume(String^ rtspUrl,double vol) {
            msclr::interop::marshal_context context;
            std::string url = context.marshal_as<std::string>(rtspUrl);
            SharedSourcePipeline* pipeline = SourceManager::Instance().GetOrCreateSource(url);
            if (pipeline)
                pipeline->SetVolume(vol);
        }
    }; 
}
