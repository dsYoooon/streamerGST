#ifndef SHARED_SOURCE_PIPELINE_H
#define SHARED_SOURCE_PIPELINE_H

#include <gst/gst.h>
#include <string>
#include <Windows.h>
#include "QCAP.H"  
#include <gst/gst.h>
//#include <atomic>
#include <thread>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <gst/app/gstappsrc.h>

struct CaptureData {
    // 기존 멤버
    GstAppSrc* videoSrc;
    GstBufferPool* videoPool;
    GstAppSrc* audioSrc;
    GstBufferPool* audioPool;
    double lastSampleTime = 0.0;

    // 새로 추가
    double   videoBaseTime = -1.0;  // 첫 프레임 SampleTime
    guint64  videoLastPts = 0;     // 직전 PTS
    double   audioBaseTime = -1.0;  // 첫 프레임 SampleTime
    guint64  audioLastPts = 0;     // 직전 PTS

};


class SharedSourcePipeline {
public:
    SharedSourcePipeline(const std::string& rtspUrl);
    ~SharedSourcePipeline();

    // GstPipeline을 생성하고 내부 요소들을 구성한 후 PLAYING 상태로 전환
    bool Init();

    // tee에서 request pad를 생성하여 반환
    GstPad* GetRequestPad();

    // (디버깅용) READY 상태로 전환
    void SetReady();
    void SetPlay();
    void CheckPipeline();
    // 최상위 GstPipeline(SSP)을 반환 (동적 연결용)
    GstElement* GetPipeline() const { return pipeline_; }
    void ActivateDecodePad();
    void ActivateFakePad();
    // 파이프라인 종료 및 정리
    void Shutdown();

    void ToggleMute();
    //captur변수
    void SetVolume(double vol);
    //HWND CreatePlaybackWindow(int left, int top, int width, int height);
private:
    int sourceIdx = 0;
    bool isMute = true;
    double CaptureFPS;
    HWND  windowHandle;
    static void on_pad_added(GstElement* src, GstPad* new_pad, gpointer user_data);
    bool CreateFakesrcBranch(GstElement** branchOut);   // 테스트용: videotestsrc 분기
    bool CreateImageBranch(GstElement** branchOut);     // 이미지 파일 소스 분기 (filesrc → decodebin → imagefreeze)
    bool CreateVideoBranch(GstElement** branchOut);     // 동영상 파일 소스 분기 (filesrc → qtdemux → h264parse)
    bool CreateRTSPBranch(GstElement** branchOut);      // RTSP 분기
    bool CreateCaptureBranch(GstElement** branchOut, bool useAudio);
    // --- 캡쳐 소스용 분기 추가 ---

    std::string rtspUrl_;
    GstElement* pipeline_; // 최상위 파이프라인
    GstElement* rtspsrc_;
    GstElement* depay_;
    GstElement* parse_;
    // 기존 decoder_ 멤버는 소비자 bin으로 이동하므로 제거합니다.
    GstElement* tee_;
    GstElement* selector_ = nullptr;
    GstPad* selectorDecodePad_ = nullptr;
    GstPad* selectorFakePad_ = nullptr;

    // 캡쳐 소스용 QCAP 장치 핸들 (SSP 내부에서 독립적으로 관리)
    PVOID qcapDevice_;

    // QCAP SDK 함수 타입 정의
    typedef int (WINAPI* PFN_QCAP_CREATE)(CHAR*, int, void*, PVOID*, BOOL, BOOL);
    typedef int (WINAPI* PFN_QCAP_REGISTER_VIDEO_PREVIEW_CALLBACK)(PVOID, void*, PVOID);
    typedef int (WINAPI* PFN_QCAP_REGISTER_AUDIO_PREVIEW_CALLBACK)(PVOID, void*, PVOID);
    typedef int (WINAPI* PFN_QCAP_RUN)(PVOID);
    typedef int (WINAPI* PFN_QCAP_STOP)(PVOID);
    typedef int (WINAPI* PFN_QCAP_DESTROY)(PVOID);

    //QCAP 멤버변수
    HMODULE hQ = nullptr;
    PFN_QCAP_CREATE               fQ_CREATE = nullptr;
    PFN_QCAP_REGISTER_VIDEO_PREVIEW_CALLBACK fQ_VID = nullptr;
    PFN_QCAP_REGISTER_AUDIO_PREVIEW_CALLBACK fQ_AUD = nullptr;
    PFN_QCAP_RUN                  fQ_RUN = nullptr;
    PFN_QCAP_STOP                 fQ_STOP = nullptr;
    PFN_QCAP_DESTROY              fQ_DESTROY = nullptr;
    CaptureData* captureData_ = nullptr;
    std::thread mainLoopThread;
    GMainLoop* mainLoop_ = nullptr;
    GMainContext* mainCont = nullptr;

};

#endif // SHARED_SOURCE_PIPELINE_H
