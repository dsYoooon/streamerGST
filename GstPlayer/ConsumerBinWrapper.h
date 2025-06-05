#pragma once
#include <msclr/marshal_cppstd.h>
#include "ConsumerBin.h"

using namespace System;

namespace GStreamerWrapperCLI {

    public ref class ConsumerBinWrapper {
    private:
        ConsumerBin* consumerBin_;
    public:
        // 새 생성자: top, left, width, height 인수를 받아 ConsumerBin 생성
        ConsumerBinWrapper(int left, int top, int width, int height, int zorder) {
            consumerBin_ = new ConsumerBin(left, top, width, height, zorder);
        }
        ConsumerBinWrapper(double rLeft, double rTop, double rWidth, double rHeight, int wallWidth, int wallHeight, int zorder) {
            consumerBin_ = new ConsumerBin(rLeft,  rTop,  rWidth,  rHeight,  wallWidth,  wallHeight,  zorder);
        }
        // 기본 생성자 (필요 시)
        ConsumerBinWrapper() {
            consumerBin_ = new ConsumerBin();
        }
        ~ConsumerBinWrapper() {
            this->!ConsumerBinWrapper();
        }
        !ConsumerBinWrapper() {
            if (consumerBin_) {
                //consumerBin_->Shutdown();
                delete consumerBin_;
                consumerBin_ = nullptr;
            }
        }
     
        bool IsSame(double rLeft, double rTop, double rWidth, double rHeight) {
            return consumerBin_->IsSame(rLeft, rTop, rWidth, rHeight);
        }
        bool Init() {
            return consumerBin_->Init();
        }
      
        bool Start() {
            return consumerBin_->Start();
        }
        void SyncParent() {
            consumerBin_->SyncParent();
        }
        // consumer bin 반환 (동적 추가/제거용)
        IntPtr GetBin() {
            return IntPtr(consumerBin_->GetBin());
        }
        void CheckBin(int idx) {
            consumerBin_->CheckBin(idx);
        };
        void MoveZorder(bool isTop) {
            consumerBin_->MoveZorder(isTop);
        }
        void SetWindow(int left, int top, int width, int height) {
            consumerBin_->SetWindow(left, top, width, height);
        }
        void ShowWin(bool isShowing) {
            consumerBin_->ShowWin(isShowing);
        }
        void ReconfigureSink()
        {
            consumerBin_->ReconfigureSink();
        }

    };
}
