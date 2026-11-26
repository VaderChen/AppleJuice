# Install AppleJuice on your Mac

Use an Apple Silicon Mac running macOS 13 or later. In the Apple menu, **About This Mac** shows your chip and macOS version.

1. Open the [latest release](https://github.com/VaderChen/AppleJuice/releases/latest). Under **Assets**, download the macos-arm64 DMG and its SHA256SUMS. The source archives are for developers.
2. To check the download, put both files in the same folder and run `shasum -a 256 -c SHA256SUMS` there. Check that the DMG is reported as OK.
3. Open the DMG, copy AppleJuice.app into Applications, eject the disk image and open the installed copy. Build 727 of 0.26.1126 shipped with Developer ID signing and Apple notarization. For later builds, check their release notes. Confirm the ordinary first-open prompt only for the expected download. If macOS reports damage or blocks verification, download again and verify the checksum before continuing.
4. Choose **Add EXE**, select the Windows executable you own and keep its neighboring files together. Set the working directory to the game folder. Review the mapped drives and grant the write access needed for saves. Choose Play. Only run trusted files; a mapped directory can expose files on your Mac.

## Install Steam with Run once

Choose **Run once** in the Launcher and select **SteamSetup.exe** in the macOS file picker. Follow the setup wizard. To return to the Launcher first, clear **Run Steam** before choosing **Finish**. Look for **Steam.exe** in the left sidebar.

[Watch the 39-second installation walkthrough](https://vaderchen.github.io/AppleJuice/?lang=en#steam-install)

## Frequently asked questions

**Do I need Windows or Xcode?** The DMG includes the runtime. Supply the game and its bundled files. The [source build guide](guide.en.md) is for developers.

**A game opens a window and stops.** Check its exact edition, architecture and rating in the [tested list](https://vaderchen.github.io/AppleJuice/tested/?lang=en). Confirm its working directory and required game files. Include the last relevant redacted log lines when reporting a failure.

**Why is frame interpolation unavailable?** It requires a supported device on macOS 26 or later, with input up to 1920 × 1080. In-game XeSS mapped to temporal MetalFX takes precedence over Launcher interpolation and 2× upscaling.

**How do I upgrade or uninstall?** Quit games and the Launcher. To upgrade, replace the app in Applications with the verified new version. To uninstall, move the app to Trash. Your mapped games and saves may live elsewhere. The default Launcher data folder is `~/Library/Application Support/Win64Launcher`. Back up any files you wish to keep before removing data separately.
