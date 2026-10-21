# AppleJuice

[繁體中文](../README.md) · **English** · [日本語](README.ja.md) · [한국어](README.ko.md)

AppleJuice provides the Windows compatibility layer and Go/WKWebView Win64 Launcher for Apple Silicon Macs. Its C++20 runtime loads PE files and maps Windows APIs to macOS and Metal. CPU translation and execution are provided by the separate AppleJuiceKernel static SDK.

This repository includes the static SDK in `x64kernel/`; AppleJuice builds and runs using that SDK alone. Core development, tools and documentation are maintained in AppleJuiceKernel. Statically linked executables and Launcher bundles are distributable AppleJuice products, with the required licenses retained.


[使用文件](README.md) · [建置與封裝](build-and-release.md)

## License

Copyright (C) 2026 VaderChen.

AppleJuice's original content, including the x64arm engine and Win64 Launcher, uses the [source-available, no-commercial-sales license](../LICENSE.en.md). Non-sale use, modification and free sharing, including internal business use, are permitted. Software sales, paid hosting/SaaS, paid services and integration into paid products are prohibited. Ordinary employee wages are not considered selling; advertising, referral commissions and user-data monetization through provision of the Software are also restricted. Contact the copyright holder for a separate written license where needed; an inquiry does not grant permission.

This is a custom source-available license and does not claim to meet the OSI Open Source Definition. READMEs are summaries; the complete Traditional Chinese license governs. Third-party components, separately licensed content and external Windows programs retain their own licenses.

Full license: [繁體中文](../LICENSE.md) · [English](../LICENSE.en.md) · [日本語](../LICENSE.ja.md) · [한국어](../LICENSE.ko.md)

[Commercial sales policy](../COMMERCIAL-LICENSE.md) · [Scope and source record](licensing.md) · [Third-party notices](../THIRD-PARTY-NOTICES.md)
