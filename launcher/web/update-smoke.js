// 版本更新 Smoke：搭配 --update-test-version，實際從 GitHub 下載、驗證並安裝到測試 App 的位置。
(async () => {
  const checks = [], captures = [];
  const deadline = Date.now() + (smokeConfig.timeoutSeconds - 2) * 1000;
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  function check(name, condition, detail = '') { api('smokeLog', {data: `${condition ? '通過' : '失敗'} ${name}`}); if (!condition) throw new Error(`${name}：${detail || '條件不符'}`); checks.push(name); }
  async function until(predicate, label) { while (Date.now() < deadline) { if (await predicate()) return; await wait(50); } throw new Error(`等待逾時：${label}`); }
  async function capture(name) { await wait(150); await api('smokeSnapshot', {data: name}); await until(() => api('smokeSnapshotReady', {data: name}), name); captures.push(name); }
  try {
    await window.launcherReady;
    if (smokeConfig.settingsTest === 'latest') {
      // 已是最新版：啟動檢查不跳窗；關於頁按鈕開啟對話框並顯示結果。
      await until(() => updateStatus.phase === 'latest', '啟動檢查結果');
      await wait(500);
      check('已是最新版時啟動不詢問', !$('update-dialog').open);
      $('app-settings').click(); $('global-tab-about').click();
      check('關於頁有偵測版本更新按鈕', !$('global-about').hidden && !$('check-update').disabled);
      $('check-update').click();
      await until(() => $('update-dialog').open && updateStatus.phase === 'latest' && $('update-summary').textContent.includes(updateStatus.current), '手動檢查結果');
      check('手動檢查顯示已是最新版本', $('update-summary').textContent.includes(updateStatus.current) && $('update-now').hidden && !$('update-later').hidden && $('update-status').textContent === '');
      await capture('update-latest.png');
      $('update-later').click();
      check('關閉對話框', !$('update-dialog').open);
      await api('smokeResult', {result: {passed: true, scenario: 'update-latest', checks, captures, status: updateStatus}});
      return;
    }
    await until(() => $('update-dialog').open, '啟動時的更新詢問');
    check('啟動時詢問是否更新', !$('update-now').hidden && !$('update-later').hidden && $('update-summary').textContent.includes(updateStatus.latest));
    await capture('update-prompt.png');
    $('update-now').click();
    await until(() => updateStatus.phase !== 'checking' && updateStatus.phase !== 'available', '開始更新');
    if (updateStatus.phase === 'downloading') {
      await until(() => updateStatus.phase !== 'downloading' || updateStatus.received > 0, '下載資料');
      if (updateStatus.phase === 'downloading') {
        check('下載中顯示進度條與大小', !$('update-progress-area').hidden && $('update-progress').value > 0 && /MB（\d+%）$/.test($('update-count').textContent) && $('update-later').hidden);
        await capture('update-progress.png');
      }
    }
    await until(() => ['restarting', 'error', 'unavailable'].includes(updateStatus.phase), '安裝完成');
    check('更新安裝完成並準備重新啟動', updateStatus.phase === 'restarting', updateStatus.error);
    await capture('update-restarting.png');
    await api('smokeResult', {result: {passed: true, scenario: 'update', checks, captures, status: updateStatus}});
  } catch (error) { await api('smokeResult', {result: {passed: false, scenario: 'update', checks, captures, status: updateStatus, error: String(error)}}); }
})();
