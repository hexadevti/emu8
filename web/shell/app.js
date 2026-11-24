// emu8 — browser shell. Owns the page: layout (portrait / landscape side bars / landscape overlay),
// canvas presentation, on-screen pad + keyboard, physical keyboard + Gamepad API, file uploads into
// the IndexedDB-backed /sdcard, and audio (AudioWorklet reading the wasm ring through the SAB).
'use strict';

const $ = (s) => document.querySelector(s);
const app = $('#app'), canvas = $('#screen'), ctx = canvas.getContext('2d', { alpha: false });
const bootMsg = $('#boot-msg');
let M = null;                 // the Emscripten module once running

const FOLDERS = ['roms', 'apple2', 'c64', 'nes', 'atari2600', 'msx', 'msx2', 'sms', 'coleco', 'zx', 'pcxt'];
const PLATFORM = { APPLE2: 0, C64: 1, NES: 2, ATARI: 3, MSX: 5, SMS: 6, PCXT: 7, COLECO: 9, ZX: 10 };

if (!self.crossOriginIsolated) {
  bootMsg.removeAttribute('data-i18n');
  bootMsg.textContent = t('boot.noSAB');
}

// ======================= persistence (IDBFS) =======================
let syncBusy = false, syncAgain = false, syncTimer = 0;
function persist(cb) {
  if (!M) return cb && cb();
  if (syncBusy) { syncAgain = true; if (cb) setTimeout(() => persist(cb), 200); return; }
  syncBusy = true;
  M.FS.syncfs(false, (err) => {
    syncBusy = false;
    if (err) console.warn('syncfs', err);
    if (syncAgain) { syncAgain = false; schedulePersist(); }
    cb && cb();
  });
}
function schedulePersist() { clearTimeout(syncTimer); syncTimer = setTimeout(() => persist(), 400); }

// ======================= module =======================
const moduleCfg = {
  print: (t) => console.log(t),
  printErr: (t) => console.warn(t),
  webPersist: () => schedulePersist(),
  webReboot: () => persist(() => { try { sessionStorage.setItem('emu8-reboot', '1'); } catch (e) {} location.reload(); }),
  preRun: [(mod) => {
    const FS = mod.FS, IDBFS = mod.IDBFS || (FS.filesystems && FS.filesystems.IDBFS);
    for (const d of ['/sdcard', '/persist']) { try { FS.mkdir(d); } catch (e) {} FS.mount(IDBFS, {}, d); }
    mod.addRunDependency('idbfs');
    FS.syncfs(true, (err) => {
      if (err) console.warn('syncfs load', err);
      for (const f of FOLDERS) { try { FS.mkdir('/sdcard/' + f); } catch (e) {} }
      if (/^(localhost|127\.0\.0\.1)$/.test(location.hostname)) devPreload(FS).finally(() => mod.removeRunDependency('idbfs'));
      else mod.removeRunDependency('idbfs');
    });
  }],
};

// Local test server only (web/serve.py): copy the repo's sdcard/ (ROMs + games) into /sdcard.
async function devPreload(FS) {
  let list;
  try { const r = await fetch('/__sdcard_list'); if (!r.ok) return; list = await r.json(); } catch (e) { return; }
  let n = 0;
  for (const f of list) {
    const dst = '/sdcard/' + f.path;
    try { if (FS.stat(dst).size === f.size) continue; } catch (e) {}
    bootMsg.textContent = t('boot.copying', f.path);
    const parts = dst.split('/'); let d = '';
    for (let i = 1; i < parts.length - 1; i++) { d += '/' + parts[i]; try { FS.mkdir(d); } catch (e) {} }
    const r = await fetch('/__sdcard/' + f.path.split('/').map(encodeURIComponent).join('/'));
    if (r.ok) { FS.writeFile(dst, new Uint8Array(await r.arrayBuffer())); n++; }
  }
  if (n) await new Promise((res) => FS.syncfs(false, () => res()));
  console.log('devPreload: ' + n + ' file(s) copied from the local sdcard');
}

createEmu8(moduleCfg).then((mod) => {
  M = mod;
  // no "tap to start" screen: go straight to the emulator; audio starts on the first touch / key
  // (startAudio listeners). A reboot (system change, reset) skips the file picker too.
  let again = false;
  try { again = sessionStorage.getItem('emu8-reboot') === '1'; sessionStorage.removeItem('emu8-reboot'); } catch (e) {}
  $('#boot').hidden = true;
  if (!again && listDir('/sdcard/roms').length === 0) openFiles();
  requestAnimationFrame(frame);
  setInterval(() => { if (!document.hidden) persist(); }, 15000);   // emulator writes (SAVE, disks)
}).catch((e) => { bootMsg.removeAttribute('data-i18n'); bootMsg.textContent = t('boot.fail', e); });

// Hidden (tab switched, app minimised, phone locked): pause the whole emulator and the audio so the
// phone isn't kept busy (and hot) in the background; it carries on where it was when shown again.
function setHidden(h) {
  if (M && M._web_hidden) M._web_hidden(h ? 1 : 0);
  if (actx) { if (h) actx.suspend().catch(() => {}); else actx.resume().catch(() => {}); }
}
document.addEventListener('visibilitychange', () => { if (document.hidden) persist(); setHidden(document.hidden); });
window.addEventListener('pageshow', () => setHidden(document.hidden));
window.addEventListener('pagehide', () => { persist(); setHidden(true); });

// ======================= video =======================
let lastFrame = -1, img = null, viewBuf = null;
const shot = document.createElement('canvas');
let wasMenu = false, shotShown = false;
let curW = 0, curH = 0, stretch = false;
function frame() {
  requestAnimationFrame(frame);
  pollGamepads();
  if (!M) return;
  const no = M._web_frame_no();
  if (no === lastFrame) return;
  lastFrame = no;
  // Settings sheet: behind it stays the game's last picture, not the classic menu being drawn there
  const menu = !!M._web_menu_open();
  if (menu && !wasMenu && canvas.width) {              // the canvas still holds the game: keep a copy
    shot.width = canvas.width; shot.height = canvas.height; shot.getContext('2d').drawImage(canvas, 0, 0);
  }
  wasMenu = menu;
  app.classList.toggle('emu-menu', menu);
  if (menu && !msClassic && (msPage === 'main' || msPage === 'closed')) {
    if (!shotShown && shot.width) { canvas.width = shot.width; canvas.height = shot.height; ctx.drawImage(shot, 0, 0); }
    shotShown = true;
    return;
  }
  shotShown = false;
  const fw = M._web_fb_w(), fh = M._web_fb_h(), ptr = M._web_frame_ptr();
  if (!ptr || !fw || !fh) return;
  if (!viewBuf) viewBuf = M._malloc(20);
  M._web_view_rect(viewBuf);
  const v = M.HEAP32.subarray(viewBuf >> 2, (viewBuf >> 2) + 5);
  const [vx, vy, vw, vh, st] = v;
  if (!img || img.width !== fw || img.height !== fh) img = new ImageData(fw, fh);
  img.data.set(M.HEAPU8.subarray(ptr, ptr + fw * fh * 4));
  if (canvas.width !== vw || canvas.height !== vh) { canvas.width = vw; canvas.height = vh; }
  if (!!st !== stretch) { stretch = !!st; canvas.classList.toggle('stretch', stretch); layout(); }
  if (vw !== curW || vh !== curH) { curW = vw; curH = vh; }
  ctx.putImageData(img, -vx, -vy, vx, vy, vw, vh);
  viewRect = [vx, vy, vw, vh];
}
let viewRect = [0, 0, 320, 240];

// ======================= layout =======================
let kbdOn = false, padHidden = false;
function layout() {
  const W = window.innerWidth, H = window.innerHeight;
  app.classList.remove('portrait', 'side', 'overlay');
  if (H >= W * 0.9) {
    app.classList.add('portrait');
    const pad = Math.max(110, Math.min(170, (H - W * 0.75) * 0.5, W * 0.42));
    app.style.setProperty('--dpad', pad + 'px');
    app.style.setProperty('--ab', pad + 'px');
    app.style.setProperty('--kh', Math.max(30, Math.min(48, (H - W * 0.75 - 60) / 6.5)) + 'px');
  } else {
    // image fitted to the full height (4:3); is the space beside it enough for the pad?
    const imgW = H * 4 / 3;
    const side = (W - imgW) / 2;
    if (side >= 130) {
      app.classList.add('side');
      app.style.setProperty('--sidew', side + 'px');
      const pad = Math.min(side - 16, H * 0.42, 170);
      app.style.setProperty('--dpad', pad + 'px');
      app.style.setProperty('--ab', pad + 'px');
    } else {
      app.classList.add('overlay');
      app.style.removeProperty('--sidew');
      const pad = Math.max(110, Math.min(160, H * 0.38));
      app.style.setProperty('--dpad', pad + 'px');
      app.style.setProperty('--ab', pad + 'px');
    }
    app.style.setProperty('--kh', Math.max(26, Math.min(40, H / 11)) + 'px');
  }
  app.classList.toggle('kbd-on', kbdOn);
  app.classList.toggle('pad-hidden', padHidden);
}
window.addEventListener('resize', layout);
window.addEventListener('orientationchange', () => setTimeout(layout, 200));
layout();

// ======================= input: keys =======================
function key(hid, down) { if (M) M._web_key(hid, down ? 1 : 0); }
function tapKey(hid, mods = []) {
  mods.forEach((m) => key(m, true));
  key(hid, true);
  setTimeout(() => { key(hid, false); mods.forEach((m) => key(m, false)); }, 60);
}

const CODE2HID = {};
'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('').forEach((c, i) => { CODE2HID['Key' + c] = 0x04 + i; });
'1234567890'.split('').forEach((c, i) => { CODE2HID['Digit' + c] = 0x1e + i; });
Object.assign(CODE2HID, {
  Enter: 0x28, Escape: 0x29, Backspace: 0x2a, Tab: 0x2b, Space: 0x2c, Minus: 0x2d, Equal: 0x2e,
  BracketLeft: 0x2f, BracketRight: 0x30, Backslash: 0x31, Semicolon: 0x33, Quote: 0x34, Backquote: 0x35,
  Comma: 0x36, Period: 0x37, Slash: 0x38, CapsLock: 0x39, IntlBackslash: 0x64, IntlRo: 0x87,
  F1: 0x3a, F2: 0x3b, F3: 0x3c, F4: 0x3d, F5: 0x3e, F6: 0x3f, F7: 0x40, F8: 0x41, F9: 0x42, F10: 0x43, F11: 0x44, F12: 0x45,
  Insert: 0x49, Home: 0x4a, PageUp: 0x4b, Delete: 0x4c, End: 0x4d, PageDown: 0x4e,
  ArrowRight: 0x4f, ArrowLeft: 0x50, ArrowDown: 0x51, ArrowUp: 0x52, NumpadEnter: 0x58,
  ControlLeft: 0xe0, ShiftLeft: 0xe1, AltLeft: 0xe2, MetaLeft: 0xe3, ControlRight: 0xe4, ShiftRight: 0xe5, AltRight: 0xe6,
});
const dialogOpen = () => !$('#files').hidden || !$('#boot').hidden;
// The cores' key maps assume a US keyboard (HID position + Shift). Symbols are mapped by the
// CHARACTER typed (e.key), so ABNT2 / other layouts and soft keyboards that send no separate Shift
// event still give the right character: the needed Shift state is forced around the key.
const US_PLAIN = "1234567890-=[]\\;',./`", US_SHIFT = '!@#$%^&*()_+{}|:"<>?~';
const US_HID = [0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33, 0x34, 0x36, 0x37, 0x38, 0x35];
function charToHid(ch) {
  let i = US_PLAIN.indexOf(ch); if (i >= 0) return [US_HID[i], false];
  i = US_SHIFT.indexOf(ch); if (i >= 0) return [US_HID[i], true];
  return null;
}
let physShift = false;
const downByCode = new Map();          // e.code -> [hid, forcedShift(-1/0/1)]
window.addEventListener('keydown', (e) => {
  if (dialogOpen()) return;
  if (e.code === 'ShiftLeft' || e.code === 'ShiftRight') physShift = true;
  let h = CODE2HID[e.code], force = -1;
  const mapped = (e.key && e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) ? charToHid(e.key) : null;
  if (mapped) { h = mapped[0]; if (mapped[1] !== physShift) force = mapped[1] ? 1 : 0; }
  if (h === undefined) return;
  e.preventDefault();
  if (e.repeat) return;
  if (force === 1) key(0xe1, true);
  if (force === 0) { key(0xe1, false); key(0xe5, false); }
  key(h, true);
  downByCode.set(e.code, [h, force]);
});
window.addEventListener('keyup', (e) => {
  if (e.code === 'ShiftLeft' || e.code === 'ShiftRight') physShift = false;
  const d = downByCode.get(e.code);
  const h = d ? d[0] : CODE2HID[e.code];
  if (h === undefined) return;
  e.preventDefault();
  downByCode.delete(e.code);
  key(h, false);
  if (d && d[1] === 1 && !physShift) key(0xe1, false);
  if (d && d[1] === 0 && physShift) key(0xe1, true);
});
window.addEventListener('blur', () => { for (const c in CODE2HID) key(CODE2HID[c], false); });

// ======================= input: on-screen pad =======================
let touchPad = 0, gamePad = 0, lastSentPad = -1;
function sendPad() {
  const m = touchPad | gamePad;
  if (m === lastSentPad || !M) return;
  lastSentPad = m;
  M._web_pad(m);
}

// Floating controls, like phone games: each side of the pad is a touch zone (#pad-left/#pad-right).
// The d-pad (or the Apple stick) is centred where the thumb lands and follows it when it slides past
// the rim; A/B move so the button nearest the first touch sits under the thumb, and later touches pick
// the nearest button (between the two = both). Everything goes home when the fingers lift.
const dpad = $('#dpad'), zoneL = $('#pad-left'), zoneR = $('#pad-right'), abtns = $('#abtns');
function restCenter(el) {         // centre without our translate (also mid-transition: computed style is live)
  const r = el.getBoundingClientRect(), m = new DOMMatrix(getComputedStyle(el).transform);
  return [r.left + r.width / 2 - m.e, r.top + r.height / 2 - m.f];
}
let dc = null;                    // the d-pad's centre while a finger holds it
function dpadPlace(x, y) {
  const [rx, ry] = restCenter(dpad);
  dc = [x, y]; dpad.classList.add('held');
  dpad.style.transform = `translate(${x - rx}px, ${y - ry}px)`;
}
function dpadFollow(x, y, R) {    // dragged past the rim: the pad comes along, the direction holds
  const dx = x - dc[0], dy = y - dc[1], d = Math.hypot(dx, dy);
  if (d > R) dpadPlace(x - dx * R / d, y - dy * R / d);
}
function dpadHome() { dc = null; dpad.classList.remove('held'); dpad.style.transform = ''; }

let dpId = null;
function dpadMask(e) {
  const R = dpad.offsetWidth / 2;
  if (!dc) dpadPlace(e.clientX, e.clientY);
  dpadFollow(e.clientX, e.clientY, R);
  const dx = e.clientX - dc[0], dy = e.clientY - dc[1];
  if (Math.hypot(dx, dy) < R * 0.24) return 0;
  const a = Math.atan2(dy, dx) * 180 / Math.PI;        // -180..180, 0 = right, 90 = down
  let m = 0;
  if (a > -67.5 && a < 67.5) m |= 8;                     // right
  if (a > 112.5 || a < -112.5) m |= 4;                   // left
  if (a > 22.5 && a < 157.5) m |= 2;                     // down
  if (a < -22.5 && a > -157.5) m |= 1;                   // up
  return m;
}
// Apple II+/IIe: the joystick is analog (two paddles) with two buttons, no SELECT/START. The pad
// becomes a stick: the knob follows the finger and the paddles follow it proportionally.
let appleStick = false, stickPtr = -1, stickTouch = null, padStick = [0, 0, 0], lastStick = '';
const knob = document.createElement('div'); knob.className = 'knob'; dpad.appendChild(knob);
function sendStick(x, y, on) {
  const k = x + ',' + y + ',' + on;
  if (k === lastStick || !M || !M._web_stick) return;
  lastStick = k; M._web_stick(x, y, on);
}
function stickMove(e) {
  const R = dpad.offsetWidth * 0.38;
  if (!dc) dpadPlace(e.clientX, e.clientY);
  dpadFollow(e.clientX, e.clientY, R);
  let dx = e.clientX - dc[0], dy = e.clientY - dc[1];
  const d = Math.hypot(dx, dy); if (d > R) { dx *= R / d; dy *= R / d; }
  knob.style.transform = `translate(${dx}px, ${dy}px)`;
  // circle -> square: on the rim's diagonal both paddles reach their ends, like the real stick
  let nx = dx / R, ny = dy / R; const mx = Math.max(Math.abs(nx), Math.abs(ny));
  if (mx > 0) { const sc = Math.hypot(nx, ny) / mx; nx *= sc; ny *= sc; }
  padStick = [Math.round(Math.max(-1, Math.min(1, nx)) * 32767), Math.round(Math.max(-1, Math.min(1, ny)) * 32767), 1];
  sendStick(...padStick);
}
function stickEnd() {
  stickPtr = -1; stickTouch = null; knob.style.transform = ''; padStick = [0, 0, 0]; sendStick(0, 0, 0);
  if (dpId === null) dpadHome();
}
function dpadUpdate(m) {
  touchPad = (touchPad & ~0x0f) | m;
  dpad.querySelector('.up').classList.toggle('on', !!(m & 1));
  dpad.querySelector('.down').classList.toggle('on', !!(m & 2));
  dpad.querySelector('.left').classList.toggle('on', !!(m & 4));
  dpad.querySelector('.right').classList.toggle('on', !!(m & 8));
  sendPad();
}
zoneL.addEventListener('pointerdown', (e) => {
  e.preventDefault();
  if (appleStick) {
    if (e.pointerType !== 'touch' && stickPtr < 0) {
      try { zoneL.setPointerCapture(e.pointerId); } catch (_) {}
      stickPtr = e.pointerId; dpadPlace(e.clientX, e.clientY); stickMove(e);
    }
    return;
  }
  if (dpId !== null) return;                             // one thumb steers; others are ignored
  try { zoneL.setPointerCapture(e.pointerId); } catch (_) {}
  dpId = e.pointerId; dpadPlace(e.clientX, e.clientY); dpadUpdate(0);
});
zoneL.addEventListener('pointermove', (e) => {
  if (!appleStick && e.pointerId === dpId) dpadUpdate(dpadMask(e));   // the stick follows its finger on window
});
for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) zoneL.addEventListener(ev, (e) => {
  if (e.pointerId === dpId) { dpId = null; dpadUpdate(0); dpadHome(); }
});
// The stick's finger is tracked on the window: it keeps steering when it slides off the pad or the
// browser drops the pointer capture, and only lifting it (or a cancel) lets the stick spring back.
window.addEventListener('pointermove', (e) => { if (appleStick && e.pointerId === stickPtr) stickMove(e); }, true);
for (const ev of ['pointerup', 'pointercancel']) window.addEventListener(ev, (e) => { if (e.pointerId === stickPtr) stickEnd(); }, true);
// A finger runs the stick through touch events instead: holding it still is a long press to the phone,
// and its long-press gesture (callout, loupe, selection) cancels the pointer under a held stick. The
// touch is the browser's to keep only if touchstart isn't prevented: preventing it keeps it ours, and
// the touch keeps reporting moves even outside the pad until the finger lifts.

const findTouch = (list) => { for (const t of list) if (t.identifier === stickTouch) return t; return null; };
zoneL.addEventListener('touchstart', (e) => {
  e.preventDefault();
  if (!appleStick || stickTouch !== null) return;
  const t = e.changedTouches[0]; stickTouch = t.identifier; stickPtr = 1e9; dpadPlace(t.clientX, t.clientY); stickMove(t);
}, { passive: false });
zoneL.addEventListener('touchmove', (e) => {
  e.preventDefault();
  const t = stickTouch !== null && findTouch(e.changedTouches); if (t && appleStick) stickMove(t);
}, { passive: false });
for (const ev of ['touchend', 'touchcancel']) zoneL.addEventListener(ev, (e) => {
  if (stickTouch !== null && findTouch(e.changedTouches)) { stickTouch = null; stickEnd(); }
});
// the pad's buttons are held too: no long-press gestures, zoom or callout anywhere on the controls
$('#controls').addEventListener('touchstart', (e) => e.preventDefault(), { passive: false });
$('#controls').addEventListener('contextmenu', (e) => e.preventDefault());
function setAppleStick(on) {
  if (on === appleStick) return;
  appleStick = on; app.classList.toggle('apple-pad', on);
  dpId = null; dpadUpdate(0); stickEnd(); dpadHome();
}

// ---- A/B ----
const abBtns = [...abtns.querySelectorAll('.ab')].map((el) => ({ el, bit: +el.dataset.bit }));
const AB_BITS = abBtns.reduce((m, b) => m | b.bit, 0);
const abPtr = new Map();
function abCenters(rest) {
  const m = rest ? new DOMMatrix(getComputedStyle(abtns).transform) : { e: 0, f: 0 };
  return abBtns.map(({ el }) => { const r = el.getBoundingClientRect(); return [r.left + r.width / 2 - m.e, r.top + r.height / 2 - m.f]; });
}
function abMask(x, y) {
  const d = abCenters(false).map(([cx, cy]) => Math.hypot(x - cx, y - cy));
  const order = d.map((v, i) => i).sort((i, j) => d[i] - d[j]);
  const size = abBtns[0].el.offsetWidth;
  // on the line between two buttons (and close to them): both, for run + jump
  if (order.length > 1 && d[order[1]] - d[order[0]] < size * 0.22 && d[order[1]] < size * 1.1)
    return abBtns[order[0]].bit | abBtns[order[1]].bit;
  return abBtns[order[0]].bit;
}
function abAnchor(x, y) {         // the button nearest the first touch lands under the thumb
  const c = abCenters(true);
  let k = 0, best = Infinity;
  c.forEach(([cx, cy], i) => { const d = Math.hypot(x - cx, y - cy); if (d < best) { best = d; k = i; } });
  abtns.classList.add('held');
  abtns.style.transform = `translate(${x - c[k][0]}px, ${y - c[k][1]}px)`;
}
function abUpdate() {
  let m = 0; for (const v of abPtr.values()) m |= v;
  for (const b of abBtns) b.el.classList.toggle('on', !!(m & b.bit));
  touchPad = (touchPad & ~AB_BITS) | m;
  sendPad();
}
zoneR.addEventListener('pointerdown', (e) => {
  e.preventDefault();
  try { zoneR.setPointerCapture(e.pointerId); } catch (_) {}
  if (!abPtr.size) abAnchor(e.clientX, e.clientY);
  abPtr.set(e.pointerId, abMask(e.clientX, e.clientY)); abUpdate();
});
zoneR.addEventListener('pointermove', (e) => {
  if (abPtr.has(e.pointerId)) { abPtr.set(e.pointerId, abMask(e.clientX, e.clientY)); abUpdate(); }
});
for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) zoneR.addEventListener(ev, (e) => {
  if (!abPtr.delete(e.pointerId)) return;
  if (!abPtr.size) { abtns.classList.remove('held'); abtns.style.transform = ''; }
  abUpdate();
});

document.querySelectorAll('[data-bit]:not(.ab)').forEach((b) => {
  const bit = +b.dataset.bit;
  const ptrs = new Set();
  const upd = () => { b.classList.toggle('on', ptrs.size > 0); touchPad = ptrs.size ? (touchPad | bit) : (touchPad & ~bit); sendPad(); };
  b.addEventListener('pointerdown', (e) => { e.preventDefault(); b.setPointerCapture(e.pointerId); ptrs.add(e.pointerId); upd(); });
  for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) b.addEventListener(ev, (e) => { ptrs.delete(e.pointerId); upd(); });
  b.addEventListener('contextmenu', (e) => e.preventDefault());
});

// ======================= input: Gamepad API =======================
function pollGamepads() {
  if (!navigator.getGamepads) return;
  let m = 0, gs = null;
  for (const gp of navigator.getGamepads()) {
    if (!gp) continue;
    const b = (i) => gp.buttons[i] && gp.buttons[i].pressed;
    const ax = gp.axes[0] || 0, ay = gp.axes[1] || 0;
    // Apple: the analog stick drives the paddles proportionally (the D-pad buttons stay digital)
    if (appleStick) { if (Math.hypot(ax, ay) > 0.1) gs = [Math.round(ax * 32767), Math.round(ay * 32767), 1]; }
    else {
      if (ay < -0.5) m |= 1;
      if (ay > 0.5) m |= 2;
      if (ax < -0.5) m |= 4;
      if (ax > 0.5) m |= 8;
    }
    if (b(12)) m |= 1;
    if (b(13)) m |= 2;
    if (b(14)) m |= 4;
    if (b(15)) m |= 8;
    if (b(0)) m |= 16;
    if (b(1) || b(2)) m |= 32;
    if (b(8)) m |= 64;
    if (b(9)) m |= 128;
  }
  if (m !== gamePad) { gamePad = m; sendPad(); }
  if (appleStick && stickPtr < 0) sendStick(...(gs || [0, 0, 0]));
}

// ======================= input: touch on the screen =======================
const scrPtr = new Set();
function fbPoint(e) {
  // canvas content box (object-fit contain unless stretched)
  const r = canvas.getBoundingClientRect();
  const [vx, vy, vw, vh] = viewRect;
  let cw = r.width, ch = r.height, ox = 0, oy = 0;
  if (!stretch) {
    const s = Math.min(r.width / vw, r.height / vh);
    cw = vw * s; ch = vh * s; ox = (r.width - cw) / 2; oy = (r.height - ch) / 2;
  }
  const x = Math.floor(vx + (e.clientX - r.left - ox) * vw / cw);
  const y = Math.floor(vy + (e.clientY - r.top - oy) * vh / ch);
  return [x, y, x >= vx && y >= vy && x < vx + vw && y < vy + vh];
}
canvas.addEventListener('pointerdown', (e) => {
  e.preventDefault();
  const [x, y, inside] = fbPoint(e);
  if (!inside || !M) return;
  canvas.setPointerCapture(e.pointerId);
  scrPtr.add(e.pointerId);
  M._web_touch(x, y, 1);
});
canvas.addEventListener('pointermove', (e) => {
  if (!scrPtr.has(e.pointerId) || !M) return;
  const [x, y] = fbPoint(e);
  M._web_touch(x, y, 1);
});
for (const ev of ['pointerup', 'pointercancel']) canvas.addEventListener(ev, (e) => {
  if (!scrPtr.delete(e.pointerId) || !M) return;
  M._web_touch(-1, -1, 0);
});
canvas.addEventListener('contextmenu', (e) => e.preventDefault());

// ======================= on-screen keyboard =======================
// One layout per machine, drawn like its real keyboard. Every key ends up as USB-HID codes that the
// core's own host-key map (src/shared/*keymap.cpp) turns into the machine key, exactly like a USB
// keyboard on the device. Kinds of key:
//   H(label, hid, w, opt)  a fixed HID code; opt.sh = the HID sent instead while SHIFT is latched,
//                          opt.red / opt.top = small legend drawn on the key (ZX symbols, f2..)
//   C(ch, sc, label, w)    a CHARACTER: typed through the US-PC keycap ("*" = Shift-8) with the SHIFT
//                          state it needs; sc = the character while SHIFT is latched (C64 Shift-2 = ")
//   M(label, hid, w)       a modifier that latches until the next key (SHIFT, CTRL, C=, GRAPH, ...)
const H = (l, h, w = 1, o = {}) => ({ l, h, w, ...o });
const C = (c, sc, l, w = 1) => ({ l: l || c, c, sc, top: sc, w });
const MK = (l, h, w = 1, o = {}) => ({ l, h, w, latch: true, mod: true, ...o });
const md = (k) => ({ ...k, mod: true });
const hidOf = (c) => 0x04 + c.charCodeAt(0) - 65;
const letters = (s, red) => s.split('').map((c, i) => H(c, hidOf(c), 1, red ? { red: red[i] } : {}));
const digits = (sc) => '1234567890'.split('').map((c, i) => C(c, sc && sc[i] !== ' ' ? sc[i] : undefined));
const ARROWS = [md(H('←', 0x50)), md(H('↓', 0x51)), md(H('→', 0x4f))];

const LAYOUTS = {
  pc: { rows: [
    [md(H('Esc', 0x29)), ...[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12].map((n) => md(H('F' + n, 0x39 + n)))],
    [...digits('!@#$%^&*()'), C('-', '_'), C('=', '+'), md(H('⌫', 0x2a, 1.5))],
    [md(H('Tab', 0x2b, 1.3)), ...letters('QWERTYUIOP'), C('[', '{'), C(']', '}')],
    [MK('Ctrl', 0xe0, 1.5), ...letters('ASDFGHJKL'), C(';', ':'), C("'", '"'), md(H('↵', 0x28, 1.5))],
    [MK('Shift', 0xe1, 1.8), ...letters('ZXCVBNM'), C(',', '<'), C('.', '>'), C('/', '?'), md(H('↑', 0x52))],
    [MK('Alt', 0xe2, 1.3), md(H('Caps', 0x39, 1.3)), C('\\', '|'), C('`', '~'), H('', 0x2c, 5), md(H('Del', 0x4c)), ...ARROWS],
  ] },
  // Apple IIe: arrows ← → ↓ ↑ in the bottom-right corner; RESET is the toolbar's (F11 = CPU reset)
  apple: { rows: [
    [md(H('esc', 0x29, 1.2)), ...digits('!@#$%^&*()'), C('-', '_'), C('=', '+'), md(H('delete', 0x4c, 1.5))],
    [md(H('tab', 0x2b, 1.5)), ...letters('QWERTYUIOP'), C('[', '{'), C(']', '}')],
    [MK('control', 0xe0, 1.7), ...letters('ASDFGHJKL'), C(';', ':'), C("'", '"'), md(H('return', 0x28, 1.7))],
    [MK('shift', 0xe1, 2.2), ...letters('ZXCVBNM'), C(',', '<'), C('.', '>'), C('/', '?'), MK('shift', 0xe1, 2.2)],
    [md(H('caps lock', 0x39, 1.2)), MK('○', 0xe2, 1, { top: 'open' }), C('`', '~'), H('', 0x2c, 4.5), C('\\', '|'),
      MK('●', 0xe6, 1, { top: 'solid' }), md(H('←', 0x50)), md(H('→', 0x4f)), md(H('↓', 0x51)), md(H('↑', 0x52))],
  ] },
  // Apple II+: upper case only, the symbols over the digits / N M P, REPT left of RETURN, ← → beside ;+
  // (the real RESET key, top right, is the toolbar's RESET)
  apple2p: { rows: [
    [...digits('!"#$%&\'() '), C(':', '*'), C('-', '=')],
    [md(H('ESC', 0x29, 1.2)), ...letters('QWERTYUIO'), C('P', '@'), md(H('REPT', 0, 1.2, { rept: true })), md(H('RETURN', 0x28, 2))],
    [MK('CTRL', 0xe0, 1.4), ...letters('ASDFGHJKL'), C(';', '+'), md(H('←', 0x50)), md(H('→', 0x4f))],
    [MK('SHIFT', 0xe1, 1.8), ...letters('ZXCVB'), C('N', '^'), C('M', ']'), C(',', '<'), C('.', '>'), C('/', '?'), MK('SHIFT', 0xe1, 1.8)],
    [H('', 0x2c, 1)],
  ] },
  c64: { rows: [
    [md(H('RUN STOP', 0x29, 1.3)), md(H('CLR HOME', 0x4a, 1.3, { sh: 0x4d })), md(H('INST DEL', 0x2a, 1.3, { sh: 0x49 })),
      md(H('RESTORE', 0x4b, 1.3)), ...[1, 3, 5, 7].map((n) => H('f' + n, 0x39 + n, 1, { sh: 0x3a + n, top: 'f' + (n + 1), cls: 'fkey' }))],
    [H('←', 0x35), ...digits(' "#$%&\'()'), C('+'), C('-'), H('£', 0x31)],
    [MK('CTRL', 0xe0, 1.4), ...letters('QWERTYUIOP'), C('@'), C('*'), C('^', undefined, '↑')],
    [MK('C=', 0xe2, 1.4), ...letters('ASDFGHJKL'), C(':', '['), C(';', ']'), C('='), md(H('RETURN', 0x28, 1.8))],
    [MK('SHIFT', 0xe1, 1.8), ...letters('ZXCVBNM'), C(',', '<'), C('.', '>'), C('/', '?'), md(H('↑', 0x52))],
    [H('', 0x2c, 7), ...ARROWS],
  ] },
  msx: { rows: [
    [...[1, 2, 3, 4, 5].map((n) => H('F' + n, 0x39 + n, 1, { top: 'F' + (n + 5), cls: 'fkey' })),
      md(H('STOP', 0x48)), md(H('SELECT', 0x4b, 1.2)), md(H('HOME', 0x4a, 1, { top: 'CLS', sh: 0x4d })), md(H('INS', 0x49)), md(H('DEL', 0x4c))],
    [md(H('ESC', 0x29)), ...digits('!@#$%^&*()'), C('-', '_'), C('=', '+'), C('\\', '|'), md(H('BS', 0x2a, 1.2))],
    [md(H('TAB', 0x2b, 1.4)), ...letters('QWERTYUIOP'), C('[', '{'), C(']', '}'), md(H('RETURN', 0x28, 1.6))],
    [MK('CTRL', 0xe0, 1.6), ...letters('ASDFGHJKL'), C(';', ':'), C("'", '"'), C('`', '~')],
    [MK('SHIFT', 0xe1, 2), ...letters('ZXCVBNM'), C(',', '<'), C('.', '>'), C('/', '?'), md(H('↑', 0x52))],
    [md(H('CAPS', 0x39, 1.2)), MK('GRAPH', 0xe2, 1.3), H('', 0x2c, 5), MK('CODE', 0x4e, 1.3), ...ARROWS],
  ] },
  zx: { rows: [
    '1234567890'.split('').map((c, i) => H(c, 0x1e + i, 1, { red: "!@#$%&'()_"[i] })),
    letters('QWERTYUIOP', ['<=', '<>', '>=', '<', '>', 'AND', 'OR', 'AT', ';', '"']),
    [...letters('ASDFGHJKL', ['STOP', 'NOT', 'STEP', 'TO', 'THEN', '↑', '-', '+', '=']), md(H('ENTER', 0x28, 1.3))],
    [MK('CAPS SHIFT', 0xe1, 1.5), ...letters('ZXCVBNM', [':', '£', '?', '/', '*', ',', '.']),
      MK('SYMBOL SHIFT', 0xe0, 1.5, { cls: 'sym' }), H('SPACE', 0x2c, 1.5, { top: 'BREAK' })],
    [md(H('EDIT', 0x1e, 1, { caps: true })), md(H('EXTEND', 0x2b, 1.2)), md(H('DELETE', 0x2a, 1.2)), md(H('BREAK', 0x29, 1.2)),
      md(H('←', 0x50)), md(H('↑', 0x52)), md(H('↓', 0x51)), md(H('→', 0x4f)), H('RESET', 0x45, 1.2, { cls: 'warn' })],
  ] },
  coleco: { small: true, rows: [
    [...'123456'.split('').map((c, i) => H(c, 0x1e + i)), H('*', 0x2d)],
    [...'789'.split('').map((c) => H(c, 0x1e + (+c) - 1)), H('0', 0x27), H('#', 0x2e), H('RESET', 0x45, 2, { cls: 'warn' })],
  ] },
  atari: { small: true, rows: [[H('SELECT', 0x35, 1, { cls: 'mod' }), H('RESET', 0x38, 1, { cls: 'warn' })]] },
  sms: { small: true, rows: [[H('PAUSE', 0x44, 1, { cls: 'mod' }), H('RESET', 0x45, 1, { cls: 'warn' })]] },
};
const LAYOUT_OF = { [PLATFORM.APPLE2]: 'apple', [PLATFORM.C64]: 'c64', [PLATFORM.MSX]: 'msx', [PLATFORM.ZX]: 'zx',
  [PLATFORM.COLECO]: 'coleco', [PLATFORM.ATARI]: 'atari', [PLATFORM.SMS]: 'sms' };

const kbd = $('#kbd');
const latched = new Set();
function setLatch(h, on) {
  if (on) latched.add(h); else latched.delete(h);
  key(h, on);
  kbd.querySelectorAll(`.latchable[data-hid="${h}"]`).forEach((x) => x.classList.toggle('latch', on));
}
function releaseLatched() { for (const h of [...latched]) setLatch(h, false); }

// Press one key; returns the function that releases it.
// Apple II+ REPT, like the real key: held together with a character key it re-types that key
// (~10/s) for as long as both are down; on its own it does nothing.
let heldKey = null;                                  // the character key being held: { k, rel }
function retype(h) {
  if (!h || heldKey !== h) return;
  h.rel(); h.rel = () => {};                         // let go, then press again (a new key-down edge)
  setTimeout(() => { if (heldKey === h) h.rel = oskPress(h.k); }, 35);
}
function oskPress(k) {
  if (k.rept) {
    const t = setInterval(() => retype(heldKey), 100);
    return () => clearInterval(t);
  }
  const sh = latched.has(0xe1);
  let hid = k.h, force = null;                       // force: SHIFT state the key needs (null = as latched)
  if (sh && k.sh !== undefined) { hid = k.sh; force = false; }
  else if (k.c !== undefined) {
    const ch = sh && k.sc ? k.sc : k.c;
    const m = charToHid(ch);
    if (m) { hid = m[0]; if ((sh && k.sc) || m[1]) force = m[1]; }
    else hid = hidOf(ch.toUpperCase());
  }
  const flip = force !== null && force !== sh;
  if (flip) key(0xe1, force);
  if (k.caps && !sh) key(0xe1, true);                // ZX EDIT = CAPS SHIFT + 1
  key(hid, true);
  return () => {
    key(hid, false);
    if (k.caps && !sh) key(0xe1, false);
    if (flip) key(0xe1, sh);
  };
}

let kbdLayout = '';
function buildKbd(name) {
  if (name === kbdLayout) return;
  kbdLayout = name;
  releaseLatched();
  const L = LAYOUTS[name];
  kbd.innerHTML = '';
  kbd.className = 'kb-' + name + (L.small ? ' small' : '');
  app.classList.toggle('kbd-small', !!L.small);
  for (const row of L.rows) {
    const r = document.createElement('div'); r.className = 'krow';
    for (const k of row) {
      const b = document.createElement('button');
      b.className = 'k' + (k.mod ? ' mod' : '') + (k.latch ? ' latchable' : '') + (k.cls ? ' ' + k.cls : '') + (k.h === 0x2c && !k.l ? ' space' : '');
      b.dataset.hid = k.h;
      b.style.flexGrow = k.w;
      if (k.top) { const t = document.createElement('small'); t.textContent = k.top; b.appendChild(t); }
      const m = document.createElement('span'); m.textContent = k.l; b.appendChild(m);
      if (k.red) { const t = document.createElement('small'); t.className = 'red'; t.textContent = k.red; b.appendChild(t); }
      if (k.latch) {
        b.addEventListener('pointerdown', (e) => { e.preventDefault(); setLatch(k.h, !latched.has(k.h)); });
      } else {
        let downAt = 0, release = null;
        b.addEventListener('pointerdown', (e) => {
          e.preventDefault(); try { b.setPointerCapture(e.pointerId); } catch (_) {} b.classList.add('down');
          downAt = performance.now();
          if (k.rept) release = oskPress(k);
          else {
            const h = { k, rel: oskPress(k) }; heldKey = h;
            // Apple IIe: a held key repeats by itself after a short pause, like the real keyboard
            let t = null;
            const d = kbdLayout === 'apple' && k.h !== 0x39 ? setTimeout(() => { t = setInterval(() => retype(h), 90); }, 500) : null;
            release = () => { clearTimeout(d); clearInterval(t); if (heldKey === h) heldKey = null; h.rel(); };
          }
        });
        const up = () => {
          if (!release) return;
          const rel = release; release = null;
          b.classList.remove('down');
          const wait = Math.max(0, 60 - (performance.now() - downAt));   // let the core scan the press
          setTimeout(() => { rel(); if (latched.size) setTimeout(releaseLatched, 30); }, wait);
        };
        b.addEventListener('pointerup', up); b.addEventListener('pointercancel', up);
      }
      b.addEventListener('contextmenu', (e) => e.preventDefault());
      r.appendChild(b);
    }
    kbd.appendChild(r);
  }
  if (kbdOn) setKbd(true); else layout();
}
buildKbd('pc');
// Computers start with their keyboard open, consoles with the pad: on every machine (re)start, i.e.
// leaving the system picker. The ⌨ button still toggles it freely after that.
const COMPUTERS = [PLATFORM.APPLE2, PLATFORM.C64, PLATFORM.MSX, PLATFORM.PCXT, PLATFORM.ZX];
let runningPlat = null;
setInterval(() => {
  if (!M) return;
  const p = M._web_platform();
  M._web_osk_open(kbdOn ? 1 : 0);
  buildKbd(p === PLATFORM.APPLE2 && !M._web_apple_iie() ? 'apple2p' : (LAYOUT_OF[p] || 'pc'));
  const run = M._web_splash && M._web_splash() ? -1 : p;
  if (run !== runningPlat) {
    const first = runningPlat === null;
    runningPlat = run;
    if (run >= 0) setKbd(COMPUTERS.includes(p)); else if (!first) setKbd(false);
  }
  // Apple II: the machine's RESET key lives in the toolbar (⟲ sends F11 = CPU reset)
  const rb = $('[data-act=reset]'), apple = p === PLATFORM.APPLE2;
  setAppleStick(apple);
  if (rb.classList.contains('txt') !== apple) { rb.classList.toggle('txt', apple); rb.textContent = apple ? 'RESET' : '⟲'; }
}, 400);

// ======================= toolbar =======================
function setKbd(on) {
  kbdOn = on; kbd.hidden = !on;
  if (M && M._web_osk_open) M._web_osk_open(on ? 1 : 0);
  $('[data-act=kbd]').classList.toggle('on', on);
  // a full keyboard takes the pad's place; the consoles' small panels (keypad, SELECT/RESET) sit beside it
  padHidden = on && !LAYOUTS[kbdLayout].small && (app.classList.contains('overlay') || app.classList.contains('side'));
  layout();
}
$('#toolbar').addEventListener('click', (e) => {
  const b = e.target.closest('button'); if (!b) return;
  startAudio();
  switch (b.dataset.act) {
    case 'menu': tapKey(0x43); break;                                    // F10: settings menu
    case 'kbd': setKbd(!kbdOn); break;
    case 'files': openFiles(); break;
    case 'reset': {
      const p = M ? M._web_platform() : -1;
      if (p === PLATFORM.APPLE2) tapKey(0x44);                          // F11: Apple CPU reset
      else if ([PLATFORM.SMS, PLATFORM.PCXT, PLATFORM.COLECO, PLATFORM.ZX].includes(p)) tapKey(0x45); // F12
      else if (confirm(t('confirm.reboot'))) M && M.webReboot();
      break;
    }
    case 'sound': toggleMute(); break;
    case 'fs': toggleFullscreen(); break;
  }
});
function toggleFullscreen() {
  const d = document;
  if (!d.fullscreenElement && !d.webkitFullscreenElement) {
    const el = d.documentElement;
    const p = (el.requestFullscreen || el.webkitRequestFullscreen || (() => {})).call(el, { navigationUI: 'hide' });
    if (p && p.then) p.then(() => { try { screen.orientation.lock('landscape').catch(() => {}); } catch (e) {} }).catch(() => {});
  } else {
    try { screen.orientation.unlock(); } catch (e) {}
    (d.exitFullscreen || d.webkitExitFullscreen).call(d);
  }
}

// ======================= audio =======================
let actx = null, gain = null, muted = false;
function startAudio() {
  // iOS: play through the ring/silent switch like a media app (Safari 16.4+)
  try { if (navigator.audioSession) navigator.audioSession.type = 'playback'; } catch (e) {}
  if (actx) { if (actx.state !== 'running') actx.resume(); return; }
  if (!M) return;
  try {
    actx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    actx.resume();                                          // still inside the user gesture
    // a silent one-sample buffer, started in the gesture, unlocks older iOS WebKit
    const sb = actx.createBuffer(1, 1, 22050), src = actx.createBufferSource();
    src.buffer = sb; src.connect(actx.destination); src.start(0);
  } catch (e) { console.warn('audio', e); return; }
  actx.audioWorklet.addModule('audio-worklet.js').then(() => {
    const node = new AudioWorkletNode(actx, 'emu8-ring', { numberOfInputs: 0, outputChannelCount: [1] });
    node.port.postMessage({ buf: M.HEAPU8.buffer, ptr: M._web_audio_ring() });
    gain = actx.createGain(); gain.gain.value = muted ? 0 : 1;
    node.connect(gain).connect(actx.destination);
  }).catch((e) => console.warn('audio worklet', e));
}
function toggleMute() {
  muted = !muted;
  if (gain) gain.gain.value = muted ? 0 : 1;
  $('[data-act=sound]').textContent = muted ? '🔇' : '🔊';
}
for (const ev of ['pointerdown', 'touchend', 'keydown']) document.addEventListener(ev, () => { if (M) startAudio(); }, true);

// ======================= files =======================
const folderSel = $('#folder');
FOLDERS.forEach((f) => { const o = document.createElement('option'); o.value = o.textContent = f; folderSel.appendChild(o); });
folderSel.addEventListener('change', refreshFiles);

function listDir(dir) {
  try {
    return M.FS.readdir(dir).filter((n) => n !== '.' && n !== '..').map((n) => {
      const st = M.FS.stat(dir + '/' + n);
      return { name: n, size: st.size, dir: M.FS.isDir(st.mode) };
    });
  } catch (e) { return []; }
}
function fmtSize(n) { return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : n >= 1024 ? Math.round(n / 1024) + ' KB' : n + ' B'; }
function refreshFiles() {
  const ul = $('#file-list'); ul.innerHTML = '';
  if (!M) return;
  const dir = '/sdcard/' + folderSel.value;
  const items = listDir(dir).sort((a, b) => a.name.localeCompare(b.name));
  if (!items.length) { const li = document.createElement('li'); li.innerHTML = '<span class="hint"></span>'; li.firstChild.textContent = t('files.empty'); ul.appendChild(li); }
  for (const it of items) {
    const li = document.createElement('li');
    const s = document.createElement('span'); s.textContent = it.name + (it.dir ? '/' : '');
    const sm = document.createElement('small'); sm.textContent = it.dir ? '' : fmtSize(it.size); s.appendChild(sm);
    const del = document.createElement('button'); del.textContent = t('files.delete');
    del.onclick = () => {
      if (!confirm(t('files.confirmDelete', it.name))) return;
      try { it.dir ? M.FS.rmdir(dir + '/' + it.name) : M.FS.unlink(dir + '/' + it.name); } catch (e) { alert(t('files.deleteFail', e)); }
      refreshFiles(); schedulePersist();
    };
    li.append(s, del); ul.appendChild(li);
  }
}
function openFiles() { $('#files').hidden = false; refreshFiles(); }
$('#files [data-close]').addEventListener('click', () => { $('#files').hidden = true; recvStop(); });
$('#upload').addEventListener('change', async (e) => {
  const files = [...e.target.files]; e.target.value = '';
  if (!M || !files.length) return;
  const st = $('#upload-status');
  const dir = '/sdcard/' + folderSel.value;
  let n = 0;
  for (const f of files) {
    st.textContent = t('files.uploading', f.name);
    try { M.FS.writeFile(dir + '/' + f.name, new Uint8Array(await f.arrayBuffer())); n++; }
    catch (err) { alert(t('files.uploadFail', f.name, err)); }
  }
  persist(() => { st.textContent = t('files.saved', n, folderSel.value); });
  refreshFiles();
});
// ---- receive from PC: the phone opens a relay room (worker/relay.js) under a short code; the
// desktop page /send joins it and streams files: {"t":"file",name,size,folder} + binary chunks +
// {"t":"end"}. Each file is written to /sdcard/<folder> and answered with {"t":"saved",path}.
const EXT_FOLDER = {
  nes: 'nes', d64: 'c64', prg: 'c64', crt: 'c64', t64: 'c64', a26: 'atari2600', mx1: 'msx', mx2: 'msx',
  sms: 'sms', gg: 'sms', col: 'coleco', z80: 'zx', tzx: 'zx', tap: 'zx', sna: 'zx',
  po: 'apple2', do: 'apple2', '2mg': 'apple2', hdv: 'apple2', nib: 'apple2', woz: 'apple2',
  img: 'pcxt', ima: 'pcxt', vhd: 'pcxt', hdd: 'pcxt',
};   // .dsk/.rom/.bin are shared by several systems: they go to the folder selected here
const RECV_IDLE_MS = 10 * 60 * 1000, RECV_MAX = 64 * 1024 * 1024;
let recv = null;   // { ws, code, idle, cur:{name,size,folder,buf,got} }

function recvCode() {
  const A = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789', r = crypto.getRandomValues(new Uint8Array(5));
  return [...r].map((b) => A[b % A.length]).join('');
}
function recvStatus(key, ...a) { $('#recv-status').textContent = t(key, ...a); }
function recvTouch() { clearTimeout(recv.idle); recv.idle = setTimeout(recvStop, RECV_IDLE_MS); }
function recvFolder(name, asked) {
  if (FOLDERS.includes(asked)) return asked;
  const ext = (name.match(/\.([^.]+)$/) || [])[1];
  return EXT_FOLDER[(ext || '').toLowerCase()] || folderSel.value;
}

function recvStart() {
  recvStop();
  const code = recvCode();
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/api/relay/${code}?role=recv`);
  ws.binaryType = 'arraybuffer';
  recv = { ws, code, idle: 0, cur: null };
  $('#recv').hidden = false; $('#btn-recv').hidden = true;
  $('#recv-code').textContent = code;
  $('#recv-url').textContent = location.origin + '/send?c=' + code;
  recvStatus('recv.connecting');
  recvTouch();
  const reply = (o) => { try { ws.send(JSON.stringify(o)); } catch (e) {} };
  ws.onopen = () => recvStatus('recv.waiting');
  ws.onclose = () => { if (recv && recv.ws === ws) { recvStatus('recv.closed'); recv.ws = null; } };
  ws.onmessage = (e) => {
    if (!recv || recv.ws !== ws) return;
    recvTouch();
    const c = recv.cur;
    if (typeof e.data !== 'string') {           // a chunk of the current file
      if (!c || c.bad) return;
      const b = new Uint8Array(e.data);
      if (c.got + b.length > c.size) { c.bad = 'size'; return; }
      c.buf.set(b, c.got); c.got += b.length;
      recvStatus('recv.getting', c.name, Math.floor(100 * c.got / Math.max(1, c.size)));
      return;
    }
    let m; try { m = JSON.parse(e.data); } catch (err) { return; }
    if (m.t === 'peer') recvStatus(m.on ? 'recv.pc' : 'recv.waiting');
    else if (m.t === 'file') {
      const name = String(m.name || '').replace(/[\/:*?"<>|\x00-\x1f]/g, '_').replace(/^\.+/, '').slice(0, 120);
      const size = Number(m.size) | 0;
      if (!name || size < 0 || size > RECV_MAX) { recv.cur = { name: name || '?', bad: 'size' }; return; }
      recv.cur = { name, size, folder: recvFolder(name, m.folder), buf: new Uint8Array(size), got: 0 };
      recvStatus('recv.getting', name, 0);
    } else if (m.t === 'end' && c) {
      recv.cur = null;
      if (c.bad || c.got !== c.size) { reply({ t: 'error', name: c.name, msg: t('recv.badFile') }); return; }
      const dir = '/sdcard/' + c.folder;
      try {
        try { M.FS.mkdir(dir); } catch (err) {}
        M.FS.writeFile(dir + '/' + c.name, c.buf);
      } catch (err) { reply({ t: 'error', name: c.name, msg: String(err) }); return; }
      reply({ t: 'saved', name: c.name, path: c.folder + '/' + c.name });
      recvStatus('recv.saved', c.name, c.folder);
      refreshFiles(); schedulePersist();
    }
  };
}
function recvStop() {
  if (!recv) return;
  clearTimeout(recv.idle);
  if (recv.ws) { recv.ws.onclose = null; try { recv.ws.close(); } catch (e) {} }
  recv = null;
  $('#recv').hidden = true; $('#btn-recv').hidden = false;
}
$('#btn-recv').addEventListener('click', () => { if (M) recvStart(); });
$('#btn-recv-stop').addEventListener('click', recvStop);
$('#btn-reboot').addEventListener('click', () => { if (M) M.webReboot(); });

if ('serviceWorker' in navigator) { /* no SW: the headers come from the server (_headers) */ }

// ======================= settings sheet =======================
// The emulator's own menu (optionsui.cpp) stays the source of truth: it opens with F10 as before and
// the sheet is a touch-first view of its main page (web_menu_state JSON), sending the same actions
// back (web_menu_cmd). Sub-pages (CONFIG, HELP, ROMS, snapshots), the system picker's settings and
// "Clássico" show
// the classic menu on the screen.
const msheet = $('#msheet');
let msSeq = -1, msPage = 'closed', msState = null, msClassic = false, msFilesKey = '', msTgKey = '', msActKey = '', msText = '', msVolDrag = false;
function msCmd(cmd, arg = 0) { if (M) M._web_menu_cmd(cmd, arg); }
function msRead() {
  const ptr = M._web_menu_state(), h = M.HEAPU8;
  let end = ptr; while (h[end]) end++;
  const t = new TextDecoder().decode(h.slice(ptr, end));
  if (t === msText) return 'same';
  try { const s = JSON.parse(t); msText = t; return s; } catch (e) { return null; }
}
function msRender(s) {
  $('#ms-title').textContent = s.title;
  // Only what changed is rebuilt: replacing a button under a finger would swallow the tap.
  // settings: one tile per toggle, tap = next value (exactly like tapping it in the classic menu)
  const tg = $('#ms-toggles'), tgKey = JSON.stringify(s.toggles);
  if (tgKey !== msTgKey) { msTgKey = tgKey; tg.innerHTML = '';
  for (const [slot, label, value] of s.toggles) {
    const b = document.createElement('button'); b.className = 'ms-tile';
    b.innerHTML = '<small></small><b></b>';
    b.firstChild.textContent = label; b.lastChild.textContent = value;
    b.onclick = () => msCmd(1, slot);
    tg.appendChild(b);
  } }
  $('#ms-volrow').hidden = !s.hasVol;
  if (!msVolDrag) $('#ms-vol').value = s.vol;
  $('#ms-volv').textContent = $('#ms-vol').value;
  $('#ms-cfg').textContent = t(s.cfg === 'CONFIG' ? 'ms.config' : 'ms.help');
  $('#ms-snap').hidden = !s.snap;
  $('#ms-more').textContent = s.more ? t('ms.partial') : '';
  // files: rebuilt only when the list changes (keeps the scroll position); the selection is a class
  const ul = $('#ms-files'), key = JSON.stringify(s.files);
  if (key !== msFilesKey) {
    msFilesKey = key; ul.innerHTML = '';
    if (!s.files.length) { const li = document.createElement('li'); li.className = 'empty'; li.textContent = t('ms.empty'); ul.appendChild(li); }
    s.files.forEach(([name, dir, mounted, bad], i) => {
      const li = document.createElement('li');
      li.className = (bad ? 'bad' : '');
      li.innerHTML = '<span class="ic"></span><span class="nm"></span>';
      li.firstChild.textContent = name === '..' ? '↩' : dir ? '📁' : '💾';
      li.children[1].textContent = name === '..' ? t('ms.back') : dir ? name.replace(/^\[|\]$|\/$/g, '') : name;
      if (mounted) { const bd = document.createElement('span'); bd.className = 'badge'; bd.textContent = t('ms.inUse'); li.appendChild(bd); }
      // folder: enter it; file: first tap selects, second tap loads (the classic list's double tap)
      li.onclick = () => { if (dir || (msState && msState.sel === i)) msCmd(4, i); else msCmd(3, i); };
      ul.appendChild(li);
    });
  }
  [...ul.children].forEach((li, i) => li.classList.toggle('sel', i === s.sel));
  // actions: the classic menu's own buttons, same order
  const ac = $('#ms-acts');
  const selFile = s.sel >= 0 && s.files[s.sel] && !s.files[s.sel][1];
  const actKey = JSON.stringify(s.acts) + selFile;
  if (actKey !== msActKey) { msActKey = actKey; ac.innerHTML = '';
  for (const [id, label] of s.acts) {
    const b = document.createElement('button');
    b.textContent = I18N.en['act.' + label.trim()] ? t('act.' + label.trim()) : label;
    b.className = id === 12 ? 'warn' : id === 10 ? 'primary' : '';
    if (id !== 12 && !selFile && !/UNMOUNT|EJECT/.test(label)) b.disabled = true;
    b.onclick = () => msCmd(5, id);
    ac.appendChild(b);
  } }
}
msheet.addEventListener('click', (e) => {
  if (e.target === msheet) { msCmd(9); return; }                        // tap outside: close the menu
  const b = e.target.closest('[data-ms]'); if (!b) return;
  switch (b.dataset.ms) {
    case 'close': msCmd(9); break;
    case 'cfg': msCmd(6); break;
    case 'snap': msCmd(7); break;
    case 'classic': msClassic = true; msheet.hidden = true; break;
  }
});
const msVol = $('#ms-vol');
msVol.addEventListener('pointerdown', () => { msVolDrag = true; });
msVol.addEventListener('input', () => { $('#ms-volv').textContent = msVol.value; msCmd(2, +msVol.value); });
msVol.addEventListener('change', () => { msVolDrag = false; });
setInterval(() => {
  if (!M || !M._web_menu_seq) return;
  const seq = M._web_menu_seq();
  if (seq === msSeq) return;
  msSeq = seq;
  const s = msRead(); if (!s || s === 'same') return;
  msPage = s.page;
  if (s.page === 'closed') msClassic = false;
  const show = s.page === 'main' && !msClassic;
  if (show) { msState = s; msRender(s); }
  if (msheet.hidden === show) msheet.hidden = !show;
}, 100);

// language picker (Files dialog): re-label everything built in JS, too
const langSel = $('#lang');
for (const [k, d] of Object.entries(I18N)) { const o = document.createElement('option'); o.value = k; o.textContent = d._name; langSel.appendChild(o); }
langSel.value = LANG;
langSel.addEventListener('change', () => setLang(langSel.value));
document.addEventListener('emu8-lang', () => {
  if (!$('#files').hidden) refreshFiles();
  $('#upload-status').textContent = '';
  msFilesKey = msTgKey = msActKey = '';
  if (msState && !msheet.hidden) msRender(msState);
});
