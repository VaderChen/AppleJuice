![AppleJuice — Windows Gaming. Squeezed for Apple Silicon.](docs/images/branding/hero.svg)

# AppleJuice

Apple Silicon Mac で対応する Windows ゲームをプレイできます。独自の CPU 変換器で x86／x64 コードを実行し、Direct3D の描画を Metal に変換します。

**[Mac 版をダウンロード](https://github.com/VaderChen/AppleJuice/releases/latest)** · **[ゲーム互換性](https://vaderchen.github.io/AppleJuice/tested/?lang=ja)** · [クイックスタート](#start) · [技術ガイド](docs/product/guide.ja.md) · [実機プレイ映像](#games)

[English](README.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [☕ Buy Me a Coffee](https://buymeacoffee.com/vaderchen)

[![Release](https://img.shields.io/github/v/release/VaderChen/AppleJuice)](https://github.com/VaderChen/AppleJuice/releases/latest) ![macOS 13+](https://img.shields.io/badge/macOS-13%2B-394a40) ![Apple Silicon](https://img.shields.io/badge/architecture-ARM64-394a40) [![Source available](https://img.shields.io/badge/license-source--available-394a40)](LICENSE.ja.md)

<a id="start"></a>

## クイックスタート

Apple Silicon · macOS 13 以降。配布版にはランタイムが含まれます。

1. リリースページから arm64 用 DMG をダウンロードし、Finder で開いて AppleJuice.app を「アプリケーション」にドラッグします。
2. AppleJuice の「EXE を追加」でゲームを選び、作業フォルダとドライブを設定します。セーブに必要な書き込みを許可し、再生ボタンを押します。

[インストールとトラブル対処](docs/product/install.md) · [Steam のインストール手順を見る（39 秒）](docs/media/gameplay/steam-temporary-install.mp4)

<a id="games"></a>

## 実機プレイ映像

**[78 秒のプレイ映像を見る](docs/media/gameplay/applejuice-gameplay.mp4)** · 0:00 Steam ライブラリから起動

両作品のプレイ映像は Apple M4 Pro、ユニファイドメモリ 64 GiB、macOS 27.2.1、AppleJuice 0.26.1126（ビルド 727）で収録しました。両作品とも D3D11 を使用。ゲーム出力と無音の録画は 1920 × 1080 で、ランチャーの MetalFX 拡大とフレーム補間はオフです。

| 0:12 · フロンティアハンター ～エルザの運命の輪～ | 0:44 · テイルズ オブ アライズ |
|---|---|
| ![エルザが船室を走り、折り返す場面。](docs/images/gameplay/frontier-hunter.png) | ![ダンジョンを進み、敵と遭遇して戦闘に入る場面。](docs/images/gameplay/tales-of-arise.png) |
| Steam 15701452 · CPU 2 コア | Steam 12823607 · CPU 4 コア |

11 月 26 日版では『三國志XI with パワーアップキット』が既定の設定でゲーム本来のフレームレート上限に達しました。 [Release ↗](https://github.com/VaderChen/AppleJuice/releases/tag/v0.26.1126-build-727)

## ゲーム互換性

<!-- compatibility:start -->

**動作確認済みゲーム · 46** (2026-11-26) · 別掲のツール · 5

- <img src="docs/images/support-levels/good.png" width="16" alt=""> 良好 · 30
- <img src="docs/images/support-levels/metalfx.png" width="16" alt=""> 良好（MetalFX） · 5
- <img src="docs/images/support-levels/slow.png" width="16" alt=""> 低効率 · 7
- <img src="docs/images/support-levels/menu.png" width="16" alt=""> 起動可 · 2
- <img src="docs/images/support-levels/none.png" width="16" alt=""> 動作不可・対応中 · 2

<!-- compatibility:end -->

「良好（MetalFX）」は、Launcher のフレーム補間または 2 倍拡大（MetalFX）を有効にすると「良好」の基準を満たすことを示します。無効の場合は「低効率」です。

DEATH STRANDING DIRECTOR'S CUT は現在「起動可」です。第 3 章のセーブ読込とキャラクター移動は確認済みですが、白いちらつきが発生する場合があり、安定した 50 FPS には達していません。問題を解決し、実際のプレイと他の場面の検証を終えた後に評価を更新します。「良好」は安定して 50 FPS を超えるか、アプリ自身のフレームレート上限に達することを基準とします。

記録は当時のバージョンと設定によるもので、ゲーム全体の流れ、他のバージョン、すべての設定での動作を示すものではありません。

[ゲーム互換性 ↗](https://vaderchen.github.io/AppleJuice/tested/?lang=ja) · [ゲームの動作を報告](https://github.com/VaderChen/AppleJuice/issues/new?template=game.md)

## アーキテクチャ

常駐 AOT はゲストの命令を ARM64 コードに変換し、常駐ワーカーで実行します。ランチャーではゲームごとに CPU 数を設定できます。Direct3D 9〜12 の対応範囲は異なり、D3D12 は部分対応です。

MetalFX による 2 倍拡大と HRTF ヘッドフォンミキシングを選択できます。フレーム補間には macOS 26 以降と対応機器が必要です。補間で表示フレームが増えてもゲーム自体の更新頻度は変わりません。

![PE / ARM64 / Metal](docs/images/branding/runtime.svg)

[技術ガイド](docs/product/guide.ja.md) · [PE → ARM64 / Direct3D → Metal](docs/product/architecture.md) · [ベンチマーク](https://vaderchen.github.io/AppleJuice/geekbench7/?lang=ja)

## 開発方針

Windows API の対応範囲を広げ、ゲームの互換性とパフォーマンスを改善します。

独自のソース公開ライセンスでは、非販売目的の利用、改変、無償配布が認められています。販売と有償サービスには制限があります。

[Support](SUPPORT.md) · [Contributing](CONTRIBUTING.md) · [License](LICENSE.ja.md) · [Third-party notices](THIRD-PARTY-NOTICES.md)

各ゲームには販売元のライセンスが適用されます。AppleJuice は Apple および Microsoft から独立したプロジェクトです。
