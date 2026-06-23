// m0_bench.cpp - Apple IIGS feasibility gates (M0 + M0.5): PSRAM/SRAM memory benchmarks.
//
// Throwaway validation experiments. The ENTIRE file is wrapped so that, without a build flag,
// it adds nothing to the firmware (no static buffers, no code). Two flags select what runs:
//   -DIIGS_M0_BENCH   : the worst-case latency battery (B1..B9). Verdict: CONDITIONAL - the
//                       32KB D-cache is a cliff. PSRAM with locality is free (~46ns); random
//                       PSRAM over MBs is catastrophic (522ns read / 911ns write).
//   -DIIGS_M05_BENCH  : the REALISTIC follow-up. Models code-fetch (sequential + loop locality,
//                       which M0 showed is cheap) and sweeps the "scattered PSRAM data" fraction
//                       to find the break-even where the emulated 65C816 drops below 2.8MHz.
//                       Also measures bank-reload (PSRAM->SRAM copy) throughput.
//
// Run from the top of setup() (before videoSetup/tasks/IRQs) so nothing perturbs timing. Uses
// ESP.getCycleCount() (the project's timing idiom) and min-of-7 runs to reject interrupt noise.

#if defined(IIGS_M0_BENCH) || defined(IIGS_M05_BENCH)

#include <Arduino.h>
#include "esp_heap_caps.h"
#include <string.h>

// ============================================================ shared config + helpers
static const int N_RUNS = 7;          // runs per pattern; the MINIMUM delta is reported

// Region sizes are powers of two so indices mask with & (no modulo in the hot loop).
static const uint32_t SRAM_BYTES = 256u * 1024u;        // 4 hot banks ($00,$01,$E0,$E1)
static const uint32_t SRAM_MASK  = SRAM_BYTES - 1;
static const uint32_t PS_BYTES   = 4u * 1024u * 1024u;  // expansion RAM stand-in
static const uint32_t PS_MASK    = PS_BYTES - 1;

static float    NS_PER_CYC = 1000.0f / 240.0f;          // set from real CPU freq at startup
volatile uint32_t gSink = 0;                            // anti-DCE sink (printed at the end)

// 3-op xorshift32: a cheap deterministic PRNG (no Math.random, no call, no lock). Seed != 0.
static inline uint32_t xrnd(uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static void sortDeltas(uint32_t* d, int n) {            // insertion sort (n=7)
  for (int i = 1; i < n; i++) {
    uint32_t k = d[i]; int j = i - 1;
    while (j >= 0 && d[j] > k) { d[j + 1] = d[j]; j--; }
    d[j + 1] = k;
  }
}

static void showFree(const char* tag) {
  Serial.printf("free: internal=%u spiram=%u total8=%u  (%s)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), tag);
}

// One timed run; accumulates the batch result into the volatile sink to defeat dead-code elim.
#define TIME_BATCH(EXPR) do {                                  \
    uint32_t s = ESP.getCycleCount();                          \
    uint32_t snk = (EXPR);                                     \
    uint32_t e = ESP.getCycleCount();                          \
    gSink ^= snk; d[r] = e - s;                                \
  } while (0)

// ====================================================================================
// ============================== M0: worst-case latency battery =======================
// ====================================================================================
#ifdef IIGS_M0_BENCH

static const uint32_t N_FAST = 4u * 1000u * 1000u;
static const uint32_t N_SLOW = 256u * 1024u;            // (the first flashed build used 2M; per-
                                                        //  access numbers are iter-count-independent)
static const uint32_t WIN16      = 16u * 1024u;
static const uint32_t WIN16_MASK = WIN16 - 1;
static const uint32_t WIN64_MASK = (64u * 1024u) - 1;
static const uint32_t WIN_BASE   = 1u * 1024u * 1024u;

static IRAM_ATTR uint32_t batchSeqRead(const uint8_t* p, uint32_t mask, uint32_t N) {
  uint32_t acc = 0, idx = 0;
  for (uint32_t i = 0; i < N; i++) { acc += p[idx & mask]; idx++; }
  return acc;
}
static IRAM_ATTR uint32_t batchRandRead(const uint8_t* p, uint32_t mask, uint32_t N, uint32_t seed) {
  uint32_t acc = 0, r = seed;
  for (uint32_t i = 0; i < N; i++) acc += p[xrnd(r) & mask];
  return acc;
}
static IRAM_ATTR uint32_t batchSeqWrite(uint8_t* p, uint32_t mask, uint32_t N) {
  uint32_t idx = 0;
  for (uint32_t i = 0; i < N; i++) { p[idx & mask] = (uint8_t)idx; idx++; }
  return p[N & mask];
}
static IRAM_ATTR uint32_t batchRandWrite(uint8_t* p, uint32_t mask, uint32_t N, uint32_t seed) {
  uint32_t r = seed;
  for (uint32_t i = 0; i < N; i++) { uint32_t v = xrnd(r); p[v & mask] = (uint8_t)v; }
  return p[r & mask];
}
static IRAM_ATTR uint32_t batchMixed(uint8_t* sram, uint32_t smask,
                                     uint8_t* psWin, uint32_t wmask, uint32_t N, uint32_t seed) {
  uint32_t acc = 0, r = seed, wc = 0;
  for (uint32_t i = 0; i < N; i++) {
    uint32_t v = xrnd(r);
    bool toSram  = v & 1;
    bool isWrite = (wc == 0); if (++wc == 5) wc = 0;
    uint32_t a = v >> 1;
    if (toSram) { if (isWrite) sram[a & smask]  = (uint8_t)v; else acc += sram[a & smask]; }
    else        { if (isWrite) psWin[a & wmask] = (uint8_t)v; else acc += psWin[a & wmask]; }
  }
  return acc;
}

static uint8_t* bm_bankPtr[256];
static inline bool bm_isIObank(uint8_t b) { return b == 0x00 || b == 0x01; }
static IRAM_ATTR uint8_t bm_readIO(uint8_t b, uint16_t o)            { (void)b; (void)o; return 0; }
static IRAM_ATTR void    bm_writeIO(uint8_t b, uint16_t o, uint8_t v){ (void)b; (void)o; (void)v; }
static IRAM_ATTR uint8_t bm_read24(uint32_t a) {
  uint8_t bank = (uint8_t)(a >> 16); uint16_t off = (uint16_t)a;
  if ((off & 0xF000) == 0xC000 && bm_isIObank(bank)) return bm_readIO(bank, off);
  uint8_t* p = bm_bankPtr[bank];
  return p ? p[off] : 0;
}
static IRAM_ATTR void bm_write24(uint32_t a, uint8_t v) {
  uint8_t bank = (uint8_t)(a >> 16); uint16_t off = (uint16_t)a;
  if ((off & 0xF000) == 0xC000 && bm_isIObank(bank)) { bm_writeIO(bank, off, v); return; }
  uint8_t* p = bm_bankPtr[bank];
  if (p) p[off] = v;
}
static void bm_initBankPtr(uint8_t* sram, uint8_t* ps) {
  for (int b = 0; b < 256; b++) bm_bankPtr[b] = nullptr;
  for (int b = 0; b < 4;  b++) bm_bankPtr[b]      = sram + (size_t)b * 0x10000;
  for (int b = 0; b < 64; b++) bm_bankPtr[16 + b] = ps   + (size_t)b * 0x10000;
}
static IRAM_ATTR uint32_t batchMixed24(uint32_t N, uint32_t seed) {
  uint32_t acc = 0, r = seed, wc = 0;
  for (uint32_t i = 0; i < N; i++) {
    uint32_t v = xrnd(r);
    bool toSram  = v & 1;
    bool isWrite = (wc == 0); if (++wc == 5) wc = 0;
    uint32_t addr = toSram ? ((((v >> 1) & 3) << 16) | ((v >> 3) & 0xFFFF))
                           : (((uint32_t)16 << 16)    | ((v >> 1) & 0xFFFF));
    if (isWrite) bm_write24(addr, (uint8_t)v); else acc += bm_read24(addr);
  }
  return acc;
}

static void reportM0(const char* name, uint32_t* d, int n, uint32_t N) {
  sortDeltas(d, n);
  uint32_t mn = d[0], md = d[n / 2], mx = d[n - 1];
  float ns = (float)mn * NS_PER_CYC;
  Serial.printf("M0 %-24s min=%-9lu med=%-9lu max=%-9lu cyc | %6.2f ns/acc | %5.2f cyc/acc | %7.2f Macc/s\n",
                name, (unsigned long)mn, (unsigned long)md, (unsigned long)mx,
                ns / N, (float)mn / N, (float)N / ns * 1000.0f);
}

void runIIgsM0Bench() {
  NS_PER_CYC = 1000.0f / (float)ESP.getCpuFreqMHz();
  Serial.println();
  Serial.println("=== IIGS M0 PSRAM/SRAM latency benchmark ===");
  Serial.printf("CPU %u MHz (%.3f ns/cyc), %d runs/pattern (min reported)\n",
                (unsigned)ESP.getCpuFreqMHz(), NS_PER_CYC, N_RUNS);
  showFree("before alloc");
  uint8_t* sram = (uint8_t*)heap_caps_malloc(SRAM_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint8_t* ps   = (uint8_t*)ps_malloc(PS_BYTES);
  if (!sram || !ps) { Serial.printf("ALLOC FAIL sram=%p ps=%p -- abort\n", sram, ps); return; }
  showFree("after alloc");
  memset(sram, 0, SRAM_BYTES); memset(ps, 0, PS_BYTES);
  uint8_t* psWindow = ps + WIN_BASE;
  uint32_t d[N_RUNS];

  Serial.println("--- baselines (internal SRAM) ---");
  batchSeqRead(sram, SRAM_MASK, N_FAST);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchSeqRead(sram, SRAM_MASK, N_FAST));
  reportM0("B1 SRAM seq read", d, N_RUNS, N_FAST);
  batchRandRead(sram, SRAM_MASK, N_FAST, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchRandRead(sram, SRAM_MASK, N_FAST, 0xC0DECAFE + r));
  reportM0("B2 SRAM rand read", d, N_RUNS, N_FAST);
  Serial.println("--- PSRAM reads ---");
  batchSeqRead(ps, PS_MASK, N_FAST);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchSeqRead(ps, PS_MASK, N_FAST));
  reportM0("B3 PSRAM seq read", d, N_RUNS, N_FAST);
  batchRandRead(ps, PS_MASK, N_SLOW, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchRandRead(ps, PS_MASK, N_SLOW, 0xC0DECAFE + r));
  reportM0("B4 PSRAM rand 4MB read", d, N_RUNS, N_SLOW);
  batchRandRead(psWindow, WIN16_MASK, N_FAST, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchRandRead(psWindow, WIN16_MASK, N_FAST, 0xC0DECAFE + r));
  reportM0("B5 PSRAM rand 16K win", d, N_RUNS, N_FAST);
  Serial.println("--- PSRAM writes ---");
  batchSeqWrite(ps, PS_MASK, N_FAST);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchSeqWrite(ps, PS_MASK, N_FAST));
  reportM0("B6 PSRAM seq write", d, N_RUNS, N_FAST);
  batchRandWrite(ps, PS_MASK, N_SLOW, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchRandWrite(ps, PS_MASK, N_SLOW, 0xC0DECAFE + r));
  reportM0("B7 PSRAM rand 4MB write", d, N_RUNS, N_SLOW);
  Serial.println("--- the gate ---");
  batchMixed(sram, SRAM_MASK, psWindow, WIN64_MASK, N_FAST, 1);
  for (int r = 0; r < N_RUNS; r++)
    TIME_BATCH(batchMixed(sram, SRAM_MASK, psWindow, WIN64_MASK, N_FAST, 0xC0DECAFE + r));
  reportM0("B8 MIXED (gate)", d, N_RUNS, N_FAST);
  bm_initBankPtr(sram, ps);
  batchMixed24(N_SLOW, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchMixed24(N_SLOW, 0xC0DECAFE + r));
  reportM0("B9 MIXED via read24", d, N_RUNS, N_SLOW);
  Serial.printf("M0 sink=%08x (anti-DCE proof)\n", (unsigned)gSink);
  Serial.println("=== M0 end ===");
  free(sram); free(ps);   // so a looping caller doesn't leak the 256KB+4MB each iteration
}
#endif // IIGS_M0_BENCH

// ====================================================================================
// ===================== M0.5: realistic mix + scatter-fraction sweep ==================
// ====================================================================================
#ifdef IIGS_M05_BENCH

static const uint32_t N_CODE = 4u * 1000u * 1000u;   // code-stream patterns (cheap)
static const uint32_t N_MIX  = 2u * 1000u * 1000u;   // per scatter-fraction
static const uint32_t SCATTER_REGION_MASK = (1u * 1024u * 1024u) - 1;  // 1MB random data region

// Models 65C816 code fetch from a PSRAM bank: advance 1-2 bytes (opcode+operand), loop back
// within a small window (hot loop -> stays in the 32KB HW cache), and every `relocEvery`
// accesses jump to a fresh cold window (far call/branch into uncached code).
static IRAM_ATTR uint32_t batchCodeStream(const uint8_t* ps, uint32_t mask, uint32_t N,
                                          uint32_t winSize, uint32_t relocEvery, uint32_t seed) {
  uint32_t acc = 0, r = seed, base = 0, off = 0, rc = relocEvery;
  for (uint32_t i = 0; i < N; i++) {
    acc += ps[(base + off) & mask];
    off += 1 + (xrnd(r) & 1);
    if (off >= winSize) off = 0;
    if (--rc == 0) { base = (xrnd(r) & mask) & ~(winSize - 1); rc = relocEvery; }
  }
  return acc;
}

// The realistic access mix. Each access is drawn into one of three buckets:
//   ~55% SRAM hot bank (zero page / stack / low RAM)                       -> cheap
//   middle: PSRAM code fetch (sequential window, loop locality)            -> cheap (HW-cached)
//   top `f`: PSRAM scattered DATA, random over 1MB (>> 32KB cache)         -> expensive
// scatterThresh = (1-f) * 2^32 sets the scattered fraction f.
static IRAM_ATTR uint32_t batchMixSweep(uint8_t* sram, uint32_t smask, uint8_t* ps, uint32_t psmask,
                                        uint32_t codeWin, uint32_t scatterThresh,
                                        uint32_t N, uint32_t seed) {
  uint32_t acc = 0, r = seed, cbase = 0, coff = 0, crc = 64;
  const uint32_t SRAM_T = 0x8CCCCCCCu;   // ~55% of 2^32 go to the SRAM hot bank
  for (uint32_t i = 0; i < N; i++) {
    uint32_t draw = xrnd(r);
    if (draw < SRAM_T) {                                  // SRAM hot bank
      acc += sram[(draw >> 1) & smask];
    } else if (draw < scatterThresh) {                    // PSRAM code (sequential, local)
      acc += ps[(cbase + coff) & psmask];
      if (++coff >= codeWin) coff = 0;
      if (--crc == 0) { cbase = (xrnd(r) & psmask) & ~(codeWin - 1); crc = 64; }
    } else {                                              // PSRAM scattered data (random 1MB)
      acc += ps[xrnd(r) & SCATTER_REGION_MASK];
    }
  }
  return acc;
}

// Print effective cost AND the resulting sustainable emulated-MHz at a few interpreter-overhead
// assumptions (host cycles of decode+execute per emulated cycle). 2.8MHz is the IIGS target.
static void reportMix(const char* name, uint32_t* d, int n, uint32_t N) {
  sortDeltas(d, n);
  float cyc = (float)d[0] / N, ns = cyc * NS_PER_CYC;
  Serial.printf("M05 %-26s %6.2f ns/acc | %6.2f cyc/acc | MHz@Cint{15,25,40}= %4.2f / %4.2f / %4.2f\n",
                name, ns, cyc, 240.0f / (cyc + 15), 240.0f / (cyc + 25), 240.0f / (cyc + 40));
}

void runIIgsM05Bench() {
  NS_PER_CYC = 1000.0f / (float)ESP.getCpuFreqMHz();
  Serial.println();
  Serial.println("=== IIGS M0.5 realistic-mix + scatter sweep ===");
  Serial.printf("CPU %u MHz (%.3f ns/cyc), %d runs/pattern (min). MHz = 240/(cyc/acc + Cinterp).\n",
                (unsigned)ESP.getCpuFreqMHz(), NS_PER_CYC, N_RUNS);
  showFree("before alloc");
  uint8_t* sram = (uint8_t*)heap_caps_malloc(SRAM_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint8_t* ps   = (uint8_t*)ps_malloc(PS_BYTES);
  if (!sram || !ps) { Serial.printf("ALLOC FAIL sram=%p ps=%p -- abort\n", sram, ps); return; }
  showFree("after alloc");
  memset(sram, 0, SRAM_BYTES); memset(ps, 0, PS_BYTES);
  uint32_t d[N_RUNS];

  Serial.println("--- code fetch from PSRAM (sequential + loop locality) ---");
  batchCodeStream(ps, PS_MASK, N_CODE, 8 * 1024, 256, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchCodeStream(ps, PS_MASK, N_CODE, 8 * 1024, 256, 0xC0DECAFE + r));
  reportMix("C1 code 8K win, reloc/256", d, N_RUNS, N_CODE);
  batchCodeStream(ps, PS_MASK, N_CODE, 8 * 1024, 32, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchCodeStream(ps, PS_MASK, N_CODE, 8 * 1024, 32, 0xC0DECAFE + r));
  reportMix("C2 code 8K win, reloc/32", d, N_RUNS, N_CODE);

  Serial.println("--- realistic mix: 55% SRAM + code + f%% scattered PSRAM data ---");
  const float fr[] = { 0.00f, 0.05f, 0.10f, 0.20f, 0.35f };
  for (int k = 0; k < 5; k++) {
    uint32_t thr = (fr[k] <= 0.0f) ? 0xFFFFFFFFu
                                   : (uint32_t)((double)(1.0f - fr[k]) * 4294967296.0);
    batchMixSweep(sram, SRAM_MASK, ps, PS_MASK, 8 * 1024, thr, N_MIX, 1);
    for (int r = 0; r < N_RUNS; r++)
      TIME_BATCH(batchMixSweep(sram, SRAM_MASK, ps, PS_MASK, 8 * 1024, thr, N_MIX, 0xC0DECAFE + r));
    char nm[40]; snprintf(nm, sizeof(nm), "MIX scattered=%2d%%", (int)(fr[k] * 100 + 0.5f));
    reportMix(nm, d, N_RUNS, N_MIX);
  }

  Serial.println("--- bank-reload throughput (PSRAM -> SRAM copy) ---");
  for (int sz = 0; sz < 2; sz++) {
    uint32_t bytes = (sz == 0) ? 64u * 1024u : 4u * 1024u;
    uint32_t best = 0xFFFFFFFF, rseed = 0xBEEF;
    for (int r = 0; r < N_RUNS; r++) {
      uint32_t src = xrnd(rseed) & (PS_MASK & ~(bytes - 1));   // bytes-aligned, in range
      uint32_t s = ESP.getCycleCount();
      memcpy(sram, ps + src, bytes);
      uint32_t e = ESP.getCycleCount();
      gSink ^= sram[r & SRAM_MASK];
      if ((e - s) < best) best = e - s;
    }
    float ns = (float)best * NS_PER_CYC;
    Serial.printf("M05 reload %5uB  %8.0f ns | %6.1f MB/s | ~%.2f us/bank-switch\n",
                  (unsigned)bytes, ns, (float)bytes / ns * 1000.0f, ns / 1000.0f);
  }

  Serial.printf("M05 sink=%08x (anti-DCE proof)\n", (unsigned)gSink);
  Serial.println("Read: find the scattered%% where MHz crosses 2.8 -> that's the locality budget.");
  Serial.println("=== M0.5 end ===");
  free(sram); free(ps);   // so the looping caller doesn't leak the 256KB+4MB each iteration
}
#endif // IIGS_M05_BENCH

#endif // IIGS_M0_BENCH || IIGS_M05_BENCH
