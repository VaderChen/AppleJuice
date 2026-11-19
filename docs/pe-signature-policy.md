# PE 簽章與信任來源

AppleJuice 以 Authenticode 雜湊、PKCS#7 簽章、程式碼簽章用途、有效期間及信任鏈檢查外部 PE。Microsoft 簽章政策使用固定的 Microsoft 公開根；憑證中的發行者名稱不會單獨構成信任。

分類器使用以下三個 Microsoft 官方公開根，PEM 位於 `src/services/pe_signature_roots.hpp`。SHA-1 與官方 [PKI repository](https://www.microsoft.com/pkiops/docs/repository.htm) 比對；以下 SHA-256 為下載 DER 檔案的雜湊。

| 根憑證 | 官方來源 | DER SHA-256 |
| --- | --- | --- |
| Microsoft Root Certificate Authority 2010 | [Microsoft PKI](https://www.microsoft.com/pki/certs/MicRooCerAut_2010-06-23.crt) | `df545bf919a2439c36983b54cdfc903dfa4f37d3996d8d84b4c31eec6f3c163e` |
| Microsoft Root Certificate Authority 2011 | [Microsoft PKI](https://www.microsoft.com/pki/certs/MicRooCerAut2011_2011_03_22.crt) | `847df6a78497943f27fc72eb93f9a637320a02b561d0a91b09e87a7807ed7c61` |
| Microsoft ECC Product Root Certificate Authority 2018 | [Microsoft PKI](https://www.microsoft.com/pkiops/certs/Microsoft%20ECC%20Product%20Root%20Certificate%20Authority%202018.crt) | `caca93b9d23d2b6fa76e8b8471931e0df3ec6f63af3cdbb936c41954a1872326` |

每份 PKCS#7 支援單一 signer，並可列舉及選擇巢狀 Authenticode 次要簽章。RFC 3161 與舊式 countersignature 時間戳會驗證內容雜湊、時間戳簽章與 TSA 憑證鏈；TSA 使用該次驗證的明確根，不讀取 OpenSSL 預設根或 macOS 信任庫，程式碼簽章專用根也不會授予 TSA 信任。上述路徑已有獨立真實簽章與負面案例 Smoke。線上撤銷查詢仍未完成，非 canonical PE 配置也會拒絕；要求撤銷檢查且無可驗證結果時回報離線錯誤。此實作尚非完整 Windows WinTrust 或線上 Code Integrity。

2026-10-11 起，Windows 憑證鏈、ROOT provider、WinTrust 與 Schannel 共用 Sandbox 信任來源：上述內建 Microsoft 公開根，加上 Windows C 槽內的 `C:/ProgramData/MacWin/Certificates/ROOT.pem`。檔案透過既有 Windows 檔案系統映射與 DACL 讀取；檔案損壞或存取遭拒會回報失敗。`SSL_CERT_FILE`、`SSL_CERT_DIR`、宿主預設 CA 路徑與 macOS 根憑證不參與這些 Windows 驗證，不自動遷移宿主資料。ROOT provider 目前仍為唯讀；這個 PEM 配置不是原生 Windows 系統憑證儲存格式，也沒有因此完成其他系統 store 的持久操作。正式 Microsoft 簽章政策分類器保留固定 Microsoft 根的限制。

已驗證獨立測試根下真正簽署的 PE、篡改內容、未簽署、錯誤 EKU、未知信任根、截斷與尾端資料。自訂測試根只傳入測試用明確根介面，正式 Microsoft 根介面仍拒絕該測試簽章。證據：`pe-signature-smoke-16/report.json`。真 PE 的 x86／x64 × Interpreter／JIT 四組驗證新 unsigned DLL 拒絕、預載映像與 OS provider 保留、資源映像、政策查詢、旗標錯誤及不可降低；證據：`signature-policy-pe-smoke-16/report.json`。兩者皆位於 `build/steam-api-20261007`。正式 Microsoft 簽署 PE 的接受案例尚待實測。

Steam 第 12 輪的 GPU fatal 已由實際反組譯與服務追蹤確認：政策 8 回 50 後進入 Chromium `PCHECK`，於 `0x140054740` 觸發 INT3。第 13 輪政策 8 實際成功，GPU 未重現該斷點並持續運作；主程序稍後因另一個 AppKit 座標例外退出。此結果不代表 Steam 登入頁已完成。

公開根用於驗證第三方程式簽章，不含私鑰或個人簽署資料。Windows 信任來源亦可包含映射的 Windows 磁碟內 `C:/ProgramData/MacWin/Certificates/ROOT.pem`；其存取依該磁碟的檔案權限處理，不自動遷移宿主個人憑證。

此功能不代表完整 Windows WinTrust 或線上 Code Integrity。巢狀 Authenticode、舊式 countersignature、線上撤銷查詢及非標準 PE 配置仍有限制；支援個別驗證路徑不代表任意已簽署程式均可執行。
