// 真 WKWebView 經後端測試 API 設定與加入 PE，再由列表執行圖示驗證打包後端。
(async () => {
  const checks = [], runs = [], captures = [];
  const deadline = Date.now() + (smokeConfig.timeoutSeconds - 2) * 1000;
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  function check(name, condition) { api('smokeLog', {data: `${condition ? '通過' : '失敗'} ${name}`}); if (!condition) throw new Error(name); checks.push(name); }
  async function until(predicate, label) { while (Date.now() < deadline) { if (await predicate()) return; await wait(80); } throw new Error(`等待逾時：${label}`); }
  async function click(id) { const element = $(id); check(`可操作 ${id}`, element && !element.disabled && !element.hidden); element.click(); await until(() => !busy, id); }
  try {
    await window.launcherReady;
    check('V8 後端與雙架構測試已內建', state.nativeV8Ready && state.nativeV8TestsReady);
    check('V8 原型預設關閉', !state.globalSettings.nativeV8Bridge);
    await perform(async () => applyState(await api('saveGlobalSettings', {globalSettings: {...state.globalSettings, nativeV8Bridge: true}})));
    await click('app-settings');
    check('實驗性功能分頁已移除', !$('global-tab-experimental') && !$('global-experimental'));
    await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存其他設定');
    check('儲存其他設定保留 V8 開關', state.globalSettings.nativeV8Bridge === true);
    await click('app-settings-close');
    await perform(async () => applyState((await api('addNativeV8Tests')).state));
    await until(() => state.programs.length === 2, '加入兩個 PE');
    check('內建測試為真 x86／x64 PE', state.programs.map(p => p.architecture).sort().join(',') === 'x64,x86');
    for (const p of state.programs) {
      const action = document.querySelector(`[data-id="${p.id}"] .program-action`);
      check('測試有列表執行圖示', action && !action.disabled); action.click();
      await until(() => state.programs.find(v => v.id === p.id)?.runId, '產生執行階段');
      await until(() => ['exited', 'failed'].includes(state.programs.find(v => v.id === p.id)?.status), '測試結束');
      const result = state.programs.find(v => v.id === p.id);
      check(p.architecture + ' 模組測試成功退出', result.status === 'exited' && result.exitCode === 0);
      const log = await api('diagnostics', {id: result.id, runId: result.runId});
      check(p.architecture + ' 使用 ARM64 V8 且完成生命週期', JSON.stringify(log).includes('backend=arm64-module') && JSON.stringify(log).includes('live_isolates=0'));
      runs.push({architecture: p.architecture, runId: result.runId, exitCode: result.exitCode});
    }
    await api('smokeResult', {result: {passed: true, scenario: 'v8', checks, runs, captures}});
  } catch (error) { await api('smokeResult', {result: {passed: false, scenario: 'v8', checks, runs, captures, error: String(error), stack: error.stack}}); }
})();
