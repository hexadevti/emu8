// End-to-end test: the web app's protocol client against the real firmware server code
// (src/shared/sdserial.cpp built as host/sdserial_host.cpp), talking over the child's stdio.
//
//   node tools/sdmanager/test/protocol.test.mjs <path-to-sdserial_host[.exe]>
//
// Runs every scenario twice: on a clean line, then with --noise (log lines between frames and
// corrupted replies), which forces the resync + retry paths.

import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, readdirSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import assert from 'node:assert/strict';
import { Emu8Link, DeviceError, AbortedError } from '../web/protocol.js';

const exe = process.argv[2];
if (!exe) { console.error('usage: node protocol.test.mjs <sdserial_host>'); process.exit(2); }

function randomBytes(n, seed) {
  const out = new Uint8Array(n);
  let x = seed >>> 0 || 1;
  for (let i = 0; i < n; i++) { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; out[i] = x & 0xff; }
  return out;
}

async function run(noise) {
  const sd = mkdtempSync(join(tmpdir(), 'emu8sd-'));
  const child = spawn(exe, noise ? [sd, '--noise'] : [sd], { stdio: ['pipe', 'pipe', 'inherit'] });
  const logs = [];
  const link = new Emu8Link(b => { child.stdin.write(b); }, { onLog: l => logs.push(l), baud: 1_000_000 });
  child.stdout.on('data', d => link.receive(new Uint8Array(d)));
  const label = noise ? 'noisy line' : 'clean line';
  const step = async (name, fn) => { await fn(); console.log(`  ok  ${label}: ${name}`); };

  try {
    await step('hello', async () => {
      const info = await link.hello();
      assert.equal(info.version, 1);
      assert.equal(info.sdMounted, true);
      assert.equal(info.maxPayload, 4096);
    });

    await step('mkdirs + empty listing', async () => {
      await link.mkdirs('/roms/nes');
      await link.mkdir('/roms/nes');                       // idempotent
      assert.deepEqual(await link.list('/roms/nes'), []);
    });

    const big = randomBytes(300_000, 7);
    await step('upload + download 300 KB round trip', async () => {
      let last = 0;
      await link.upload('/roms/nes/game.nes', big, { onProgress: d => { assert.ok(d >= last); last = d; } });
      assert.equal(last, big.length);
      const back = await link.download('/roms/nes/game.nes');
      assert.equal(back.length, big.length);
      assert.ok(Buffer.from(back).equals(Buffer.from(big)), 'content differs');
      assert.deepEqual(readdirSync(join(sd, 'roms/nes')), ['game.nes']);   // no .part left
    });

    await step('empty file', async () => {
      await link.upload('/empty.bin', new Uint8Array(0));
      assert.deepEqual(await link.stat('/empty.bin'), { dir: false, size: 0 });
      assert.equal((await link.download('/empty.bin')).length, 0);
    });

    await step('overwrite keeps only the new content', async () => {
      const v2 = randomBytes(5000, 99);
      await link.upload('/roms/nes/game.nes', v2);
      const back = await link.download('/roms/nes/game.nes');
      assert.ok(Buffer.from(back).equals(Buffer.from(v2)));
    });

    await step('cancelled upload leaves the old file alone', async () => {
      const ctl = new AbortController();
      const junk = randomBytes(100_000, 3);
      await assert.rejects(
        link.upload('/roms/nes/game.nes', junk, { signal: ctl.signal, onProgress: d => { if (d > 20000) ctl.abort(); } }),
        AbortedError);
      assert.equal((await link.stat('/roms/nes/game.nes')).size, 5000);
      assert.ok(!existsSync(join(sd, 'roms/nes/game.nes.part')));
    });

    await step('listing pages through 300 entries', async () => {
      await link.mkdir('/many');
      for (let i = 0; i < 300; i++)
        await link.upload(`/many/a_rather_long_file_name_to_fill_pages_${String(i).padStart(3, '0')}.dsk`, randomBytes(i, i + 1));
      const list = await link.list('/many');
      assert.equal(list.length, 300);
      const names = new Set(list.map(e => e.name));
      assert.equal(names.size, 300);
      const e17 = list.find(e => e.name.endsWith('_017.dsk'));
      assert.equal(e17.size, 17);
      assert.equal(e17.path, '/many/' + e17.name);
    });

    await step('stat, rename, dir flags', async () => {
      assert.equal(await link.stat('/nope'), null);
      assert.deepEqual(await link.stat('/roms'), { dir: true, size: 0 });
      await link.rename('/empty.bin', '/roms/renamed.bin');
      assert.equal(await link.stat('/empty.bin'), null);
      assert.ok(await link.stat('/roms/renamed.bin'));
      // A resend after a lost reply finds the work already done and must not report failure.
      await link.rename('/empty.bin', '/roms/renamed.bin');
      // Case-only rename (FAT sees the target as "existing").
      await link.rename('/roms/renamed.bin', '/roms/RENAMED.bin');
      assert.ok((await link.list('/roms')).find(e => e.name === 'RENAMED.bin'));
      await link.rename('/roms/RENAMED.bin', '/roms/renamed.bin');
      const root = await link.list('/');
      assert.ok(root.find(e => e.name === 'roms' && e.dir));
    });

    await step('errors are reported, not hung', async () => {
      await assert.rejects(link.download('/missing.rom'), e => e instanceof DeviceError && e.status === 2);
      await assert.rejects(link.list('/roms/../..'), e => e instanceof DeviceError && e.status === 4);
      await assert.rejects(link.rename('/roms', '/many'), DeviceError);   // target exists
      await assert.rejects(link.remove('/roms'), DeviceError);            // not empty
    });

    await step('recursive delete', async () => {
      await link.removeTree('/many');
      await link.removeTree('/roms');
      assert.deepEqual(await link.list('/'), []);
    });

    await step('bye', async () => { await link.bye(); });
    if (noise) assert.ok(logs.some(l => l.includes('some log line')), 'device log lines should reach onLog');
  } finally {
    child.stdin.end();
    child.kill();
    rmSync(sd, { recursive: true, force: true });
  }
}

await run(false);
await run(true);
console.log('all protocol tests passed');
