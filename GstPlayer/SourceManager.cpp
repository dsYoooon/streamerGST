#include "SourceManager.h"
#include <gst/gst.h>

SourceManager& SourceManager::Instance() {
    static SourceManager instance;
    return instance;
}

SourceManager::SourceManager() {
    // 필요한 초기화 작업이 있다면 추가
}

SourceManager::~SourceManager() {
    // 모든 공유 소스를 정리
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : sources_) {
        pair.second->Shutdown();
        delete pair.second;
    }
    sources_.clear();
}

SharedSourcePipeline* SourceManager::GetOrCreateSource(const std::string& rtspUrl) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(rtspUrl);
    if (it != sources_.end()) {
        return it->second;
    }
    // 새로운 SharedSourcePipeline 생성
    SharedSourcePipeline* pipeline = new SharedSourcePipeline(rtspUrl);
    if (pipeline->Init()) {
        sources_[rtspUrl] = pipeline;
        Sleep(100);
        g_printerr("Source created for: %s\n", rtspUrl.c_str());
        return pipeline;
    }
    else {
        g_printerr("Failed to initialize source for: %s\n", rtspUrl.c_str());
        delete pipeline;
        return nullptr;
    }
}

void SourceManager::CheckTee(const std::string& rtspUrl) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(rtspUrl);
    if (it != sources_.end()) {
        it->second->CheckPipeline();
    }
}

void SourceManager::ToggleMute(const std::string& rtspUrl) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(rtspUrl);
    if (it != sources_.end()) {
        it->second->ToggleMute();
    }
}
void SourceManager::SetVolume(const std::string& rtspUrl, double vol) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(rtspUrl);
    if (it != sources_.end()) {
        it->second->SetVolume(vol);
    }
}

bool SourceManager::AttachConsumerBin(const std::string& rtspUrl, GstElement* sinkBin, bool useRTSPBranch) {
    if (!sinkBin) {
        g_printerr("AttachConsumerBin: sink bin is null\n");
        return false;
    }
    SharedSourcePipeline* pipeline = GetOrCreateSource(rtspUrl);
    if (!pipeline) {
        g_printerr("AttachConsumerBin: failed to get pipeline for %s\n", rtspUrl.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    GstElement* sspPipeline = pipeline->GetPipeline();
    if (!sspPipeline) {
        g_printerr("AttachConsumerBin: SSP pipeline is null for %s\n", rtspUrl.c_str());
        return false;
    }

    // logic moved from managed wrapper
    const gchar* tee_name = nullptr;
    const gchar* ghost_pad = nullptr;
    const gchar* sel = nullptr;

    bool isRtsp = (rtspUrl.find("rtsp://") != std::string::npos);
    bool isVideo = (rtspUrl.find("video://") != std::string::npos);
    bool isImage = (rtspUrl.find("image://") != std::string::npos);

    if (isRtsp) {
        tee_name = "tee_rtsp_h264";
        ghost_pad = "sink_rtsp_h264";
        sel = "request_pad_rtsp_h264";
    }
    else if (isVideo) {
        tee_name = "tee_file_h264";
        ghost_pad = "sink_file_h264";
        sel = "request_pad_file_h264";
    }
    else if (isImage) {
        tee_name = "tee_file_image";
        ghost_pad = "sink_file_image";
        sel = "request_pad_file_image";
    }
    else {
        tee_name = "tee_video_test";
        ghost_pad = "sink_video_test";
        sel = "request_pad_video_test";
    }

    const gchar* sinkBinName = gst_element_get_name(sinkBin);
    if (!gst_bin_get_by_name(GST_BIN(sspPipeline), sinkBinName)) {
        gst_bin_add(GST_BIN(sspPipeline), sinkBin);
        gst_element_sync_state_with_parent(sinkBin);
    }

    GstElement* tee = gst_bin_get_by_name(GST_BIN(sspPipeline), tee_name);
    if (!tee) {
        g_printerr("AttachConsumerBin: Unable to find tee element in pipeline\n");
        return false;
    }

    if (GstPad* old_pad = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), "last_tee_pad")) {
        if (isVideo || isRtsp)
            gst_element_release_request_pad(tee, old_pad);
        g_object_set_data(G_OBJECT(sinkBin), "last_tee_pad", NULL);
    }

    GstPad* tee_req = gst_element_request_pad_simple(tee, "src_%u");
    if (!tee_req) {
        g_printerr("AttachConsumerBin: Failed to request pad from tee\n");
        gst_object_unref(tee);
        return false;
    }
    g_object_set_data(G_OBJECT(sinkBin), "last_tee_pad", tee_req);

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

    if (GstElement* input_selector = gst_bin_get_by_name(GST_BIN(sinkBin), "input_selector")) {
        GstPad* active_in = (GstPad*)g_object_get_data(G_OBJECT(sinkBin), sel);
        if (active_in) {
            g_object_set(input_selector, "active-pad", active_in, NULL);
        }
        gst_object_unref(input_selector);
    }

    gst_element_set_state(sspPipeline, GST_STATE_PLAYING);
    gst_element_sync_state_with_parent(sinkBin);

    gst_object_unref(tee);
    if (isVideo) {
        gst_element_seek_simple(GST_ELEMENT(sspPipeline), GST_FORMAT_TIME,
            (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
    }
    return true;
}

bool SourceManager::DetachConsumerBin(GstElement* sinkBin) {
    if (!sinkBin) {
        g_printerr("DetachConsumerBin: sink bin is null\n");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    g_object_set_data(G_OBJECT(sinkBin), "last_tee_pad", nullptr);

    const char* ghost_names[] = { "sink_rtsp_h264", "sink_file_h264", "sink_file_image", "sink_video_test" };
    for (auto name : ghost_names) {
        if (GstPad* ghostPad = gst_element_get_static_pad(sinkBin, name)) {
            gst_element_remove_pad(sinkBin, ghostPad);
            gst_object_unref(ghostPad);
        }
    }

    gst_object_unref(sinkBin);
    g_print("DetachConsumerBin: Successfully detached sinkbin.\n");
    return true;
}

bool SourceManager::AutoDetachConsumerBin(GstElement* sinkBin) {
    if (!sinkBin) {
        g_printerr("AutoDetachConsumerBin: Sink bin is null.\n");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    GstObject* parentObj = gst_element_get_parent(sinkBin);
    if (!parentObj) {
        g_printerr("AutoDetachConsumerBin: Sink bin has no parent.\n");
        return false;
    }
    GstBin* sspPipeline = GST_BIN(GST_ELEMENT(parentObj));
    GstElement* volume = gst_bin_get_by_name(GST_BIN(sspPipeline), "volume");
    if (!volume) {
        
        
    }
    else {
        g_object_set(volume, "volume", 0, NULL);
        gst_object_unref(volume);
    }

    
    
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
    }

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
    }

    if (GstElement* selector = gst_bin_get_by_name(GST_BIN(sinkBin), "input_selector")) {
        g_object_set(selector, "active-pad", NULL, NULL);
        gst_object_unref(selector);
    }

    gst_element_send_event(sinkBin, gst_event_new_flush_start());
    gst_element_send_event(sinkBin, gst_event_new_flush_stop(FALSE));
    if (!gst_bin_remove(sspPipeline, sinkBin)) {
        g_printerr("AutoDetachConsumerBin: Failed to remove sinkbin from pipeline\n");
        gst_object_unref(parentObj);
        return false;
    }

    g_print("AutoDetachConsumerBin: Successfully detached sinkbin from pipeline.\n");
    return true;
}

void SourceManager::ReleaseSource(const std::string& rtspUrl) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(rtspUrl);
    if (it != sources_.end()) {
        it->second->Shutdown();
        delete it->second;
        sources_.erase(it);
        g_printerr("Source released for: %s\n", rtspUrl.c_str());
    }
}
