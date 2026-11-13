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
  async function checkCacheActions() {
    const bridge = window.launcher, requests = [];
    const empty = {entries:[],totalBytes:0,running:false};
    const example = {entries:[{id:'program:0123456789abcdef',kind:'program',name:'測試快取',path:'/smoke/cache.exe',bytes:4096}],totalBytes:4096,running:false};
    window.launcher = payload => {
      const request = JSON.parse(new TextDecoder().decode(Uint8Array.from(atob(payload), c=>c.charCodeAt(0))));
      if (request.action==='clearCache') { requests.push(request.data); return Promise.resolve(empty); }
      return bridge(payload);
    };
    try {
      renderCacheUsage(example);
      const button = document.querySelector('.cache-delete');
      check('快取單項使用可辨識的刪除按鈕', button.textContent===t('刪除') && getComputedStyle(button).borderTopStyle!=='none' && !button.disabled);
      button.click(); await until(()=>requests.length===1 && !$('cache-list').children.length,'單項刪除回應');
      check('單項刪除沿用既有服務', requests[0]===example.entries[0].id);
      renderCacheUsage(example);
      check('全部清除按鈕位於總用量旁', !$('cache-clear-all').disabled && $('cache-clear-all').closest('.cache-total') && $('cache-clear-all').textContent===t('全部清除'));
      await capture('cache-delete-buttons.png');
      $('cache-clear-all').click(); $('cache-clear-all').click();
      await until(()=>requests.length===2 && !$('cache-list').children.length,'全部清除回應');
      check('全部清除只送一次且空快取停用', requests[1]==='all' && $('cache-clear-all').disabled);
      renderCacheUsage({...example,running:true});
      check('程式執行中停用所有快取刪除按鈕', $('cache-clear-all').disabled && document.querySelector('.cache-delete').disabled);
    } finally { window.launcher=bridge; await refreshCacheUsage(); }
  }
  function checkControlSizing() {
    const selects = [...document.querySelectorAll('select')];
    check('所有下拉選單統一為 36px 高度', selects.length > 0 && selects.every(input => Math.abs(parseFloat(getComputedStyle(input).height)-36)<1));
    const rows = [...document.querySelectorAll('.global-drive-row')], right = rows[0].querySelector('.drive-path').getBoundingClientRect().right;
    const rowRight = rows[0].getBoundingClientRect().right;
    check('磁碟路徑輸入框及操作列對齊', rows.every(row => Math.abs(row.querySelector('.drive-path').getBoundingClientRect().right-right)<1 && Math.abs(row.lastElementChild.getBoundingClientRect().right-rowRight)<1));
    check('磁碟選單、輸入框及按鈕同高且操作有外框', rows.every(row => [...row.querySelectorAll('input,select,button')].every(input => Math.abs(input.getBoundingClientRect().height-36)<1)) && rows.every(row => [...row.querySelectorAll('button')].every(button => button.classList.contains('button') && getComputedStyle(button).borderTopStyle!=='none')));
  }
  function checkAboutContent() {
    const links = [...document.querySelectorAll('#global-about .about-links a')];
    check('關於頁移除介紹並提供專案與贊助連結', links.length===2 && links[0].href==='https://github.com/VaderChen/AppleJuice' && links[1].href==='https://buymeacoffee.com/vaderchen' && !$('global-about').textContent.includes('在 Apple Silicon 上執行'));
    const originalStatus = updateStatus;
    renderUpdate({...originalStatus,phase:'latest',current:'0.0.0'});
    check('頁尾不顯示已是最新版本文案', $('update-status').textContent==='');
    renderUpdate(originalStatus);
    check('獨立視窗提示移除指定文字與平台標籤', !$('gui-description') && !$('gui-process'));
  }
  async function checkSettingsDialogGeometry() {
    $('global-tab-general').focus();
    check('設定分頁移除選取外框',getComputedStyle($('global-tab-general')).outlineStyle==='none' && getComputedStyle($('global-tab-general')).boxShadow==='none');
    check('Windows 字型說明與 CPU 冗長說明已移除', !$('windows-font-help') && !$('global-cpu').textContent.includes('多數遊戲在雙核表現最好'));
    await click('global-tab-cpu');
    check('核心資訊簡化為實體核心', $('cpu-system-cores').textContent.startsWith('實體核心：'));
    for (const [width, height] of [[1180, 800], [960, 650]]) {
      await api('smokeSize', {cols: width, rows: height}); await delay(120);
      const dialog = $('app-settings-dialog'), initial = dialog.getBoundingClientRect();
      for (const button of document.querySelectorAll('[data-global-tab]')) {
        await click(button.id);
        const rect = dialog.getBoundingClientRect(), footer = document.querySelector('.app-settings-footer').getBoundingClientRect();
        check(`${width}×${height} ${button.dataset.globalTab} 分頁固定高度且按鈕不溢出`, Math.abs(rect.height-initial.height)<1 && Math.abs(rect.top-initial.top)<1 && footer.bottom <= rect.bottom && rect.height <= innerHeight-47);
        check('更新按鈕僅在關於頁的左下角顯示', $('settings-update-actions').hidden === (button.dataset.globalTab !== 'about'));
        if (button.dataset.globalTab === 'about') {
          const update = $('check-update').getBoundingClientRect(), cancel = $('global-settings-cancel').getBoundingClientRect();
          checkAboutContent();
          await capture(`settings-about-${width}.png`);
          check('更新按鈕位於頁尾左側', $('check-update').closest('.app-settings-footer') && update.right < cancel.left && update.top >= footer.top);
        }
      }
      await click('global-tab-advanced');
      for (let i=0; i<8; i++) await click('global-add-drive');
      checkControlSizing();
      check('長內容只在內容區捲動', document.querySelector('.app-settings-pages').scrollHeight > document.querySelector('.app-settings-pages').clientHeight && Math.abs(dialog.getBoundingClientRect().height-initial.height)<1);
      await capture(`settings-fixed-${width}.png`);
      await click('global-settings-cancel'); await click('app-settings');
    }
    await api('smokeSize', {cols:1180,rows:800}); await delay(100);
  }
  async function checkProgramCPUCapsules() {
    await click('tab-settings');
    const inputs = [...document.querySelectorAll('input[name="program-cpu-cores"]')], maximum = state.cpuLimits[selected().architecture === 'x86' ? 'x86Max' : 'x64Max'];
    check('個別程式核心選項為使用全域設定與 2／4／6 膠囊', inputs.map(input => input.value).join(',') === '0,2,4,6' && inputs.every(input => input.type === 'radio') && $('program-cpu-cores-0').checked);
    for (const input of inputs) {
      check('個別核心選項依宿主上限停用', input.disabled === (Number(input.value)>maximum));
      if (input.disabled) continue;
      await click(input.id);
      check('個別程式核心膠囊互斥', inputs.filter(input => input.checked).length === 1 && readSettings().cpuCores === Number(input.value));
      await click('save');
      check(`個別程式核心 ${input.value} 已保存`, (selected().cpuCores || 0) === Number(input.value));
    }
    await click('program-cpu-cores-0'); await click('save');
    check('個別程式可回到使用全域設定', (selected().cpuCores || 0) === 0 && $('program-cpu-cores-0').checked);
    await capture('program-cpu-capsules.png');
  }
  async function checkGPUCapsules(program) {
    const limits = state.gpuMemoryLimits, inputs = [...document.querySelectorAll('input[name="gpu-memory"]')];
    check('GPU 容量為 4G、6G、8G、16G 互斥膠囊', inputs.map(input => input.value).join(',') === '4096,6144,8192,16384' && inputs.every(input => input.type === 'radio') && !document.querySelector('#global-gpu input[type="range"]'));
    check('GPU 超過系統記憶體 50% 的選項停用', inputs.every(input => input.disabled === (Number(input.value) * 1024 * 1024 > limits.systemMemoryBytes / 2)));
    const other = state.programs.find(p => p.id !== program.id), originalOther = other?.gpuMemoryMiB;
    for (const input of inputs.filter(input => !input.disabled)) {
      await click(input.id);
      check(`GPU ${input.value} 選項互斥並更新容量`, inputs.filter(input => input.checked).length === 1 && globalGPUMemoryMiB === Number(input.value));
      await delay(200);
      check('狀態輪詢保留 GPU 容量草稿', globalGPUMemoryMiB === Number(input.value));
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 GPU 膠囊');
      check(`GPU ${input.value} 獨立儲存`, selected().gpuMemoryMiB === Number(input.value) && (!other || state.programs.find(p => p.id === other.id).gpuMemoryMiB === originalOther));
      await click('app-settings-close'); await click('app-settings'); await click('global-tab-gpu');
      check(`GPU ${input.value} 重開保留選取`, input.checked);
    }
    const saved = selected().gpuMemoryMiB;
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: state.globalSettings, id: program.id, settings: {gpuMemoryMiB: 512}})));
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-gpu');
    check('既有非膠囊 GPU 容量仍顯示', globalGPUMemoryMiB === 512 && !document.querySelector('input[name="gpu-memory"]:checked'));
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '保留自訂 GPU 容量');
    check('儲存其他設定不覆寫既有自訂 GPU 容量', selected().gpuMemoryMiB === 512);
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: state.globalSettings, id: program.id, settings: {gpuMemoryMiB: saved}})));
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-gpu');
    for (const maximum of [2048, 6144, 8192]) {
      loadGPUChoices(saved, {...limits, maxMiB: maximum, systemMemoryBytes: maximum * 2 * 1024 * 1024}, false);
      check(`GPU 系統一半容量 ${maximum} MiB 正確停用選項`, inputs.every(input => input.disabled === (Number(input.value) > maximum)));
      const disabled = inputs.find(input => input.disabled);
      if (disabled) { disabled.click(); check('GPU 停用選項無法改變容量', globalGPUMemoryMiB === saved); }
    }
    loadGPUChoices(saved, limits, false);
    const alternate = inputs.find(input => !input.disabled && Number(input.value) !== saved);
    if (alternate) await click(alternate.id);
    await click('global-settings-cancel'); await click('app-settings'); await click('global-tab-gpu');
    check('取消 GPU 草稿還原已儲存容量', globalGPUMemoryMiB === saved);
    await capture('gpu-memory-capsules.png');
  }
  async function checkOpenGLPreferences() {
    await click('global-tab-cpu');
    check('CPU 分頁提供兩種架構與 resident AOT 模式', !$('global-cpu').hidden && $('global-gpu').hidden && $('execution-mode').textContent.includes('Resident AOT') && $('global-cpu').textContent.includes('x86'));
    const originalCores = {x64: state.globalSettings.cpuCoresX64, x86: state.globalSettings.cpuCoresX86};
    check('編譯執行緒設定與說明已移除', !document.querySelector('#compiler-workers, #compiler-workers-value, #compiler-workers-help, label[for="compiler-workers"]'));
    const originalWorkers = state.globalSettings.compilerWorkers;
    for (const value of [0, 4, 8, 2]) {
      await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, compilerWorkers: value}})));
      await click('app-settings-close'); await click('app-settings'); await click('global-tab-cpu');
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 CPU 設定');
      check(`儲存其他設定保留既有編譯執行緒數 ${value}`, state.globalSettings.compilerWorkers === value);
      await click('global-settings-cancel'); await click('app-settings'); await click('global-tab-cpu');
      check(`取消其他設定保留既有編譯執行緒數 ${value}`, state.globalSettings.compilerWorkers === value);
    }
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, compilerWorkers: originalWorkers}})));
    const savedCores = {};
    for (const architecture of ['x64', 'x86']) {
      const id = 'cpu-cores-' + architecture, maximum = state.cpuLimits[architecture + 'Max'];
      const inputs = [...document.querySelectorAll(`input[name="${id}"]`)];
      check(`${architecture} 提供 2、4、6 個核心的互斥膠囊`, inputs.length === 3 && inputs.map(input => input.value).join(',') === '2,4,6' && inputs.every(input => input.type === 'radio') && !$(id).querySelector('input[type="range"]'));
      check(`${architecture} 停用超過宿主核心數的選項`, inputs.every(input => input.disabled === (Number(input.value) > maximum)));
      for (const input of inputs.filter(input => !input.disabled)) {
        await click(input.id);
        const value = Number(input.value);
        check(`${architecture} ${value} 個核心互斥且更新提示`, inputs.filter(input => input.checked).length === 1 && $(id + '-value').textContent === t`${value.toLocaleString(currentLanguage)} 個核心` && $(id + '-warning').hidden === (value <= 2));
        await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 CPU 核心數');
        check(`${architecture} 膠囊儲存 ${value}`, state.globalSettings['cpuCores' + architecture.toUpperCase()] === value);
        await click('app-settings-close'); await click('app-settings'); await click('global-tab-cpu');
        check(`${architecture} 膠囊重新開啟 ${value}`, input.checked);
      }
      savedCores[architecture] = state.globalSettings['cpuCores' + architecture.toUpperCase()];
    }
    for (const architecture of ['x64', 'x86']) if (!$('cpu-cores-' + architecture + '-2').disabled) await click('cpu-cores-' + architecture + '-2');
    await click('global-settings-cancel'); await click('app-settings'); await click('global-tab-cpu');
    check('取消不儲存兩種架構的核心草稿', ['x64', 'x86'].every(architecture => globalCPUSelections[architecture] === savedCores[architecture]));
    loadCPUChoices(state.globalSettings, {...state.cpuLimits, x64Max: 3, x86Max: 1});
    check('低核心宿主停用無法使用的膠囊', !$('cpu-cores-x64-2').disabled && $('cpu-cores-x64-4').disabled && $('cpu-cores-x64-6').disabled && [...document.querySelectorAll('input[name="cpu-cores-x86"]')].every(input => input.disabled));
    const beforeDisabledClick = globalCPUSelections.x86;
    $('cpu-cores-x86-6').click();
    check('停用選項無法改變核心數', globalCPUSelections.x86 === beforeDisabledClick);
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, cpuCoresX64: 1, cpuCoresX86: 1}})));
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-cpu');
    check('既有非膠囊核心數仍顯示且不自動選取', ['x64', 'x86'].every(architecture => globalCPUSelections[architecture] === 1 && !document.querySelector(`input[name="cpu-cores-${architecture}"]:checked`)));
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '保留既有核心數');
    check('儲存其他設定保留既有單核心', state.globalSettings.cpuCoresX64 === 1 && state.globalSettings.cpuCoresX86 === 1);
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, cpuCoresX64: originalCores.x64, cpuCoresX86: originalCores.x86}})));
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-cpu');
    await capture('cpu-settings.png');
    await click('global-tab-gpu');
    check('GPU 分頁已移除 OpenGL 選單並固定 AUTO', !$('global-gpu').hidden && !$('opengl-backend') && state.globalSettings.openGLBackend === 'auto');
    for (const mode of ['metal', 'legacy']) {
      await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, openGLBackend: mode}})));
      check(`舊 OpenGL 設定 ${mode} 回到 AUTO`, state.globalSettings.openGLBackend === 'auto');
    }
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存固定 AUTO');
    check('儲存其他設定仍使用 AUTO', state.globalSettings.openGLBackend === 'auto');
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
    if (smokeConfig.scenario === 'settings' && smokeConfig.settingsTest === 'startup') {
      await delay(1000);
      await api('smokeResult', {result:{passed:true,checks:['正常啟動角色載入主畫面且不操作對話框'],captures:[],state}});
      return;
    }
    if (smokeConfig.scenario === 'settings' && ['order-save','order-reload'].includes(smokeConfig.settingsTest)) {
      const reload = smokeConfig.settingsTest === 'order-reload';
      if (!reload) {
        await api('smokePick', {paths:smokeConfig.programs}); await click('add');
        while ($('analysis-dialog').open) await click('analysis-cancel');
        const ids = state.programs.map(p=>p.id), selection = state.selected;
        async function drag(source, target, after) {
          const from=document.querySelector(`[data-id="${source}"] .select-program`);
          const to=document.querySelector(`[data-id="${target}"]`), box=to.getBoundingClientRect();
          const transfer=new DataTransfer(), options={bubbles:true,cancelable:true,dataTransfer:transfer,clientX:box.left+30,clientY:after?box.bottom-5:box.top+5};
          check('程式項目可拖曳',from.draggable);
          from.dispatchEvent(new DragEvent('dragstart',options));
          to.dispatchEvent(new DragEvent('dragover',options));
          check('插入位置有提示',to.classList.contains(after?'drop-after':'drop-before'));
          to.dispatchEvent(new DragEvent('drop',options));
          from.dispatchEvent(new DragEvent('dragend',options));
          await until(()=>!busy,'保存拖曳順序');
        }
        await drag(ids[2],ids[0],false);
        check('向上拖曳保存順序',state.programs.map(p=>p.id).join() === [ids[2],ids[0],ids[1]].join());
        await drag(ids[2],ids[1],true);
        check('向下拖曳保存順序',state.programs.map(p=>p.id).join() === ids.join());
        $('search').value='visible'; $('search').dispatchEvent(new Event('input'));
        await drag(ids[2],ids[0],false);
        check('篩選排序保留隱藏程式',state.programs.map(p=>p.id).join() === [ids[2],ids[0],ids[1]].join());
        $('search').value=''; $('search').dispatchEvent(new Event('input'));
        check('拖曳不改變選取程式',state.selected===selection);
        check('拖曳不會啟動程式',state.programs.every(p=>!active(p)));
      }
      check('列表順序符合保存結果',state.programs.map(p=>p.name).join() === 'visible-c.exe,visible-a.exe,hidden-b.exe');
      await capture(reload?'library-order-reloaded.png':'library-order.png');
      await api('smokeResult',{result:{passed:true,checks,captures,state}}); return;
    }
    if (smokeConfig.scenario === 'settings' && ['install-discovery','install-reload'].includes(smokeConfig.settingsTest)) {
      const reload=smokeConfig.settingsTest==='install-reload';
      if (!reload) {
        await api('smokePick',{paths:smokeConfig.programs}); await click('add');
        while ($('analysis-dialog').open) await click('analysis-cancel');
        await click('app-settings'); await click('global-tab-advanced'); await click('global-add-drive');
        const row=[...document.querySelectorAll('.global-drive-row')].find(row=>row.querySelector('.drive-letter')?.value==='D');
        row.querySelector('.drive-path').value=smokeConfig.output+'/drive-d';
        await click('global-settings-save'); await until(()=>!$('global-settings-save').disabled,'儲存測試磁碟'); await click('app-settings-close');
        await click('tab-settings'); $('install-root').value=smokeConfig.output+'/install-root'; $('install-root').dispatchEvent(new Event('input',{bubbles:true}));
        await click('save'); const id=selected().id;
        await click('run'); await until(()=>['exited','failed'].includes(state.programs.find(p=>p.id===id)?.status),'安裝完成與探索',90000);
        const installer=state.programs.find(p=>p.id===id);
        check('Windows 安裝測試程式成功退出',installer.status==='exited' && installer.exitCode===0,JSON.stringify(installer));
        check('安裝結束自動加入且不切換目前程式',state.selected===id && state.programs.length===4,JSON.stringify(state.programs));
      }
      const installed=state.programs.filter(p=>!p.path.endsWith('setup-discovery.exe'));
      check('捷徑、無捷徑目錄與登錄安裝各加入一個程式',installed.length===3 && installed.some(p=>p.path.endsWith('installed.exe')) && installed.some(p=>p.path.endsWith('secondary.exe')) && installed.some(p=>p.path.endsWith('custom.exe')));
      check('不加入解除安裝工具',!state.programs.some(p=>p.path.endsWith('uninstall.exe')));
      const target=installed.find(p=>p.path.endsWith('installed.exe'));
      check('保留 Unicode 捷徑名稱、參數與安裝映射',target.name==='測試應用程式' && target.arguments==='--from-shortcut' && target.workDir.endsWith('測試產品') && target.installRoot.endsWith('install-root') && target.cDrive===state.globalSettings.cDrive,JSON.stringify(target));
      await capture(reload?'installed-library-reloaded.png':'installed-library.png');
      if(reload) {
        await choose(target.id); await click('run'); await until(()=>selected().status==='exited' || selected().status==='failed','啟動已安裝程式');
        if(selected().status==='exited' && selected().exitCode===0) await until(()=>text(target.id).includes('installed-payload-ok'),'已安裝程式輸出');
        check('重開後可從列表啟動且收到捷徑參數',selected().status==='exited' && selected().exitCode===0 && text(target.id).includes('installed-payload-ok'),JSON.stringify(selected()));
        check('啟動已安裝程式不重複加入',state.programs.length===4);
      }
      await api('smokeResult',{result:{passed:true,checks,captures,state}}); return;
    }
    if (smokeConfig.scenario === 'settings' && smokeConfig.settingsTest === 'cpu-defaults') {
      check('新設定預設 x86／x64 四核與 2 個編譯 worker', state.globalSettings.cpuCoresX64 === Math.min(4, state.cpuLimits.x64Max) && state.globalSettings.cpuCoresX86 === Math.min(4, state.cpuLimits.x86Max) && state.globalSettings.compilerWorkers === 2);
      await click('app-settings'); await click('global-tab-cpu');
      check('CPU 分頁已移除編譯執行緒設定', !$('compiler-workers') && !$('compiler-workers-help'));
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 CPU 預設');
      check('儲存 CPU 設定仍保留預設編譯執行緒數', state.globalSettings.compilerWorkers === 2);
      for (const architecture of ['x64', 'x86']) {
        const id = 'cpu-cores-' + architecture;
        for (const input of document.querySelectorAll(`input[name="${id}"]`)) {
          if (input.disabled) continue;
          await click(input.id);
          check(`${architecture} ${input.value} 核依門檻顯示提示`, $(id + '-warning').hidden === (Number(input.value) <= 2));
        }
      }
      await capture('cpu-core-warning.png');
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存膠囊核心設定');
      const expectedGlobalX86 = [6, 4, 2].find(value => value <= state.cpuLimits.x86Max) || 1;
      check('提示不阻擋儲存膠囊核心設定', state.globalSettings.cpuCoresX86 === expectedGlobalX86);
      await click('app-settings-close');
      await api('smokePick', { paths: smokeConfig.programs }); await click('add');
      await choose(state.programs[0].id); await click('tab-settings');
      const maximum = state.cpuLimits[selected().architecture === 'x86' ? 'x86Max' : 'x64Max'];
      const choices = [0, 2, 4, 6].filter(value => value <= maximum);
      for (const value of choices) {
        await click('program-cpu-cores-' + value);
        check(`個別程式 ${value} 核依有效值顯示提示`, $('program-cpu-warning').hidden === ((value || expectedGlobalX86) <= 2));
      }
      await capture('program-core-warning.png');
      await click('save'); await until(() => !busy, '儲存個別程式核心數');
      check('提示不阻擋個別程式保存設定', selected().cpuCores === choices.at(-1));
      await api('smokeResult', { result: { passed: true, checks, captures, state } });
      return;
    }
    if (smokeConfig.scenario === 'settings' && ['gpu-save', 'gpu-reload'].includes(smokeConfig.settingsTest)) {
      const reload = smokeConfig.settingsTest === 'gpu-reload';
      if (!reload) {
        await api('smokePick', { paths: smokeConfig.programs }); await click('add');
        check('新程式採用目前 GPU 預設並遵守系統上限', state.programs.length === 2 && state.programs.every(p => p.gpuMemoryMiB === state.gpuMemoryLimits.defaultMiB));
      }
      const programs = [...state.programs], values = [6144, 4096].map(value => Math.min(value, state.gpuMemoryLimits.maxMiB));
      check('跨建置測試保留兩個程式', programs.length === 2);
      for (let i = 0; i < programs.length; ++i) {
        await choose(programs[i].id);
        if (!reload) await checkProgramCPUCapsules();
        await click('app-settings'); await click('global-tab-cpu');
        check('執行模式顯示 Resident AOT', $('execution-mode').textContent.includes('Resident AOT'));
        await click('global-tab-gpu');
        const memory = $('gpu-memory');
        check('GPU 提供四個互斥容量膠囊', [...memory.querySelectorAll('input')].map(input => input.value).join(',') === '4096,6144,8192,16384');
        if (!reload) {
          await checkGPUCapsules(programs[i]);
          if ($('gpu-memory-' + values[i])) await click('gpu-memory-' + values[i]);
          else loadGPUChoices(values[i], state.gpuMemoryLimits, false);
          await delay(200);
          check('狀態輪詢不覆蓋 GPU 草稿', globalGPUMemoryMiB === values[i]);
          await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '保存 GPU');
        }
        check(`${reload ? '重新 BUILD 後' : '儲存後'}保留個別 GPU 容量`, globalGPUMemoryMiB === values[i] && selected().gpuMemoryMiB === values[i]);
        await click('app-settings-close'); await click('tab-settings'); await click('save');
        check('主設定儲存保留 GPU 容量', selected().gpuMemoryMiB === values[i]);
      }
      await choose(programs[0].id); await click('app-settings'); await click('global-tab-gpu');
      check('切換程式及重開設定保留 GPU 容量', globalGPUMemoryMiB === values[0]);
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
    check('設定分頁包含 Windows 介面、CPU、GPU 與快取', [...document.querySelectorAll('[data-global-tab]')].map(button => button.dataset.globalTab).join(',') === 'general,advanced,windows,cpu,gpu,cache,about');
    await checkSettingsDialogGeometry();
    await click('global-tab-cache'); await until(() => / GB$/.test($('cache-total').textContent), '快取用量');
    check('快取分頁顯示 GB 總用量且每個項目有刪除按鈕', !$('global-cache').hidden && $('global-general').hidden && /^\d+\.\d{2} GB$/.test($('cache-total').textContent) &&
      [...document.querySelectorAll('#cache-list .cache-row')].every(row => row.dataset.cacheId && row.querySelector('button.cache-delete')?.textContent === t('刪除') && getComputedStyle(row.querySelector('button')).borderTopStyle !== 'none' && / GB$/.test(row.querySelector('output').textContent)));
    await checkCacheActions();
    check('實驗性功能頁面與控制項已移除', !document.querySelector('#global-tab-experimental, #global-experimental, #native-library-bridge, #native-v8-bridge, #add-native-v8-tests'));
    await click('global-tab-gpu');
    $('global-tab-gpu').dispatchEvent(new KeyboardEvent('keydown', {key: 'ArrowDown', bubbles: true}));
    check('鍵盤可由 GPU 直接切換快取', !$('global-cache').hidden && document.activeElement === $('global-tab-cache'));
    const originalLibraryBridge = state.globalSettings.nativeLibraryBridge, originalV8 = state.globalSettings.nativeV8Bridge;
    for (const value of [true, false]) {
      await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, nativeLibraryBridge: value}})));
      await click('app-settings-close'); await click('app-settings');
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存其他設定');
      check(`儲存其他設定保留既有橋接值 ${value}`, state.globalSettings.nativeLibraryBridge === value && state.globalSettings.nativeV8Bridge === originalV8);
    }
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, nativeLibraryBridge: originalLibraryBridge}})));
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
    check('GPU 記憶體採用目前預設且受系統上限限制', globalGPUMemoryMiB === state.gpuMemoryLimits.defaultMiB);
    check('GPU 已移除指定文字並保留膠囊與系統資訊', !$('gpu-memory').disabled && !$('gpu-settings-program') && !$('gpu-memory-help') && !$('gpu-memory').querySelector('legend') && !$('settings').querySelector('#gpu-memory'));
    await checkGPUCapsules(hello);
    // 保留舊版有效自訂容量，不因控制項改版而覆寫。
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: state.globalSettings, id: hello.id, settings: {gpuMemoryMiB: 512}})));
    await click('app-settings-close'); await click('app-settings'); await click('global-tab-gpu');
    check('既有非膠囊容量仍顯示且不自動選取', globalGPUMemoryMiB === 512 && !document.querySelector('input[name="gpu-memory"]:checked'));
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '保留既有 GPU 容量');
    check('GPU 容量獨立儲存', selected().gpuMemoryMiB === 512 && state.programs.find(p => p.id === same.id).gpuMemoryMiB === state.gpuMemoryLimits.defaultMiB);
    check('GPU 儲存保留未儲存的啟動參數草稿', selected().arguments === '' && $('arguments').value === '--profile "中文設定"');
    await capture('gpu-memory-capsules.png');
    if (!$('gpu-memory-4096').disabled) await click('gpu-memory-4096');
    await click('global-settings-cancel');
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
    // 執行頁的顯示開關：切換即寫入此程式設定，不影響同名 EXE，也不連帶儲存「程式設定」頁未儲存的草稿。
    const savedArguments = selected().arguments;
    $('arguments').value = '--draft-only'; $('arguments').dispatchEvent(new Event('input', { bubbles: true }));
    await click('tab-workspace'); check('顯示開關位於執行頁', !$('workspace').hidden && $('present-upscale').closest('.run-switches') !== null && $('present-upscale').getAttribute('role') === 'switch');
    await click('present-upscale'); await click('present-interpolation');
    check('顯示開關切換即儲存', selected().presentUpscale === true && selected().presentInterpolation === true);
    check('顯示開關不儲存啟動參數草稿', selected().arguments === savedArguments && $('arguments').value === '--draft-only');
    check('顯示開關不影響同名 EXE', state.programs.find(p => p.id === same.id).presentUpscale !== true && state.programs.find(p => p.id === same.id).presentInterpolation !== true);
    await capture('run-display-switches.png');
    await click('present-upscale'); await click('present-interpolation');
    check('顯示開關可關閉', selected().presentUpscale !== true && selected().presentInterpolation !== true);
    $('arguments').value = savedArguments; $('arguments').dispatchEvent(new Event('input', { bubbles: true }));
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
