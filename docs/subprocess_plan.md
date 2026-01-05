# Screen capture 서버 서브프로세스 전환 사전 분석

본 문서는 현재 스트리머 솔루션의 구성과 ScreenCaptureServer 경로를 분리된 워커 프로세스로 이전하기 위한 준비 내용을 정리한다. 변경 범위는 기존 공개 API 시그니처를 유지하며, RTSP 서버 실행 경로만 우선 격리하는 것을 목표로 한다.

## 솔루션 구조 개요
- **GstPlayer (C++/CLI)**: `GStreamerWrapper` 관리 래퍼를 제공하며, WPF에서 직접 호출된다. 현재 GStreamer 초기화 및 파이프라인 생성/종료가 모두 이 프로젝트에서 수행된다.
- **GStreamerDotNetTest (WPF)**: 예제 클라이언트. `MainWindow`에서 `_player.StartScreenCaptureServer(...)`를 호출하여 멀티 스트림 설정을 전달한다.【F:GStreamerDotNetTest/MainWindow.xaml.cs†L192-L235】
- **ScreenCaptureServer (네이티브)**: RTSP 서버와 멀티 스트림 파이프라인을 구성한다. `RunScreenCaptureRtspServer`가 GMainLoop를 별도 스레드로 띄우고, `StopScreenCaptureRtspServer`가 종료를 담당한다.【F:GstPlayer/ScreenCaptureServer.cpp†L1668-L1708】【F:GstPlayer/ScreenCaptureServer.cpp†L1719-L1747】

## 현재 호출 흐름
1. WPF에서 `GstPlayer.StartScreenCaptureServer(serverIp, configs)` 호출.【F:GStreamerDotNetTest/MainWindow.xaml.cs†L183-L201】
2. C++/CLI 래퍼가 `StreamConfig` 배열을 `StreamConfigNative`로 변환 후 `RunScreenCaptureRtspServer`를 직접 호출한다.【F:GstPlayer/GStreamerWrapper.cpp†L386-L419】
3. `RunScreenCaptureRtspServer`는 GStreamer를 초기화하고 GLib 메인 루프/RTSP 서버를 백그라운드 스레드에서 실행한다. 서버 상태는 전역 `g_ctx`와 `g_server_thread`로 관리된다.【F:GstPlayer/ScreenCaptureServer.cpp†L1668-L1708】
4. 정지는 `StopScreenCaptureRtspServer`가 g_main_context를 깨워 파이프라인과 세션 풀을 종료한 후 스레드를 join한다.【F:GstPlayer/ScreenCaptureServer.cpp†L1719-L1747】

## 프로세스 분리 대상 및 데이터 흐름
- 분리 대상: GStreamer 초기화, RTSP 서버 파이프라인 생성(`configure_rtsp_server`/`start_rtsp_server`) 및 메인 루프 관리 전부.
- 관리 계층과 공유하는 데이터: `StreamConfigNative`(모니터/해상도/비트레이트/오디오/멀티캐스트/프로필 등)와 서버 IP 문자열.【F:GstPlayer/ScreenCaptureServer.h†L7-L26】
- 현재 상태 관리: 전역 `RtspServerContext`(`g_ctx`)와 `g_server_thread`로 단일 인스턴스만 지원.【F:GstPlayer/ScreenCaptureServer.cpp†L23-L48】【F:GstPlayer/ScreenCaptureServer.cpp†L1668-L1708】

## 제안 IPC 및 워커 구성
- **워커 EXE (네이티브 콘솔)**: `GStreamerWorker`를 신규 추가하여 기존 `RunScreenCaptureRtspServer`/`StopScreenCaptureRtspServer` 로직을 재사용하거나 포팅. 엔트리포인트에서 IPC 서버를 초기화하고 명령 루프를 실행.
- **IPC 방식**: Windows Named Pipe 기반, 길이 프리픽스된 JSON 프레이밍 `[uint32 length][json payload]`를 사용.
- **프로토콜 필드 예시**: `protocolVersion`, `type`, `requestId`, `payload`. 최소 명령: `ping`, `start_server`, `stop_server`, `get_status`, `shutdown`.
- **상태 모델**: `Idle / Starting / Running / Stopping / Error` 보유. 오류 발생 시 응답 메시지로 코드/메시지 반환, 치명적 오류는 프로세스 종료로 격리.

## 래퍼(C++/CLI) 변경 방향
- 기존 GStreamer 직접 호출 코드를 비활성화하고, 워커 프로세스 생성 및 Named Pipe 클라이언트로 교체.
- `StartScreenCaptureServer`/`Stop()` 시나리오: 워커 프로세스 실행 후 `start_server` 전송 → 종료 시 `stop_server` → 필요하면 `shutdown` 또는 강제 종료.
- 헬스 체크: 주기적 `ping`을 보내고 타임아웃 시 워커 재기동. 프로세스 핸들 감시로 비정상 종료 감지.
- 에러 전파: 예외 대신 오류 코드/메시지를 상위(.NET)로 전달하고 UI가 죽지 않도록 방어.

## 빌드 및 배포 고려사항
- 솔루션에 `GStreamerWorker`(C++/x64 콘솔) 프로젝트 추가, 산출물 경로를 `bin/x64/<Configuration>/GStreamerWorker.exe`로 통일.
- 기존 GStreamer DLL/플러그인 검색 경로 설정을 워커로 이전하고, 빌드 후 출력 폴더에 함께 복사.
- 설치/배포 스크립트에서 워커 EXE와 런타임을 포함.

## 다음 단계 제안
1. 워커 프로젝트 초기 스켈레톤 추가 및 Named Pipe 프로토콜 스텁 구현.
2. 기존 `RunScreenCaptureRtspServer`/`StopScreenCaptureRtspServer` 로직을 워커로 이동하고 래퍼에서 IPC 경유 호출로 전환.
3. 하트비트 및 재시작 정책 구현 후 WPF 통합 테스트(시작/중지/비정상 종료 시 UI 응답성 확인).
4. 문서에 IPC 스펙, 로그 위치, 트러블슈팅 절차를 추가.

## 진행 상황 업데이트
- `GStreamerWorker` 네이티브 콘솔 프로젝트를 솔루션에 추가하고, `ScreenCaptureServer` 구현을 직접 링크해 Named Pipe 기반 IPC 서버(`ping`/`start_server`/`stop_server`/`get_status`/`shutdown`)를 노출했습니다.
- C++/CLI 래퍼는 `WorkerClient`를 통해 워커 프로세스를 생성/접속하고, `StartScreenCaptureServer`/`Stop()` 경로에서 기존 직접 호출 대신 IPC로 명령을 전달하도록 전환했습니다.
