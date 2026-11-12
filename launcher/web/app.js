'use strict';
const $ = id => document.getElementById(id);
const bridgePending = new Map();
let bridgeSequence = 0;
window.launcherResponse = (id, reply) => {
  const pending = bridgePending.get(id); if (!pending) return;
  bridgePending.delete(id);
  if (reply.error) pending.reject(new Error(reply.error)); else pending.resolve(reply.value);
};
window.launcher = payload => {
  const id = String(++bridgeSequence);
  return new Promise((resolve, reject) => {
    bridgePending.set(id, { resolve, reject });
    window.launcherRequest(id, payload).catch(error => {
      const pending = bridgePending.get(id); if (!pending) return;
      bridgePending.delete(id); pending.reject(error);
    });
  });
};
function setText(node, text) {
  if (typeof node === 'string') node = $(node);
  if (node.textContent !== text) node.textContent = text;
}
const api = (action, data = {}) => {
  const bytes = new TextEncoder().encode(JSON.stringify({ action, language: currentLanguage, ...data }));
  let binary = '';
  for (let i = 0; i < bytes.length; i += 16384) binary += String.fromCharCode(...bytes.subarray(i, i + 16384));
  return window.launcher(btoa(binary));
};
const terminals = new Map();
const drafts = new Map();
const LOG_LIMIT_BYTES = 128 * 1024;
const logEncoder = new TextEncoder();
class RuntimeLog {
  constructor() {
    this.decoder = new TextDecoder();
    this.textDecoder = new TextDecoder('utf-8', { ignoreBOM: true });
    this.clear();
  }
  clear() {
    this.cachedText = ''; this.textDirty = false; this.start = 0;
    this.buffer = new Uint8Array();
    this.byteLength = 0; this.discardedBytes = 0;
    this.revision = (this.revision || 0) + 1;
  }
  get text() {
    if (this.textDirty) {
      const end = this.start + this.byteLength;
      if (end <= this.buffer.length) this.cachedText = this.textDecoder.decode(this.buffer.subarray(this.start, end));
      else {
        const data = new Uint8Array(this.byteLength), first = this.buffer.length - this.start;
        data.set(this.buffer.subarray(this.start)); data.set(this.buffer.subarray(0, this.byteLength - first), first);
        this.cachedText = this.textDecoder.decode(data);
      }
      this.textDirty = false;
    }
    return this.cachedText;
  }
  write(bytes = new Uint8Array(), complete = false) {
    // 保留跨批次的 UTF-8 字元；容量按解碼後文字的實際位元組計算。
    const text = this.decoder.decode(bytes, { stream: !complete });
    if (!text) return;
    const data = logEncoder.encode(text);
    const needed = Math.min(LOG_LIMIT_BYTES, this.byteLength + data.length);
    if (this.buffer.length < needed) {
      const grown = new Uint8Array(Math.min(LOG_LIMIT_BYTES, Math.max(4096, needed, this.buffer.length * 2)));
      const first = Math.min(this.byteLength, this.buffer.length - this.start);
      grown.set(this.buffer.subarray(this.start, this.start + first));
      grown.set(this.buffer.subarray(0, this.byteLength - first), first);
      this.buffer = grown; this.start = 0;
    }
    if (data.length >= LOG_LIMIT_BYTES) {
      let skip = data.length - LOG_LIMIT_BYTES;
      while (skip < data.length && (data[skip] & 0xc0) === 0x80) ++skip;
      this.discardedBytes += this.byteLength + skip; this.start = 0;
      this.byteLength = data.length - skip; this.buffer.set(data.subarray(skip));
    } else {
      let skip = Math.max(0, this.byteLength + data.length - LOG_LIMIT_BYTES);
      while (skip < this.byteLength && (this.buffer[(this.start + skip) % this.buffer.length] & 0xc0) === 0x80) ++skip;
      this.start = (this.start + skip) % this.buffer.length;
      this.byteLength -= skip; this.discardedBytes += skip;
      const end = (this.start + this.byteLength) % this.buffer.length;
      const first = Math.min(data.length, this.buffer.length - end);
      this.buffer.set(data.subarray(0, first), end); this.buffer.set(data.subarray(first), 0);
      this.byteLength += data.length;
    }
    this.textDirty = true; ++this.revision;
  }
}
let state = { programs: [], selected: '', engineReady: false, running: 0 };
let programsByID = new Map();
let windowsFonts = [];
let currentID = '', tab = 'workspace', listSignature = '', busy = false;
let toastTimer, pollTimer;
let mutationVersion = 0;
let programCPUSelection = 0;
const statusNames = { ready: '準備就緒', running: '執行中', stopping: '正在停止…', exited: '已結束', stopped: '已停止', failed: '執行失敗' };
const active = p => p && ['running', 'stopping'].includes(p.status);
const selected = () => programsByID.get(state.selected);
const errorText = error => translateMessage(typeof error === 'string' ? error : error?.message || String(error));
function toast(message, error = false) {
  clearTimeout(toastTimer); $('toast').textContent = message; $('toast').classList.toggle('error', error); $('toast').hidden = false;
  toastTimer = setTimeout(() => { $('toast').hidden = true; }, error ? 6500 : 3000);
}
async function perform(fn) {
  if (busy) return;
  busy = true; ++mutationVersion; render();
  try { await fn(); } catch (error) { toast(errorText(error), true); }
  finally { ++mutationVersion; busy = false; render(); }
}
function settingsFrom(p) { return { arguments: p.arguments, workDir: p.workDir, cDrive: p.cDrive, installRoot: p.installRoot || '', writable: p.writable, maxInstructions: p.maxInstructions, gpuMemoryMiB: p.gpuMemoryMiB || state.gpuMemoryLimits.defaultMiB, cpuCores: p.cpuCores || 0, presentUpscale: p.presentUpscale === true, presentInterpolation: p.presentInterpolation === true }; }
function readSettings() {
  const p = selected();
  return { arguments: $('arguments').value, workDir: $('work-dir').value, cDrive: $('c-drive').value, installRoot: $('install-root').value, writable: $('writable').checked, maxInstructions: p?.maxInstructions ?? 0, gpuMemoryMiB: p?.gpuMemoryMiB || state.gpuMemoryLimits.defaultMiB, cpuCores: programCPUSelection, presentUpscale: $('present-upscale').checked, presentInterpolation: $('present-interpolation').checked };
}
function rememberDraft() { if (currentID) drafts.set(currentID, readSettings()); }
function loadSettings(p) {
  const s = drafts.get(p.id) || settingsFrom(p);
  $('arguments').value = s.arguments; $('work-dir').value = s.workDir; $('c-drive').value = s.cDrive; $('install-root').value = s.installRoot || ''; $('writable').checked = s.writable; $('present-upscale').checked = s.presentUpscale === true; $('present-interpolation').checked = s.presentInterpolation === true;
  loadProgramCPUChoices(s.cpuCores || 0, p.architecture);

}
function loadProgramCPUChoices(value, architecture) {
  programCPUSelection = value;
  loadCapsuleChoices('program-cpu-cores', value, state.cpuLimits[architecture === 'x86' ? 'x86Max' : 'x64Max']);
}
for (const input of document.querySelectorAll('input[name="program-cpu-cores"]')) {
  input.oninput = () => { if (input.checked && !input.disabled) programCPUSelection = Number(input.value); };
}
function updateInstallLocation(prefix = '') {
  const chosen = $(prefix + 'install-root').value;
  const cDrive = $(prefix + 'c-drive').value.replace(/\/+$/, '');
  $(prefix + 'install-default').textContent = chosen ? t`安裝位置：${chosen}` : t`預設：${cDrive}/Program Files`;
}
function updateProgramCPUWarning() {
  const p = selected(), architecture = p?.architecture === 'x86' ? 'X86' : 'X64';
  const cores = programCPUSelection || state.globalSettings?.['cpuCores' + architecture] || 0;
  $('program-cpu-warning').hidden = !p || cores <= 2;
}
function updateDirty() { const p = selected(); $('dirty').hidden = !p || JSON.stringify(readSettings()) === JSON.stringify(settingsFrom(p)); $('install-read-only').hidden = !$('install-root').value || $('writable').checked; updateInstallLocation(); updateProgramCPUWarning(); }
function applyState(next) {
 state = next;
 programsByID = new Map();
 for (const program of state.programs) if (!programsByID.has(program.id)) programsByID.set(program.id, program);
 if (!$('app-settings-dialog').open) applyAppearance(state.globalSettings?.language, state.globalSettings?.theme);
 render();
}
function showTab(value) { tab = value; render(); if (tab === 'workspace') fitSelected(); }
let draggingProgram = '';
function clearProgramDrag() {
  draggingProgram = '';
  for (const row of $('program-list').children) row.classList.remove('dragging', 'drop-before', 'drop-after');
}
function wireProgramDrag(row, select, id) {
  select.draggable = true;
  select.addEventListener('dragstart', event => {
    if (busy) { event.preventDefault(); return; }
    draggingProgram = id;
    event.dataTransfer.effectAllowed = 'move';
    event.dataTransfer.setData('application/x-applejuice-program', id);
    row.classList.add('dragging');
    closeProgramMenu();
  });
  select.addEventListener('dragend', () => { clearProgramDrag(); renderList(); });
  row.addEventListener('dragover', event => {
    if (!draggingProgram || draggingProgram === id || busy) return;
    event.preventDefault(); event.dataTransfer.dropEffect = 'move';
    for (const item of $('program-list').children) item.classList.remove('drop-before', 'drop-after');
    const bounds = row.getBoundingClientRect();
    row.classList.add(event.clientY < bounds.top + bounds.height / 2 ? 'drop-before' : 'drop-after');
    const listBounds = $('program-list').getBoundingClientRect();
    if (event.clientY < listBounds.top + 28) $('program-list').scrollTop -= 18;
    if (event.clientY > listBounds.bottom - 28) $('program-list').scrollTop += 18;
  });
  row.addEventListener('dragleave', event => {
    if (!row.contains(event.relatedTarget)) row.classList.remove('drop-before', 'drop-after');
  });
  row.addEventListener('drop', event => {
    if (!draggingProgram) return;
    event.preventDefault(); event.stopPropagation();
    const source = draggingProgram, bounds = row.getBoundingClientRect();
    const after = event.clientY >= bounds.top + bounds.height / 2;
    clearProgramDrag();
    if (source !== id && !busy) perform(async () => applyState(await api('moveProgram', {id: source, data: id, after})));
  });
}
function renderList() {
  if (draggingProgram) return;
  updateProgramMenu();
  const filter = $('search').value.toLocaleLowerCase();
  const visible = state.programs.filter(p => !p.temporary && (p.name.toLocaleLowerCase().includes(filter) || p.path.toLocaleLowerCase().includes(filter)));
  const signature = JSON.stringify([currentLanguage, state.selected, visible.map(p => [p.id, p.path, p.kind, p.status, p.missing])]);
  if (signature !== listSignature) {
    listSignature = signature; $('program-list').replaceChildren();
    for (const p of visible) {
      const row = document.createElement('div'); row.className = 'program-row'; row.classList.toggle('selected', p.id === state.selected); row.dataset.id = p.id; row.setAttribute('role', 'listitem'); row.title = p.path;
      const select = document.createElement('button'); select.className = 'select-program'; select.setAttribute('aria-label', t`選取 ${p.name}`); select.setAttribute('aria-pressed', String(p.id === state.selected));
      const icon = document.createElement('span'); icon.className = `mini-icon ${p.kind}`; icon.textContent = p.kind === 'cli' ? '>_' : '▤'; icon.setAttribute('aria-hidden', 'true');
      const body = document.createElement('span'); body.className = 'row-body';
      const name = document.createElement('span'); name.className = 'row-name'; name.textContent = p.name;
      const details = document.createElement('span'); details.className = 'row-detail';
      const kind = document.createElement('span'); kind.textContent = p.missing ? t('檔案不存在') : `${p.temporary ? t('臨時 · ') : ''}${p.kind.toUpperCase()} · ${p.architecture || 'x64'}`;
      details.append(kind); body.append(name, details); select.append(icon, body); row.append(select);
      const control = document.createElement('button'); control.className = `program-action ${active(p) ? 'is-running' : ''}`; control.textContent = active(p) ? '■' : '▶'; control.setAttribute('aria-label', `${active(p) ? t('停止') : t('執行')} ${p.name}`); control.title = active(p) ? t('停止程式') : t('執行程式'); control.dataset.programAction = p.id;
      if (p.id === state.selected) control.id = active(p) ? 'stop' : 'run';
      control.onclick = () => perform(() => active(p) ? stopProgram(p.id) : startProgram(p.id)); row.append(control);
      if (active(p)) { const dot = document.createElement('i'); dot.className = 'row-dot'; row.append(dot); }
      select.addEventListener('click', () => perform(async () => { rememberDraft(); applyState(await api('select', { id: p.id })); }));
      wireProgramDrag(row, select, p.id);
      $('program-list').append(row);
    }
  }
  for (const button of document.querySelectorAll('[data-program-action]')) {
    const p = programsByID.get(button.dataset.programAction);
    button.disabled = busy || analysisActive(p) || p.status === 'stopping' || (!active(p) && (p.missing || !state.engineReady));
  }
  $('count').textContent = state.programs.filter(p => !p.temporary).length;
  $('list-empty').hidden = visible.length > 0;
  $('list-empty').textContent = state.programs.some(p => !p.temporary) ? t('沒有符合的程式。') : t('加入第一個 EXE，開始建立你的程式庫。');
}
function render() {
  setText('app-build-version', state.version || '');
  renderList();
  renderAnalysis();
  $('engine-dot').classList.toggle('ready', state.engineReady); $('engine-label').textContent = state.engineReady ? t('執行引擎已就緒') : t('找不到執行引擎');
  const temporaryCount = state.programs.filter(item => item.temporary).length;
  $('library-summary').textContent = t`${state.programs.length - temporaryCount} 個程式${temporaryCount ? t` · ${temporaryCount} 個臨時程式` : ''}${state.running ? t` · ${state.running} 個執行中` : t(' · 選取程式，開始執行。')}`;
  renderTemporarySessions();
  for (const id of ['add', 'empty-add', 'temporary']) $(id).disabled = busy;
  const p = selected(); $('empty').hidden = !!p; $('detail').hidden = !p;
  if (!p) { currentID = ''; return; }
  if (currentID !== p.id) { currentID = p.id; loadSettings(p); tab = 'workspace'; }
  const isCLI = p.kind === 'cli';
  setText('tab-workspace', isCLI ? t('終端') : t('執行狀態'));
  $('tab-settings').hidden = !!p.temporary;
  $('tab-log').hidden = isCLI;
  if (isCLI && tab === 'log') tab = 'workspace';
  for (const name of ['workspace', 'settings', 'log']) { $(`tab-${name}`).setAttribute('aria-selected', String(tab === name)); $(name).hidden = tab !== name; }
  $('cli-panel').hidden = !isCLI; $('gui-panel').hidden = isCLI;
  $('analyze').disabled = busy || p.missing || !state.engineReady || active(p);
  setText('analyze', analysisActive(p) ? t('查看分析進度') : t('程式加速分析'));
  $('config-path').textContent = p.configPath; $('save').disabled = busy; $('remove').disabled = busy || active(p) || analysisActive(p);
  $('config-file').hidden = !!p.temporary;
  $('settings-note').textContent = p.temporary ? t('本次設定不會加入程式庫，關閉後清除。') : t('每個 EXE 使用自己的設定檔，下次啟動時套用。');
  setText('save', p.temporary ? t('套用設定') : t('儲存設定'));
  setText('remove', p.temporary ? t('關閉臨時程式') : t('移出列表'));
  $('remove-note').textContent = p.temporary ? t('關閉後清除臨時項目，安裝與輸出檔案會保留。') : t('移出列表會保留原始 EXE 與設定檔。');
  $('diagnostics').disabled = !p.runId;
  $('terminal-state').textContent = p.runId ? t(statusNames[p.status]) : t('尚未執行');
  $('run-message').classList.toggle('error', p.status === 'failed' || p.missing);
  $('run-message').textContent = p.missing ? t('找不到原始 EXE，請確認磁碟已連接，或重新加入檔案。') : p.error ? t`${translateMessage(p.error)}，詳細原因可查看「執行紀錄」。` : active(p) ? (isCLI ? t('可直接輸入文字；切換程式時，終端內容會保留。') : t('程式在獨立的桌面視窗中執行。')) : p.runId ? (p.status === 'exited' ? t`程式已結束，退出碼 ${p.exitCode}。再次按下「執行」可重新啟動。` : t('程式已停止，可重新啟動。')) : t('啟動參數、工作資料夾與 C 槽映射可在「程式設定」調整。');
  if (p.temporary && ['exited', 'stopped'].includes(p.status)) $('run-message').textContent = t`本次臨時執行${p.status === 'exited' ? t`已結束，退出碼 ${p.exitCode}` : t('已停止')}，可關閉此工作階段。`;
  $('gui-title').textContent = active(p) ? t('程式正在獨立視窗執行') : t('以獨立視窗開啟');
  renderUsage(p);
  for (const [id, item] of terminals) { item.host.hidden = id !== p.id || !isCLI; }
  $('terminal-idle').hidden = !!p.runId; $('clear-terminal').disabled = !terminals.has(p.id);
  if (p.runId) ensureTerminal(p);
  renderLog();
  updateDirty();
  if (isCLI && tab === 'workspace') requestAnimationFrame(fitSelected);
}
function ensureTerminal(p) {
  let item = terminals.get(p.id);
  if (item?.runId === p.runId) return item;
  if (item) { item.term?.dispose(); item.host.remove(); }
  const host = document.createElement('div'); host.className = 'terminal-host'; host.hidden = p.id !== state.selected || p.kind !== 'cli'; $('terminals').append(host);
  item = { runId: p.runId, host, input: Promise.resolve(), log: p.kind === 'gui' ? new RuntimeLog() : null };
  if (p.kind === 'cli') {
    const term = new Terminal({ fontFamily: 'Menlo, Monaco, monospace', fontSize: 12, lineHeight: 1.3, scrollback: 5000, cursorBlink: true, cursorStyle: 'bar', allowProposedApi: false, theme: { background: '#172125', foreground: '#c4d6d4', cursor: '#75b8a6', selectionBackground: '#4d766b80', black: '#203136', red: '#d9897e', green: '#87b69b', yellow: '#c9b781', blue: '#89a7c5', magenta: '#ba9cbc', cyan: '#82b4b4', white: '#d2ddda' } });
    const fit = new FitAddon.FitAddon(); term.loadAddon(fit); term.open(host); item.term = term; item.fit = fit;
    term.onData(data => {
      const live = programsByID.get(p.id);
      if (!active(live) || live.runId !== p.runId) return;
      // 順序化送出；以 Unicode code point 切割，避免貼上長文字時切斷 surrogate pair。
      const points = Array.from(data);
      for (let i = 0; i < points.length; i += 4096) {
        const chunk = points.slice(i, i + 4096).join('');
        item.input = item.input.then(() => api('input', { id: p.id, runId: p.runId, data: chunk })).catch(error => toast(errorText(error), true));
      }
    });
    term.onResize(({ cols, rows }) => { if (active(programsByID.get(p.id))) api('resize', { id: p.id, runId: p.runId, cols, rows }).catch(() => {}); });
  }
  terminals.set(p.id, item); return item;
}
function usageNumber(value, maximumFractionDigits = 1) {
  return value.toLocaleString(currentLanguage, { maximumFractionDigits, minimumFractionDigits: maximumFractionDigits });
}
function usageBytes(value, rate = false) {
  const units = ['B', 'KiB', 'MiB', 'GiB'];
  let index = 0;
  while (value >= 1024 && index < units.length - 1) { value /= 1024; ++index; }
  return `${usageNumber(value, index ? 1 : 0)} ${units[index]}${rate ? '/s' : ''}`;
}
function renderUsage(p) {
  const running = p.kind === 'gui' && active(p);
  $('gui-idle').hidden = running; $('usage-panel').hidden = !running;
  $('gui-panel').classList.toggle('show-usage', running);
  if (!running) return;
  const usage = p.usage?.pid === p.pid ? p.usage : null;
  const stale = usage && (Date.now() - usage.sampledAt > 7500 || usage.sampledAt > Date.now() + 3000);
  const fresh = usage && !stale;
  $('usage-panel').dataset.run = p.runId;
  $('usage-panel').dataset.sequence = String(usage?.sequence || 0);
  setText('usage-state', stale ? t('等待更新…') : !usage || usage.intervalSeconds === 0 ? t('正在取樣…') : t`已更新 ${new Date(usage.sampledAt).toLocaleTimeString(currentLanguage)}`);
  setText('usage-cpu', fresh && usage.cpuPercent != null ? `${usageNumber(usage.cpuPercent)}%` : '—');
  setText('usage-gpu', fresh && usage.gpuPercent != null ? `${usageNumber(usage.gpuPercent)}%` : '—');
  setText('usage-memory', fresh && usage.memoryBytes != null ? usageBytes(usage.memoryBytes) : '—');
  setText('usage-gpu-memory', fresh ? `GPU 記憶體合計 ${usageBytes(usage.gpuMemoryBytes)} · 每程序上限 ${usageBytes(usage.gpuMemoryLimitBytes)}${usage.gpuTimingAvailable ? '' : ' · 計時不可用'}` : t('正在取樣…'));
  const cache = fresh && usage.compilerCache;
  const lookups = cache ? cache.hits + cache.misses + cache.rejects : 0;
  setText('usage-cache', cache ? `EXE／DLL 快取：${lookups ? `${(cache.hits / lookups * 100).toFixed(1)}%（${cache.hits}／${lookups}）` : '尚無查詢'} · 背景命中 ${cache.backgroundHits} · 編譯每程序 ${cache.workers} worker（已建立 ${cache.activeWorkers}） · ${usage.processCount} 個程序` : '編譯記憶體快取正在取樣…');
  const hasNetwork = fresh && usage.receiveBytesPerSecond != null && usage.sendBytesPerSecond != null;
  setText('usage-network', hasNetwork ? usageBytes(usage.receiveBytesPerSecond + usage.sendBytesPerSecond, true) : '—');
  setText('usage-network-detail', hasNetwork ? `↑ ${usageBytes(usage.sendBytesPerSecond, true)}  ↓ ${usageBytes(usage.receiveBytesPerSecond, true)}` : t('正在取樣…'));
}
function renderLog() {
  const p = selected(); if (!p || tab !== 'log') return;
  const item = terminals.get(p.id), log = item?.runId === p.runId ? item.log : null;
  const content = $('runtime-log-content'), runKey = `${p.id}:${p.runId}`;
  const changedRun = content.dataset.run !== runKey;
  const follow = changedRun || content.scrollHeight - content.clientHeight - content.scrollTop < 32;
  const signature = `${runKey}:${log?.revision || 0}`;
  if (content.dataset.signature !== signature) {
    content.textContent = log?.text || '';
    content.dataset.signature = signature; content.dataset.run = runKey;
    if (follow) requestAnimationFrame(() => { content.scrollTop = content.scrollHeight; });
  }
  $('log-size').textContent = `${(log?.byteLength || 0).toLocaleString(currentLanguage)} / 131,072 B`;
  $('log-truncated').hidden = !log?.discardedBytes;
  $('clear-log').disabled = !log?.byteLength;
  $('runtime-log-empty').hidden = !!log?.text;
  $('runtime-log-empty').textContent = !p.runId ? t('執行後會在此顯示程式的標準輸出與錯誤輸出。') : active(p) ? t('正在等待執行輸出…') : t('尚無執行輸出。');
}
function fitSelected() {
  const p = selected(), item = p && terminals.get(p.id);
  if (!item?.term || tab !== 'workspace' || $('cli-panel').hidden || item.host.clientHeight < 40) return;
  item.fit.fit(); $('terminal-size').textContent = `${item.term.cols} × ${item.term.rows}`;
}
new ResizeObserver(fitSelected).observe($('workspace'));
async function poll() {
  if (busy) { pollTimer = setTimeout(poll, 100); return; }
  const version = mutationVersion;
  try {
    const result = await api('poll');
    if (!busy && version === mutationVersion) applyState(result.state);
    const received = new Map();
    for (const stream of result.streams) {
      if (!received.has(stream.id)) received.set(stream.id, new Set());
      received.get(stream.id).add(stream.runId);
      const p = programsByID.get(stream.id); if (!p || p.runId !== stream.runId) continue;
      const item = ensureTerminal(p); const bytes = Uint8Array.from(atob(stream.data), c => c.charCodeAt(0));
      item.log?.write(bytes);
      if (item.term) await new Promise(resolve => item.term.write(bytes, resolve));
    }
    // 結束後再等佇列排空，才處理未完整結束的最後一個字元。
    for (const p of state.programs) {
      const item = terminals.get(p.id);
      if (item?.runId === p.runId && !active(p) && !received.get(p.id)?.has(p.runId)) item.log?.write(undefined, true);
    }
    renderLog();
  } catch (error) { toast(t`無法更新執行狀態：${errorText(error)}`, true); }
  pollTimer = setTimeout(poll, 100);
}
async function addPrograms() {
  const result = await api('add'); rememberDraft(); applyState(result.state);
  if (result.errors.length) toast(result.errors.map(translateMessage).join('\n'), true);
  else if (result.added) toast(t`已加入 ${result.added} 個程式`);
  // JIT 快取（SMAP）暫停使用，新增程式後不再提示加速分析。
}
$('add').onclick = $('empty-add').onclick = () => perform(addPrograms);
$('search').oninput = renderList;
async function startProgram(id) {
  if (id !== state.selected) { rememberDraft(); applyState(await api('select', { id })); }
  const p = selected(); if (!p) return;
  const settings = readSettings(), item = terminals.get(p.id);
  applyState(await api('start', { id: p.id, settings, cols: item?.term?.cols || 80, rows: item?.term?.rows || 24 }));
  drafts.delete(p.id); showTab('workspace');
  if (p.kind === 'cli') { const terminal = ensureTerminal(selected()); requestAnimationFrame(() => { fitSelected(); terminal.term.focus(); }); }
}
async function stopProgram(id) { applyState(await api('stop', { id })); }
$('save').onclick = () => perform(async () => { const id = state.selected; const next = await api('save', { id, settings: readSettings() }); drafts.delete(id); applyState(next); toast(selected()?.temporary ? t('本次設定已套用') : t('此程式的設定已儲存')); });
async function removeProgram(id) { const next = await api('remove', { id }); const item = terminals.get(id); item?.term?.dispose(); item?.host.remove(); terminals.delete(id); drafts.delete(id); applyState(next); }
// 全域取消 WebKit 預設選單；程式列使用同一個 remove API，保留 EXE 與設定。
const programMenu = document.createElement('div');
programMenu.id = 'program-context-menu'; programMenu.className = 'program-context-menu';
programMenu.setAttribute('role', 'menu'); programMenu.setAttribute('aria-label', t('程式項目')); programMenu.hidden = true;
const programMenuRemove = document.createElement('button');
programMenuRemove.id = 'program-context-remove'; programMenuRemove.type = 'button'; programMenuRemove.setAttribute('role', 'menuitem');
programMenu.append(programMenuRemove); document.body.append(programMenu);
let programMenuID = '', programMenuOrigin = null;
function closeProgramMenu(restoreFocus = false) {
  programMenu.hidden = true; programMenuID = '';
  if (restoreFocus && programMenuOrigin?.isConnected) programMenuOrigin.focus();
  programMenuOrigin = null;
}
function updateProgramMenu() {
  programMenu.setAttribute('aria-label', t('程式項目'));
  if (!programMenuID) return;
  const p = programsByID.get(programMenuID);
  if (!p || !document.querySelector(`[data-id="${programMenuID}"]`)) { closeProgramMenu(); return; }
  setText(programMenuRemove, p.temporary ? t('關閉臨時程式') : t('從列表移除'));
  programMenuRemove.disabled = busy || active(p);
  programMenuRemove.title = active(p) ? t('請先停止程式再移除') : t('保留原始 EXE 與設定檔');
}
document.addEventListener('contextmenu', event => {
  event.preventDefault(); closeProgramMenu();
  const row = event.target.closest?.('#program-list .program-row');
  if (!row) return;
  programMenuID = row.dataset.id; programMenuOrigin = row.querySelector('.select-program');
  updateProgramMenu(); if (!programMenuID) return;
  programMenu.hidden = false;
  const rect = row.getBoundingClientRect();
  const x = event.clientX || rect.left + 16, y = event.clientY || rect.top + 16;
  programMenu.style.left = `${Math.max(4, Math.min(x, innerWidth - programMenu.offsetWidth - 4))}px`;
  programMenu.style.top = `${Math.max(4, Math.min(y, innerHeight - programMenu.offsetHeight - 4))}px`;
  programMenuRemove.focus();
}, true);
programMenuRemove.onclick = () => {
  const id = programMenuID, p = programsByID.get(id);
  if (!p || busy || active(p)) return;
  closeProgramMenu(); perform(() => removeProgram(id));
};
document.addEventListener('pointerdown', event => { if (!programMenu.contains(event.target)) closeProgramMenu(); }, true);
document.addEventListener('keydown', event => { if (event.key === 'Escape' && programMenuID) { event.preventDefault(); closeProgramMenu(true); } });
document.addEventListener('scroll', () => closeProgramMenu(), true);
window.addEventListener('resize', () => closeProgramMenu());
window.addEventListener('blur', () => closeProgramMenu());
$('remove').onclick = () => perform(() => removeProgram(state.selected));
$('settings-form').onsubmit = event => event.preventDefault();
$('settings-form').oninput = () => { rememberDraft(); updateDirty(); };
for (const [button, input, title] of [['pick-work', 'work-dir', '選擇工作資料夾'], ['pick-drive', 'c-drive', '選擇 C 槽資料夾'], ['pick-install', 'install-root', '選擇安裝目錄']]) $(button).onclick = () => perform(async () => { const paths = await api('folder', { data: t(title) }); if (paths.length) { $(input).value = paths[0]; rememberDraft(); updateDirty(); } });
$('temporary').onclick = () => perform(async () => {
  rememberDraft();
  const next = await api('temporary'); if (!next) return;
  applyState(next); showTab('workspace');
  if (selected()?.kind === 'cli') { const terminal = ensureTerminal(selected()); requestAnimationFrame(() => { fitSelected(); terminal.term.focus(); }); }
});
function renderTemporarySessions() {
  const sessions = state.programs.filter(p => p.temporary), select = $('temporary-session-select');
  const previous = select.value;
  $('temporary-sessions').hidden = sessions.length === 0;
  const signature = JSON.stringify([currentLanguage, sessions.map(p => [p.id, p.name, p.status])]);
  if (select.dataset.signature !== signature) {
    select.dataset.signature = signature;
    select.replaceChildren(...sessions.map((p, index) => { const option = document.createElement('option'); option.value = p.id; option.textContent = `${p.name} · ${index + 1} · ${t(statusNames[p.status])}`; return option; }));
  }
  select.value = selected()?.temporary ? state.selected : sessions.some(p => p.id === previous) ? previous : sessions[0]?.id || '';
  const p = sessions.find(p => p.id === select.value);
  select.disabled = busy;
  $('temporary-stop').disabled = busy || !active(p) || p.status === 'stopping';
  $('temporary-close').disabled = busy || !p || active(p);
}
$('temporary-session-select').onchange = () => { const id = $('temporary-session-select').value; perform(async () => { rememberDraft(); applyState(await api('select', { id })); }); };
$('temporary-stop').onclick = () => perform(() => stopProgram($('temporary-session-select').value));
$('temporary-close').onclick = () => perform(() => removeProgram($('temporary-session-select').value));
for (const name of ['workspace', 'settings', 'log']) $(`tab-${name}`).onclick = () => showTab(name);
$('reveal').onclick = () => perform(() => api('reveal', { id: state.selected }));
$('diagnostics').onclick = () => perform(async () => { $('log-content').textContent = await api('diagnostics', { id: state.selected }); $('log-dialog').showModal(); });
$('close-log').onclick = () => $('log-dialog').close();
$('clear-terminal').onclick = () => { terminals.get(state.selected)?.term?.clear(); };
$('clear-log').onclick = () => { terminals.get(state.selected)?.log?.clear(); renderLog(); };
document.addEventListener('keydown', event => { if (event.metaKey && event.key.toLowerCase() === 'o') { event.preventDefault(); perform(addPrograms); } });
document.addEventListener('dragover', event => event.preventDefault());
document.addEventListener('drop', event => event.preventDefault());
window.launcherReady = Promise.all([api('snapshot'), api('windowsFonts')]).then(([initial, fonts]) => { windowsFonts = fonts || []; applyState(initial); poll(); pollUpdate(); });
window.launcherReady.catch(error => toast(t`啟動失敗：${errorText(error)}`, true));

// 快取用量：以 GB（十進位）顯示，每個項目可獨立清除；有程式執行中時不可清除。
const cacheSize = bytes => t`${(bytes / 1e9).toFixed(2)} GB`;
function renderCacheUsage(usage) {
  const list = $('cache-list'), entries = usage?.entries || [];
  setText('cache-total', cacheSize(usage?.totalBytes || 0));
  $('cache-clear-all').disabled = Boolean(usage?.running) || entries.length === 0;
  $('cache-empty').hidden = entries.length > 0; $('cache-running').hidden = !usage?.running;
  list.replaceChildren(...entries.map(entry => {
    const row = document.createElement('div'), text = document.createElement('div'), title = document.createElement('strong'), detail = document.createElement('span');
    const size = document.createElement('output'), clear = document.createElement('button');
    row.className = 'cache-row'; row.setAttribute('role', 'listitem'); row.dataset.cacheId = entry.id;
    const shared = { legacyShaders: [t`舊版共用著色器快取`, t`改為每個程式獨立之前的著色器快取，新版本不再使用。`], resources: [t`生成資源快取`, t`字型等生成資源，所有程式共用。`], identity: [t`映像身分快取`, t`EXE／DLL 的內容識別結果，所有程式共用。`] }[entry.kind];
    title.textContent = shared ? shared[0] : entry.name || t`未知程式`;
    detail.textContent = shared ? shared[1] : entry.path ? t`著色器快取 · ${entry.path}` : t`著色器快取`;
    detail.title = detail.textContent; size.textContent = cacheSize(entry.bytes);
    clear.type = 'button'; clear.className = 'button secondary cache-delete'; clear.textContent = t`刪除`; clear.disabled = Boolean(usage.running);
    clear.setAttribute('aria-label', t`刪除 ${title.textContent}`);
    clear.onclick = async () => {
      clear.disabled = true;
      try { renderCacheUsage(await api('clearCache', { data: entry.id })); toast(t`已刪除快取`); }
      catch (error) { toast(errorText(error), true); refreshCacheUsage(); }
    };
    text.append(title, detail); row.append(text, size, clear); return row;
  }));
}
async function refreshCacheUsage() {
  setText('cache-total', t`正在計算…`);
  try { renderCacheUsage(await api('cacheUsage')); } catch (error) { toast(errorText(error), true); }
}
$('cache-refresh').onclick = refreshCacheUsage;
$('cache-clear-all').onclick = async () => {
  $('cache-clear-all').disabled = true;
  for (const button of document.querySelectorAll('.cache-delete')) button.disabled = true;
  try { renderCacheUsage(await api('clearCache', {data: 'all'})); toast(t`已刪除快取`); }
  catch (error) { toast(errorText(error), true); await refreshCacheUsage(); }
};
function showGlobalTab(name) {
  $('settings-update-actions').hidden = name !== 'about';
  for (const button of document.querySelectorAll('[data-global-tab]')) {
    const current = button.dataset.globalTab === name;
    button.setAttribute('aria-selected', String(current)); button.tabIndex = current ? 0 : -1;
    $('global-' + button.dataset.globalTab).hidden = !current;
  }
  if (name === 'cache') refreshCacheUsage();
}
for (const button of document.querySelectorAll('[data-global-tab]')) {
  button.onclick = () => showGlobalTab(button.dataset.globalTab);
  button.onkeydown = event => {
    const tabs = [...document.querySelectorAll('[data-global-tab]')], index = tabs.indexOf(button);
    const target = event.key === 'ArrowDown' ? (index + 1) % tabs.length : event.key === 'ArrowUp' ? (index + tabs.length - 1) % tabs.length : event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : -1;
    if (target >= 0) { event.preventDefault(); tabs[target].click(); tabs[target].focus(); }
  };
}
function addGlobalDrive(letter, path = '') {
  const row = document.createElement('div'); row.className = 'global-drive-row' + (letter === 'C' ? ' required-drive' : '');
  if (letter === 'C') { const label = document.createElement('label'); label.htmlFor = 'global-c-drive'; label.textContent = t('C: 必填'); row.append(label); }
  else { const select = document.createElement('select'); select.className = 'drive-letter'; select.setAttribute('aria-label', t('磁碟代號')); for (let n = 68; n <= 90; n++) { const option = document.createElement('option'); option.value = String.fromCharCode(n); option.textContent = option.value + ':'; select.append(option); } select.value = letter; row.append(select); }
  const input = document.createElement('input'); input.className = 'drive-path'; input.value = path; input.required = true; input.placeholder = t('選擇或輸入目錄'); input.setAttribute('aria-label', letter + t(': 對應目錄')); input.autocomplete = 'off'; input.spellcheck = false; if (letter === 'C') input.id = 'global-c-drive'; row.append(input);
  const pick = document.createElement('button'); pick.type = 'button'; pick.className = 'button secondary drive-pick'; pick.textContent = t('選擇…'); pick.onclick = () => perform(async () => { const paths = await api('folder', { data: t('選擇磁碟對應目錄') }); if (paths.length) input.value = paths[0]; }); row.append(pick);
  if (letter !== 'C') { const remove = document.createElement('button'); remove.type = 'button'; remove.className = 'button secondary danger drive-remove'; remove.textContent = t('移除'); remove.onclick = () => { row.remove(); $('global-add-drive').disabled = false; }; row.append(remove); }
  $('global-drive-rows').append(row);
}
let globalGPUProgramID = '';
const globalCPUSelections = {};
let globalGPUMemoryMiB = 0;
function loadCapsuleChoices(id, value, maximum, disabled = false) {
  $(id).disabled = disabled;
  for (const input of document.querySelectorAll(`input[name="${id}"]`)) {
    input.disabled = Number(input.value) > maximum;
    input.checked = !input.disabled && Number(input.value) === value;
  }
}
function updateCPUReadouts() {
  for (const architecture of ['x64', 'x86']) {
    const id = 'cpu-cores-' + architecture, cores = globalCPUSelections[architecture];
    setText(id + '-value', t`${cores.toLocaleString(currentLanguage)} 個核心`);
    $(id + '-warning').hidden = cores <= 2;
  }
  setText('cpu-system-cores', t`實體核心：${state.cpuLimits.physicalCores.toLocaleString(currentLanguage)}`);
}
function loadCPUChoices(defaults, limits) {
  for (const architecture of ['x64', 'x86']) {
    const cores = defaults['cpuCores' + architecture.toUpperCase()];
    globalCPUSelections[architecture] = cores;
    loadCapsuleChoices('cpu-cores-' + architecture, cores, limits[architecture + 'Max']);
  }
  updateCPUReadouts();
}
for (const architecture of ['x64', 'x86']) {
  for (const input of document.querySelectorAll(`input[name="cpu-cores-${architecture}"]`)) {
    input.onchange = () => {
      if (input.checked && !input.disabled) { globalCPUSelections[architecture] = Number(input.value); updateCPUReadouts(); }
    };
  }
}
function updateGPUMemoryReadout() {
  const limits = state.gpuMemoryLimits;
  setText('gpu-memory-value', globalGPUMemoryMiB.toLocaleString(currentLanguage) + ' MiB');
  if (limits) setText('gpu-memory-system', t`系統記憶體：${(limits.systemMemoryBytes / 1024 / 1024).toLocaleString(currentLanguage)} MiB · 上限 50%`);
}
function loadGPUChoices(value, limits, disabled) {
  globalGPUMemoryMiB = value;
  loadCapsuleChoices('gpu-memory', value, Math.min(limits.maxMiB, limits.systemMemoryBytes / 2 / 1024 / 1024), disabled);
  updateGPUMemoryReadout();
}
for (const input of document.querySelectorAll('input[name="gpu-memory"]')) {
  input.onchange = () => {
    if (input.checked && !input.disabled && !$('gpu-memory').disabled) { globalGPUMemoryMiB = Number(input.value); updateGPUMemoryReadout(); }
  };
}
function updateWindowsFontPreview() {
  const font = $('windows-font').value;
  $('windows-font-preview').style.fontFamily = font === 'auto' ? '-apple-system, sans-serif' : JSON.stringify(font) + ', sans-serif';
}
$('windows-font').onchange = updateWindowsFontPreview;
$('app-settings').onclick = () => {
  closeProgramMenu(); $('global-drive-rows').replaceChildren();
  const defaults = state.globalSettings || { cDrive: '', drives: {} };
  $('certificate-roots-file').checked = defaults.certificateRootsFile === true;
  $('certificate-roots-path').value = defaults.certificateRootsPath || 'ProgramData/MacWin/Certificates/ROOT.pem';
  $('windows-font').replaceChildren(new Option(t('自動（macOS 系統向量字型）'), 'auto'), ...windowsFonts.map(font => new Option(font, font)));
  $('windows-font').value = defaults.windowsFont || 'auto';
  updateWindowsFontPreview();
  $('ui-language').value = defaults.language || 'auto'; $('ui-theme').value = defaults.theme || 'forest';
  loadCPUChoices(defaults, state.cpuLimits);
  const program = selected(); globalGPUProgramID = program?.id || '';
  const limits = state.gpuMemoryLimits;
  loadGPUChoices(program?.gpuMemoryMiB || limits.defaultMiB, limits, !program);
  setText('app-version', t`版本 ${state.version}`);
  addGlobalDrive('C', defaults.cDrive);
  for (const [letter, path] of Object.entries(defaults.drives || {}).sort()) addGlobalDrive(letter, path);
  $('global-add-drive').disabled = Object.keys(defaults.drives || {}).length >= 23;
  $('global-settings-error').hidden = true; $('global-settings-status').textContent = '';
  showGlobalTab('general'); $('app-settings-dialog').showModal(); $('global-tab-general').focus();
};
$('global-add-drive').onclick = () => {
  const used = new Set([...document.querySelectorAll('.drive-letter')].map(input => input.value));
  for (let n = 68; n <= 90; n++) { const letter = String.fromCharCode(n); if (!used.has(letter)) { addGlobalDrive(letter); break; } }
  $('global-add-drive').disabled = document.querySelectorAll('.drive-letter').length >= 23;
};
for (const id of ['app-settings-close', 'global-settings-cancel']) $(id).onclick = () => { $('app-settings-dialog').close(); restoreAppearance(); };
$('global-settings-form').onsubmit = event => { event.preventDefault(); $('global-settings-save').click(); };
$('global-gpu-form').onsubmit = event => { event.preventDefault(); $('global-settings-save').click(); };
$('global-cpu-form').onsubmit = event => { event.preventDefault(); $('global-settings-save').click(); };
$('global-settings-save').onclick = async () => {
  $('global-settings-error').hidden = true;
  if (!$('global-cpu-form').checkValidity()) { showGlobalTab('cpu'); $('global-cpu-form').reportValidity(); return; }
  if (!$('global-gpu-form').checkValidity()) { showGlobalTab('gpu'); $('global-gpu-form').reportValidity(); return; }
  if (!$('global-settings-form').checkValidity()) { showGlobalTab('advanced'); $('global-settings-form').reportValidity(); return; }
  const defaults = { windowsFont: $('windows-font').value, nativeV8Bridge: state.globalSettings.nativeV8Bridge === true, nativeLibraryBridge: state.globalSettings.nativeLibraryBridge === true, compilerWorkers: state.globalSettings.compilerWorkers ?? 2, cpuCoresX64: globalCPUSelections.x64, cpuCoresX86: globalCPUSelections.x86, openGLBackend: 'auto', cDrive: $('global-c-drive').value, drives: {}, language: $('ui-language').value, theme: $('ui-theme').value };
  defaults.certificateRootsFile = $('certificate-roots-file').checked;
  defaults.certificateRootsPath = $('certificate-roots-path').value;
  for (const row of document.querySelectorAll('.global-drive-row')) {
    const letter = row.querySelector('.drive-letter')?.value; if (!letter) continue;
    if (Object.hasOwn(defaults.drives, letter)) { showGlobalTab('advanced'); $('global-settings-error').textContent = t('磁碟代號不可重複'); $('global-settings-error').hidden = false; return; }
    defaults.drives[letter] = row.querySelector('.drive-path').value;
  }
  $('global-settings-save').disabled = true; ++mutationVersion;
  try { applyState(await api('saveGlobalSettings', { globalSettings: defaults, id: globalGPUProgramID, settings: { gpuMemoryMiB: globalGPUMemoryMiB } })); $('global-settings-status').textContent = t('已儲存。介面偏好已套用，執行設定於下次啟動生效。'); }
  catch (error) { $('global-settings-error').textContent = errorText(error); $('global-settings-error').hidden = false; }
  finally { ++mutationVersion; $('global-settings-save').disabled = false; }
};

// 分析工作在引擎子程序進行；UI 依實際完成檔案數顯示進度，不模擬百分比。
const analysisQueue = [];
let analysisID = '', analysisMode = '';
const analysisActive = p => p?.analysis && ['running', 'stopping'].includes(p.analysis.status);
function renderAnalysis() {
  const dialog = $('analysis-dialog');
  if (!analysisID && analysisQueue.length && !document.querySelector('dialog[open]')) {
    analysisID = analysisQueue.shift(); analysisMode = 'prompt';
  }
  if (!analysisID) return;
  const p = programsByID.get(analysisID);
  if (!p) { analysisID = ''; dialog.close(); return; }
  if (!dialog.open) dialog.showModal();
  $('analysis-name').textContent = p.name;
  const prompt = analysisMode === 'prompt', running = analysisActive(p), job = p.analysis;
  $('analysis-prompt').hidden = !prompt; $('analysis-progress-area').hidden = prompt;
  $('analysis-start').hidden = !prompt; $('analysis-start').disabled = busy || state.programs.some(analysisActive) || active(p) || !state.engineReady;
  setText('analysis-cancel', prompt ? t('稍後再說') : running ? t('取消分析') : t('完成'));
  $('analysis-cancel').disabled = busy || job?.status === 'stopping';
  $('analysis-spinner').hidden = !running;
  if (!prompt) {
    $('analysis-stage').textContent = t(job?.stage) || t('準備分析');
    $('analysis-message').textContent = translateMessage(job?.message) || t('正在準備…');
    const progress = $('analysis-progress');
    progress.max = job?.total || 1;
    if (job?.total) progress.value = job.completed; else progress.removeAttribute('value');
    $('analysis-count').textContent = job?.total ? t`${job.completed} / ${job.total} 個檔案${running ? t(' · 正在建立快取') : ''}` : t('正在確認分析範圍');
  }
}
async function beginAnalysis(id) {
  const next = await api('analyze', { id }); analysisID = id; analysisMode = 'progress'; applyState(next);
}
$('analyze').onclick = () => perform(async () => {
  if (analysisActive(selected())) { analysisID = state.selected; analysisMode = 'progress'; renderAnalysis(); }
  else await beginAnalysis(state.selected);
});
$('analysis-start').onclick = () => perform(() => beginAnalysis(analysisID));
async function dismissAnalysis() {
  if (analysisMode === 'progress' && analysisActive(programsByID.get(analysisID))) {
    applyState(await api('cancelAnalysis', { id: analysisID })); return;
  }
  analysisID = ''; analysisMode = ''; $('analysis-dialog').close(); renderAnalysis();
}
$('analysis-cancel').onclick = () => perform(dismissAnalysis);
$('analysis-dialog').addEventListener('cancel', event => { event.preventDefault(); perform(dismissAnalysis); });

// 預覽只影響介面，儲存後才寫入全域設定；取消或 Escape 還原。
function previewAppearance() {
 applyAppearance($('ui-language').value, $('ui-theme').value);
 updateCPUReadouts(); updateGPUMemoryReadout(); setText('app-version', t`版本 ${state.version}`);
 const rows = [...document.querySelectorAll('.global-drive-row')].map(row => [row.querySelector('.drive-letter')?.value || 'C', row.querySelector('.drive-path').value]);
 $('global-drive-rows').replaceChildren();
 for (const [letter, path] of rows) addGlobalDrive(letter, path);
 programMenu.setAttribute('aria-label', t('程式項目'));
 render(); renderUpdate();
}
for (const id of ['ui-language', 'ui-theme']) $(id).onchange = previewAppearance;
function restoreAppearance() {
 applyAppearance(state.globalSettings?.language, state.globalSettings?.theme);
 render(); renderUpdate();
}
$('app-settings-dialog').addEventListener('close', restoreAppearance);
window.addEventListener('languagechange', () => {
 if ($('app-settings-dialog').open) previewAppearance();
 else { applyAppearance(state.globalSettings?.language, state.globalSettings?.theme); render(); }
});

// 版本更新：後端在背景偵測、下載與安裝，對話框輪詢顯示進度。
// 啟動時的檢查若發現可安裝的新版，只詢問一次；關於頁按鈕直接進入更新流程。
const updateBusyPhases = ['checking', 'downloading', 'verifying', 'installing', 'restarting'];
let updateTimer = 0, updatePrompted = false, updateStatus = { phase: 'idle' };
const megabytes = bytes => (bytes / 1048576).toFixed(1);
function updateMessage(status) {
  switch (status.phase) {
    case 'checking': return t('正在檢查 GitHub 最新版本…');
    case 'latest': return t`已是最新版本（${status.current}）`;
    case 'available': return t`AppleJuice ${status.latest} 已發布，目前版本 ${status.current}。是否要更新？`;
    case 'downloading': return t`正在下載 ${status.latest}…`;
    case 'verifying': return t('正在驗證簽章與公證…');
    case 'installing': return t('正在安裝更新…');
    case 'restarting': return t`已更新為 ${status.latest}，正在重新啟動…`;
    case 'unavailable': case 'error': return errorText(status.error);
  }
  return '';
}
function renderUpdate(status = updateStatus) {
  updateStatus = status;
  const busy = updateBusyPhases.includes(status.phase), prompt = status.phase === 'available', message = updateMessage(status);
  setText('update-status', ['idle', 'latest'].includes(status.phase) ? '' : message);
  $('check-update').disabled = busy;
  if (!$('update-dialog').open) return;
  setText('update-summary', busy ? (status.latest ? `AppleJuice ${status.latest}` : '') : message);
  $('update-progress-area').hidden = !busy;
  setText('update-message', busy ? message : '');
  const progress = $('update-progress');
  if (status.phase === 'downloading' && status.total > 0) {
    progress.max = status.total; progress.value = Math.min(status.received || 0, status.total);
    setText('update-count', `${megabytes(status.received || 0)} / ${megabytes(status.total)} MB（${Math.floor(100 * (status.received || 0) / status.total)}%）`);
  } else {
    progress.removeAttribute('value'); setText('update-count', '');
  }
  $('update-now').hidden = !prompt;
  $('update-later').hidden = busy;
  setText('update-later', prompt ? t('稍後再說') : t('關閉'));
}
async function pollUpdate() {
  clearTimeout(updateTimer);
  try {
    const status = await api('updateStatus');
    if (status.phase === 'available' && !updatePrompted) { updatePrompted = true; $('update-dialog').showModal(); }
    renderUpdate(status);
    if (updateBusyPhases.includes(status.phase)) updateTimer = setTimeout(pollUpdate, 300);
  } catch (error) {
    renderUpdate({ phase: 'error', error: error.message || String(error) });
  }
}
async function startUpdate() {
  updatePrompted = true;
  if (!$('update-dialog').open) $('update-dialog').showModal();
  try { renderUpdate(await api('installUpdate')); } catch (error) { renderUpdate({ phase: 'error', error: error.message || String(error) }); return; }
  pollUpdate();
}
$('check-update').onclick = startUpdate;
$('update-now').onclick = startUpdate;
$('update-later').onclick = () => $('update-dialog').close();
$('update-dialog').addEventListener('cancel', event => { if (updateBusyPhases.includes(updateStatus.phase)) event.preventDefault(); });
window.addEventListener('languagechange', () => renderUpdate());
