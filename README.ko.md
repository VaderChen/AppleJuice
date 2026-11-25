# AppleJuice

[繁體中文](README.md) · [English](README.en.md) · [日本語](README.ja.md) · [한국어](README.ko.md)

AppleJuice는 Apple Silicon Mac에서 지원되는 Windows 프로그램을 실행하는 호환성 프로젝트입니다. 그래픽 실행기 **Win64 Launcher**와 명령줄 도구 **x64arm**을 제공합니다. PE/DLL 로딩과 Windows 호환 서비스를 제공하며 창, 그래픽, 오디오 및 입력을 macOS 네이티브 인터페이스와 Metal에 연결합니다.

배포판의 최소 요구 사항은 **Apple Silicon 및 macOS 13.0 이상**입니다. 프레임 보간만 **macOS 26 이상**과 지원 장치가 필요합니다. 조건에 맞지 않으면 보간을 끄며 일반 실행, MetalFX 확대, 공간 오디오를 위해 macOS 27 업그레이드를 요구하지 않습니다.

완전한 `x64kernel/` 정적 SDK가 포함되어 있어 일반 빌드에는 커널 소스가 필요하지 않습니다. 커널은 별도의 AppleJuiceKernel에서 제공합니다. 정적으로 링크한 실행 파일과 Launcher 패키지도 AppleJuice의 배포 대상이며, 배포 시 해당 라이선스를 유지해야 합니다.

## 목표와 특징

- **Windows 시스템 파일 없이 실행**: Windows를 설치하거나 Windows 시스템 DLL을 제공하지 않고 지원되는 프로그램을 실행하는 것을 목표로 합니다. 프로그램과 함께 제공되는 종속 파일은 사용자가 준비합니다.
- **네이티브 ARM64 명령 우선 사용**: x86/x64 명령의 의미와 프로그램 동작을 유지하면서 Apple Silicon의 ARM64 명령과 NEON 벡터 연산을 가능한 한 사용합니다.
- **네이티브 하드웨어 가속 활용**: 그래픽과 GPU 연산에는 Metal을 우선 사용하고, 영상 처리 같은 작업에는 macOS 프레임워크와 사용 가능한 하드웨어 가속을 활용합니다.

## Geekbench 7 CPU 점수

2026년 11월 25일 같은 Apple M4 Pro(64 GB, macOS 27.2.1)에서 Geekbench 7.0.0 Pro(빌드 703423)를 실행했습니다. AppleJuice는 Launcher 0.26.1125(빌드 1313)로 Windows x86_64 버전을 실행했고, Rosetta 2는 macOS x86_64 버전, 네이티브 환경은 macOS ARM64 버전을 사용했습니다.

각 환경에서 CPU 구성별로 전체 테스트를 한 번씩 실행했습니다. 각 항목은 한 번 예열한 뒤 세 번 측정했습니다. 그래프는 Geekbench가 내보낸 공식 점수로, 높을수록 좋습니다. 1 CPU는 싱글 코어 16개 항목, 2 CPU와 4 CPU는 멀티코어를 지원하는 8개 항목을 포함합니다. CPU 수는 각 항목에서 실제로 사용한 작업 스레드 수입니다.

| CPU 구성 | AppleJuice | Rosetta 2 | 네이티브 ARM64 |
|---:|---:|---:|---:|
| 1 CPU | 1,387 | 1,281 | 3,057 |
| 2 CPU | 2,251 | 2,100 | 5,604 |
| 4 CPU | 4,137 | 3,990 | 10,784 |

![1 CPU, 2 CPU, 4 CPU 종합 점수](docs/images/geekbench7-scores-2026-11-25/cpu.png)

[항목별 점수 보기](https://vaderchen.github.io/AppleJuice/geekbench7/?lang=ko)

## 시작하기

Apple Silicon Mac, Xcode Command Line Tools, CMake 3.24 이상, Ninja, Python 3, Perl, make, Go 1.23 이상 및 MinGW x64 GCC(`x86_64-w64-mingw32-gcc`)가 필요합니다. 프런트엔드 리소스가 포함되어 있어 일반 빌드에는 Node.js가 필요하지 않습니다. 첫 Go 빌드에는 네트워크 연결 또는 모듈 캐시가 필요합니다.

저장소 루트에서 실행하거나 Finder에서 `run.command`를 두 번 클릭합니다.

```sh
./run.command
```

빌드가 끝나면 `build/launcher/Win64 Launcher.app`이 열립니다. “EXE 추가”를 눌러 준비한 Windows 프로그램을 선택하고 작업 폴더, 명령줄, 드라이브 매핑 및 쓰기 권한을 설정한 뒤 재생 버튼을 누릅니다. 콘솔 프로그램은 내장 터미널을 사용하고 GUI 프로그램은 별도 창으로 열립니다.

## 소스에서 빌드

```sh
cmake --preset release
cmake --build --preset release --target x64arm --parallel 4
./build/release/x64arm doctor
python3 scripts/build_launcher.py --jobs 4
```

처음 세 줄은 CLI를 빌드하고 확인하며, 마지막 줄은 Launcher를 빌드하고 패키징합니다. SDK의 헤더, 정적 라이브러리, CMake 설정 및 라이선스를 함께 유지하고 서로 다른 버전을 혼용하지 마세요. 고정된 타사 소스와 라이선스도 포함되어 있습니다.

## 문서와 호환성

[시작하기](docs/getting-started.md) · [Launcher 사용법](docs/launcher.md) · [빌드와 패키징](docs/build-and-release.md) · [실행과 호환성](docs/realworld-support.md) · [GPU 메모리](docs/gpu-memory.md) · [Geekbench 실행 방법](docs/geekbench7-single-core.md) · [문서 목록](docs/README.md)

상세 사용 문서는 현재 번체 중국어로 제공됩니다. x64 OpenCL과 Vulkan은 각각 macOS OpenCL.framework 및 Vulkan loader/MoltenVK를 사용합니다. 요구 사항과 제한은 [OpenCL/Vulkan](docs/opencl-vulkan-support.md)을 참고하세요.

### CPU와 멀티코어

32비트와 64비트 프로그램 모두 기본적으로 resident AOT를 사용합니다. Launcher에서 프로그램별로 게스트 CPU 코어 수를 설정할 수 있으며, 여러 게스트 스레드는 상주 worker에서 병렬로 실행됩니다. 2026년 11월 2일 Geekbench 7의 공식 멀티코어 8개 항목으로 측정한 결과, 싱글 코어 기준 대비 2코어에서 1.79~2.18배, 4코어에서 2.84~4.35배였습니다.

### Direct3D 9

D3D9/D3D9Ex 장치, 텍스처와 버퍼, 고정 기능 및 프로그래밍 가능한 렌더링, 스왑 체인과 Metal 화면 출력을 제공합니다. 특수 형식, 장치 기능 및 렌더링 상태 조합에는 호환성 제한이 있습니다.

### Direct3D 10/10.1

D3D10/D3D10.1 장치, 리소스, 셰이더, 렌더링 및 DXGI 스왑 체인을 Metal에 연결합니다. 장치 생성 성공이나 버전 번호가 모든 기능 및 형식 지원을 의미하지는 않습니다.

### Direct3D 11

장치와 컨텍스트, 리소스와 뷰, DXBC 셰이더 변환, Draw/Dispatch 및 DXGI 화면 출력을 제공하며 그래픽과 GPU 연산 경로를 포함합니다. D3D11.1~11.4 인터페이스는 같은 처리 경로로 연결되며 셰이더는 백그라운드에서 변환할 수 있습니다. 고급 기능, 형식 및 확장 인터페이스는 프로그램의 요구 사항에 따라 확인해야 합니다. 보고된 feature level이 완전한 11.x 호환성을 의미하지는 않습니다. vCPU가 여러 개일 때 반환값이 없는 메서드는 명령 스트림을 거쳐 전용 스레드에서 순서대로 실행되어 렌더링 스레드의 대기가 줄어듭니다(`X64ARM_D3D11_STREAM=0`으로 끔).

### Direct3D 12

장치, 명령 큐/목록, 리소스와 디스크립터, 루트 서명, 그래픽/연산, fence 및 DXGI 화면 출력의 일부 기능을 Metal로 실행하며, 명령 목록은 전용 제출 스레드에서 재생합니다. 간접 명령과 Bundle 실행 경로가 있지만 DXR, Mesh Shader, Work Graph 같은 고급 기능은 완전한 호환성 보장 범위에 포함되지 않습니다.

### x86(32비트 Windows)

x86_64(64비트) 외에도 x86 PE32 EXE와 해당 DLL을 로드할 수 있습니다. 32비트와 64비트 프로그램 모두 기본적으로 resident AOT를 사용합니다. 함께 제공되는 DLL의 비트 수는 EXE와 같아야 하며, 한 프로세스에서 32비트와 64비트 DLL을 혼용할 수 없습니다. x86의 API 및 그래픽 지원 범위는 x64와 다를 수 있으며, 완전한 WOW64, 16비트 프로그램, 커널 드라이버 또는 DRM 지원을 의미하지는 않습니다.

자세한 범위와 사용 안내는 [Direct3D 및 x86 호환성](docs/direct3d-and-x86.md)(번체 중국어)을 참고하세요.

### 기타 API

- **OpenGL/WebGL**: OpenGL/WGL은 기본적으로 Metal로 렌더링하며 초기화에 실패하면 macOS OpenGL을 사용합니다. WebGL이 사용하는 EGL/GLES는 ANGLE의 Metal 백엔드에서 처리합니다.
- **오디오**: DirectSound(3D 위치 지정 및 녹음 포함), XAudio2, WASAPI, X3DAudio를 Core Audio로 출력하며 스테레오 좌우 채널과 패닝을 유지합니다.
- **입력**: DirectInput(포스 피드백 포함), XInput, Raw Input은 macOS 실제 입력 장치의 데이터를 사용합니다.
- **동영상**: Media Foundation과 D3D11 DXVA의 H.264 인코딩/디코딩은 VideoToolbox 하드웨어 가속을 사용합니다. Windows Media(ASF)의 VC-1/WMV와 WMA는 내장된 디코더 전용 FFmpeg로 디코딩합니다. HEVC와 AV1은 아직 완료되지 않았습니다.

AppleJuice는 개발 중이며 전체 Windows, 모든 프로그램 및 모든 API 매개변수 조합을 완전히 지원하지 않습니다. 프로그램 로딩이나 창 표시가 성공해도 전체 프로그램의 호환성이 보장되지는 않습니다. 드라이브 매핑은 완전한 OS 샌드박스가 아니며 활성화된 서비스는 매핑된 파일이나 네트워크에 접근할 수 있습니다. 신뢰할 수 있는 프로그램만 실행하고 필요한 접근 권한을 설정하세요.

저장소에는 `src/`, `include/`, `launcher/`, `cmake/`, 필수 빌드 스크립트, `third_party/`, `x64kernel/` 및 사용 문서가 포함되어 있습니다.

## 네이티브 MetalFX 및 다채널 HRTF 헤드폰 시뮬레이션

AppleJuice가 화면 표시 및 오디오 출력 단계에서 기본 제공하는 기능입니다. 게임 자체가 MetalFX나 HRTF를 지원하지 않아도 게임 수정이나 모드 설치 없이 직접 켤 수 있습니다.

### MetalFX 업스케일링 및 프레임 보간

- **2배 확대 표시**: 메인 화면 스위치로 macOS 네이티브 MetalFX 공간 업스케일러를 사용해 게임 출력을 2배로 확대합니다. 게임 자체의 업스케일링 지원은 필요하지 않습니다.
- **프레임 보간**: 본 프로젝트가 별도로 macOS 네이티브 VideoToolbox를 이용해 완성된 두 프레임 사이에 중간 화면을 생성합니다. 게임의 깊이 정보나 모션 벡터가 필요하지 않습니다. macOS 26 이상과 지원 장치가 필요하며 입력은 최대 1920×1080입니다. 게임 자체의 업데이트 빈도는 높아지지 않습니다.
- **게임 내 XeSS**: 게임이 XeSS를 제공하면 MetalFX 시간적 업스케일링으로 연결할 수 있습니다. 이 경로가 켜지면 Launcher의 프레임 보간과 2배 확대를 자동으로 끄고 게임 업스케일러 출력을 표시합니다.

### 다채널 HRTF 헤드폰 시뮬레이션

"헤드폰 공간 믹싱"을 켜면 5.1, 7.1 등 각 채널을 가상 스피커 위치에 배치하고 HRTF로 일반 헤드폰용 바이노럴 스테레오를 생성하며 저음 채널도 유지합니다. 게임이나 영상 프로그램에 HRTF 기능이 없어도 사용할 수 있으며, 공간감은 실제 입력 채널 정보에 따라 달라집니다.

메인 화면과 설정 → 오디오에 연동된 스위치가 있습니다. 기본값은 꺼짐이며 변경 사항은 프로그램을 다음에 시작할 때 적용됩니다. 현재 머리 추적은 사용하지 않습니다.

### Steam 등 다른 런처에서 게임 실행

AppleJuice에서 Steam을 선택하여 헤드폰 공간 믹싱, 2배 확대 표시, 프레임 보간을 설정한 다음 Steam에서 게임을 실행하세요. 새 게임과 해당 하위 프로세스가 최신 설정을 읽으므로 스위치 변경 후 Steam을 다시 시작할 필요가 없습니다. 이미 실행 중인 게임은 다시 시작해야 새 화면 및 오디오 설정이 적용됩니다.

AppleJuice 라이브러리에 등록된 게임은 EXE별로 저장한 표시 설정을 우선합니다. 미등록 게임은 런처의 현재 표시 설정을 상속하며 공간 오디오는 전역 설정입니다. 같은 처리를 다른 런처에도 적용하며 게임 이름별 예외는 사용하지 않습니다.

## 실측 기록 / 2026-11-26 / 게임 총 46개 / 도구 5개

- <img src="docs/images/support-levels/good.png" width="16" height="16" alt="원활" title="원활"> 원활: 29
- <img src="docs/images/support-levels/metalfx.png" width="16" height="16" alt="원활 (MetalFX)" title="원활 (MetalFX)"> 원활 (MetalFX): 5
- <img src="docs/images/support-levels/slow.png" width="16" height="16" alt="저효율" title="저효율"> 저효율: 8
- <img src="docs/images/support-levels/menu.png" width="16" height="16" alt="실행 가능" title="실행 가능"> 실행 가능: 2
- <img src="docs/images/support-levels/none.png" width="16" height="16" alt="미작동·작업 중" title="미작동·작업 중"> 미작동·작업 중: 2

**[전체 목록 보기(언어 전환, 지원 수준별 탭)](https://vaderchen.github.io/AppleJuice/tested/?lang=ko)**

"원활 (MetalFX)"는 Launcher의 프레임 보간 또는 2배 확대(MetalFX)를 켜면 원활 기준을 충족한다는 뜻이며, 끄면 저효율에 해당합니다.

DEATH STRANDING DIRECTOR'S CUT은 현재 "실행 가능"으로 평가합니다. 3장 저장 파일 불러오기와 캐릭터 이동은 확인했지만 흰색 깜박임이 발생할 수 있으며 안정적인 50 FPS에는 도달하지 못했습니다. 문제를 해결하고 실제 플레이와 다른 장면의 검증을 마친 뒤 지원 수준을 갱신합니다. "원활"은 50 FPS를 안정적으로 초과하거나 프로그램 자체의 프레임률 상한에 도달한 경우를 뜻합니다.

실측 기록은 당시 버전과 설정 기준이며, 전체 게임 진행, 다른 버전 또는 모든 설정에서의 동작을 의미하지 않습니다.

## 개발 후원

AppleJuice가 도움이 되었다면 커피 한 잔으로 지속적인 개발을 응원해 주세요.

[☕ Buy Me a Coffee](https://buymeacoffee.com/vaderchen)

## 라이선스

Copyright (C) 2026 VaderChen.

x64arm 엔진과 Win64 Launcher를 포함한 AppleJuice 자체 콘텐츠에는 [소스 공개·상업적 판매 금지 라이선스](LICENSE.ko.md)를 적용합니다. 기업 내부 사용을 포함한 비판매 사용, 수정 및 무료 공유를 허용합니다. 소프트웨어 판매, 유료 호스팅/SaaS, 유료 서비스 및 유료 제품 통합은 금지합니다. 직원의 통상적인 급여는 판매로 보지 않지만 소프트웨어 제공을 통한 광고 수익, 추천 수수료 및 사용자 데이터 수익화도 제한됩니다. 필요한 경우 저작권자와 별도의 서면 라이선스를 논의하세요. 문의 자체로 허가가 부여되지는 않습니다.

AppleJuiceKernel을 정적으로 링크한 실행 파일과 Launcher 패키지도 AppleJuice의 배포 대상입니다. 배포물에 커널과 타사 라이선스 고지를 유지해야 하며, 저장소 분리는 기존 라이선스 제한을 변경하지 않습니다.

자체 소스 공개 라이선스이며 OSI 오픈 소스 정의에 부합한다고 주장하지 않습니다. README는 요약이며 번체 중국어 전문이 기준입니다. 타사 구성 요소, 별도로 라이선스된 콘텐츠 및 외부 Windows 프로그램에는 각자의 라이선스가 적용됩니다.

전체 라이선스: [繁體中文](LICENSE.md) · [English](LICENSE.en.md) · [日本語](LICENSE.ja.md) · [한국어](LICENSE.ko.md)

[상업적 판매 정책](COMMERCIAL-LICENSE.md) · [적용 범위 및 출처](docs/licensing.md) · [타사 라이선스](THIRD-PARTY-NOTICES.md)
