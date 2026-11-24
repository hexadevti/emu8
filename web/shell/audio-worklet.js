// AudioWorklet: drains emu8's mono float ring (in the wasm SharedArrayBuffer) and resamples it to
// the context rate. Header (int32): [0] write idx, [1] read idx, [2] source rate, [3] size, [4] heartbeat;
// samples (float32) start 32 bytes after the header.
class Emu8Ring extends AudioWorkletProcessor {
  constructor() {
    super();
    this.hdr = null; this.data = null; this.pos = 0; this.last = 0;
    this.port.onmessage = (e) => {
      const { buf, ptr } = e.data;
      this.hdr = new Int32Array(buf, ptr, 8);
      this.size = Atomics.load(this.hdr, 3) >>> 0 || 32768;
      this.data = new Float32Array(buf, ptr + 32, this.size);
    };
  }
  process(_in, outputs) {
    const out = outputs[0][0];
    if (!this.hdr) { out.fill(0); return true; }
    const h = this.hdr;
    Atomics.add(h, 4, 1);                                   // heartbeat: "a real consumer is here"
    const rate = Atomics.load(h, 2) >>> 0;
    if (!rate) { out.fill(0); return true; }
    const step = rate / sampleRate;
    const mask = this.size - 1;
    let r = Atomics.load(h, 1) >>> 0;
    const w = Atomics.load(h, 0) >>> 0;
    let avail = (w - r) >>> 0;
    // keep latency bounded: if far too much is queued, skip ahead
    const maxQ = rate * 0.15;
    if (avail > maxQ) { const skip = (avail - rate * 0.06) | 0; r = (r + skip) >>> 0; avail -= skip; }
    let pos = this.pos;
    for (let i = 0; i < out.length; i++) {
      if (avail >= 2) {
        const a = this.data[r & mask], b = this.data[(r + 1) & mask];
        const v = a + (b - a) * pos;
        out[i] = v; this.last = v;
        pos += step;
        while (pos >= 1 && avail >= 2) { pos -= 1; r = (r + 1) >>> 0; avail--; }
      } else {
        this.last *= 0.995; out[i] = this.last;           // underrun: fade instead of clicking
      }
    }
    this.pos = pos;
    Atomics.store(h, 1, r | 0);
    for (let c = 1; c < outputs[0].length; c++) outputs[0][c].set(out);
    return true;
  }
}
registerProcessor('emu8-ring', Emu8Ring);
