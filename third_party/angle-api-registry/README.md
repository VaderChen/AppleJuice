# ANGLE／Khronos ABI 來源

XML 以固定 gzip 快照保存，來源網址與未壓縮內容的 SHA-256 記錄於 `manifest.json`。gzip 的 mtime 固定為零。

執行 `python3 scripts/generate_angle_api.py` 重建 ABI 表、Smoke wrapper、extension 契約、`profile.json` 與 `webgl-scope.json`。再次執行的內容必須完全相同。

產生器按 XML 節點移除參數名稱，並從完整 C 型別推斷元素與指標層級；不能依字串替換名稱或假定所有參數都有 `ptype`。registry 的 `glGetVertexAttribIiv`／`Iuiv` 長度需按查詢 pname 處理，multi-draw 的陣列長度則按 drawcount。

範圍為 GLES 2.0／3.0、WebGL 相關 ANGLE extension 及 EGL 平台／context／surface／sync。GL desktop 專用及 ES 3.1 專用的交互入口不列入。實際 GPU 能力與 API 結果另由 Smoke 記錄。
