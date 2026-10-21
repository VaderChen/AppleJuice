# AppleJuice

[繁體中文](../README.md) · [English](README.en.md) · **日本語** · [한국어](README.ko.md)

AppleJuice は Apple Silicon Mac 向けの Windows 互換レイヤーと Go／WKWebView の Win64 Launcher を提供します。C++20 ランタイムが PE を読み込み、Windows API を macOS と Metal に対応付けます。CPU の変換と実行には、独立した AppleJuiceKernel の静的 SDK を使用します。

このリポジトリには `x64kernel/` の静的 SDK が含まれ、AppleJuice はその SDK だけでビルド・実行できます。コアの開発、ツール、関連文書は AppleJuiceKernel で管理します。必要なライセンスを同梱した静的リンク済み実行ファイルと Launcher は、AppleJuice の配布対象です。


[使用文件](README.md) · [建置與封裝](build-and-release.md)

## ライセンス

Copyright (C) 2026 VaderChen.

x64arm エンジンと Win64 Launcher を含む AppleJuice の自作部分には、YOURDESK を参考にした[ソース公開・商業販売禁止ライセンス](../LICENSE.ja.md)を適用します。企業内利用を含む非販売の利用、変更、無料共有を許可します。ソフトウェア販売、有料ホスティング／SaaS、有料サービス、有料製品への統合は禁止します。従業員の通常の給与は販売とみなしませんが、ソフトウェア提供による広告収入、紹介報酬、利用者データの収益化も制限します。必要な場合は著作権者と別途書面による許諾をご相談ください。問い合わせだけでは許諾になりません。

独自のソース公開ライセンスであり、OSI のオープンソース定義に適合すると主張しません。README は要約です。繁体中国語の全文を正本とし、第三者コンポーネント、別途ライセンスの内容、外部 Windows プログラムには各自のライセンスを適用します。

ライセンス全文：[繁體中文](../LICENSE.md) · [English](../LICENSE.en.md) · [日本語](../LICENSE.ja.md) · [한국어](../LICENSE.ko.md)

[商業販売ポリシー](../COMMERCIAL-LICENSE.md) · [適用範囲と出典](licensing.md) · [第三者ライセンス](../THIRD-PARTY-NOTICES.md)
