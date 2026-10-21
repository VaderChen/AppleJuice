# macOS 簽署

Launcher 建置腳本會簽署必要元件並驗證完整 app。預設使用本機 ad-hoc 簽署：

```sh
python3 scripts/build_launcher.py --jobs 4
codesign --verify --deep --strict 'build/launcher/Win64 Launcher.app'
```

若已自行準備有效的簽署身分，可指定憑證名稱或 SHA-1：

```sh
python3 scripts/build_launcher.py \
  --sign-identity '<簽署憑證名稱或 SHA-1>' --jobs 4
```

需要額外授權的發行配置，須自行準備符合識別碼、簽署身分及所需能力的 profile，再使用 `--cross-arch-profile` 指定。不要提交憑證、私鑰、profile 或個人帳號資料。儲存庫隨附的是程式所需 entitlements 範本，不含個人簽署資料。

ad-hoc 簽署與本機封裝檢查不代表已完成 Developer ID 發布或 Apple 公證。散布時保留完整 app、必要授權及適用的簽署結構；建置方式見[建置與封裝](build-and-release.md)。
