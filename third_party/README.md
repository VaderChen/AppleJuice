# 第三方來源與授權

AppleJuice 的固定第三方來源、來源資訊、雜湊及適用授權隨本目錄提供。CMake 使用這些固定來源建置；一般建置不下載浮動版本。

OpenSSL 使用保留原始編譯來源的發行用來源包，移除不參與靜態函式庫建置的測試、範例與金鑰／憑證容器。上游與隨附來源包的 SHA-256、來源及整理範圍見 [OpenSSL 來源索引](openssl-source.json)；CMake 核對隨附來源包雜湊。

完整元件、來源與授權索引見 [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)。CPU 核心的第三方聲明與授權隨 `x64kernel/share/AppleJuiceKernel/licenses/` 提供。散布成品時應保留適用的授權及著作權聲明。
