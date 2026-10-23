// 原生 WKWebView Smoke 僅由 --smoke 模式載入；操作和正式版共用的 DOM、xterm 與 Go bridge。
(async () => {
  const checks = [], captures = [], transcripts = {};
  const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
  function check(name, condition, detail = '') { api('smokeLog', {data: `${condition ? '通過' : '失敗'} ${name}`}); if (!condition) throw new Error(`${name}：${detail || '條件不符'}`); checks.push(name); }
  async function until(predicate, message, timeout = 18000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) { if (await predicate()) return; await delay(80); }
    throw new Error(`等待逾時：${message}`);
  }
  async function click(id) { check(`可操作 ${id}`, !$(id).disabled && !$(id).hidden); $(id).click(); await until(() => !busy, id); }
  async function capture(name) { await delay(150); await api('smokeSnapshot', { data: name }); await until(() => api('smokeSnapshotReady', { data: name }), name); captures.push(name); }
  async function choose(id) { const row = document.querySelector(`[data-id="${id}"] .select-program`); if (!row && state.programs.some(p => p.id === id && p.temporary)) { $('temporary-session-select').value = id; $('temporary-session-select').dispatchEvent(new Event('change')); } else if (!row) throw new Error('找不到程式列表項目'); else row.click(); await until(() => !busy && state.selected === id, '選取程式'); }
  function checkSidebarActions() {
    const add = $('add').getBoundingClientRect(), temporary = $('temporary').getBoundingClientRect();
    check(`側欄兩個入口在 ${innerWidth}px 視窗同列等寬`, Math.abs(add.top - temporary.top) < 1 && Math.abs(add.width - temporary.width) < 1 && add.right < temporary.left);
    check(`側欄入口在 ${innerWidth}px 視窗不溢出`, ['add', 'temporary'].every(id => $(id).scrollWidth <= $(id).clientWidth));
  }
  function text(id) {
    const term = terminals.get(id)?.term; if (!term) return '';
    const buffer = term.buffer.active, rows = [];
    for (let i = 0; i < buffer.length; i++) rows.push(buffer.getLine(i).translateToString(true));
    return rows.join('\n');
  }
  async function checkWindowsFonts() {
    await click('global-tab-windows');
    const tabs = [...document.querySelectorAll('[data-global-tab]')].map(button => button.dataset.globalTab);
    check('Windows 介面頁位於進階下方', tabs.indexOf('windows') === tabs.indexOf('advanced') + 1 && !$('global-windows').hidden && $('windows-font').closest('[role=tabpanel]').id === 'global-windows');
    const font = $('windows-font'), original = state.globalSettings.windowsFont;
    const choices = ['PingFang TC', 'Helvetica'].filter(family => windowsFonts.includes(family));
    if (choices.length < 2) choices.splice(0, choices.length, ...windowsFonts.slice(0, 2));
    check('字型選單來自本機向量字型', choices.length === 2 && font.options.length === windowsFonts.length + 1 && font.options[0].value === 'auto');
    for (const family of choices) {
      font.value = family; font.dispatchEvent(new Event('change'));
      check('字型預覽套用 ' + family, getComputedStyle($('windows-font-preview')).fontFamily.includes(family));
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 Windows 字型');
      check('字型儲存 ' + family, state.globalSettings.windowsFont === family && !$('global-windows').hidden);
      await click('app-settings-close'); await click('app-settings'); await click('global-tab-windows');
      check('字型重新開啟 ' + family, font.value === family);
    }
    await capture('windows-font-settings.png');
    font.value = 'auto'; font.dispatchEvent(new Event('change'));await click('global-settings-cancel');await click('app-settings');await click('global-tab-windows');
    check('取消字型草稿不覆寫已儲存選擇', font.value === choices[1] && state.globalSettings.windowsFont === choices[1]);
    font.value = original; font.dispatchEvent(new Event('change'));await click('global-settings-save');await until(() => !$('global-settings-save').disabled, '還原 Windows 字型');
  }
  async function checkOpenGLPreferences() {
    await click('global-tab-cpu');
    check('CPU 分頁提供兩種架構與 resident AOT 模式', !$('global-cpu').hidden && $('global-gpu').hidden && $('execution-mode').textContent.includes('Resident AOT') && $('global-cpu').textContent.includes('x86'));
    const originalCores = {x64: state.globalSettings.cpuCoresX64, x86: state.globalSettings.cpuCoresX86};
    const compiler = $('compiler-workers');
    check('編譯執行緒預設 2 且可調 0–8', state.globalSettings.compilerWorkers === 2 && compiler.min === '0' && compiler.max === '8' && compiler.step === '1');
    for (const value of [0, 4, 8, 2]) {
      compiler.value = value; compiler.dispatchEvent(new Event('input'));
      const label = value ? t`${value.toLocaleString(currentLanguage)} 個執行緒` : t('同步編譯');
      check(`編譯執行緒 ${value} 即時顯示`, $('compiler-workers-value').textContent === label && compiler.getAttribute('aria-valuetext') === label);
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存編譯執行緒');
      check(`編譯執行緒儲存 ${value}`, state.globalSettings.compilerWorkers === value);
      await click('app-settings-close'); await click('app-settings'); await click('global-tab-cpu');
      check(`編譯執行緒重新開啟 ${value}`, Number(compiler.value) === value);
    }
    compiler.value = 0; compiler.dispatchEvent(new Event('input'));
    await click('global-settings-cancel'); await click('app-settings'); await click('global-tab-cpu');
    check('取消保留原編譯執行緒設定', Number(compiler.value) === 2 && state.globalSettings.compilerWorkers === 2);
    for (const architecture of ['x64', 'x86']) {
      const id = 'cpu-cores-' + architecture, input = $(id), maximum = state.cpuLimits[architecture + 'Max'];
      check(`${architecture} 核心滑桿每格 1 且不超過宿主實體核心`, input.type === 'range' && input.min === '1' && input.step === '1' && Number(input.max) === maximum && maximum <= state.cpuLimits.physicalCores && !input.disabled);
      input.value = '1'; input.stepUp(); input.dispatchEvent(new Event('input'));
      check(`${architecture} 核心數與無障礙文字即時更新`, Number(input.value) === Math.min(2, maximum) && $(id + '-value').textContent === t`${Number(input.value).toLocaleString(currentLanguage)} 個核心` && input.getAttribute('aria-valuetext') === $(id + '-value').textContent);
      for (const value of [1, maximum]) {
        input.value = value; input.dispatchEvent(new Event('input'));
        await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 CPU 核心數');
        check(`${architecture} 核心數儲存邊界 ${value}`, state.globalSettings['cpuCores' + architecture.toUpperCase()] === value);
        await click('app-settings-close'); await click('app-settings'); await click('global-tab-cpu');
        check(`${architecture} 核心數重新開啟 ${value}`, Number(input.value) === value);
      }
      input.value = maximum + 1; input.dispatchEvent(new Event('input'));
      check(`${architecture} 滑桿不接受超限核心數`, Number(input.value) === maximum);
    }
    await capture('cpu-settings.png');
    for (const architecture of ['x64', 'x86']) $('cpu-cores-' + architecture).value = 1;
    await click('global-settings-cancel'); await click('app-settings'); await click('global-tab-cpu');
    check('取消不儲存兩種架構的核心草稿', Number($('cpu-cores-x64').value) === state.cpuLimits.x64Max && Number($('cpu-cores-x86').value) === state.cpuLimits.x86Max);
    for (const architecture of ['x64', 'x86']) $('cpu-cores-' + architecture).value = originalCores[architecture];
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '還原 CPU 預設');
    await click('global-tab-gpu');
    check('GPU 分頁集中繪圖設定', !$('global-gpu').hidden && $('opengl-backend').closest('[role=tabpanel]').id === 'global-gpu' && $('gpu-memory').closest('[role=tabpanel]').id === 'global-gpu' && !$('global-advanced').querySelector('#opengl-backend'));
    check('OpenGL 預設 auto 與三種選項', $('opengl-backend').value === 'auto' && [...$('opengl-backend').options].map(o => o.value).join(',') === 'auto,metal,legacy');
    for (const mode of ['metal', 'legacy', 'auto']) {
      $('opengl-backend').value = mode; await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 OpenGL 後端');
      check('OpenGL 儲存 ' + mode, state.globalSettings.openGLBackend === mode);
      await click('app-settings-close'); await click('app-settings'); await click('global-tab-gpu');
      check('OpenGL 重新開啟 ' + mode, $('opengl-backend').value === mode);
    }
    $('opengl-backend').value = 'legacy'; await click('global-settings-cancel'); await click('app-settings'); await click('global-tab-gpu');
    check('取消保留 OpenGL auto', $('opengl-backend').value === 'auto');
    const rect = $('opengl-backend').getBoundingClientRect();
    check('OpenGL 選單在設定頁範圍內', rect.width > 0 && rect.right <= $('app-settings-dialog').getBoundingClientRect().right);
    await capture('opengl-settings.png');
    await click('global-tab-advanced');
    await checkCertificateRootPreferences();
  }
  async function checkCertificateRootPreferences() {
    const certificateFile = $('certificate-roots-file'), certificatePath = $('certificate-roots-path');
    check('根憑證使用預設關閉，生成位置可獨立設定且為相對路徑', !certificateFile.checked && !certificatePath.disabled && certificatePath.value === 'ProgramData/MacWin/Certificates/ROOT.pem');
    await click('certificate-roots-file'); certificatePath.value = '憑證/public.pem';
    check('啟用使用憑證仍可設定保存位置', !certificatePath.disabled);
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存根憑證開關');
    check('保存根憑證開關與相對位置', state.globalSettings.certificateRootsFile && state.globalSettings.certificateRootsPath === '憑證/public.pem');
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-advanced');
    check('重開設定保留根憑證選擇', certificateFile.checked && !certificatePath.disabled && certificatePath.value === '憑證/public.pem');
    await click('certificate-roots-file'); await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '關閉根憑證檔案');
    check('可恢復預設不使用根憑證檔案且仍能設定生成位置', !state.globalSettings.certificateRootsFile && !certificatePath.disabled);
  }
  try {
    await window.launcherReady;
    if (smokeConfig.scenario === 'settings' && smokeConfig.settingsTest === 'cpu-defaults') {
      check('新設定預設 x86 雙核與 2 個編譯 worker', state.globalSettings.cpuCoresX86 === Math.min(2, state.cpuLimits.x86Max) && state.globalSettings.compilerWorkers === 2);
      await click('app-settings'); await click('global-tab-cpu');
      for (const workers of [0, 4, 2]) {
        $('compiler-workers').value = workers; $('compiler-workers').dispatchEvent(new Event('input'));
        await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存編譯 worker');
        await click('app-settings-close'); await click('app-settings'); await click('global-tab-cpu');
        check(`遵守 UI 明確設定的 ${workers} 個編譯 worker`, state.globalSettings.compilerWorkers === workers && Number($('compiler-workers').value) === workers);
      }
      for (const architecture of ['x64', 'x86']) {
        const id = 'cpu-cores-' + architecture, maximum = state.cpuLimits[architecture + 'Max'];
        for (const value of [1, Math.min(2, maximum), Math.min(3, maximum)]) {
          $(id).value = value; $(id).dispatchEvent(new Event('input'));
          check(`${architecture} ${value} 核依門檻顯示提示`, $(id + '-warning').hidden === (value <= 2));
        }
      }
      await capture('cpu-core-warning.png');
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存超過雙核設定');
      check('提示不阻擋儲存超過雙核的設定', state.globalSettings.cpuCoresX86 === Math.min(3, state.cpuLimits.x86Max));
      await click('app-settings-close');
      await api('smokePick', { paths: smokeConfig.programs }); await click('add');
      await choose(state.programs[0].id); await click('tab-settings');
      const maximum = state.cpuLimits[selected().architecture === 'x86' ? 'x86Max' : 'x64Max'];
      for (const value of [0, Math.min(2, maximum), Math.min(3, maximum)]) {
        $('program-cpu-cores').value = value; $('program-cpu-cores').dispatchEvent(new Event('input', { bubbles: true }));
        check(`個別程式 ${value} 核依有效值顯示提示`, $('program-cpu-warning').hidden === ((value || Math.min(3, maximum)) <= 2));
      }
      await capture('program-core-warning.png');
      await click('save'); await until(() => !busy, '儲存個別程式核心數');
      check('提示不阻擋個別程式保存設定', selected().cpuCores === Math.min(3, maximum));
      await api('smokeResult', { result: { passed: true, checks, captures, state } });
      return;
    }
    if (smokeConfig.scenario === 'settings' && ['gpu-save', 'gpu-reload'].includes(smokeConfig.settingsTest)) {
      const reload = smokeConfig.settingsTest === 'gpu-reload';
      if (!reload) {
        await api('smokePick', { paths: smokeConfig.programs }); await click('add');
        check('新程式採用 4 GiB 預設並遵守系統上限', state.programs.length === 2 && state.programs.every(p => p.gpuMemoryMiB === Math.min(4096, state.gpuMemoryLimits.maxMiB)));
      }
      const programs = [...state.programs], values = [3072, 1024];
      check('跨建置測試保留兩個程式', programs.length === 2);
      for (let i = 0; i < programs.length; ++i) {
        await choose(programs[i].id); await click('app-settings'); await click('global-tab-cpu');
        check('執行模式顯示 Resident AOT', $('execution-mode').textContent.includes('Resident AOT'));
        await click('global-tab-gpu');
        const memory = $('gpu-memory');
        check('GPU 刻度為 512 MiB', Number(memory.step) === 512 && Number(memory.min) === 512);
        if (!reload) {
          memory.value = values[i]; memory.dispatchEvent(new Event('input', { bubbles: true }));
          await delay(200);
          check('狀態輪詢不覆蓋 GPU 草稿', Number(memory.value) === values[i]);
          await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '保存 GPU');
        }
        check(`${reload ? '重新 BUILD 後' : '儲存後'}保留個別 GPU 容量`, Number(memory.value) === values[i] && selected().gpuMemoryMiB === values[i]);
        await click('app-settings-close'); await click('tab-settings'); await click('save');
        check('主設定儲存保留 GPU 容量', selected().gpuMemoryMiB === values[i]);
      }
      await choose(programs[0].id); await click('app-settings'); await click('global-tab-gpu');
      check('切換程式及重開設定保留 GPU 容量', Number($('gpu-memory').value) === values[0]);
      await capture(reload ? 'gpu-after-build.png' : 'gpu-before-build.png');
      await click('app-settings-close');
      if (reload) {
        await click('run'); await until(() => selected().status === 'exited', 'GPU 設定傳入實際引擎');
        await click('diagnostics');
        check('重新 BUILD 後啟動仍採用自訂 GPU 容量', $('log-content').textContent.includes(`gpu_memory_limit_bytes=${values[0] * 1024 * 1024}`));
        $('close-log').click();
      }
      await api('smokeResult', { result: { passed: true, checks, captures, state } });
      return;
    }
    if (smokeConfig.scenario === 'settings' && smokeConfig.settingsTest === 'certificate-roots') {
      await click('app-settings'); await click('global-tab-advanced');
      await checkCertificateRootPreferences();
      await api('smokeResult', { result: { passed: true, checks, captures, state } });
      return;
    }
    check('自動語言依系統偏好解析', resolveLanguage('auto', ['fr-FR', 'ja-JP']) === 'ja' && resolveLanguage('auto', ['zh-TW']) === 'zh-Hant' && resolveLanguage('auto', ['ko-KR']) === 'ko' && resolveLanguage('auto', ['de-DE']) === 'en');
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, language: 'zh-Hant', theme: 'forest'}})));
    await click('app-settings');
    check('設定分頁包含 Windows 介面、CPU、GPU 與實驗性功能', [...document.querySelectorAll('[data-global-tab]')].map(button => button.dataset.globalTab).join(',') === 'general,advanced,windows,cpu,gpu,experimental,about');
    await click('global-tab-experimental');
    check('原生函式庫橋接層預設關閉', !$('global-experimental').hidden && !$('native-library-bridge').checked && !state.globalSettings.nativeLibraryBridge);
    check('V8 原型預設關閉且可加入內建測試', !$('native-v8-bridge').checked && !state.globalSettings.nativeV8Bridge && $('add-native-v8-tests').disabled === !state.nativeV8TestsReady);
    await click('native-library-bridge'); await click('global-settings-save');
    await until(() => !$('global-settings-save').disabled, '儲存橋接層');
    check('橋接層開關已保存', state.globalSettings.nativeLibraryBridge === true);
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-experimental');
    check('重新開啟保留橋接層', $('native-library-bridge').checked);
    await click('native-library-bridge'); await click('global-settings-cancel');
    await click('app-settings'); await click('global-tab-experimental');
    check('取消不改動橋接層設定', $('native-library-bridge').checked && state.globalSettings.nativeLibraryBridge === true);
    await click('native-library-bridge'); await click('global-settings-save');
    await until(() => !$('global-settings-save').disabled, '關閉橋接層');
    check('橋接層可關閉', state.globalSettings.nativeLibraryBridge === false);
    await click('global-tab-general');
    check('未選取程式時停用個別 GPU 容量設定', $('gpu-memory').disabled);
    check('一般頁提供五個語言選項與五種配色', $('ui-language').options.length === 5 && $('ui-theme').options.length === 5);
    const names = {en: 'Settings', ja: '設定', ko: '설정', 'zh-Hant': '設定'};
    for (const [language, heading] of Object.entries(names)) {
      $('ui-language').value = language; $('ui-language').dispatchEvent(new Event('change'));
      check(`語言 ${language} 即時切換`, document.documentElement.lang === language && $('app-settings-title').textContent === heading && $('save').textContent === t('儲存設定'));
    }
    $('ui-language').value = 'en'; $('ui-language').dispatchEvent(new Event('change'));
    const colors = new Set();
    for (const theme of ['forest', 'ocean', 'violet', 'amber', 'dark']) {
      $('ui-theme').value = theme; $('ui-theme').dispatchEvent(new Event('change'));
      colors.add(getComputedStyle(document.documentElement).getPropertyValue('--accent'));
      check(`配色 ${theme} 即時預覽`, document.documentElement.dataset.theme === theme && !$('global-general').hidden, JSON.stringify({theme: document.documentElement.dataset.theme, selected: $('ui-theme').value, generalHidden: $('global-general').hidden, dialogOpen: $('app-settings-dialog').open}));
      check(`配色 ${theme} 選單未溢出`, $('ui-theme').getBoundingClientRect().right <= $('app-settings-dialog').getBoundingClientRect().right);
      await capture(`appearance-${theme}.png`);
    }
    check('五種配色使用不同主色', colors.size === 5);
    await click('global-settings-cancel');
    check('取消還原已儲存語言與配色', document.documentElement.lang === 'zh-Hant' && document.documentElement.dataset.theme === 'forest');
    await click('app-settings');
    $('ui-language').value = 'ko'; $('ui-theme').value = 'dark'; $('ui-language').dispatchEvent(new Event('change'));
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存介面偏好');
    check('從一般頁儲存不跳到進階頁', !$('global-general').hidden);
    await click('app-settings-close'); await click('app-settings');
    check('重新開啟設定保留語言及配色', $('ui-language').value === 'ko' && $('ui-theme').value === 'dark' && state.globalSettings.language === 'ko');
    await capture('appearance-korean.png');
    $('ui-language').value = 'zh-Hant'; $('ui-theme').value = 'forest'; $('ui-language').dispatchEvent(new Event('change'));
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '還原測試語言'); await click('app-settings-close');

    if (smokeConfig.scenario === 'settings') {
      await click('app-settings'); await click('global-tab-advanced');
      await checkOpenGLPreferences();
      await checkWindowsFonts();
      await api('smokeResult', { result: { passed: true, checks, captures, state } });
      return;
    }

    check('空程式庫顯示加入入口', !$('empty').hidden && state.programs.length === 0);
    check('右側不再有程式資訊頂部區塊', !document.querySelector('.detail-header'));
    checkSidebarActions();
    await capture('01-empty.png');
    if (smokeConfig.probePickers !== false) { const native = await api('smokeProbe'); check('原生 EXE 選擇器顯示及取消', native.length === 1); for (let i=0;i<3;i++) { const temporary = await api('smokeProbeTemporary'); check(`臨時選擇器第 ${i+1} 次開啟及取消`, temporary.length === 1); } }
    await api('smokePick', { paths: smokeConfig.programs }); await click('add');
    check('加入四個實際 EXE', state.programs.length === 4);
    check('首次加入顯示加速分析 DLG', $('analysis-dialog').open && !$('analysis-prompt').hidden);
    await capture('analysis-first-add.png');
    while ($('analysis-dialog').open) await click('analysis-cancel');
    check('略過分析仍保留新增程式', state.programs.length === 4);
    await click('app-settings');
    for (const language of ['en', 'ja', 'ko']) {
      $('ui-language').value = language; $('ui-language').dispatchEvent(new Event('change'));
      check(`語言 ${language} 保留程式名稱及路徑`, state.programs.every(p => { const row = document.querySelector(`[data-id="${p.id}"]`); return row.title === p.path && row.querySelector('.row-name').textContent === p.name; }));
      check(`語言 ${language} 更新動態按鈕`, $('save').textContent === t('儲存設定') && $('analyze').textContent === t('程式加速分析'));
      await capture(`appearance-${language}-apps.png`);
    }
    await click('global-settings-cancel');

    await click('tab-settings');
    check('設定頁提供程式加速分析', !$('analyze').disabled && $('analyze').textContent === '程式加速分析');
    await click('analyze');
    check('分析階段與進度在 DLG', $('analysis-dialog').open && !$('analysis-progress-area').hidden && $('analysis-progress').closest('dialog') === $('analysis-dialog') && $('analysis-spinner').closest('dialog') === $('analysis-dialog'));
    await until(() => selected().analysis && !analysisActive(selected()), '加速分析完成', 60000);
    check('成功建立 resident AOT 快取', selected().analysis.status === 'completed' && $('analysis-progress').value === $('analysis-progress').max);
    await capture('analysis-completed.png'); await click('analysis-cancel');
    const [hello, jq, gui, same] = state.programs;
    check('由 PE 子系統辨識 CLI 與 GUI', hello.kind === 'cli' && jq.kind === 'cli' && gui.kind === 'gui');
    check('同名 EXE 有獨立設定路徑', hello.name === same.name && hello.configPath !== same.configPath);
    const initialDefaults = state.globalSettings.cDrive;
    await click('app-settings'); check('設定對話框與一般分頁', $('app-settings-dialog').open && !$('global-general').hidden);
    await click('global-tab-about'); check('關於分頁', !$('global-about').hidden && $('global-general').hidden);
    check('關於與頁尾顯示同一份建置日期及時間版本', /^0\.\d{2}\.\d{4} build \d{4}$/.test(state.version) && $('app-version').textContent.includes(state.version) && $('app-build-version').textContent === state.version);
    await capture('build-version.png');
    await click('global-tab-advanced'); check('進階分頁與必要 C', !$('global-advanced').hidden && $('global-c-drive').required && !document.querySelector('.global-drive-row .drive-remove'));
    await checkOpenGLPreferences();
    $('global-c-drive').value = ''; await click('global-settings-save');
    check('拒絕空白 C 且未改寫設定', !$('global-c-drive').checkValidity() && state.globalSettings.cDrive === initialDefaults);
    await api('smokePickFolder', { paths: [smokeConfig.output] }); document.querySelector('.global-drive-row .drive-pick').click(); await until(() => !busy, '全域目錄選擇');
    check('可選擇預設 C 目錄', $('global-c-drive').value === smokeConfig.output);
    await click('global-add-drive'); await click('global-add-drive');
    const driveRows = [...document.querySelectorAll('.global-drive-row')].slice(1);
    for (const row of driveRows) row.querySelector('.drive-path').value = smokeConfig.output;
    check('依序新增 D 與 E', driveRows.map(row => row.querySelector('.drive-letter').value).join(',') === 'D,E');
    driveRows[1].querySelector('.drive-letter').value = 'D'; await click('global-settings-save');
    check('拒絕重複磁碟代號', !$('global-settings-error').hidden && $('global-settings-error').textContent.includes('重複'));
    driveRows[1].querySelector('.drive-letter').value = 'E'; await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存全域磁碟');
    check('全域 C D E 設定已套用', state.globalSettings.cDrive === smokeConfig.output && state.globalSettings.drives.D === smokeConfig.output && state.globalSettings.drives.E === smokeConfig.output);
    check('既有 EXE 的 C 與工作目錄不變', state.programs.every(p => p.cDrive === initialDefaults) && state.programs[0].workDir === hello.workDir);
    await capture('global-settings-advanced.png');
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-advanced');
    check('重新開啟保留磁碟設定', $('global-c-drive').value === smokeConfig.output && document.querySelectorAll('.drive-letter').length === 2);
    document.querySelector('.drive-remove').click(); await click('global-settings-cancel'); await click('app-settings'); await click('global-tab-advanced');
    check('取消不儲存刪除映射', document.querySelectorAll('.drive-letter').length === 2);
    $('global-c-drive').value = initialDefaults; await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '還原預設 C'); await click('app-settings-close');

    await api('smokePick', { paths: [smokeConfig.programs[0]] }); await click('add');
    check('重複加入不增加列表', state.programs.length === 4);
    await click('tab-settings'); $('arguments').value = '--profile "中文設定"'; $('arguments').dispatchEvent(new Event('input', { bubbles: true }));
    await click('app-settings'); await click('global-tab-gpu');
    check('GPU 記憶體預設為 4 GiB 且受系統上限限制', Number($('gpu-memory').value) === Math.min(4096, state.gpuMemoryLimits.maxMiB));
    check('GPU 容量標示目前程式且移出程式設定頁', !$('gpu-memory').disabled && $('gpu-settings-program').textContent.includes(hello.name) && !$('settings').querySelector('#gpu-memory'));
    const memory = $('gpu-memory'), limits = state.gpuMemoryLimits;
    check('GPU 滑桿每格 512 MiB 且上限向下對齊系統 50%', memory.type === 'range' && Number(memory.min) === 512 && Number(memory.step) === 512 && Number(memory.max) === limits.maxMiB && limits.maxMiB * 1024 * 1024 <= limits.systemMemoryBytes / 2 && (limits.maxMiB + 512) * 1024 * 1024 > limits.systemMemoryBytes / 2);
    memory.stepUp(); memory.dispatchEvent(new Event('input', { bubbles: true }));
    check('滑桿調整一格即時更新容量與輔助文字', Number(memory.value) === Math.min(limits.defaultMiB + 512, limits.maxMiB) && memory.getAttribute('aria-valuetext') === $('gpu-memory-value').textContent);
    await delay(200); check('狀態輪詢保留 GPU 滑桿草稿', Number(memory.value) === Math.min(limits.defaultMiB + 512, limits.maxMiB) && selected().gpuMemoryMiB === limits.defaultMiB);
    memory.value = '1'; check('滑桿不低於最小 512 MiB', memory.value === '512');
    memory.stepDown(); check('滑桿不能向下跨越最小容量', memory.value === '512');
    memory.value = String(limits.maxMiB + 512); check('滑桿不能超過系統 50%', Number(memory.value) === limits.maxMiB);
    for (const value of [16, 33, limits.maxMiB + 512]) {
      let rejected = false;
      try { await api('save', { id: hello.id, settings: { ...settingsFrom(selected()), gpuMemoryMiB: value } }); } catch (error) { rejected = error.message.includes('50%'); }
      check(`後端拒絕無效 GPU 容量 ${value}`, rejected && selected().gpuMemoryMiB === state.gpuMemoryLimits.defaultMiB);
    }
    for (const value of [limits.minMiB, limits.maxMiB]) {
      memory.value = value; memory.dispatchEvent(new Event('input', { bubbles: true }));
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 GPU 容量邊界');
      check(`GPU 滑桿邊界 ${value} 獨立儲存`, selected().gpuMemoryMiB === value && state.programs.find(p => p.id === same.id).gpuMemoryMiB === state.gpuMemoryLimits.defaultMiB);
    }
    $('gpu-memory').value = '512'; memory.dispatchEvent(new Event('input', { bubbles: true }));
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存個別 GPU 容量');
    check('GPU 容量獨立儲存', selected().gpuMemoryMiB === 512 && state.programs.find(p => p.id === same.id).gpuMemoryMiB === state.gpuMemoryLimits.defaultMiB);
    check('GPU 儲存保留未儲存的啟動參數草稿', selected().arguments === '' && $('arguments').value === '--profile "中文設定"');
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-gpu');
    check('重新開啟 GPU 分頁保留容量', $('gpu-memory').value === '512');
    await capture('gpu-memory-slider.png');
    $('gpu-memory').value = '1024'; $('opengl-backend').value = 'legacy'; await click('global-settings-cancel');
    check('取消 GPU 設定不改已保存容量或主畫面草稿', selected().gpuMemoryMiB === 512 && state.globalSettings.openGLBackend === 'auto' && readSettings().gpuMemoryMiB === 512 && $('arguments').value === '--profile "中文設定"');
    await click('save'); check('設定儲存回傳正確', selected().arguments === '--profile "中文設定"');
    check('另一同名 EXE 設定不受影響', state.programs.find(p => p.id === same.id).arguments === '');
    check('安裝目錄留空顯示 C 槽 Program Files', $('install-root').value === '' && $('install-default').textContent === `預設：${selected().cDrive}/Program Files`);
    if (smokeConfig.probePickers !== false) { const nativeFolder = await api('smokeProbeFolder'); check('原生資料夾選擇器顯示及取消', nativeFolder.length === 1); }
    await api('smokePickFolder', { paths: [smokeConfig.output] }); await click('pick-install');
    await click('save'); check('安裝目錄獨立儲存', selected().installRoot === smokeConfig.output && state.programs.find(p => p.id === same.id).installRoot === '');
    await choose(same.id); await click('tab-settings'); check('另一個 EXE 保留預設安裝目錄', $('install-root').value === '');
    await choose(hello.id); await click('tab-settings'); check('切換程式後恢復安裝目錄', $('install-root').value === smokeConfig.output);
    $('install-root').value = ''; $('install-root').dispatchEvent(new Event('input', { bubbles: true })); await click('save');
    check('清空安裝目錄恢復預設位置', selected().installRoot === '');
    await capture('02-settings.png');
    await click('run'); await until(() => selected().status === 'exited', 'hello EXE 結束');
    await until(() => text(hello.id).includes('Hello from Windows x64 EXE on ARM64!'), '真實 EXE 輸出');
    transcripts.hello = text(hello.id);
    check('CLI 終端沒有引擎診斷', !transcripts.hello.includes('unresolved_imports=') && !transcripts.hello.includes('RAX='));
    await click('diagnostics'); check('獨立執行紀錄保留退出原因', $('log-content').textContent.includes('reason=exited')); check('GPU 容量傳入實際引擎', $('log-content').textContent.includes('gpu_memory_limit_bytes=536870912')); $('close-log').click();
    check('CLI 不顯示 LOG 且不另存輸出', $('tab-log').hidden && $('log').hidden && !terminals.get(hello.id).log);
    await choose(jq.id); await click('tab-settings'); $('arguments').value = '--unbuffered -c .'; await click('save'); await click('run');
    await until(() => active(selected()) && terminals.get(jq.id)?.term, 'jq 互動啟動');
    const terminal = terminals.get(jq.id).term;
    terminal.input('{"message": "中文😀", "n": 42}\r', true);
    await until(() => text(jq.id).includes('{"message":"中文😀","n":42}'), 'jq Unicode 互動回覆');
    check('jq 可即時回覆中文與 emoji', true);
    check('互動終端物件持續存在', terminal === terminals.get(jq.id).term);
    terminal.input('{"backspace": 12X\x7f}\r', true);
    await until(() => text(jq.id).includes('{"backspace":12}'), 'PTY Backspace 編輯');
    terminal.input('{"unicodeErase": "中X\x7f\x7f文"}\r', true);
    await until(() => text(jq.id).includes('{"unicodeErase":"文"}'), 'UTF-8 退格編輯');
    check('Backspace 以完整 UTF-8 字元刪除', true);
    const cols = terminal.cols;
    await api('smokeSize', { cols: 960, rows: 700 }); await until(() => terminal.cols < cols, 'WebView 終端尺寸調整');
    check('視窗縮放會調整 xterm 欄數', terminal.cols < cols);
    checkSidebarActions();
    await capture('03-terminal-small.png');
    await api('smokeSize', { cols: 1180, rows: 800 }); await until(() => terminal.cols >= cols, '還原視窗尺寸');
    await capture('04-terminal.png'); transcripts.jq = text(jq.id);
    await choose(hello.id); check('切換程式保留各自終端', text(hello.id).includes('Hello from Windows x64 EXE on ARM64!'));
    await choose(jq.id); terminal.input('\x04', true);
    await until(() => selected().status === 'exited', 'Ctrl D 正常結束 jq');
    check('jq 正常退出碼為零', selected().exitCode === 0);
    await click('run'); await until(() => active(selected()), '再次啟動 jq'); terminals.get(jq.id).term.input('\x03', true);
    await until(() => selected().status === 'stopped', 'Ctrl C 中斷 jq');
    check('Ctrl C 停止執行', selected().status === 'stopped');
    await click('tab-settings'); $('arguments').value = 'invalid['; await click('save'); await click('run');
    await until(() => selected().status === 'failed', '客體錯誤退出');
    await until(() => /error|syntax/.test(text(jq.id)), '客體 stderr');
    check('客體 stderr 仍出現在 xterm', /error|syntax/.test(text(jq.id))); transcripts.stderr = text(jq.id);
    await click('tab-settings'); $('arguments').value = '--unbuffered -c .'; await click('save');
    const guiPlay = document.querySelector(`[data-id="${gui.id}"] .program-action`);
    check('執行圖示位於左側列表', !!guiPlay && guiPlay.title === '執行程式');
    guiPlay.click(); await until(() => !busy && state.selected === gui.id && active(selected()), '從未選取的列表圖示執行');
    check('直接按列表圖示會選取並執行該程式', state.selected === gui.id);
    await until(async () => selected().status === 'running' && await api('smokeVisible', { pid: selected().pid }), '真實 GUI EXE 視窗', 30000);
    document.querySelector(`[data-id="${gui.id}"]`).dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 80, clientY: 180 }));
    check('執行中的列表項目禁止移除', $('program-context-remove').disabled);
    document.body.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
    check('點擊其他位置關閉右鍵選單', $('program-context-menu').hidden);
    check('GUI EXE 顯示獨立桌面視窗', await api('smokeVisible', { pid: selected().pid }));
    check('GUI 執行中顯示四張資源卡片', !$('usage-panel').hidden && $('gui-idle').hidden && document.querySelectorAll('.usage-card').length === 4);
    const cardRects = [...document.querySelectorAll('.usage-card')].map(node => node.getBoundingClientRect());
    check('資源卡片為 2×2 排列', cardRects[0].top === cardRects[1].top && cardRects[2].top === cardRects[3].top && cardRects[2].top > cardRects[0].bottom && cardRects[0].left === cardRects[2].left && cardRects[1].left === cardRects[3].left);
    await until(() => selected().usage?.sequence >= 2 && selected().usage.cpuPercent != null, 'GUI 資源首次完整取樣', 12000);
    const firstUsage = selected().usage, usageRun = selected().runId;
    check('資源統計只屬於 GUI 模擬程序', firstUsage.pid === selected().pid && firstUsage.memoryBytes > 0 && firstUsage.cpuPercent >= 0 && firstUsage.receiveBytesPerSecond >= 0 && firstUsage.sendBytesPerSecond >= 0);
    check('卡片顯示實際 CPU、MEM、NET 數值', $('usage-cpu').textContent.endsWith('%') && /[KMGT]?i?B/.test($('usage-memory').textContent) && $('usage-network').textContent.endsWith('/s'));
    await click('tab-settings');
    await until(() => selected().usage?.sequence > firstUsage.sequence, '背景持續取樣', 8000);
    check('設定分頁中仍每 3 秒取樣', Math.abs(selected().usage.sampledAt - firstUsage.sampledAt - 3000) < 500);
    await click('tab-workspace');
    check('回到執行狀態顯示最新取樣', $('usage-panel').dataset.sequence === String(selected().usage.sequence) && $('usage-panel').dataset.run === usageRun);
    const labelNodes = ['tab-workspace', 'analyze', 'save', 'remove'].map(id => [id, $(id).firstChild]);
    await delay(350);
    check('背景狀態更新保留操作按鈕的文字節點', labelNodes.every(([id, node]) => $(id).firstChild === node));
    let backendFinished = false;
    const slowRequest = api('smokeDelay', { cols: 1200 }).then(() => { backendFinished = true; });
    await delay(100);
    const uiStarted = performance.now();
    await window.launcherRequest('ui-probe', btoa(JSON.stringify({ action: 'snapshot' })));
    check('慢速背景工作不阻塞原生 UI 回呼', performance.now() - uiStarted < 400 && !backendFinished);
    for (const name of ['settings', 'workspace', 'log', 'settings', 'workspace']) {
      $(`tab-${name}`).click();
      check(`GUI 執行中可切換 ${name}`, !$(name).hidden && $(`tab-${name}`).getAttribute('aria-selected') === 'true' && active(selected()));
    }
    check('慢速背景工作完成前仍可切換分頁', !backendFinished);
    await slowRequest;
    await click('tab-log');
    check('GUI 的 LOG 位於程式設定旁', $('tab-settings').nextElementSibling === $('tab-log') && !$('log').hidden && $('workspace').hidden && $('settings').hidden);
    check('GUI LOG 不超過 128KB', new TextEncoder().encode($('runtime-log-content').textContent).length <= 131072);
    await capture('07-gui-log.png'); await click('tab-workspace');
    await capture('05-gui.png'); await click('stop'); await until(() => selected().status === 'stopped', '停止 GUI');
    check('停止 GUI 後子程序已結束', !(await api('smokeVisible', { pid: selected().pid })));
    check('停止後隱藏即時卡片並回到待執行畫面', $('usage-panel').hidden && !$('gui-idle').hidden);
    await api('smokePick', { paths: [smokeConfig.invalid] }); await click('add');
    check('錯誤 EXE 顯示原因且不進入列表', state.programs.length === 4 && $('toast').classList.contains('error'));
    const backgroundMenu = new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 10, clientY: 10 });
    document.body.dispatchEvent(backgroundMenu);
    check('停用 WebView 背景原生右鍵選單', backgroundMenu.defaultPrevented && $('program-context-menu').hidden);
    const contextRow = document.querySelector(`[data-id="${same.id}"] .select-program`);
    const openRowMenu = () => { const event = new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: innerWidth - 1, clientY: innerHeight - 1 }); contextRow.dispatchEvent(event); return event; };
    check('列表右鍵取消原生選單', openRowMenu().defaultPrevented);
    check('未選取項目右鍵提供移除', state.selected !== same.id && !$('program-context-menu').hidden && !$('program-context-remove').disabled);
    const menuBounds = $('program-context-menu').getBoundingClientRect();
    check('右鍵選單保持在視窗內', menuBounds.right <= innerWidth && menuBounds.bottom <= innerHeight);
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    check('Escape 關閉右鍵選單', $('program-context-menu').hidden);
    openRowMenu(); await capture('06-context-menu.png'); await click('program-context-remove');
    check('右鍵僅移除目標項目', state.programs.length === 3 && !state.programs.some(p => p.id === same.id) && state.programs.some(p => p.id === gui.id));
    await api('smokePick', { paths: [smokeConfig.programs[3]] }); await click('add'); check('重新加入沿用同一設定檔', selected().configPath === same.configPath);
    const beforeCancel = state.programs.length, beforeSelection = state.selected;
    await api('smokePick', { paths: [] }); await click('temporary');
    check('取消檔案選擇不新增項目或改變選取', state.programs.length === beforeCancel && state.selected === beforeSelection);
    // 延遲一筆舊 poll 回應，讓真正臨時啟動先完成，再交付舊狀態。
    clearTimeout(pollTimer); await delay(150); clearTimeout(pollTimer);
    const originalBridge = window.launcher; let staleResult, releasePoll;
    window.launcher = async payload => {
      const request = JSON.parse(new TextDecoder().decode(Uint8Array.from(atob(payload), c => c.charCodeAt(0))));
      if (request.action !== 'poll') return originalBridge(payload);
      staleResult = await originalBridge(payload);
      return new Promise(resolve => { releasePoll = resolve; });
    };
    const pendingPoll = poll(); await until(() => releasePoll, '延遲背景狀態回應');
    await api('smokePick', { paths: [smokeConfig.programs[0]] }); await click('temporary');
    const temporaryHello = selected().id;
    window.launcher = originalBridge; releasePoll(staleResult); await pendingPoll;
    check('較舊背景回應不能覆蓋新啟動結果', state.selected === temporaryHello && selected().temporary && !!selected().runId);
    check('臨時執行直接啟動且不開設定 DLG', $('tab-settings').hidden && !$('temporary-dialog') && !document.querySelector(`[data-id="${temporaryHello}"]`));
    await until(() => selected().status === 'exited' && text(temporaryHello).includes('Hello from Windows x64 EXE on ARM64!'), '臨時 CLI 執行');
    check('臨時 CLI 不顯示 LOG 且不另存輸出', $('tab-log').hidden && $('log').hidden && !terminals.get(temporaryHello).log);
    check('臨時程式使用全域預設且不改原設定', selected().cDrive === state.globalSettings.cDrive && selected().gpuMemoryMiB === state.gpuMemoryLimits.defaultMiB && state.programs.find(p => p.id === hello.id).arguments === '--profile "中文設定"');
    await capture('06-temporary-workspace.png');
    await click('temporary-close'); check('關閉臨時 CLI 清除終端與工作階段', !terminals.has(temporaryHello) && !state.programs.some(p => p.id === temporaryHello));
    const temporaryJQ = [];
    for (const value of [17, 29]) {
      await api('smokePick', { paths: [smokeConfig.programs[1]] }); await click('temporary');
      const id = selected().id; temporaryJQ.push(id);
      check('執行中的臨時程式不進入列表', !document.querySelector(`[data-id="${id}"]`));
      await until(() => active(selected()) && terminals.get(id)?.term, '臨時 jq 啟動');
      terminals.get(id).term.input(`{"temporary":${value}}\r`, true);
      await until(() => text(id).includes(`"temporary": ${value}`), '臨時 jq 互動');
    }
    check('同 EXE 同時臨時執行保持獨立', temporaryJQ[0] !== temporaryJQ[1] && !text(temporaryJQ[0]).includes('29') && !text(temporaryJQ[1]).includes('17') && temporaryJQ.every(id => active(state.programs.find(p => p.id === id))));
    await choose(temporaryJQ[0]); await click('temporary-stop'); await until(() => selected().status === 'stopped', '停止第一個臨時 jq');
    check('停止一個臨時程序不影響另一個', active(state.programs.find(p => p.id === temporaryJQ[1])));
    await click('temporary-close');
    const temporaryCleanup = state.programs.find(p => p.id === temporaryJQ[1]);
    await choose(jq.id); await click('run'); await until(() => active(selected()), '清理測試的 jq');
    // 保留一個執行中的子程序，驗證原生啟動器退出時會清理自己的程序。
    const cleanupPID = selected().pid;
    await api('smokeResult', { result: { passed: true, checks, captures, transcripts, state, cleanupPID, temporaryCleanup: { id: temporaryCleanup.id, pid: temporaryCleanup.pid, cDrive: temporaryCleanup.cDrive } } });
  } catch (error) {
    for (const p of state.programs) { if (terminals.get(p.id)?.term) transcripts[p.name + ':' + p.id] = text(p.id); }
    try { await capture('failure.png'); } catch (_) {}
    await api('smokeResult', { result: { passed: false, checks, captures, transcripts, state, error: String(error), stack: error.stack } });
  }
})();
