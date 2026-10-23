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
function settingsFrom(p) { return { arguments: p.arguments, workDir: p.workDir, cDrive: p.cDrive, installRoot: p.installRoot || '', writable: p.writable, maxInstructions: p.maxInstructions, gpuMemoryMiB: p.gpuMemoryMiB || state.gpuMemoryLimits.defaultMiB, cpuCores: p.cpuCores || 0 }; }
function readSettings() {
  const p = selected();
  return { arguments: $('arguments').value, workDir: $('work-dir').value, cDrive: $('c-drive').value, installRoot: $('install-root').value, writable: $('writable').checked, maxInstructions: p?.maxInstructions ?? 0, gpuMemoryMiB: p?.gpuMemoryMiB || state.gpuMemoryLimits.defaultMiB, cpuCores: Number($('program-cpu-cores').value) };
}
function rememberDraft() { if (currentID) drafts.set(currentID, readSettings()); }
function loadSettings(p) {
  const s = drafts.get(p.id) || settingsFrom(p);
  $('arguments').value = s.arguments; $('work-dir').value = s.workDir; $('c-drive').value = s.cDrive; $('install-root').value = s.installRoot || ''; $('writable').checked = s.writable;
  const maximum = state.cpuLimits[p.architecture === 'x86' ? 'x86Max' : 'x64Max'];
  $('program-cpu-cores').replaceChildren(new Option(t('沿用全域設定'), '0'), ...Array.from({ length: maximum }, (_, i) => new Option(t`${i + 1} 個核心`, String(i + 1))));
  $('program-cpu-cores').value = s.cpuCores || 0;
}
function updateInstallLocation(prefix = '') {
  const chosen = $(prefix + 'install-root').value;
  const cDrive = $(prefix + 'c-drive').value.replace(/\/+$/, '');
  $(prefix + 'install-default').textContent = chosen ? t`安裝位置：${chosen}` : t`預設：${cDrive}/Program Files`;
}
function updateProgramCPUWarning() {
  const p = selected(), architecture = p?.architecture === 'x86' ? 'X86' : 'X64';
  const cores = Number($('program-cpu-cores').value) || state.globalSettings?.['cpuCores' + architecture] || 0;
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
function renderList() {
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
  $('gui-process').textContent = active(p) ? `PID ${p.pid} · Windows ${p.architecture || 'x64'} → Apple Silicon` : `Windows ${p.architecture || 'x64'} → Apple Silicon`;
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
window.launcherReady = Promise.all([api('snapshot'), api('windowsFonts')]).then(([initial, fonts]) => { windowsFonts = fonts || []; applyState(initial); poll(); });
window.launcherReady.catch(error => toast(t`啟動失敗：${errorText(error)}`, true));

function showGlobalTab(name) {
  for (const button of document.querySelectorAll('[data-global-tab]')) {
    const current = button.dataset.globalTab === name;
    button.setAttribute('aria-selected', String(current)); button.tabIndex = current ? 0 : -1;
    $('global-' + button.dataset.globalTab).hidden = !current;
  }
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
  const row = document.createElement('div'); row.className = 'global-drive-row';
  if (letter === 'C') { const label = document.createElement('label'); label.htmlFor = 'global-c-drive'; label.textContent = t('C: 必填'); row.append(label); }
  else { const select = document.createElement('select'); select.className = 'drive-letter'; select.setAttribute('aria-label', t('磁碟代號')); for (let n = 68; n <= 90; n++) { const option = document.createElement('option'); option.value = String.fromCharCode(n); option.textContent = option.value + ':'; select.append(option); } select.value = letter; row.append(select); }
  const input = document.createElement('input'); input.className = 'drive-path'; input.value = path; input.required = true; input.placeholder = t('選擇或輸入目錄'); input.setAttribute('aria-label', letter + t(': 對應目錄')); input.autocomplete = 'off'; input.spellcheck = false; if (letter === 'C') input.id = 'global-c-drive'; row.append(input);
  const pick = document.createElement('button'); pick.type = 'button'; pick.className = 'text-button drive-pick'; pick.textContent = t('選擇…'); pick.onclick = () => perform(async () => { const paths = await api('folder', { data: t('選擇磁碟對應目錄') }); if (paths.length) input.value = paths[0]; }); row.append(pick);
  if (letter !== 'C') { const remove = document.createElement('button'); remove.type = 'button'; remove.className = 'text-button danger drive-remove'; remove.textContent = t('移除'); remove.onclick = () => { row.remove(); $('global-add-drive').disabled = false; }; row.append(remove); }
  $('global-drive-rows').append(row);
}
let globalGPUProgramID = '';
function setRangeReadout(id, value) {
  setText(id + '-value', value); $(id).setAttribute('aria-valuetext', value);
}
function updateCPUReadouts() {
  for (const architecture of ['x64', 'x86']) {
    const id = 'cpu-cores-' + architecture;
    setRangeReadout(id, t`${Number($(id).value).toLocaleString(currentLanguage)} 個核心`);
    setText(id + '-max', Number($(id).max).toLocaleString(currentLanguage));
    $(id + '-warning').hidden = Number($(id).value) <= 2;
  }
  setText('cpu-system-cores', t`宿主實體核心：${state.cpuLimits.physicalCores.toLocaleString(currentLanguage)}`);
  const workers = Number($('compiler-workers').value);
  setRangeReadout('compiler-workers', workers ? t`${workers.toLocaleString(currentLanguage)} 個執行緒` : t('同步編譯'));
}
for (const architecture of ['x64', 'x86']) $('cpu-cores-' + architecture).oninput = updateCPUReadouts;
$('compiler-workers').oninput = updateCPUReadouts;
function updateGPUMemoryReadout() {
  const input = $('gpu-memory'), limits = state.gpuMemoryLimits;
  const value = Number(input.value).toLocaleString(currentLanguage) + ' MiB';
  setRangeReadout('gpu-memory', value);
  if (limits) {
    setText('gpu-memory-min', limits.minMiB.toLocaleString(currentLanguage) + ' MiB');
    setText('gpu-memory-max', limits.maxMiB.toLocaleString(currentLanguage) + ' MiB');
    setText('gpu-memory-system', t`系統記憶體：${(limits.systemMemoryBytes / 1024 / 1024).toLocaleString(currentLanguage)} MiB · 上限 50%`);
  }
}
$('gpu-memory').oninput = updateGPUMemoryReadout;
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
  $('opengl-backend').value = defaults.openGLBackend || 'auto';
  $('compiler-workers').value = defaults.compilerWorkers ?? 2;
  $('native-library-bridge').checked = defaults.nativeLibraryBridge === true;
  $('native-v8-bridge').checked = defaults.nativeV8Bridge === true;
  setText('native-v8-availability', state.nativeV8Ready ? t('V8 模組已內建，預設關閉。儲存後於下次啟動生效。') : t('此版本未包含 ARM64 V8 模組。'));
  $('add-native-v8-tests').disabled = !state.nativeV8Ready || !state.nativeV8TestsReady;
  for (const architecture of ['x64', 'x86']) {
    const input = $('cpu-cores-' + architecture), key = architecture.toUpperCase();
    input.max = state.cpuLimits[architecture + 'Max']; input.value = defaults['cpuCores' + key];
  }
  updateCPUReadouts();
  const program = selected(); globalGPUProgramID = program?.id || '';
  const limits = state.gpuMemoryLimits, memory = $('gpu-memory');
  memory.min = limits.minMiB; memory.max = limits.maxMiB; memory.step = limits.stepMiB;
  memory.value = program?.gpuMemoryMiB || limits.defaultMiB; memory.disabled = !program;
  updateGPUMemoryReadout();
  setText('app-version', t`版本 ${state.version}`);
  $('gpu-settings-program').textContent = program ? t`目前程式：${program.name}` : t('先從程式庫選取程式，再調整它的 GPU 記憶體。');
  addGlobalDrive('C', defaults.cDrive);
  for (const [letter, path] of Object.entries(defaults.drives || {}).sort()) addGlobalDrive(letter, path);
  $('global-add-drive').disabled = Object.keys(defaults.drives || {}).length >= 23;
  $('global-settings-error').hidden = true; $('global-settings-status').textContent = '';
  showGlobalTab('general'); $('app-settings-dialog').showModal(); $('global-tab-general').focus();
};
$('add-native-v8-tests').onclick = async () => {
  $('add-native-v8-tests').disabled = true; ++mutationVersion;
  try {
    const result = await api('addNativeV8Tests'); applyState(result.state);
    $('app-settings-dialog').close(); restoreAppearance();
    if (result.errors?.length) toast(result.errors.map(translateMessage).join('\n'), true);
  } catch (error) { $('global-settings-error').textContent = errorText(error); $('global-settings-error').hidden = false; }
  finally { ++mutationVersion; $('add-native-v8-tests').disabled = !state.nativeV8TestsReady; }
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
  const defaults = { windowsFont: $('windows-font').value, nativeV8Bridge: $('native-v8-bridge').checked, nativeLibraryBridge: $('native-library-bridge').checked, compilerWorkers: Number($('compiler-workers').value), cpuCoresX64: Number($('cpu-cores-x64').value), cpuCoresX86: Number($('cpu-cores-x86').value), openGLBackend: $('opengl-backend').value, cDrive: $('global-c-drive').value, drives: {}, language: $('ui-language').value, theme: $('ui-theme').value };
  defaults.certificateRootsFile = $('certificate-roots-file').checked;
  defaults.certificateRootsPath = $('certificate-roots-path').value;
  for (const row of document.querySelectorAll('.global-drive-row')) {
    const letter = row.querySelector('.drive-letter')?.value; if (!letter) continue;
    if (Object.hasOwn(defaults.drives, letter)) { showGlobalTab('advanced'); $('global-settings-error').textContent = t('磁碟代號不可重複'); $('global-settings-error').hidden = false; return; }
    defaults.drives[letter] = row.querySelector('.drive-path').value;
  }
  $('global-settings-save').disabled = true; ++mutationVersion;
  try { applyState(await api('saveGlobalSettings', { globalSettings: defaults, id: globalGPUProgramID, settings: { gpuMemoryMiB: Number($('gpu-memory').value) } })); $('global-settings-status').textContent = t('已儲存。介面偏好已套用，執行設定於下次啟動生效。'); }
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
 render();
}
for (const id of ['ui-language', 'ui-theme']) $(id).onchange = previewAppearance;
function restoreAppearance() {
 applyAppearance(state.globalSettings?.language, state.globalSettings?.theme);
 render();
}
$('app-settings-dialog').addEventListener('close', restoreAppearance);
window.addEventListener('languagechange', () => {
 if ($('app-settings-dialog').open) previewAppearance();
 else { applyAppearance(state.globalSettings?.language, state.globalSettings?.theme); render(); }
});
