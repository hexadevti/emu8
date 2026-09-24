// protocol.js - client for the emu8 SD file manager protocol (firmware: src/shared/sdserial.cpp,
// spec: ../PROTOCOL.md). Transport-agnostic: hand it a write(Uint8Array) function and feed every
// byte you receive into link.receive(). Used by the web app over Web Serial and by the Node tests
// over a child process.

export const CMD = {
  HELLO: 0x01, FSINFO: 0x02,
  LIST: 0x10, STAT: 0x11,
  RDOPEN: 0x20, READ: 0x21, RDCLOSE: 0x22,
  WROPEN: 0x30, WRITE: 0x31, WRCLOSE: 0x32,
  MKDIR: 0x40, REMOVE: 0x41, RENAME: 0x42,
  BAUD: 0x50, REBOOT: 0x60, BYE: 0x7f,
};

export const STATUS_TEXT = [
  'OK', 'Operation failed', 'Not found', 'SD card I/O error', 'Bad request',
  'No SD card mounted', 'No file open', 'Unknown command', 'Not supported on this board',
];

export class DeviceError extends Error {
  constructor(status, what) {
    super(`${what}: ${STATUS_TEXT[status] || `status ${status}`}`);
    this.status = status;
  }
}
export class AbortedError extends Error { constructor() { super('Cancelled'); this.name = 'AbortError'; } }

const enc = new TextEncoder();
const dec = new TextDecoder();

export function crc16(bytes, crc = 0xffff) {
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i] << 8;
    for (let b = 0; b < 8; b++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
const u16 = v => new Uint8Array([v & 0xff, (v >> 8) & 0xff]);
const u32 = v => new Uint8Array([v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]);
const rd16 = (b, o) => b[o] | (b[o + 1] << 8);
const rd32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

export function joinPath(dir, name) {
  return (dir.endsWith('/') ? dir : dir + '/') + name;
}
export function parentPath(p) {
  const i = p.replace(/\/+$/, '').lastIndexOf('/');
  return i <= 0 ? '/' : p.slice(0, i);
}

export class Emu8Link {
  /**
   * @param {(bytes: Uint8Array) => Promise<void>|void} write  sends raw bytes to the device
   * @param {{onLog?: (line: string) => void, baud?: number}} opts
   *   onLog receives text the device printed between frames (its serial log).
   */
  constructor(write, opts = {}) {
    this.write = write;
    this.onLog = opts.onLog || (() => {});
    this.baud = opts.baud || 115200;
    this.maxPayload = 1024;
    this.info = null;
    this.retries = 5;
    this.seq = 0;
    this.pending = null;          // { seq, cmd, resolve }
    this.queue = Promise.resolve();
    this.rx = new Uint8Array(0);
    this.logText = '';
  }

  // ---- receive path: find frames, verify CRC, pass everything else to onLog ----
  receive(chunk) {
    this.rx = this.rx.length ? concat(this.rx, chunk) : chunk;
    let i = 0;
    const b = this.rx;
    while (i < b.length) {
      if (b[i] !== 0xa5 || (i + 1 < b.length && b[i + 1] !== 0x5a)) { this.logByte(b[i]); i++; continue; }
      if (b.length - i < 6) break;                               // header incomplete
      const len = rd16(b, i + 4);
      if (len > (this.info ? this.maxPayload : 8192)) { this.logByte(b[i]); i++; continue; }  // not a frame
      if (b.length - i < 8 + len) break;                         // wait for the rest
      const body = b.subarray(i + 2, i + 6 + len);
      if (crc16(body) !== rd16(b, i + 6 + len)) { this.logByte(b[i]); i++; continue; }
      this.onFrame(b[i + 2], b[i + 3], b.slice(i + 6, i + 6 + len));
      i += 8 + len;
    }
    this.rx = b.slice(i);
  }

  // A corrupted length byte makes a bad frame look longer than it is, and receive() would wait
  // for bytes that never come. Called on a request timeout: drop the stale frame start and rescan.
  resync() {
    if (!this.rx.length) return;
    this.logByte(this.rx[0]);
    const rest = this.rx.subarray(1);
    this.rx = new Uint8Array(0);
    this.receive(rest.slice());
  }

  logByte(c) {
    if (c === 10) { this.flushLog(); return; }
    if (c === 13) return;
    this.logText += c >= 32 && c < 127 ? String.fromCharCode(c) : '';
    if (this.logText.length > 400) this.flushLog();
  }
  flushLog() { if (this.logText.trim()) this.onLog(this.logText); this.logText = ''; }

  onFrame(cmd, seq, payload) {
    const p = this.pending;
    if (!p || seq !== p.seq || cmd !== (p.cmd | 0x80)) return;   // stale reply to a retried request
    this.pending = null;
    p.resolve(payload);
  }

  // ---- request path ----
  frame(cmd, seq, payload) {
    const body = concat(new Uint8Array([cmd, seq]), u16(payload.length), payload);
    return concat(new Uint8Array([0xa5, 0x5a]), body, u16(crc16(body)));
  }

  // One request at a time; resends the identical frame on timeout (the device treats every
  // command as idempotent or offset-keyed, see PROTOCOL.md).
  request(cmd, payload = new Uint8Array(0), { expectBytes = 16, retries = this.retries } = {}) {
    const run = async () => {
      const seq = this.seq = (this.seq + 1) & 0xff;
      const bytes = this.frame(cmd, seq, payload);
      // Time on the wire both ways at 10 bits/byte, plus slack for the SD card and USB latency.
      const timeout = 1200 + Math.ceil(((bytes.length + expectBytes + 16) * 10 * 1000) / this.baud);
      for (let attempt = 0; attempt <= retries; attempt++) {
        const reply = await new Promise(resolve => {
          const timer = setTimeout(() => { if (this.pending?.seq === seq) this.pending = null; resolve(null); }, timeout);
          this.pending = { seq, cmd, resolve: r => { clearTimeout(timer); resolve(r); } };
          Promise.resolve(this.write(bytes)).catch(() => {});
        });
        if (reply) return reply;
        this.resync();
      }
      throw new Error(`Device did not answer (command 0x${cmd.toString(16)})`);
    };
    const result = this.queue.then(run, run);
    this.queue = result.catch(() => {});
    return result;
  }

  async ok(cmd, payload, what, opts) {
    const r = await this.request(cmd, payload, opts);
    if (r[0] !== 0) throw new DeviceError(r[0], what);
    return r;
  }

  // ---- commands ----
  async hello(retries = this.retries) {
    const r = await this.ok(CMD.HELLO, undefined, 'Hello', { retries });
    const flags = r[2];
    this.maxPayload = rd16(r, 3);
    this.info = {
      version: r[1],
      sdMounted: !!(flags & 1),
      usbLink: !!(flags & 2),
      canSetBaud: !!(flags & 4),
      maxPayload: this.maxPayload,
      board: dec.decode(r.subarray(5)),
    };
    return this.info;
  }

  async fsInfo() {
    const r = await this.ok(CMD.FSINFO, undefined, 'Card info');
    const total = rd32(r, 1) + rd32(r, 5) * 2 ** 32;
    const used = rd32(r, 9) + rd32(r, 13) * 2 ** 32;
    return total ? { total, used, free: total - used } : null;
  }

  async list(path) {
    const entries = [];
    const pathBytes = enc.encode(path);
    for (;;) {
      const r = await this.ok(CMD.LIST, concat(u16(entries.length), pathBytes), `List ${path}`,
        { expectBytes: this.maxPayload });
      let o = 3;
      for (let k = 0; k < r[1]; k++) {
        const dir = !!(r[o] & 1), size = rd32(r, o + 1), n = r[o + 5];
        entries.push({ name: dec.decode(r.subarray(o + 6, o + 6 + n)), dir, size, path: joinPath(path, dec.decode(r.subarray(o + 6, o + 6 + n))) });
        o += 6 + n;
      }
      if (!r[2]) return entries;
    }
  }

  async stat(path) {
    const r = await this.request(CMD.STAT, enc.encode(path));
    if (r[0] === 2) return null;
    if (r[0] !== 0) throw new DeviceError(r[0], `Stat ${path}`);
    return { dir: !!r[1], size: rd32(r, 2) };
  }

  /** Download a file. onProgress(doneBytes, totalBytes). */
  async download(path, { onProgress = () => {}, signal } = {}) {
    const r = await this.ok(CMD.RDOPEN, enc.encode(path), `Open ${path}`);
    const size = rd32(r, 1);
    const out = new Uint8Array(size);
    const chunk = this.maxPayload - 1;
    let off = 0;
    try {
      onProgress(0, size);
      while (off < size) {
        if (signal?.aborted) throw new AbortedError();
        const want = Math.min(chunk, size - off);
        const d = await this.ok(CMD.READ, concat(u32(off), u16(want)), `Read ${path}`, { expectBytes: want });
        const got = d.length - 1;
        if (got === 0) throw new Error(`Read ${path}: file ended early at ${off} of ${size} bytes`);
        out.set(d.subarray(1), off);
        off += got;
        onProgress(off, size);
      }
    } finally {
      await this.request(CMD.RDCLOSE).catch(() => {});
    }
    return out;
  }

  /** Upload bytes to path (replaces an existing file only once complete). */
  async upload(path, data, { onProgress = () => {}, signal } = {}) {
    await this.ok(CMD.WROPEN, enc.encode(path), `Create ${path}`);
    const chunk = this.maxPayload - 4;
    let off = 0;
    try {
      onProgress(0, data.length);
      while (off < data.length) {
        if (signal?.aborted) throw new AbortedError();
        const piece = data.subarray(off, Math.min(off + chunk, data.length));
        const r = await this.ok(CMD.WRITE, concat(u32(off), piece), `Write ${path}`);
        const pos = rd32(r, 1);
        if (pos !== off + piece.length) throw new Error(`Write ${path}: device at ${pos}, expected ${off + piece.length}`);
        off = pos;
        onProgress(off, data.length);
      }
    } catch (e) {
      await this.request(CMD.WRCLOSE, new Uint8Array([0])).catch(() => {});
      throw e;
    }
    await this.ok(CMD.WRCLOSE, new Uint8Array([1]), `Save ${path}`);
  }

  mkdir(path) { return this.ok(CMD.MKDIR, enc.encode(path), `Create folder ${path}`); }
  async remove(path) {
    // NOT_FOUND is success: it is also what a resend gets when the first delete's reply was lost.
    const r = await this.request(CMD.REMOVE, enc.encode(path));
    if (r[0] !== 0 && r[0] !== 2) throw new DeviceError(r[0], `Delete ${path}`);
  }
  rename(from, to) { return this.ok(CMD.RENAME, concat(enc.encode(from), new Uint8Array([0]), enc.encode(to)), `Rename ${from}`); }

  /** Delete a file, or a folder and everything in it. onStep(path) is called per item. */
  async removeTree(path, onStep = () => {}) {
    const st = await this.stat(path);
    if (!st) return;
    if (st.dir) for (const e of await this.list(path)) await this.removeTree(e.path, onStep);
    onStep(path);
    await this.remove(path);
  }

  /** mkdir -p */
  async mkdirs(path) {
    let cur = '';
    for (const part of path.split('/').filter(Boolean)) {
      cur += '/' + part;
      await this.mkdir(cur);
    }
  }

  /** Ask the device to change baud. The caller must reopen its port at `baud` and call hello(). */
  async setBaud(baud) {
    await this.ok(CMD.BAUD, u32(baud), 'Change speed');
    this.baud = baud;
  }

  reboot() { return this.ok(CMD.REBOOT, undefined, 'Reboot', { retries: 0 }); }
  bye() { return this.request(CMD.BYE, undefined, { retries: 0 }).catch(() => {}); }
}
