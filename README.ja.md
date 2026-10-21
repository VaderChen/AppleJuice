# AppleJuice

[繁體中文](README.md) · [English](README.en.md) · [日本語](README.ja.md) · [한국어](README.ko.md)

AppleJuice は、Apple Silicon Mac 上で対応する Windows アプリケーションを実行する互換性プロジェクトです。GUI の **Win64 Launcher** とコマンドラインツール **x64arm** を提供します。PE／DLL の読み込みと Windows 互換サービスを担い、ウィンドウ、描画、音声、入力を macOS のネイティブインターフェイスと Metal に接続します。

完全な `x64kernel/` 静的 SDK を同梱しており、通常のビルドにカーネルのソースは不要です。カーネルは別の AppleJuiceKernel で提供します。静的リンクした実行ファイルと Launcher パッケージも AppleJuice の配布対象であり、適用されるライセンスを保持する必要があります。

## 目標と特徴

- **Windows システムファイルを不要にする**：Windows のインストールや Windows システム DLL の提供なしに、対応するアプリケーションを実行することを目指します。アプリケーションと付属の依存ファイルは利用者が用意します。
- **ネイティブ ARM64 命令を優先する**：x86／x64 命令の意味とアプリケーションの動作を保ちながら、Apple Silicon の ARM64 命令と NEON ベクトル演算を可能な限り使用します。
- **ネイティブのハードウェアアクセラレーションを使う**：描画と GPU 計算には Metal を優先し、動画処理などには macOS のフレームワークと利用可能なハードウェア支援を活用します。

## Geekbench 7 シングルコアの総経過時間

2026 年 10 月 21 日、Apple M4 Pro、メモリ 64 GiB、macOS 27.0.1 の環境で、Geekbench 7.0.0 Pro（build 703423）の 16 個のシングルコア測定項目をテストしました。AppleJuice には Launcher 0.26.1021（build 10、エンジン `ced244119617`）を使用し、16 項目すべてを同じ実行ファイルで測定しました。

**各項目のネイティブ ARM64 の総経過時間を 1.0 としています。倍率が低いほど高速です。** 総経過時間はプロセスの起動から終了までで、初期化、キャリブレーション、ウォームアップ、本測定、プロセス内の結果書き込みを含みます。計測ツールによる入力準備、アイドル待機、終了後の後処理は含みません。4 枚のグラフは同じ Y 軸スケールを使い、それぞれ 4 項目を示します。

AppleJuice は Windows x86_64 版、Rosetta は macOS x86_64 版、ネイティブ実行は macOS ARM64 版を使用します。グラフは各項目を 1 回実行した総経過時間を比較し、当時のテスト環境での性能を示しています。

![総経過時間の倍率: File Compression, Navigation, HTML5 Browser, PDF Viewer](docs/images/geekbench7-single-core-2026-10-21/chart-1.png)

![総経過時間の倍率: Photo Library, Clang, Text Processing, Asset Compression](docs/images/geekbench7-single-core-2026-10-21/chart-2.png)

![総経過時間の倍率: HDR, Photo Editor, Ray Tracer, Structure from Motion](docs/images/geekbench7-single-core-2026-10-21/chart-3.png)

![総経過時間の倍率: Game Physics, Video Encoder, Audio Encoder, Video Player](docs/images/geekbench7-single-core-2026-10-21/chart-4.png)

## はじめに

Apple Silicon Mac、Xcode Command Line Tools、CMake 3.24 以降、Ninja、Python 3、Perl、make、Go 1.23 以降、MinGW x64 GCC（`x86_64-w64-mingw32-gcc`）を用意してください。フロントエンドのリソースは同梱されており、通常のビルドに Node.js は不要です。初回の Go ビルドにはネットワーク接続またはモジュールキャッシュが必要です。

リポジトリのルートで実行するか、Finder で `run.command` をダブルクリックします。

```sh
./run.command
```

ビルド後に `build/launcher/Win64 Launcher.app` が開きます。「EXE を追加」から用意した Windows アプリケーションを選び、作業フォルダ、コマンドライン、ドライブマッピング、書き込み権限を設定して再生ボタンを押します。コンソールアプリケーションは内蔵ターミナルを、GUI アプリケーションは独立したウィンドウを使用します。

## ソースからビルド

```sh
cmake --preset release
cmake --build --preset release --target x64arm --parallel 4
./build/release/x64arm doctor
python3 scripts/build_launcher.py --jobs 4
```

最初の 3 行で CLI をビルドして確認し、最後の行で Launcher をビルドしてパッケージ化します。SDK のヘッダー、静的ライブラリ、CMake 設定、ライセンスを一式で保持し、異なるバージョンを混在させないでください。固定された第三者ソースとライセンスも同梱しています。

## ドキュメントと互換性

[はじめに](docs/getting-started.md) · [Launcher の操作](docs/launcher.md) · [ビルドとパッケージ化](docs/build-and-release.md) · [実行と互換性](docs/realworld-support.md) · [GPU メモリ](docs/gpu-memory.md) · [Geekbench の実行方法](docs/geekbench7-single-core.md) · [ドキュメント一覧](docs/README.md)

詳細な使用ドキュメントは現在、繁体中国語で提供しています。x64 OpenCL は macOS OpenCL.framework、Vulkan は Vulkan loader／MoltenVK を使用します。要件と制限は [OpenCL／Vulkan](docs/opencl-vulkan-support.md) を参照してください。

### Direct3D 9

D3D9／D3D9Ex のデバイス、テクスチャとバッファ、固定機能とプログラマブル描画、スワップチェーン、Metal による表示を提供します。特殊な形式、デバイス機能、描画状態の組み合わせには互換性の制限があります。

### Direct3D 10／10.1

D3D10／D3D10.1 のデバイス、リソース、シェーダー、描画、DXGI スワップチェーンを Metal に接続します。デバイスの作成成功やバージョン番号だけで、すべての機能や形式への対応を判断することはできません。

### Direct3D 11

デバイスとコンテキスト、リソースとビュー、DXBC シェーダー変換、Draw／Dispatch、DXGI 表示を提供し、描画と GPU 計算の経路を含みます。高度な機能、形式、拡張インターフェイスはアプリケーションの要件に応じて確認が必要です。feature level の表示は 11.x 全体の互換性を示しません。

### Direct3D 12

デバイス、コマンドキュー／リスト、リソースと記述子、ルートシグネチャ、描画／計算、fence、DXGI 表示の一部を Metal で実行します。間接コマンドと Bundle の実行経路がありますが、DXR、Mesh Shader、Work Graph などの高度な機能について完全な互換性は保証しません。

### x86（32 ビット Windows）

x86_64（64 ビット）に加えて、x86 PE32 の EXE と対応する DLL を読み込めます。32／64 ビットとも既定で resident AOT を使用します。付属 DLL のビット数は EXE と一致させる必要があり、同一プロセスで 32／64 ビット DLL を混在させることはできません。API や描画機能の対応範囲は x64 と異なる場合があり、完全な WOW64、16 ビットアプリケーション、カーネルドライバー、DRM への対応を示すものではありません。

詳細と使用上の案内は [Direct3D と x86 の互換性](docs/direct3d-and-x86.md)（繁体中国語）を参照してください。

AppleJuice は開発中です。Windows 全体、すべてのアプリケーション、すべての API 引数の組み合わせには対応していません。読み込みやウィンドウの表示が成功しても、アプリケーション全体の互換性を保証するものではありません。ドライブマッピングは完全な OS サンドボックスではなく、有効なサービスはマッピング先のファイルやネットワークにアクセスする場合があります。信頼できるプログラムだけを実行し、必要に応じてアクセス権を設定してください。

リポジトリには `src/`、`include/`、`launcher/`、`cmake/`、必要なビルドスクリプト、`third_party/`、`x64kernel/` と使用ドキュメントを含みます。

## ライセンス

Copyright (C) 2026 VaderChen.

x64arm エンジンと Win64 Launcher を含む AppleJuice の自作部分には、[ソース公開・商業販売禁止ライセンス](LICENSE.ja.md)を適用します。企業内利用を含む非販売の利用、変更、無料共有を許可します。ソフトウェア販売、有料ホスティング／SaaS、有料サービス、有料製品への統合は禁止します。従業員の通常の給与は販売とみなしませんが、ソフトウェア提供による広告収入、紹介報酬、利用者データの収益化も制限します。必要な場合は著作権者と別途書面による許諾をご相談ください。問い合わせだけでは許諾になりません。

AppleJuiceKernel を静的リンクした実行ファイルと Launcher パッケージも AppleJuice の配布対象です。製品にはカーネルと第三者のライセンス表示を保持してください。リポジトリの分離によって既存のライセンス制限は変わりません。

独自のソース公開ライセンスであり、OSI のオープンソース定義に適合すると主張しません。README は要約です。繁体中国語の全文を正本とし、第三者コンポーネント、別途ライセンスの内容、外部 Windows プログラムには各自のライセンスを適用します。

ライセンス全文：[繁體中文](LICENSE.md) · [English](LICENSE.en.md) · [日本語](LICENSE.ja.md) · [한국어](LICENSE.ko.md)

[商業販売ポリシー](COMMERCIAL-LICENSE.md) · [適用範囲と出典](docs/licensing.md) · [第三者ライセンス](THIRD-PARTY-NOTICES.md)
