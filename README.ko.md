# AppleJuice

[繁體中文](README.md) · [English](README.en.md) · [日本語](README.ja.md) · [한국어](README.ko.md)

AppleJuice는 Apple Silicon Mac에서 지원되는 Windows 프로그램을 실행하는 호환성 프로젝트입니다. 그래픽 실행기 **Win64 Launcher**와 명령줄 도구 **x64arm**을 제공합니다. PE/DLL 로딩과 Windows 호환 서비스를 제공하며 창, 그래픽, 오디오 및 입력을 macOS 네이티브 인터페이스와 Metal에 연결합니다.

완전한 `x64kernel/` 정적 SDK가 포함되어 있어 일반 빌드에는 커널 소스가 필요하지 않습니다. 커널은 별도의 AppleJuiceKernel에서 제공합니다. 정적으로 링크한 실행 파일과 Launcher 패키지도 AppleJuice의 배포 대상이며, 배포 시 해당 라이선스를 유지해야 합니다.

## 목표와 특징

- **Windows 시스템 파일 없이 실행**: Windows를 설치하거나 Windows 시스템 DLL을 제공하지 않고 지원되는 프로그램을 실행하는 것을 목표로 합니다. 프로그램과 함께 제공되는 종속 파일은 사용자가 준비합니다.
- **네이티브 ARM64 명령 우선 사용**: x86/x64 명령의 의미와 프로그램 동작을 유지하면서 Apple Silicon의 ARM64 명령과 NEON 벡터 연산을 가능한 한 사용합니다.
- **네이티브 하드웨어 가속 활용**: 그래픽과 GPU 연산에는 Metal을 우선 사용하고, 영상 처리 같은 작업에는 macOS 프레임워크와 사용 가능한 하드웨어 가속을 활용합니다.

## Geekbench 7 싱글 코어 전체 경과 시간

2026년 10월 21일 Apple M4 Pro, 메모리 64 GiB, macOS 27.0.1 환경에서 Geekbench 7.0.0 Pro(build 703423)의 싱글 코어 항목 16개를 테스트했습니다. AppleJuice는 Launcher 0.26.1021(build 10, 엔진 `ced244119617`)을 사용했으며, 16개 항목 모두 동일한 실행 파일로 측정했습니다.

**각 항목의 네이티브 ARM64 전체 경과 시간을 1.0으로 정규화했으며, 배수가 낮을수록 빠릅니다.** 전체 경과 시간은 프로세스 시작부터 종료까지이며 초기화, 보정, 워밍업, 본 측정 및 프로세스 내부의 결과 기록을 포함합니다. 측정 도구의 입력 준비, 유휴 대기 및 종료 후 정리는 제외합니다. 네 그래프는 동일한 Y축 범위를 사용하며 각각 네 항목을 표시합니다.

AppleJuice는 Windows x86_64 버전, Rosetta는 macOS x86_64 버전, 네이티브 실행은 macOS ARM64 버전을 사용합니다. 그래프는 각 항목을 한 번 실행한 전체 경과 시간을 비교하여 당시 테스트 환경에서의 성능을 보여 줍니다.

![전체 경과 시간 배수: File Compression, Navigation, HTML5 Browser, PDF Viewer](docs/images/geekbench7-single-core-2026-10-21/chart-1.png)

![전체 경과 시간 배수: Photo Library, Clang, Text Processing, Asset Compression](docs/images/geekbench7-single-core-2026-10-21/chart-2.png)

![전체 경과 시간 배수: HDR, Photo Editor, Ray Tracer, Structure from Motion](docs/images/geekbench7-single-core-2026-10-21/chart-3.png)

![전체 경과 시간 배수: Game Physics, Video Encoder, Audio Encoder, Video Player](docs/images/geekbench7-single-core-2026-10-21/chart-4.png)

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

### Direct3D 9

D3D9/D3D9Ex 장치, 텍스처와 버퍼, 고정 기능 및 프로그래밍 가능한 렌더링, 스왑 체인과 Metal 화면 출력을 제공합니다. 특수 형식, 장치 기능 및 렌더링 상태 조합에는 호환성 제한이 있습니다.

### Direct3D 10/10.1

D3D10/D3D10.1 장치, 리소스, 셰이더, 렌더링 및 DXGI 스왑 체인을 Metal에 연결합니다. 장치 생성 성공이나 버전 번호가 모든 기능 및 형식 지원을 의미하지는 않습니다.

### Direct3D 11

장치와 컨텍스트, 리소스와 뷰, DXBC 셰이더 변환, Draw/Dispatch 및 DXGI 화면 출력을 제공하며 그래픽과 GPU 연산 경로를 포함합니다. 고급 기능, 형식 및 확장 인터페이스는 프로그램의 요구 사항에 따라 확인해야 합니다. 보고된 feature level이 완전한 11.x 호환성을 의미하지는 않습니다.

### Direct3D 12

장치, 명령 큐/목록, 리소스와 디스크립터, 루트 서명, 그래픽/연산, fence 및 DXGI 화면 출력의 일부 기능을 Metal로 실행합니다. 간접 명령과 Bundle 실행 경로가 있지만 DXR, Mesh Shader, Work Graph 같은 고급 기능은 완전한 호환성 보장 범위에 포함되지 않습니다.

### x86(32비트 Windows)

x86_64(64비트) 외에도 x86 PE32 EXE와 해당 DLL을 로드할 수 있습니다. 32비트와 64비트 프로그램 모두 기본적으로 resident AOT를 사용합니다. 함께 제공되는 DLL의 비트 수는 EXE와 같아야 하며, 한 프로세스에서 32비트와 64비트 DLL을 혼용할 수 없습니다. x86의 API 및 그래픽 지원 범위는 x64와 다를 수 있으며, 완전한 WOW64, 16비트 프로그램, 커널 드라이버 또는 DRM 지원을 의미하지는 않습니다.

자세한 범위와 사용 안내는 [Direct3D 및 x86 호환성](docs/direct3d-and-x86.md)(번체 중국어)을 참고하세요.

AppleJuice는 개발 중이며 전체 Windows, 모든 프로그램 및 모든 API 매개변수 조합을 완전히 지원하지 않습니다. 프로그램 로딩이나 창 표시가 성공해도 전체 프로그램의 호환성이 보장되지는 않습니다. 드라이브 매핑은 완전한 OS 샌드박스가 아니며 활성화된 서비스는 매핑된 파일이나 네트워크에 접근할 수 있습니다. 신뢰할 수 있는 프로그램만 실행하고 필요한 접근 권한을 설정하세요.

저장소에는 `src/`, `include/`, `launcher/`, `cmake/`, 필수 빌드 스크립트, `third_party/`, `x64kernel/` 및 사용 문서가 포함되어 있습니다.

## 라이선스

Copyright (C) 2026 VaderChen.

x64arm 엔진과 Win64 Launcher를 포함한 AppleJuice 자체 콘텐츠에는 [소스 공개·상업적 판매 금지 라이선스](LICENSE.ko.md)를 적용합니다. 기업 내부 사용을 포함한 비판매 사용, 수정 및 무료 공유를 허용합니다. 소프트웨어 판매, 유료 호스팅/SaaS, 유료 서비스 및 유료 제품 통합은 금지합니다. 직원의 통상적인 급여는 판매로 보지 않지만 소프트웨어 제공을 통한 광고 수익, 추천 수수료 및 사용자 데이터 수익화도 제한됩니다. 필요한 경우 저작권자와 별도의 서면 라이선스를 논의하세요. 문의 자체로 허가가 부여되지는 않습니다.

AppleJuiceKernel을 정적으로 링크한 실행 파일과 Launcher 패키지도 AppleJuice의 배포 대상입니다. 배포물에 커널과 타사 라이선스 고지를 유지해야 하며, 저장소 분리는 기존 라이선스 제한을 변경하지 않습니다.

자체 소스 공개 라이선스이며 OSI 오픈 소스 정의에 부합한다고 주장하지 않습니다. README는 요약이며 번체 중국어 전문이 기준입니다. 타사 구성 요소, 별도로 라이선스된 콘텐츠 및 외부 Windows 프로그램에는 각자의 라이선스가 적용됩니다.

전체 라이선스: [繁體中文](LICENSE.md) · [English](LICENSE.en.md) · [日本語](LICENSE.ja.md) · [한국어](LICENSE.ko.md)

[상업적 판매 정책](COMMERCIAL-LICENSE.md) · [적용 범위 및 출처](docs/licensing.md) · [타사 라이선스](THIRD-PARTY-NOTICES.md)
