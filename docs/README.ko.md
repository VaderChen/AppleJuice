# AppleJuice

[繁體中文](../README.md) · [English](README.en.md) · [日本語](README.ja.md) · **한국어**

AppleJuice는 Apple Silicon Mac용 Windows 호환 계층과 Go/WKWebView Win64 Launcher를 제공합니다. C++20 런타임은 PE 파일을 로드하고 Windows API를 macOS 및 Metal에 연결합니다. CPU 변환과 실행은 별도 AppleJuiceKernel의 정적 SDK가 담당합니다.

이 저장소는 `x64kernel/` 정적 SDK를 포함하며, AppleJuice는 이 SDK만으로 빌드하고 실행합니다. 코어 개발, 도구 및 관련 문서는 AppleJuiceKernel에서 관리합니다. 필수 라이선스를 유지한 정적 링크 실행 파일과 Launcher 번들은 AppleJuice 배포 대상입니다.


[使用文件](README.md) · [建置與封裝](build-and-release.md)

## 라이선스

Copyright (C) 2026 VaderChen.

x64arm 엔진과 Win64 Launcher를 포함한 AppleJuice 자체 콘텐츠에는 [소스 공개·상업적 판매 금지 라이선스](../LICENSE.ko.md)를 적용합니다. 기업 내부 사용을 포함한 비판매 사용, 수정 및 무료 공유를 허용합니다. 소프트웨어 판매, 유료 호스팅/SaaS, 유료 서비스 및 유료 제품 통합은 금지합니다. 직원의 통상적인 급여는 판매로 보지 않지만 소프트웨어 제공을 통한 광고 수익, 추천 수수료 및 사용자 데이터 수익화도 제한됩니다. 필요한 경우 저작권자와 별도의 서면 라이선스를 논의하세요. 문의 자체로 허가가 부여되지는 않습니다.

자체 소스 공개 라이선스이며 OSI 오픈 소스 정의에 부합한다고 주장하지 않습니다. README는 요약이며 번체 중국어 전문이 기준입니다. 타사 구성 요소, 별도로 라이선스된 콘텐츠 및 외부 Windows 프로그램에는 각자의 라이선스가 적용됩니다.

전체 라이선스: [繁體中文](../LICENSE.md) · [English](../LICENSE.en.md) · [日本語](../LICENSE.ja.md) · [한국어](../LICENSE.ko.md)

[상업적 판매 정책](../COMMERCIAL-LICENSE.md) · [적용 범위 및 출처](licensing.md) · [타사 라이선스](../THIRD-PARTY-NOTICES.md)
