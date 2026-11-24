// audio_web.cpp — browser audio. Replaces src/desktop/audio_sdl.cpp (and so audio_amp.cpp/speaker.cpp).
//
// Everything ends up in ONE mono float ring in wasm memory (shared with the page): an AudioWorklet
// (web/shell/audio-worklet.js) reads it through the SharedArrayBuffer and resamples to the context
// rate. Layout of the ring header (int32 words, exported by web_audio_ring()):
//   [0] write index  [1] read index  [2] source sample rate  [3] ring size (power of two)
//   [4] consumer heartbeat (bumped by the worklet every block)   then float samples.
//
//   * ampBegin/ampWriteDac8/ampWriteMono (PSG/SID/APU/TIA cores) push samples and BLOCK while more
//     than ~80 ms are buffered, like i2s_write / SDL_QueueAudio back-pressure paces those tasks.
//   * The Apple/PC 1-bit speaker is synthesized from the toggle timestamps by a producer thread,
//     the same reconstruction audio_sdl.cpp's callback does.
// Until the page's AudioContext runs (browsers need a tap first) nobody consumes the ring; the
// producer then advances the read index itself at the real-time rate so the cores keep pacing.
#if defined(BOARD_DESKTOP) && defined(EMU_WEB)

#include "emu.h"
#include <emscripten.h>
#include <atomic>
#include <thread>
#include <chrono>

#define RING_N 32768
struct AudioRing {
  std::atomic<uint32_t> w;
  std::atomic<uint32_t> r;
  std::atomic<uint32_t> rate;
  uint32_t              size;
  std::atomic<uint32_t> beat;
  uint32_t              pad[3];
  float                 data[RING_N];
};
static AudioRing g_ring;
static bool      g_ringInit = false;

static void ringInit(int rate) {
  if (!g_ringInit) {
    g_ring.w = 0; g_ring.r = 0; g_ring.size = RING_N; g_ring.beat = 0;
    g_ringInit = true;
  }
  g_ring.rate = (uint32_t)rate;
}

// Fake consumer while the page's audio is not running: keep `r` moving at the real rate.
static void virtualConsume() {
  static uint32_t lastBeat = 0;
  static int64_t lastBeatUs = 0, lastUs = 0;
  const int64_t now = esp_timer_get_time();
  const uint32_t b = g_ring.beat.load(std::memory_order_relaxed);
  if (b != lastBeat) { lastBeat = b; lastBeatUs = now; lastUs = now; return; }
  if (now - lastBeatUs < 150000) { lastUs = now; return; }   // the worklet is alive
  const int64_t dt = now - lastUs;
  const uint32_t n = (uint32_t)(dt * (int64_t)g_ring.rate.load() / 1000000);
  if (!n) return;
  lastUs = now;
  uint32_t r = g_ring.r.load(), w = g_ring.w.load();
  uint32_t avail = w - r;
  g_ring.r.store(r + (n < avail ? n : avail));
}

static void pushBlocking(const float *s, int n) {
  const uint32_t maxBuf = (uint32_t)(g_ring.rate.load() * 0.08);
  for (int i = 0; i < n;) {
    for (;;) {
      virtualConsume();
      uint32_t fill = g_ring.w.load(std::memory_order_acquire) - g_ring.r.load(std::memory_order_acquire);
      if (fill < maxBuf || !running) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    uint32_t w = g_ring.w.load(std::memory_order_relaxed);
    int chunk = n - i; if (chunk > 128) chunk = 128;
    for (int k = 0; k < chunk; k++) g_ring.data[(w + k) & (RING_N - 1)] = s[i + k];
    g_ring.w.store(w + chunk, std::memory_order_release);
    i += chunk;
  }
}

// ===================== tap (kept for API parity with audio_sdl.cpp) =====================
int desktopAudioRate() { return (int)g_ring.rate.load(); }
int desktopAudioSnapshot(float *out, int n) { for (int i = 0; i < n; i++) out[i] = 0; return n; }

// ===================== push model =====================
static bool g_ampOn = false;
void ampBegin(int sampleRate) {
  if (g_ampOn) return;
  ringInit(sampleRate);
  g_ampOn = true;
  printLog("audio: web output on");
}
void ampWriteDac8(const uint16_t *dacBuf, int n) {
  if (!g_ampOn) return;
  float tmp[128];
  for (int i = 0; i < n;) {
    int chunk = n - i; if (chunk > 128) chunk = 128;
    for (int k = 0; k < chunk; k++) tmp[k] = (float)((int)dacBuf[i + k] - 32768) / 32768.0f;
    pushBlocking(tmp, chunk);
    i += chunk;
  }
}
void ampWriteMono(const int16_t *mono, int n) {
  if (!g_ampOn) return;
  float tmp[128];
  for (int i = 0; i < n;) {
    int chunk = n - i; if (chunk > 128) chunk = 128;
    for (int k = 0; k < chunk; k++) tmp[k] = (float)mono[i + k] / 32768.0f;
    pushBlocking(tmp, chunk);
    i += chunk;
  }
}

// ===================== Apple/PC 1-bit speaker =====================
#define SPK_FS 44100
static const int TOG_RING = 8192;
static volatile uint32_t g_togTs[TOG_RING];
static volatile uint32_t g_togW = 0, g_togR = 0;

static void speakerRender(float *out, int frames) {
  static float dcInPrev = 0, dcOutPrev = 0, lp = 0;
  static uint32_t pcAcc = 0;
  static double playUs = 0; static bool anchored = false;
  static bool level = false;
  const double dt = 1.0e6 / (double)SPK_FS;
  const uint32_t LAG = 35000;
  uint32_t now = (uint32_t)micros();
  if (!anchored) { playUs = (double)(now - LAG); anchored = true; }
  int32_t gap = (int32_t)(now - (uint32_t)playUs);
  if (gap < 0 || gap > (int32_t)(LAG * 4)) playUs = (double)(now - LAG);
  bool pcxt = (currentPlatform == PLATFORM_PCXT);
  for (int k = 0; k < frames; k++) {
    int amp = (sound && !menuOpen()) ? ((int)volume << 4) : 0;
    int16_t s;
    if (pcxt) {
      int f = g_pcSpkFreq;
      if (g_pcSpkOn && f > 0) {
        pcAcc += (uint32_t)f;
        if (pcAcc >= (uint32_t)SPK_FS) pcAcc -= (uint32_t)SPK_FS;
        s = (pcAcc < (uint32_t)(SPK_FS / 2)) ? (int16_t)amp : (int16_t)(-amp);
      } else s = 0;
    } else {
      uint32_t t = (uint32_t)playUs;
      while (g_togR != g_togW && (int32_t)(g_togTs[g_togR & (TOG_RING - 1)] - t) <= 0) { level = !level; g_togR++; }
      s = level ? (int16_t)amp : (int16_t)(-amp);
    }
    float x = (float)s;
    float hp = x - dcInPrev + 0.999f * dcOutPrev;
    dcInPrev = x; dcOutPrev = hp;
    lp += 0.5f * (hp - lp);
    // x6: the 1-bit speaker's level (volume<<4) is right for the boards' amps but ~-24 dBFS here,
    // inaudible on a phone speaker.
    float v = lp * 6.0f / 32768.0f;
    out[k] = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
    playUs += dt;
  }
}

void webHiddenWait();
static bool g_spkOn = false;
void speakerSetup() {
  if (g_spkOn) return;
  ringInit(SPK_FS);
  g_spkOn = true;
  std::thread([]() {
    float buf[256];
    while (running) {
      webHiddenWait();             // page hidden: nothing to play
      speakerRender(buf, 256);
      pushBlocking(buf, 256);      // paces itself to the ring's real-time drain
    }
  }).detach();
  printLog("speaker: web 1-bit speaker on");
}

void speakerToggle() {
  speaker_state = !speaker_state;
  uint32_t w = g_togW;
  if ((uint32_t)(w - g_togR) < TOG_RING) { g_togTs[w & (TOG_RING - 1)] = (uint32_t)micros(); g_togW = w + 1; }
}

extern "C" EMSCRIPTEN_KEEPALIVE void *web_audio_ring() { return &g_ring; }

#endif // BOARD_DESKTOP && EMU_WEB

#if defined(BOARD_DESKTOP) && defined(EMU_WEB)
extern "C" EMSCRIPTEN_KEEPALIVE void web_audio_dbg(int *o) {
  o[0] = (int)g_togW; o[1] = (int)g_togR; o[2] = sound ? 1 : 0; o[3] = (int)volume; o[4] = menuOpen() ? 1 : 0; o[5] = (int)micros();
}
#endif
