// app.js - emu8 SD Manager web app: Web Serial transport + file browser UI.
import { Emu8Link, DeviceError, joinPath, parentPath } from './protocol.js';
import { zipStore } from './zip.js';

const $ = id => document.getElementById(id);
const DEFAULT_BAUD = 115200;
const PLACES = ['/', '/roms', '/roms/apple2', '/roms/c64', '/roms/msx', '/roms/iigs', '/roms/pcxt'];

// Extension -> which emu8 system opens it (README "microSD card preparation").
const SYSTEMS = {
  dsk: 'Apple · MSX · PC', do: 'Apple II', po: 'Apple II', nib: 'Apple II', hdv: 'Apple II', '2mg': 'Apple II / IIGS',
  prg: 'C64', d64: 'C64', crt: 'C64',
  nes: 'NES', a26: 'Atari 2600', sms: 'Master System',
  sna: 'ZX Spectrum', z80: 'ZX Spectrum', tap: 'ZX Spectrum', tzx: 'ZX Spectrum',
  rom: 'MSX / ROM', mx1: 'MSX', bin: 'ROM / Atari / SMS',
  img: 'PC-XT', ima: 'PC-XT', vhd: 'PC-XT', hdd: 'PC-XT',
  cfg: 'Settings',
};

const state = {
  conn: null,           // SerialConn
  link: null,           // Emu8Link
  info: null,
  cwd: '/',
  entries: [],
  sel: new Set(),       // selected paths
  sort: { key: 'name', dir: 'asc' },
  filter: '',
  queue: [],            // pending transfer jobs
  running: false,
  listToken: 0,
};

// ---------------------------------------------------------------------------------------------
// Web Serial transport
// ---------------------------------------------------------------------------------------------
class SerialConn {
  constructor(port, onBytes) {
    this.port = port;
    this.onBytes = onBytes;
    this.reader = null;
    this.writer = null;
    this.readLoop = null;
  }
  async open(baudRate) {
    await this.port.open({ baudRate, bufferSize: 65536 });
    this.baud = baudRate;
    this.writer = this.port.writable.getWriter();
    this.reader = this.port.readable.getReader();
    this.readLoop = (async () => {
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value?.length) this.onBytes(value);
        }
      } catch { /* port closed or unplugged */ }
      finally { try { this.reader.releaseLock(); } catch {} }
    })();
  }
  async write(bytes) { await this.writer.write(bytes); }
  async close() {
    try { await this.reader?.cancel(); } catch {}
    await this.readLoop;
    try { this.writer?.releaseLock(); } catch {}
    try { await this.port.close(); } catch {}
  }
  async reopen(baudRate) { await this.close(); await this.open(baudRate); }
}

// ---------------------------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------------------------
const sleep = ms => new Promise(r => setTimeout(r, ms));

function fmtSize(n) {
  if (n < 1024) return `${n} B`;
  const u = ['KB', 'MB', 'GB', 'TB'];
  let i = -1;
  do { n /= 1024; i++; } while (n >= 1024 && i < u.length - 1);
  return `${n < 10 ? n.toFixed(1) : Math.round(n)} ${u[i]}`;
}
function fmtTime(s) {
  if (!isFinite(s)) return '';
  s = Math.round(s);
  return s < 60 ? `${s}s` : `${Math.floor(s / 60)}m ${String(s % 60).padStart(2, '0')}s`;
}
function el(tag, props = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'text') e.textContent = v;
    else e.setAttribute(k, v);
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
}

let toastTimer = 0;
function toast(msg, isError = false) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'toast' + (isError ? ' error' : '');
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, isError ? 6000 : 3000);
}
function fail(e) {
  if (e?.name === 'AbortError') return;
  console.error(e);
  toast(e?.message || String(e), true);
}

/** Modal confirm/prompt. Returns the input text (prompt), true (confirm) or null when cancelled. */
function dialog({ title, text = '', input = null, ok = 'OK', danger = false }) {
  const d = $('dlg');
  $('dlgTitle').textContent = title;
  $('dlgText').textContent = text;
  $('dlgText').hidden = !text;
  const inp = $('dlgInput');
  inp.hidden = input === null;
  inp.value = input ?? '';
  $('dlgOk').textContent = ok;
  $('dlgOk').className = danger ? 'primary destructive' : 'primary';
  d.returnValue = 'cancel';
  d.showModal();
  if (input !== null) {
    inp.focus();
    const dot = inp.value.lastIndexOf('.');
    inp.setSelectionRange(0, dot > 0 ? dot : inp.value.length);
  }
  return new Promise(resolve => {
    d.addEventListener('close', () => {
      if (d.returnValue !== 'ok') return resolve(null);
      resolve(input !== null ? inp.value.trim() : true);
    }, { once: true });
  });
}

const BAD_NAME = /[\\/:*?"<>|\x00-\x1f]/;
function validName(n) {
  if (!n || n === '.' || n === '..' || BAD_NAME.test(n)) {
    toast('Names cannot be empty or contain \\ / : * ? " < > |', true);
    return false;
  }
  return true;
}

function setConnState(kind, text) {
  const p = $('connState');
  p.className = 'pill ' + kind;
  p.textContent = text;
}

function log(line) {
  const l = $('log');
  const atBottom = l.scrollTop + l.clientHeight >= l.scrollHeight - 4;
  l.textContent += line + '\n';
  if (l.textContent.length > 200_000) l.textContent = l.textContent.slice(-150_000);
  if (atBottom) l.scrollTop = l.scrollHeight;
}

// ---------------------------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------------------------
async function connect() {
  if (!('serial' in navigator)) { $('unsupported').hidden = false; return; }
  if (state.conn) return disconnect();
  let port;
  try { port = await navigator.serial.requestPort(); } catch { return; }   // user closed the picker
  let heard = 0;   // bytes seen before HELLO succeeds: tells "silent port" from "old firmware" below
  const conn = new SerialConn(port, bytes => { heard += bytes.length; state.link?.receive(bytes); });
  const link = new Emu8Link(b => conn.write(b), { onLog: log, baud: DEFAULT_BAUD });
  state.conn = conn;
  state.link = link;
  $('connectBtn').textContent = 'Disconnect';
  setConnState('busy', 'Opening port…');
  try {
    await conn.open(DEFAULT_BAUD);
    setConnState('busy', 'Waiting for board…');
    log('--- port open, looking for emu8 ---');
    // Generous retries: opening the port reboots some boards, and the server task only starts
    // once the emulator has finished booting.
    state.info = await link.hello(16);
    if (!state.info.sdMounted) toast('Connected, but the board reports no SD card mounted', true);
    await applySpeed(Number($('speed').value));
    onConnected();
  } catch (e) {
    const msg = !/did not answer/.test(e.message) ? e.message
      : heard
        ? 'The board is printing (see Device log) but the SD manager did not answer. Flash a firmware built with the SD manager (sdserial) and try again.'
        : 'Nothing came from this port. Check it is the board\'s port, that the board is powered and running, and that no other program has the port open.';
    await dropConnection();
    toast(msg, true);
  }
}

async function applySpeed(baud) {
  const { link, conn, info } = state;
  if (!info?.canSetBaud || link.baud === baud) return;
  const old = link.baud;
  setConnState('busy', `Switching to ${baud} baud…`);
  await link.setBaud(baud);
  await conn.reopen(baud);
  try {
    await link.hello(3);
    log(`--- link speed ${baud} baud ---`);
  } catch {
    // The adapter or cable cannot hold that rate. The board drops back to 115200 on its own
    // after a few silent seconds; follow it.
    link.baud = DEFAULT_BAUD;
    await conn.reopen(DEFAULT_BAUD);
    await sleep(4500);
    await link.hello(10);
    $('speed').value = String(DEFAULT_BAUD);
    toast(`${baud} baud did not work on this cable; using ${DEFAULT_BAUD}`, true);
    if (old !== DEFAULT_BAUD) log(`--- fell back to ${DEFAULT_BAUD} baud ---`);
  }
  setConnState('on', 'Connected');
}

function onConnected() {
  const { info } = state;
  setConnState('on', 'Connected');
  $('boardName').textContent = info.board;
  $('speedWrap').hidden = !info.canSetBaud;
  $('rebootBtn').hidden = false;
  $('welcome').hidden = true;
  $('app').hidden = false;
  refreshCardInfo();
  refreshPlaces();
  navigate('/');
}

async function dropConnection() {
  const conn = state.conn;
  state.conn = null;
  state.link = null;
  state.info = null;
  for (const job of state.queue) job.ctl.abort();
  state.current?.ctl.abort();
  await conn?.close();
  $('connectBtn').textContent = 'Connect';
  setConnState('off', 'Not connected');
  $('boardName').textContent = '';
  $('speedWrap').hidden = true;
  $('rebootBtn').hidden = true;
  $('app').hidden = true;
  $('welcome').hidden = false;
}

async function disconnect() {
  if (state.running && !await dialog({ title: 'Disconnect?', text: 'Transfers in progress will be cancelled.', ok: 'Disconnect', danger: true })) return;
  await state.link?.bye();
  await dropConnection();
}

async function reboot() {
  if (!await dialog({ title: 'Reboot the board?', text: 'The emulator restarts and loads the files currently on the card.', ok: 'Reboot' })) return;
  const { link, conn, info } = state;
  try {
    await link.reboot();
  } catch (e) { return fail(e); }
  if (info.usbLink) {
    // Native USB: the port disappears while the chip restarts. Start over from Connect.
    await dropConnection();
    toast('Board is restarting. Click Connect when it is back.');
    return;
  }
  setConnState('busy', 'Rebooting…');
  link.baud = DEFAULT_BAUD;
  try {
    await conn.reopen(DEFAULT_BAUD);
    state.info = await link.hello(20);
    await applySpeed(Number($('speed').value));
    setConnState('on', 'Connected');
    toast('Board restarted');
    navigate(state.cwd);
  } catch (e) {
    await dropConnection();
    fail(e);
  }
}

// ---------------------------------------------------------------------------------------------
// Browser
// ---------------------------------------------------------------------------------------------
async function refreshCardInfo() {
  const box = $('cardInfo');
  try {
    const fs = await state.link.fsInfo();
    if (!fs) { box.textContent = 'This board does not report card size.'; return; }
    const pct = Math.min(100, (fs.used / fs.total) * 100);
    box.replaceChildren(
      el('div', { class: 'meter' }, el('div', { style: `width:${pct.toFixed(1)}%` })),
      el('div', { text: `${fmtSize(fs.free)} free of ${fmtSize(fs.total)}` }));
  } catch { box.textContent = state.info?.sdMounted ? 'Card size unavailable' : 'No SD card mounted'; }
}

async function refreshPlaces() {
  const nav = $('places');
  nav.replaceChildren();
  for (const p of PLACES) {
    const a = el('a', { href: '#', text: p === '/' ? '/ (card root)' : p, 'data-path': p });
    a.addEventListener('click', ev => { ev.preventDefault(); openPlace(p, a); });
    nav.append(a);
  }
  for (const a of nav.children) {
    const p = a.dataset.path;
    if (p === '/') continue;
    try {
      const st = await state.link.stat(p);
      a.classList.toggle('missing', !st?.dir);
      if (!st?.dir) a.title = 'Not on the card yet: click to create it';
    } catch { /* keep going */ }
  }
  markPlace();
}

async function openPlace(p, a) {
  if (a.classList.contains('missing')) {
    if (!await dialog({ title: 'Create folder?', text: `${p} does not exist on the card yet.`, ok: 'Create' })) return;
    try { await state.link.mkdirs(p); } catch (e) { return fail(e); }
    refreshPlaces();
  }
  navigate(p);
}

function markPlace() {
  for (const a of $('places').children) a.classList.toggle('here', a.dataset.path === state.cwd);
}

async function navigate(path) {
  const token = ++state.listToken;
  state.cwd = path;
  state.sel.clear();
  $('filter').value = state.filter = '';
  renderCrumbs();
  markPlace();
  $('rows').replaceChildren();
  $('empty').hidden = true;
  $('loading').hidden = false;
  try {
    const entries = await state.link.list(path);
    if (token !== state.listToken) return;   // user already moved on
    state.entries = entries.filter(e => !e.name.endsWith('.part'));
  } catch (e) {
    if (token !== state.listToken) return;
    state.entries = [];
    fail(e);
    if (e instanceof DeviceError && e.status === 2 && path !== '/') return navigate(parentPath(path));
  } finally {
    if (token === state.listToken) $('loading').hidden = true;
  }
  renderRows();
}

function renderCrumbs() {
  const c = $('crumbs');
  c.replaceChildren();
  const parts = state.cwd.split('/').filter(Boolean);
  const link = (label, p) => {
    const a = el('a', { href: '#', text: label });
    a.addEventListener('click', ev => { ev.preventDefault(); navigate(p); });
    return a;
  };
  c.append(link('SD', '/'));
  let acc = '';
  for (const part of parts) {
    acc += '/' + part;
    c.append(el('span', { class: 'sep', text: '/' }), link(part, acc));
  }
  $('upBtn').disabled = state.cwd === '/';
  $('dropDir').textContent = state.cwd;
}

function sortedEntries() {
  const f = state.filter.toLowerCase();
  const { key, dir } = state.sort;
  const mul = dir === 'asc' ? 1 : -1;
  return state.entries
    .filter(e => !f || e.name.toLowerCase().includes(f))
    .sort((a, b) => {
      if (a.dir !== b.dir) return a.dir ? -1 : 1;           // folders first, always
      if (key === 'size' && a.size !== b.size) return (a.size - b.size) * mul;
      return a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' }) * mul;
    });
}

function systemOf(name) {
  const dot = name.lastIndexOf('.');
  return dot > 0 ? SYSTEMS[name.slice(dot + 1).toLowerCase()] || '' : '';
}

function renderRows() {
  const tbody = $('rows');
  const list = sortedEntries();
  const frag = document.createDocumentFragment();
  for (const e of list) {
    const chk = el('input', { type: 'checkbox', 'aria-label': `Select ${e.name}` });
    chk.checked = state.sel.has(e.path);
    chk.addEventListener('change', () => { chk.checked ? state.sel.add(e.path) : state.sel.delete(e.path); tr.classList.toggle('sel', chk.checked); updateSel(); });
    const nameLink = el('a', { href: '#', text: e.name, title: e.dir ? 'Open folder' : 'Download' });
    nameLink.addEventListener('click', ev => { ev.preventDefault(); e.dir ? navigate(e.path) : download([e]); });
    const sys = systemOf(e.name);
    const tr = el('tr', { class: state.sel.has(e.path) ? 'sel' : '' },
      el('td', { class: 'chk' }, chk),
      el('td', {}, el('div', { class: 'name' }, el('span', { class: 'ico', text: e.dir ? '📁' : '📄' }), nameLink)),
      el('td', { class: 'sys' }, sys ? el('span', { class: 'badge', text: sys }) : null),
      el('td', { class: 'num', text: e.dir ? '' : fmtSize(e.size) }),
      el('td', { class: 'rowact' },
        el('button', { text: 'Download', title: e.dir ? 'Download folder as .zip' : 'Download', onclick: () => download([e]) }),
        el('button', { text: 'Select', title: 'Select only this item (then Rename or Delete)', onclick: () => selectOnly(e) })));
    frag.append(tr);
  }
  tbody.replaceChildren(frag);
  $('empty').hidden = list.length > 0 || !$('loading').hidden;
  $('empty').textContent = state.entries.length ? 'No files match the filter.' : 'This folder is empty. Drop files here to upload them.';
  for (const th of document.querySelectorAll('th.sortable'))
    th.dataset.dir = th.dataset.sort === state.sort.key ? state.sort.dir : '';
  updateSel();
}

/** Row "⋯" button: make this the only selection so the toolbar's Rename / Delete act on it. */
function selectOnly(e) {
  state.sel.clear();
  state.sel.add(e.path);
  renderRows();
}

function selectedEntries() { return state.entries.filter(e => state.sel.has(e.path)); }

function updateSel() {
  const n = state.sel.size;
  $('selInfo').textContent = n ? `${n} selected` : '';
  $('dlSelBtn').disabled = !n;
  $('delSelBtn').disabled = !n;
  $('renameSelBtn').disabled = n !== 1;
  const visible = sortedEntries();
  $('selAll').checked = visible.length > 0 && visible.every(e => state.sel.has(e.path));
  $('selAll').indeterminate = n > 0 && !$('selAll').checked;
}

// ---------------------------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------------------------
async function makeFolder() {
  const name = await dialog({ title: 'New folder', text: `Create a folder in ${state.cwd}`, input: '', ok: 'Create' });
  if (name === null || !validName(name)) return;
  try {
    await state.link.mkdir(joinPath(state.cwd, name));
    await navigate(state.cwd);
    if (state.cwd === '/' || state.cwd === '/roms') refreshPlaces();
  } catch (e) { fail(e); }
}

async function renameSelected() {
  const [e] = selectedEntries();
  if (!e) return;
  const name = await dialog({ title: `Rename ${e.dir ? 'folder' : 'file'}`, input: e.name, ok: 'Rename' });
  if (name === null || name === e.name || !validName(name)) return;
  if (state.entries.some(x => x.name.toLowerCase() === name.toLowerCase() && x !== e))
    return toast(`${name} already exists here`, true);
  try {
    await state.link.rename(e.path, joinPath(state.cwd, name));
    await navigate(state.cwd);
  } catch (err) { fail(err); }
}

async function deleteSelected() {
  const items = selectedEntries();
  if (!items.length) return;
  const dirs = items.filter(e => e.dir).length;
  const what = items.length === 1 ? `"${items[0].name}"` : `${items.length} items`;
  const ok = await dialog({
    title: `Delete ${what}?`,
    text: (dirs ? 'Folders are deleted with everything inside them. ' : '') + 'This cannot be undone.',
    ok: 'Delete', danger: true,
  });
  if (!ok) return;
  setConnState('busy', 'Deleting…');
  let n = 0;
  try {
    for (const e of items) await state.link.removeTree(e.path, () => setConnState('busy', `Deleting… ${++n}`));
    toast(`Deleted ${what}`);
  } catch (e) { fail(e); }
  setConnState('on', 'Connected');
  await navigate(state.cwd);
  refreshCardInfo();
  refreshPlaces();
}

// ---- transfer queue ----
function addJob(job) {
  job.ctl = new AbortController();
  job.row = el('div', { class: 'xfer' },
    el('div', { class: 'what', text: job.label, title: job.label }),
    el('div', { class: 'stat', text: 'Waiting' }),
    el('button', { class: 'ghost', text: 'Cancel', onclick: () => { job.ctl.abort(); if (!job.started) finishJob(job, 'Cancelled', true); } }),
    el('div', { class: 'bar' }, el('div')));
  $('transfers').prepend(job.row);
  state.queue.push(job);
  showTab('transfers');
  updateXferCount();
  runQueue();
}

function progress(job, done, total) {
  const now = performance.now();
  job.t0 ??= now;
  const secs = (now - job.t0) / 1000;
  const rate = secs > 0.3 ? done / secs : 0;
  const pct = total ? (done / total) * 100 : 100;
  job.row.querySelector('.bar > div').style.width = pct.toFixed(1) + '%';
  job.row.querySelector('.stat').textContent = rate
    ? `${fmtSize(done)} / ${fmtSize(total)} · ${fmtSize(rate)}/s · ${fmtTime((total - done) / rate)} left`
    : `${fmtSize(done)} / ${fmtSize(total)}`;
}

function finishJob(job, text, isError = false) {
  job.finished = true;
  job.row.classList.add(isError ? 'err' : 'done');
  job.row.querySelector('.stat').textContent = text;
  job.row.querySelector('button').remove();
  if (!isError) job.row.querySelector('.bar > div').style.width = '100%';
  state.queue = state.queue.filter(j => j !== job);
  updateXferCount();
}

function updateXferCount() {
  const n = state.queue.length + (state.running ? 1 : 0);
  $('xferCount').textContent = n ? String(n) : '';
  $('speed').disabled = n > 0;
}

async function runQueue() {
  if (state.running) return;
  state.running = true;
  let touched = false;
  while (state.queue.length && state.link) {
    const job = state.queue.shift();
    if (job.finished) continue;
    job.started = true;
    state.current = job;
    updateXferCount();
    const t0 = performance.now();
    try {
      await job.run(job);
      const secs = (performance.now() - t0) / 1000;
      finishJob(job, `Done · ${fmtSize(job.bytes || 0)} in ${fmtTime(secs) || '0s'}`);
      touched ||= job.touches;
    } catch (e) {
      finishJob(job, e?.name === 'AbortError' ? 'Cancelled' : `Failed: ${e.message}`, true);
      if (e?.name !== 'AbortError') console.error(e);
    }
  }
  state.current = null;
  state.running = false;
  updateXferCount();
  if (touched && state.link) { navigate(state.cwd); refreshCardInfo(); }
}

/** files: [{file: File, rel: 'sub/dir/name.ext'}] uploaded under state.cwd */
async function queueUploads(files) {
  if (!state.link || !files.length) return;
  const base = state.cwd;
  const tops = new Set(files.map(f => f.rel.split('/')[0].toLowerCase()));
  const clash = state.entries.filter(e => tops.has(e.name.toLowerCase()));
  if (clash.length) {
    const names = clash.slice(0, 5).map(e => e.name).join('\n') + (clash.length > 5 ? `\n… and ${clash.length - 5} more` : '');
    if (!await dialog({ title: `Replace ${clash.length} existing item${clash.length > 1 ? 's' : ''}?`, text: names, ok: 'Replace' })) return;
  }
  const made = new Set();
  for (const { file, rel } of files) {
    const dest = joinPath(base, rel);
    addJob({
      label: `↑ ${dest}`,
      touches: true,
      run: async job => {
        const dir = parentPath(dest);
        if (dir !== '/' && !made.has(dir)) { await state.link.mkdirs(dir); made.add(dir); }
        const data = new Uint8Array(await file.arrayBuffer());
        job.bytes = data.length;
        await state.link.upload(dest, data, { signal: job.ctl.signal, onProgress: (d, t) => progress(job, d, t) });
      },
    });
  }
}

function saveBlob(blob, name) {
  const url = URL.createObjectURL(blob);
  const a = el('a', { href: url, download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

async function download(items) {
  if (!items.length) return;
  if (items.length === 1 && !items[0].dir) {
    const e = items[0];
    addJob({
      label: `↓ ${e.path}`,
      run: async job => {
        const data = await state.link.download(e.path, { signal: job.ctl.signal, onProgress: (d, t) => progress(job, d, t) });
        job.bytes = data.length;
        saveBlob(new Blob([data]), e.name);
      },
    });
    return;
  }
  // Several items or a folder: walk them, then save one .zip.
  const zipName = (items.length === 1 ? items[0].name : (state.cwd === '/' ? 'sdcard' : state.cwd.split('/').pop())) + '.zip';
  addJob({
    label: `↓ ${zipName}`,
    run: async job => {
      const files = [];
      const dirs = [];
      const walk = async (e, rel) => {
        if (job.ctl.signal.aborted) throw Object.assign(new Error('Cancelled'), { name: 'AbortError' });
        if (!e.dir) { files.push({ path: e.path, rel, size: e.size }); return; }
        dirs.push(rel);
        for (const c of await state.link.list(e.path)) if (!c.name.endsWith('.part')) await walk(c, `${rel}/${c.name}`);
      };
      job.row.querySelector('.stat').textContent = 'Listing…';
      for (const e of items) await walk(e, e.name);
      const total = files.reduce((n, f) => n + f.size, 0);
      let before = 0;
      const out = dirs.map(d => ({ name: d, dir: true }));
      for (const f of files) {
        const data = await state.link.download(f.path, { signal: job.ctl.signal, onProgress: d => progress(job, before + d, total) });
        out.push({ name: f.rel, data });
        before += data.length;
      }
      job.bytes = before;
      saveBlob(zipStore(out), zipName);
    },
  });
}

// ---- drag & drop (files and whole folders) ----
async function filesFromDrop(dt) {
  const out = [];
  const entries = [...dt.items].map(i => i.webkitGetAsEntry?.()).filter(Boolean);
  if (!entries.length) return [...dt.files].map(file => ({ file, rel: file.name }));
  const walk = async (entry, prefix) => {
    if (entry.isFile) {
      const file = await new Promise((res, rej) => entry.file(res, rej));
      out.push({ file, rel: prefix + entry.name });
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      for (;;) {
        const batch = await new Promise((res, rej) => reader.readEntries(res, rej));
        if (!batch.length) break;
        for (const c of batch) await walk(c, `${prefix}${entry.name}/`);
      }
    }
  };
  for (const e of entries) await walk(e, '');
  return out;
}

// ---------------------------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------------------------
function showTab(name) {
  for (const t of document.querySelectorAll('.tab')) t.classList.toggle('active', t.dataset.tab === name);
  $('transfers').hidden = name !== 'transfers';
  $('log').hidden = name !== 'log';
}

function init() {
  if (!('serial' in navigator)) $('unsupported').hidden = false;
  $('connectBtn').addEventListener('click', () => connect());
  $('connectBtn2').addEventListener('click', () => connect());
  $('rebootBtn').addEventListener('click', () => reboot());
  $('speed').addEventListener('change', async () => {
    if (!state.link) return;
    try { await applySpeed(Number($('speed').value)); } catch (e) { fail(e); await dropConnection(); }
  });
  $('upBtn').addEventListener('click', () => navigate(parentPath(state.cwd)));
  $('refreshBtn').addEventListener('click', () => { navigate(state.cwd); refreshCardInfo(); });
  $('mkdirBtn').addEventListener('click', makeFolder);
  $('renameSelBtn').addEventListener('click', renameSelected);
  $('delSelBtn').addEventListener('click', deleteSelected);
  $('dlSelBtn').addEventListener('click', () => download(selectedEntries()));
  $('uploadBtn').addEventListener('click', () => $('fileInput').click());
  $('uploadDirBtn').addEventListener('click', () => $('dirInput').click());
  $('fileInput').addEventListener('change', ev => {
    queueUploads([...ev.target.files].map(file => ({ file, rel: file.name })));
    ev.target.value = '';
  });
  $('dirInput').addEventListener('change', ev => {
    queueUploads([...ev.target.files].map(file => ({ file, rel: file.webkitRelativePath || file.name })));
    ev.target.value = '';
  });
  $('filter').addEventListener('input', ev => { state.filter = ev.target.value; renderRows(); });
  $('selAll').addEventListener('change', ev => {
    for (const e of sortedEntries()) ev.target.checked ? state.sel.add(e.path) : state.sel.delete(e.path);
    renderRows();
  });
  for (const th of document.querySelectorAll('th.sortable')) {
    th.addEventListener('click', () => {
      const key = th.dataset.sort;
      state.sort = { key, dir: state.sort.key === key && state.sort.dir === 'asc' ? 'desc' : 'asc' };
      renderRows();
    });
  }
  for (const t of document.querySelectorAll('.tab')) t.addEventListener('click', () => showTab(t.dataset.tab));
  // Enter in the prompt must confirm; the form's implicit submit would pick Cancel (first button).
  $('dlgInput').addEventListener('keydown', ev => {
    if (ev.key === 'Enter') { ev.preventDefault(); $('dlg').close('ok'); }
  });
  $('clearBtn').addEventListener('click', () => {
    if (!$('log').hidden) $('log').textContent = '';
    else for (const r of [...$('transfers').children]) if (r.classList.contains('done') || r.classList.contains('err')) r.remove();
  });

  const wrap = $('listWrap');
  let depth = 0;
  wrap.addEventListener('dragenter', ev => { if (!state.link) return; ev.preventDefault(); depth++; $('dropHint').hidden = false; });
  wrap.addEventListener('dragover', ev => { if (state.link) ev.preventDefault(); });
  wrap.addEventListener('dragleave', () => { if (--depth <= 0) { depth = 0; $('dropHint').hidden = true; } });
  wrap.addEventListener('drop', async ev => {
    ev.preventDefault();
    depth = 0;
    $('dropHint').hidden = true;
    if (!state.link) return;
    try { queueUploads(await filesFromDrop(ev.dataTransfer)); } catch (e) { fail(e); }
  });

  document.addEventListener('keydown', ev => {
    if (!state.link || ev.target.closest('input[type=text], input[type=search], dialog')) return;
    if (ev.key === 'Delete' && state.sel.size) deleteSelected();
    else if (ev.key === 'F2' && state.sel.size === 1) renameSelected();
    else if (ev.key === 'Backspace' && state.cwd !== '/') navigate(parentPath(state.cwd));
    else if (ev.key === 'F5') { ev.preventDefault(); navigate(state.cwd); }
  });

  navigator.serial?.addEventListener('disconnect', ev => {
    if (state.conn && ev.target === state.conn.port) {
      dropConnection();
      toast('Board disconnected', true);
    }
  });
  window.addEventListener('beforeunload', ev => { if (state.running) ev.preventDefault(); });

  // Production build only: in `vite dev` a service worker would fight hot reload.
  if (import.meta.env.PROD && 'serviceWorker' in navigator && location.protocol.startsWith('http'))
    navigator.serviceWorker.register('sw.js').catch(() => {});
}

init();
