# PE 簽章與信任來源

AppleJuice 以 Authenticode 雜湊、PKCS#7 簽章、程式碼簽章用途、有效期間及信任鏈檢查外部 PE。Microsoft 簽章政策使用固定的 Microsoft 公開根；憑證中的發行者名稱不會單獨構成信任。

分類器使用以下三個 Microsoft 官方公開根，PEM 位於 `src/services/pe_signature_roots.hpp`。SHA-1 與官方 [PKI repository](https://www.microsoft.com/pkiops/docs/repository.htm) 比對；以下 SHA-256 為下載 DER 檔案的雜湊。

| 根憑證 | 官方來源 | DER SHA-256 |
| --- | --- | --- |
| Microsoft Root Certificate Authority 2010 | [Microsoft PKI](https://www.microsoft.com/pki/certs/MicRooCerAut_2010-06-23.crt) | `df545bf919a2439c36983b54cdfc903dfa4f37d3996d8d84b4c31eec6f3c163e` |
| Microsoft Root Certificate Authority 2011 | [Microsoft PKI](https://www.microsoft.com/pki/certs/MicRooCerAut2011_2011_03_22.crt) | `847df6a78497943f27fc72eb93f9a637320a02b561d0a91b09e87a7807ed7c61` |
| Microsoft ECC Product Root Certificate Authority 2018 | [Microsoft PKI](https://www.microsoft.com/pkiops/certs/Microsoft%20ECC%20Product%20Root%20Certificate%20Authority%202018.crt) | `caca93b9d23d2b6fa76e8b8471931e0df3ec6f63af3cdbb936c41954a1872326` |

公開根用於驗證第三方程式簽章，不含私鑰或個人簽署資料。Windows 信任來源亦可包含映射的 Windows 磁碟內 `C:/ProgramData/MacWin/Certificates/ROOT.pem`；其存取依該磁碟的檔案權限處理，不自動遷移宿主個人憑證。

此功能不代表完整 Windows WinTrust 或線上 Code Integrity。巢狀 Authenticode、舊式 countersignature、線上撤銷查詢及非標準 PE 配置仍有限制；支援個別驗證路徑不代表任意已簽署程式均可執行。
