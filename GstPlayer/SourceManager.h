#ifndef SOURCE_MANAGER_H
#define SOURCE_MANAGER_H

#include "SharedSourcePipeline.h"
#include <string>
#include <map>
#include <mutex>

class SourceManager {
public:
    // 싱글턴 인스턴스 반환
    static SourceManager& Instance();

    // RTSP URL에 따른 공유 소스 파이프라인을 생성하거나 재사용
    SharedSourcePipeline* GetOrCreateSource(const std::string& rtspUrl);

    // 사용이 끝난 소스를 해제
    void ReleaseSource(const std::string& rtspUrl);
    void CheckTee(const std::string& rtspUrl);
    void ToggleMute(const std::string& rtspUrl);
    void SetVolume(const std::string& rtspUrl, double vol);
    
private:
    SourceManager();
    ~SourceManager();
    // 복사 생성자 및 대입 연산자는 사용하지 않음.
    SourceManager(const SourceManager&) = delete;
    SourceManager& operator=(const SourceManager&) = delete;

    std::mutex mutex_;
    std::map<std::string, SharedSourcePipeline*> sources_;
};

#endif // SOURCE_MANAGER_H
