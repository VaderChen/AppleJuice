# AppleJuice

[繁體中文](README.md) · [English](README.en.md) · [日本語](README.ja.md) · [한국어](README.ko.md)

AppleJuice runs supported Windows applications on Apple Silicon Macs. It includes the **Win64 Launcher** graphical application and the **x64arm** command-line tool. AppleJuice loads PE/DLL files, provides Windows compatibility services, and connects windows, graphics, audio and input to native macOS interfaces and Metal.

The release requires **Apple Silicon and macOS 13.0 or later**. Frame interpolation separately requires **macOS 26 or later** and a supported device. When unavailable, interpolation is disabled; ordinary execution, MetalFX upscaling and spatial audio do not require upgrading to macOS 27.

The complete `x64kernel/` static SDK is included; normal builds do not require the kernel source. The kernel is provided separately by AppleJuiceKernel. Statically linked executables and Launcher bundles are distributable AppleJuice products, with the applicable licenses retained.

## Project goals and features

- **No Windows system files required:** Run supported applications without installing Windows or supplying Windows system DLLs. Users provide their applications and bundled dependencies.
- **Prefer native ARM64 instructions:** Preserve x86/x64 instruction semantics and application behavior while using Apple Silicon's native ARM64 instructions and NEON vector operations wherever possible.
- **Use native hardware acceleration:** Prefer Metal for graphics and GPU compute, and use native macOS frameworks and available hardware acceleration for workloads such as video processing.

## Geekbench 7 CPU scores

All three configurations ran Geekbench 7.0.0 Pro (build 703423) on the same Apple M4 Pro (64 GB, macOS 27.2.1) on November 25, 2026. AppleJuice ran the Windows x86_64 version with Launcher 0.26.1125 (build 1313). Rosetta 2 ran the macOS x86_64 version, and the native run used the macOS ARM64 version.

Each configuration contributed one full run, with one warm-up and three scored iterations per workload. The charts show the scores exported by Geekbench. Higher is better. The 1 CPU run covers 16 single-core workloads. The 2 CPU and 4 CPU runs cover the eight multi-core workloads. Each CPU count reflects the number of benchmark threads used by each workload.

| CPU configuration | AppleJuice | Rosetta 2 | Native ARM64 |
|---:|---:|---:|---:|
| 1 CPU | 1,387 | 1,281 | 3,057 |
| 2 CPU | 2,251 | 2,100 | 5,604 |
| 4 CPU | 4,137 | 3,990 | 10,784 |

![CPU scores for 1 CPU, 2 CPU and 4 CPU](docs/images/geekbench7-scores-2026-11-25/cpu.png)

[View workload scores](https://vaderchen.github.io/AppleJuice/geekbench7/?lang=en)

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

### CPU and multi-core

Both 32-bit and 64-bit applications use resident AOT by default. Launcher can set the guest CPU core count per application, and multiple guest threads run in parallel on resident workers. Measured on November 2, 2026 with the eight official Geekbench 7 multi-core workloads, scaling relative to the single-core baseline is 1.79–2.18× with 2 cores and 2.84–4.35× with 4 cores.

### Direct3D 9

Provides D3D9/D3D9Ex devices, textures and buffers, fixed-function and programmable drawing, swap chains and presentation through Metal. Special formats, device capabilities and combinations of render states still have compatibility limits.

### Direct3D 10/10.1

Provides D3D10/D3D10.1 devices, resources, shaders, drawing and DXGI swap chains through Metal. Successful device creation or a version number does not establish support for every feature or format.

### Direct3D 11

Provides devices and contexts, resources and views, DXBC shader translation, Draw/Dispatch and DXGI presentation, covering graphics and GPU compute paths. D3D11.1–11.4 interfaces are routed through the same processing path, and shaders can be translated in the background. Advanced features, formats and extended interfaces depend on the application's requirements; a reported feature level does not establish complete 11.x compatibility. With multiple vCPUs, methods that return nothing go through a command stream and run in order on a dedicated thread, so the rendering thread waits less (`X64ARM_D3D11_STREAM=0` turns it off).

### Direct3D 12

Provides a subset of devices, command queues/lists, resources and descriptors, root signatures, graphics/compute, fences and DXGI presentation through Metal, with command lists replayed on a dedicated submission thread. Indirect commands and Bundles have execution paths; advanced features such as DXR, Mesh Shaders and Work Graphs are outside the complete-compatibility commitment.

### x86 (32-bit Windows)

Alongside x86_64 (64-bit), x86 PE32 executables and matching DLLs can be loaded. Both 32-bit and 64-bit applications use resident AOT by default. Bundled DLLs must match the EXE's bitness; a process cannot mix 32-bit and 64-bit DLLs. x86 API and graphics coverage may differ from x64, and this does not establish complete support for WOW64, 16-bit applications, kernel drivers or DRM.

See [Direct3D and x86 compatibility](docs/direct3d-and-x86.md) for details and usage guidance (Traditional Chinese).

### Other APIs

- **OpenGL/WebGL**: OpenGL/WGL renders through Metal by default and falls back to macOS OpenGL if initialization fails; EGL/GLES used by WebGL is handled by ANGLE's Metal backend.
- **Audio**: DirectSound (including 3D positioning and capture), XAudio2, WASAPI and X3DAudio output through Core Audio, preserving stereo left/right channels and panning.
- **Input**: DirectInput (including force feedback), XInput and Raw Input use data from physical input devices on macOS.
- **Video**: H.264 encoding and decoding through Media Foundation and D3D11 DXVA use VideoToolbox hardware acceleration; Windows Media (ASF) VC-1/WMV and WMA are decoded by a built-in, decoder-only FFmpeg. HEVC and AV1 are not yet complete.

AppleJuice is under active development. Complete Windows support, all applications and all API parameter combinations are not fully supported. Loading a program or opening a window does not establish full application compatibility. Drive mappings are not a complete OS sandbox; enabled services may access mapped files or use the network. Run trusted programs and configure access permissions as needed.

The repository includes `src/`, `include/`, `launcher/`, `cmake/`, required build scripts, `third_party/`, `x64kernel/` and user documentation.

## Native MetalFX and multichannel HRTF headphone simulation

AppleJuice provides these features natively in its presentation and audio output layers. You can enable them manually even when a game has no built-in MetalFX or HRTF support, without modifying the game or installing a mod.

### MetalFX upscaling and frame interpolation

- **2× upscaling**: Use the main-window switch to enlarge the game's output by 2× with the native macOS MetalFX spatial upscaler. No in-game upscaling integration is required.
- **Frame interpolation**: AppleJuice separately uses native macOS VideoToolbox to generate an intermediate image between two completed frames, without requiring game-provided depth or motion vectors. This requires macOS 26 or later and a supported device, with input up to 1920×1080. Interpolation does not increase the game's own update rate.
- **In-game XeSS**: When a game provides XeSS, AppleJuice can map it to MetalFX temporal upscaling. This path automatically disables the Launcher's frame interpolation and 2× upscaling and displays the game's upscaler output.

### Multichannel HRTF headphone simulation

Enable "Spatial headphone mixing" to place channels from 5.1, 7.1 and other multichannel output in virtual speaker positions and render them as binaural stereo through ordinary headphones, preserving the low-frequency channel. Games and media applications do not need built-in HRTF support; the spatial effect depends on the channel information actually supplied.

Linked switches are available in the main window and Settings → Audio. The feature is off by default and takes effect the next time an application starts. Head tracking is currently disabled.

### Starting games through Steam or another launcher

Select Steam in AppleJuice, set Spatial headphone mixing, 2× upscaling and Frame interpolation, then start the game through Steam. Newly created games and their child processes read the latest settings, so Steam does not need to restart after a switch changes. Restart an already running game to apply new presentation and audio settings.

For a game already in the AppleJuice library, its EXE's saved display settings take priority. Games outside the library inherit the launcher's current display settings. Spatial audio is a global preference. The same process applies to other launchers without game-specific rules.

## Tested applications / 2026-11-25 / 39 games

- <img src="docs/images/support-levels/good.png" width="16" height="16" alt="Works well" title="Works well"> Works well: 28
- <img src="docs/images/support-levels/metalfx.png" width="16" height="16" alt="Works well (MetalFX)" title="Works well (MetalFX)"> Works well (MetalFX): 5
- <img src="docs/images/support-levels/slow.png" width="16" height="16" alt="Slow" title="Slow"> Slow: 12
- <img src="docs/images/support-levels/menu.png" width="16" height="16" alt="Launches" title="Launches"> Launches: 2
- <img src="docs/images/support-levels/none.png" width="16" height="16" alt="Not working (WIP)" title="Not working (WIP)"> Not working (WIP): 2

**[Full list (switch language, tabs by support level)](https://vaderchen.github.io/AppleJuice/tested/?lang=en)**

"Works well (MetalFX)" means the game meets the works-well criterion with the Launcher's frame interpolation or 2× upscaling (MetalFX) turned on; without them it is rated Slow.

DEATH STRANDING DIRECTOR'S CUT is currently rated "Launches". Loading a Chapter 3 save and moving the character have been verified, but white flicker can still occur and stable 50 FPS has not been achieved. The rating will be updated after these issues are resolved and gameplay and other scenes pass validation. "Works well" requires sustained performance above 50 FPS or reaching the application's own frame-rate cap.

Test records reflect the version and settings at the time and do not establish that complete gameplay, other versions or all settings work.

## Support development

If AppleJuice is useful to you, consider buying me a coffee to support continued development.

[☕ Buy Me a Coffee](https://buymeacoffee.com/vaderchen)

## License

Copyright (C) 2026 VaderChen.

AppleJuice's original content, including the x64arm engine and Win64 Launcher, uses the [source-available, no-commercial-sales license](LICENSE.en.md). Non-sale use, modification and free sharing, including internal business use, are permitted. Software sales, paid hosting/SaaS, paid services and integration into paid products are prohibited. Ordinary employee wages are not considered selling; advertising, referral commissions and user-data monetization through provision of the Software are also restricted. Contact the copyright holder for a separate written license where needed; an inquiry does not grant permission.

Statically linked AppleJuiceKernel executables and Launcher bundles are also distributable AppleJuice products. Retain the kernel and third-party notices with the products; separate repositories do not change the existing license restrictions.

This is a custom source-available license and does not claim to meet the OSI Open Source Definition. READMEs are summaries; the complete Traditional Chinese license governs. Third-party components, separately licensed content and external Windows programs retain their own licenses.

Full license: [繁體中文](LICENSE.md) · [English](LICENSE.en.md) · [日本語](LICENSE.ja.md) · [한국어](LICENSE.ko.md)

[Commercial sales policy](COMMERCIAL-LICENSE.md) · [Scope and source record](docs/licensing.md) · [Third-party notices](THIRD-PARTY-NOTICES.md)
