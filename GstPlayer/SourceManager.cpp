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
