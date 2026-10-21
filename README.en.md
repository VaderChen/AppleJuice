# AppleJuice

[繁體中文](README.md) · [English](README.en.md) · [日本語](README.ja.md) · [한국어](README.ko.md)

AppleJuice runs supported Windows applications on Apple Silicon Macs. It includes the **Win64 Launcher** graphical application and the **x64arm** command-line tool. AppleJuice loads PE/DLL files, provides Windows compatibility services, and connects windows, graphics, audio and input to native macOS interfaces and Metal.

The complete `x64kernel/` static SDK is included; normal builds do not require the kernel source. The kernel is provided separately by AppleJuiceKernel. Statically linked executables and Launcher bundles are distributable AppleJuice products, with the applicable licenses retained.

## Project goals and features

- **No Windows system files required:** Run supported applications without installing Windows or supplying Windows system DLLs. Users provide their applications and bundled dependencies.
- **Prefer native ARM64 instructions:** Preserve x86/x64 instruction semantics and application behavior while using Apple Silicon's native ARM64 instructions and NEON vector operations wherever possible.
- **Use native hardware acceleration:** Prefer Metal for graphics and GPU compute, and use native macOS frameworks and available hardware acceleration for workloads such as video processing.

## Geekbench 7 single-core total elapsed time

The 16 single-core workloads in Geekbench 7.0.0 Pro (build 703423) were measured on October 21, 2026, on an Apple M4 Pro with 64 GiB of memory and macOS 27.0.1. AppleJuice used the same previously validated engine from Launcher 0.26.1021 (build 10).

**Each workload's native ARM64 total elapsed time is normalized to 1.0; lower ratios are faster.** Total elapsed time runs from process start to exit and includes initialization, calibration, warm-up, the measured workload and result writing within the process. It excludes the measurement tool's input preparation, idle waiting and cleanup after exit. All four charts use the same Y-axis scale, with four workloads per chart.

AppleJuice runs the Windows x86_64 version, Rosetta runs the macOS x86_64 version, and native execution uses the macOS ARM64 version. Versions, workload resources and workload amounts were checked across all three. Each result is the total elapsed time of one valid fresh process, so these are single-round measurements.

![Total elapsed time ratio: File Compression, Navigation, HTML5 Browser, PDF Viewer](docs/images/geekbench7-single-core-2026-10-21/chart-1.png)

![Total elapsed time ratio: Photo Library, Clang, Text Processing, Asset Compression](docs/images/geekbench7-single-core-2026-10-21/chart-2.png)

![Total elapsed time ratio: HDR, Photo Editor, Ray Tracer, Structure from Motion](docs/images/geekbench7-single-core-2026-10-21/chart-3.png)

![Total elapsed time ratio: Game Physics, Video Encoder, Audio Encoder, Video Player](docs/images/geekbench7-single-core-2026-10-21/chart-4.png)

## Getting started

Requirements: an Apple Silicon Mac, Xcode Command Line Tools, CMake 3.24+, Ninja, Python 3, Perl, make, Go 1.23+ and MinGW x64 GCC (`x86_64-w64-mingw32-gcc`). Frontend assets are included; normal builds do not need Node.js. The first Go build requires network access or a populated module cache.

Run this from the repository root, or double-click `run.command` in Finder:

```sh
./run.command
```

After the build, `build/launcher/Win64 Launcher.app` opens. Click “Add EXE”, select a Windows application you provide, configure its working directory, command line, drive mappings and write permission, then click Play. Console applications use the embedded terminal; graphical applications open separate windows.

## Build from source

```sh
cmake --preset release
cmake --build --preset release --target x64arm --parallel 4
./build/release/x64arm doctor
python3 scripts/build_launcher.py --jobs 4
```

The first three lines build and check the CLI; the last line builds and packages Launcher. Keep the SDK's headers, static libraries, CMake configuration and licenses together, and do not mix versions. Pinned third-party sources and licenses are also included.

## Documentation and compatibility

[Getting started](docs/getting-started.md) · [Launcher](docs/launcher.md) · [Build and packaging](docs/build-and-release.md) · [Execution and compatibility](docs/realworld-support.md) · [GPU memory](docs/gpu-memory.md) · [Geekbench instructions](docs/geekbench7-single-core.md) · [Documentation index](docs/README.md)

Detailed usage documents are currently in Traditional Chinese. x64 OpenCL and Vulkan use macOS OpenCL.framework and a Vulkan loader/MoltenVK respectively; see [OpenCL/Vulkan](docs/opencl-vulkan-support.md) for requirements and limitations.

### Direct3D 9

Provides D3D9/D3D9Ex devices, textures and buffers, fixed-function and programmable drawing, swap chains and presentation through Metal. Special formats, device capabilities and combinations of render states still have compatibility limits.

### Direct3D 10/10.1

Provides D3D10/D3D10.1 devices, resources, shaders, drawing and DXGI swap chains through Metal. Successful device creation or a version number does not establish support for every feature or format.

### Direct3D 11

Provides devices and contexts, resources and views, DXBC shader translation, Draw/Dispatch and DXGI presentation, covering graphics and GPU compute paths. Advanced features, formats and extended interfaces depend on the application's requirements; a reported feature level does not establish complete 11.x compatibility.

### Direct3D 12

Provides a subset of devices, command queues/lists, resources and descriptors, root signatures, graphics/compute, fences and DXGI presentation through Metal. Indirect commands and Bundles have execution paths; advanced features such as DXR, Mesh Shaders and Work Graphs are outside the complete-compatibility commitment.

### x86 (32-bit Windows)

Alongside x86_64 (64-bit), x86 PE32 executables and matching DLLs can be loaded. Both 32-bit and 64-bit applications use resident AOT by default. Bundled DLLs must match the EXE's bitness; a process cannot mix 32-bit and 64-bit DLLs. x86 API and graphics coverage may differ from x64, and this does not establish complete support for WOW64, 16-bit applications, kernel drivers or DRM.

See [Direct3D and x86 compatibility](docs/direct3d-and-x86.md) for details and usage guidance (Traditional Chinese).

AppleJuice is under active development. Complete Windows support, all applications and all API parameter combinations are not fully supported. Loading a program or opening a window does not establish full application compatibility. Drive mappings are not a complete OS sandbox; enabled services may access mapped files or use the network. Run trusted programs and configure access permissions as needed.

The repository includes `src/`, `include/`, `launcher/`, `cmake/`, required build scripts, `third_party/`, `x64kernel/` and user documentation.

## License

Copyright (C) 2026 VaderChen.

AppleJuice's original content, including the x64arm engine and Win64 Launcher, uses the [source-available, no-commercial-sales license](LICENSE.en.md), adapted from YOURDESK. Non-sale use, modification and free sharing, including internal business use, are permitted. Software sales, paid hosting/SaaS, paid services and integration into paid products are prohibited. Ordinary employee wages are not considered selling; advertising, referral commissions and user-data monetization through provision of the Software are also restricted. Contact the copyright holder for a separate written license where needed; an inquiry does not grant permission.

Statically linked AppleJuiceKernel executables and Launcher bundles are also distributable AppleJuice products. Retain the kernel and third-party notices with the products; separate repositories do not change the existing license restrictions.

This is a custom source-available license and does not claim to meet the OSI Open Source Definition. READMEs are summaries; the complete Traditional Chinese license governs. Third-party components, separately licensed content and external Windows programs retain their own licenses.

Full license: [繁體中文](LICENSE.md) · [English](LICENSE.en.md) · [日本語](LICENSE.ja.md) · [한국어](LICENSE.ko.md)

[Commercial sales policy](COMMERCIAL-LICENSE.md) · [Scope and source record](docs/licensing.md) · [Third-party notices](THIRD-PARTY-NOTICES.md)
