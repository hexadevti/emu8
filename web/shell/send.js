// emu8 — desktop side of "send from PC". Joins the phone's room (code shown in the phone's Files
// dialog) through the /api/relay worker and streams files to it, one at a time:
//   {"t":"file",name,size,folder}  then binary chunks  then {"t":"end"}
// The phone answers {"t":"saved",name,path} or {"t":"error",name,msg} for each file.
'use strict';

const $ = (s) => document.querySelector(s);
const FOLDERS = ['roms', 'apple2', 'c64', 'nes', 'atari2600', 'msx', 'msx2', 'sms', 'coleco', 'zx', 'pcxt'];
const CHUNK = 256 * 1024, HIGH_WATER = 4 * 1024 * 1024, MAX_SIZE = 64 * 1024 * 1024;

let ws = null, peerOn = false, busy = false, waiter = null;
const queue = [];

const codeIn = $('#code'), stateEl = $('#state'), drop = $('#drop'), pick = $('#pick'), filesIn = $('#files');
const folderSel = $('#folder');

function fillFolders() {
  const cur = folderSel.value || 'auto';
  folderSel.innerHTML = '';
  for (const f of ['auto', ...FOLDERS]) {
    const o = document.createElement('option'); o.value = f; o.textContent = f === 'auto' ? t('send.auto') : f;
    folderSel.appendChild(o);
  }
  folderSel.value = cur;
}
fillFolders();

function setState(key, cls) { stateEl.textContent = key ? t(key) : ''; stateEl.className = cls || ''; }
function setReady(on) {
  peerOn = on;
  drop.setAttribute('aria-disabled', on ? 'false' : 'true');
  pick.disabled = !on;
  if (on) pump();
}

codeIn.addEventListener('input', () => { codeIn.value = codeIn.value.toUpperCase().replace(/[^A-Z0-9]/g, ''); });
codeIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') connect(); });
$('#connect').addEventListener('click', connect);

function connect() {
  const code = codeIn.value.trim().toUpperCase();
  if (!/^[A-HJ-NP-Z2-9]{5}$/.test(code)) { setState('send.badCode', 'off'); codeIn.focus(); return; }
  if (ws) { ws.onclose = null; ws.close(); }
  setReady(false);
  setState('send.connecting');
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/api/relay/${code}?role=send`);
  ws.binaryType = 'arraybuffer';
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    let m; try { m = JSON.parse(e.data); } catch (err) { return; }
    if (m.t === 'peer') {
      setState(m.on ? 'send.connected' : 'send.noPhone', m.on ? 'on' : 'off');
      setReady(m.on);
      if (!m.on && waiter) waiter({ t: 'error', msg: t('send.phoneGone') });
    } else if ((m.t === 'saved' || m.t === 'error') && waiter) waiter(m);
  };
  ws.onclose = () => {
    setState('send.closed', 'off'); setReady(false); ws = null;
    if (waiter) waiter({ t: 'error', msg: t('send.closed') });
  };
  try { history.replaceState(null, '', '?c=' + code); } catch (e) {}
}

// ---- file picking ----
pick.addEventListener('click', () => filesIn.click());
filesIn.addEventListener('change', () => { enqueue([...filesIn.files]); filesIn.value = ''; });
for (const ev of ['dragenter', 'dragover']) document.addEventListener(ev, (e) => {
  e.preventDefault();
  if (peerOn) drop.classList.add('over');
});
for (const ev of ['dragleave', 'drop']) document.addEventListener(ev, (e) => {
  e.preventDefault();
  if (ev === 'dragleave' && e.relatedTarget) return;
  drop.classList.remove('over');
  if (ev === 'drop' && peerOn) enqueue([...e.dataTransfer.files]);
});

function enqueue(files) {
  for (const f of files) {
    const li = document.createElement('li');
    const name = document.createElement('span'); name.textContent = f.name;
    const info = document.createElement('small'); info.textContent = t('send.queued');
    const bar = document.createElement('div'); bar.className = 'bar'; bar.appendChild(document.createElement('i'));
    li.append(name, info, bar);
    $('#list').prepend(li);
    if (f.size > MAX_SIZE) { li.className = 'err'; info.textContent = t('send.tooBig'); continue; }
    queue.push({ file: f, folder: folderSel.value, li, info, fill: bar.firstChild });
  }
  pump();
}

async function pump() {
  if (busy) return;
  busy = true;
  try {
    while (queue.length && peerOn && ws && ws.readyState === 1) {
      try { await sendOne(queue[0]); queue.shift(); } catch (e) { break; }
    }
  } finally { busy = false; }
}

const pause = (ms) => new Promise((r) => setTimeout(r, ms));

async function sendOne(job) {
  const { file, li, info, fill } = job;
  info.textContent = t('send.sending');
  const done = new Promise((res) => { waiter = res; });
  try {
    ws.send(JSON.stringify({ t: 'file', name: file.name, size: file.size, folder: job.folder }));
    for (let off = 0; off < file.size; off += CHUNK) {
      while (ws && ws.bufferedAmount > HIGH_WATER) await pause(20);
      if (!ws || ws.readyState !== 1) break;
      ws.send(await file.slice(off, off + CHUNK).arrayBuffer());
      fill.style.width = Math.round(100 * Math.min(file.size, off + CHUNK) / file.size) + '%';
    }
    if (ws && ws.readyState === 1) ws.send('{"t":"end"}');
  } catch (e) { waiter && waiter({ t: 'error', msg: String(e) }); }
  const r = await done;
  waiter = null;
  fill.style.width = '100%';
  if (r.t === 'saved') { li.className = 'ok'; info.textContent = '→ ' + r.path; }
  else {
    li.className = 'err'; info.textContent = r.msg || t('send.failed');
    if (!peerOn) throw new Error('peer gone');  // keep it queued; the loop stops until the phone is back
  }
}

document.addEventListener('emu8-lang', fillFolders);
const pre = new URLSearchParams(location.search).get('c');
if (pre) { codeIn.value = pre.toUpperCase(); connect(); } else codeIn.focus();
