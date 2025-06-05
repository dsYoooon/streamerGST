#ifndef CONSUMER_BIN_H
#define CONSUMER_BIN_H

#include <gst/gst.h>
#include <Windows.h>

class ConsumerBin {
public:
    // 생성자: 창 위치와 크기를 인수로 받음 (디코더 선택 인수는 내부 전역 index를 사용)
    ConsumerBin(int left, int top, int width, int height, int zorder);
    ConsumerBin(double rLeft, double rTop, double rWidth, double rHeight, int wallWidth, int wallHeight, int zorder);
    // 기본 생성자 (필요 시)
    ConsumerBin();
    ~ConsumerBin();

    // 내부 GstBin을 생성하고 내부 요소(디코더 포함)를 구성
    bool Init();

    void MoveZorder(bool isTop);
    void SetWindow(int left, int top, int width, int height);
    // 공유 소스의 request pad를 받아, 이 bin의 ghost pad에 연결
    
    bool IsSame(double rLeft, double rTop, double rWidth, double rHeight);
    // 이 bin의 상태를 PLAYING 상태로 전환
    bool Start();

    // 상위 bin과 상태를 동기화 (gst_element_sync_state_with_parent 호출)
    void SyncParent();

    // 내부 bin 반환 (동적 추가/제거용)
    GstElement* GetBin() const { return consumerBin_; }

    // bin 종료 및 정리
    void Shutdown();

    // sink에 윈도우 핸들을 설정 (이미 창이 생성되어 있음)
    bool SetWindowHandleOnSink();
    void CheckBin(int idx);
    void ToggleFpsDisplay();
    void ShowWin(bool isShowing);
    void ReconfigureSink();
private:
    GstElement* consumerBin_;  // 소비자 bin을 나타내는 GstBin
    HWND windowHandle;         // 생성된 재생 창의 핸들
    int _top;
    int _left;
    int _width;
    int _height;
    int _zorder;
    double _ratioTop;
    double _ratioLeft;
    double _ratioWidth;
    double _ratioHeight;
    // 내부에서 재생 창을 생성하는 함수
    HWND CreatePlaybackWindow(int left, int top, int width, int height);
};

#endif // CONSUMER_BIN_H
