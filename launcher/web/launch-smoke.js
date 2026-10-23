// 單 EXE Smoke 經正式 DOM 與 Go bridge 操作；不保存啟動參數或終端內容到結果。
(async () => {
  const checks = [], captures = [], observedRuns = [];
  let observedUsage = null;
  const config = smokeConfig.launch;
  const deadline = Date.now() + (smokeConfig.timeoutSeconds - 2) * 1000;
  const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
  const startedAt = new Date().toISOString();
  let launched = null, logResult = null;
  function check(name, condition) {
    api('smokeLog', { data: `${condition ? '通過' : '失敗'} ${name}` });
    if (!condition) throw new Error(name);
    checks.push(name);
  }
  async function until(predicate, message) {
    while (Date.now() < deadline) {
      if (await predicate()) return;
      await delay(80);
    }
    throw new Error(`等待逾時：${message}`);
  }
  async function click(id) {
    const element = $(id);
    check(`可操作 ${id}`, !!element && !element.disabled && !element.hidden);
    element.click();
    await until(() => !busy, id);
  }
  function programResult() {
    const p = launched && state.programs.find(item => item.id === launched);
    return p ? { id: p.id, kind: p.kind, temporary: !!p.temporary, status: p.status, runId: p.runId, pid: p.pid, exitCode: p.exitCode, gpuMemoryMiB: p.gpuMemoryMiB } : null;
  }
  function outputText() {
    const item = terminals.get(launched);
    if (!item?.term) return item?.log?.text || '';
    const buffer = item.term.buffer.active, rows = [];
    for (let i = 0; i < buffer.length; i++) rows.push(buffer.getLine(i).translateToString(true));
    return rows.join('\n');
  }
  async function finish(passed, error) {
    await api('smokeResult', { result: {
      passed, scenario: 'launch', checks, captures, program: programResult(), log: logResult, observedUsage, observedRuns,
      startedAt, completedAt: new Date().toISOString(), ...(error ? { error: String(error) } : {})
    } });
  }
  try {
    await window.launcherReady;
    if (config.reuseExisting) {
      check('沿用既有設定不使用臨時入口', !config.temporary);
      const entry = state.programs.find(item => item.path === config.program);
      check('程式庫中存在指定 EXE', !!entry);
      const row = document.querySelector(`[data-id="${entry.id}"] .select-program`);
      check('既有程式可從列表選取', !!row);
      row.click();
      await until(() => !busy && selected()?.id === entry.id, '選取既有程式');
    } else {
      check('單程式測試使用獨立空程式庫', state.programs.length === 0 && !$('empty').hidden);
      await api('smokePick', { paths: [config.program] });
      await click(config.temporary ? 'temporary' : 'add');
      while ($('analysis-dialog').open) await click('analysis-cancel');
      check(config.temporary ? '由臨時執行入口開啟一個 EXE' : '由加入入口新增一個 EXE', state.programs.length === 1 && !!selected());
    }
    launched = selected().id;
    if (config.temporary) {
      check('選完 EXE 直接執行且不加入列表', selected().temporary && !!selected().runId && !selected().configPath && !document.querySelector(`[data-id="${launched}"]`) && !$('temporary-dialog'));
    } else {
    const prefix = '';
    await click('tab-settings');
    if (config.settings.installRoot) {
      if (smokeConfig.probePickers !== false) {
        const native = await api('smokeProbeFolder');
        check('原生安裝目錄選擇器顯示及取消', native.length === 1);
      }
      await api('smokePickFolder', { paths: [config.settings.installRoot] });
      await click(prefix + 'pick-install');
      check('從選擇按鈕填入安裝目錄', $(prefix + 'install-root').value === config.settings.installRoot);
      await api('smokePickFolder', { paths: [] });
      await click(prefix + 'pick-install');
      check('取消選擇保留安裝目錄', $(prefix + 'install-root').value === config.settings.installRoot);
    }
    for (const [field, value] of [['arguments', config.settings.arguments], ['work-dir', config.settings.workDir], ['c-drive', config.settings.cDrive]]) {
      $(prefix + field).value = value;
      $(prefix + field).dispatchEvent(new Event('input', { bubbles: true }));
    }
    $(prefix + 'writable').checked = config.settings.writable;
    $(prefix + 'writable').dispatchEvent(new Event('input', { bubbles: true }));
    $('program-cpu-cores').value = config.settings.cpuCores || 0;
    $('program-cpu-cores').dispatchEvent(new Event('input', { bubbles: true }));

      await click('save');
      await click('app-settings'); await click('global-tab-gpu');
      $(prefix + 'gpu-memory').value = config.settings.gpuMemoryMiB || state.gpuMemoryLimits.defaultMiB;
      $(prefix + 'gpu-memory').dispatchEvent(new Event('input', { bubbles: true }));
      await click('global-settings-save'); await until(() => !$('global-settings-save').disabled, '儲存 GPU 設定');
      await click('app-settings-close');
      const play = document.querySelector(`[data-id="${launched}"] .program-action`);
      check('左側列表執行圖示可操作', !!play && !play.disabled && play.title === '執行程式');
      play.click();
      await until(() => !busy, '從列表啟動');

    check('啟動設定經 UI 儲存', selected().arguments === config.settings.arguments &&
      selected().workDir === config.settings.workDir && selected().cDrive === config.settings.cDrive &&
      selected().writable === config.settings.writable && selected().gpuMemoryMiB === (config.settings.gpuMemoryMiB || state.gpuMemoryLimits.defaultMiB) && selected().installRoot === (config.settings.installRoot || '') && (selected().cpuCores || 0) === (config.settings.cpuCores || 0));
    }
    check('UI 操作建立真正客體程序', !!selected().runId && selected().pid > 0);
    if (config.requireVisible) {
      check('視窗檢查使用 GUI 程式', selected().kind === 'gui');
      await until(async () => {
        if (!active(selected())) throw new Error('客體在顯示視窗前已結束');
        return api('smokeVisible', { pid: selected().pid });
      }, '客體原生視窗');
      check('真正客體程序顯示原生視窗', true);
    }
    if (config.capture) {
      await api('smokeSnapshot', { data: 'launch-workspace.png' });
      await until(() => api('smokeSnapshotReady', { data: 'launch-workspace.png' }), '工作區快照');
      captures.push('launch-workspace.png');
    }
    if (config.observeSeconds) {
      const restarts = config.restartCount || 0;
      check('連續啟停次數合法', Number.isInteger(restarts) && restarts >= 0 && restarts <= 4 && (!restarts || !config.temporary));
      for (let cycle = 0; cycle <= restarts; cycle++) {
        if (cycle) {
          const previousRun = selected().runId;
          const play = document.querySelector(`[data-id="${launched}"] .program-action`);
          check('停止後可由相同列表再次執行', !!play && !play.disabled);
          play.click();
          await until(() => !busy, '同一 Launcher 再次啟動');
          check('再次啟動建立新的客體執行階段', active(selected()) && selected().runId !== previousRun && selected().pid > 0);
        }
        const observeUntil = Date.now() + config.observeSeconds * 1000;
        while (Date.now() < observeUntil && !(await api('smokeStopRequested'))) {
          if (!active(selected())) throw new Error('客體在觀察期間提前結束');
          await delay(250);
        }
        check('客體在觀察期間持續執行', active(selected()));
        observedUsage = { value: selected().usage, gpu: $('usage-gpu').textContent, memory: $('usage-gpu-memory').textContent, cache: $('usage-cache').textContent };
        observedRuns.push({ cycle, program: programResult(), usage: observedUsage });
        if (config.capture) {
          await api('smokeSnapshot', { data: 'launch-running.png' });
          await until(() => api('smokeSnapshotReady', { data: 'launch-running.png' }), '執行中資源快照');
          captures.push('launch-running.png');
        }
        await click('stop');
        await until(() => !active(selected()), '由 UI 停止客體');
        check('UI 停止客體且保留執行紀錄', selected().status === 'stopped' && !!selected().runId);
      }
    } else {
      await until(() => !active(selected()), '客體自行結束');
      check('客體自行結束且退出碼符合預期', selected().exitCode === config.expectedExitCode && ['exited', 'failed'].includes(selected().status));
    }
    if (config.expectedOutput) {
      await until(() => outputText().includes(config.expectedOutput), '客體預期輸出');
      check('客體輸出包含預期內容', true);
    }
    if (selected().kind === 'gui') {
      await click('tab-log');
      check('LOG 分頁可顯示執行輸出', !$('log').hidden && $('workspace').hidden && $('tab-log').getAttribute('aria-selected') === 'true');
      const log = terminals.get(launched).log;
      logResult = { byteLength: log.byteLength, discardedBytes: log.discardedBytes, replacementCharacter: log.text.includes('�') };
      check('LOG 的 UTF-8 內容不超過 128KB', new TextEncoder().encode($('runtime-log-content').textContent).length <= 128 * 1024);
      check('LOG 的淘汰提示與容量一致', $('log-truncated').hidden === !log.discardedBytes);
      if (config.expectedOutput) {
        await until(() => $('runtime-log-content').textContent.includes(config.expectedOutput), 'LOG 預期輸出');
        check('LOG 包含客體的預期輸出', true);
      }
      await until(() => {
        const content = $('runtime-log-content');
        return content.clientHeight > 0 && content.scrollHeight - content.clientHeight - content.scrollTop < 2;
      }, 'LOG 自動捲至最新內容');
      check('GUI LOG 自動捲至最新內容', true);
      if (config.capture) {
        await api('smokeSnapshot', { data: 'launch-log.png' });
        await until(() => api('smokeSnapshotReady', { data: 'launch-log.png' }), 'LOG 快照');
        captures.push('launch-log.png');
      }
      const content = $('runtime-log-content').textContent;
      for (const name of config.temporary ? ['workspace', 'log'] : ['settings', 'workspace', 'log', 'settings', 'workspace', 'log']) {
        await click(`tab-${name}`);
        check(`可切換至 ${name} 分頁`, !$(name).hidden && $(`tab-${name}`).getAttribute('aria-selected') === 'true');
      }
      check('切換分頁保留 GUI LOG', $('runtime-log-content').textContent === content);
      if (log.byteLength) {
        await click('clear-log');
        check('清除 GUI LOG 重設容量與淘汰提示', $('runtime-log-content').textContent === '' && log.byteLength === 0 && log.discardedBytes === 0 && $('log-truncated').hidden && $('clear-log').disabled);
      }
    } else {
      check('CLI 使用終端且不另建 LOG', $('tab-log').hidden && $('log').hidden && !terminals.get(launched)?.log);
    }
    await finish(true);
  } catch (error) {
    await finish(false, error);
  }
})();
