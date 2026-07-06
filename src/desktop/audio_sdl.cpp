// audio_sdl.cpp — desktop SDL2 audio. Replaces src/shared/audio_amp.cpp AND src/shared/speaker.cpp.
//
//   * PSG / SID / APU / TIA cores push samples via ampBegin/ampWriteDac8/ampWriteMono (unchanged
//     *_audio.cpp tasks). We SDL_QueueAudio them and BLOCK the producer while the queue is full —
//     reproducing the i2s_write back-pressure that paces those tasks.
//   * The Apple/PC 1-bit speaker is synthesized per-sample from speaker_state in an SDL audio
//     callback (like the device's hardware-timer ISR), with the same DC-blocker + low-pass shaping.
//
// Only one audio source runs per platform, so the two SDL devices are mutually exclusive.
#if defined(BOARD_DESKTOP)

#include "../../emu.h"
#include <SDL.h>

// ===================== audio tap (read-only, for the ImGui spectrum analyzer) =====================
// Mirrors each FINAL output sample into a ring AFTER it is written to the SDL buffer — it can never
// change what's played. The UI grabs the most-recent window for an FFT.
#define ATAP_N 4096
static int16_t           g_atap[ATAP_N];
static volatile uint32_t g_atapW = 0;
static int               g_atapRate = 44100;
static inline void audioTap(int16_t s) { g_atap[g_atapW & (ATAP_N - 1)] = s; g_atapW++; }
int desktopAudioRate() { return g_atapRate; }
int desktopAudioSnapshot(float *out, int n) {
  if (n > ATAP_N) n = ATAP_N;
  uint32_t w = g_atapW;
  for (int i = 0; i < n; i++)
    out[i] = (float)g_atap[(w - (uint32_t)n + (uint32_t)i) & (ATAP_N - 1)] / 32768.0f;
  return n;
}

// ===================== push model: ampBegin / ampWriteDac8 / ampWriteMono =====================
static SDL_AudioDeviceID g_ampDev = 0;
static int               g_ampRate = 44100;

void ampBegin(int sampleRate) {
  if (g_ampDev) return;                         // idempotent across cores (one platform at a time)
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) SDL_InitSubSystem(SDL_INIT_AUDIO);
  g_ampRate = sampleRate;
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = sampleRate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  want.callback = nullptr;                      // queue mode (SDL_QueueAudio)
  g_ampDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!g_ampDev) { printLog("audio: SDL_OpenAudioDevice failed"); return; }
  g_ampRate = have.freq;
  g_atapRate = g_ampRate;
  SDL_PauseAudioDevice(g_ampDev, 0);
  printLog("audio: SDL output on");
}

// Block until the queued audio falls below ~80 ms — mimics i2s_write blocking on a full DMA, so the
// core's audio task self-paces. Bails out if `running` is cleared (shutdown).
static void ampBackpressure() {
  if (!g_ampDev) return;
  const Uint32 maxBytes = (Uint32)(g_ampRate * 2 /*ch*/ * 2 /*bytes*/ * 0.08);
  while (running && SDL_GetQueuedAudioSize(g_ampDev) > maxBytes) SDL_Delay(1);
}

void ampWriteDac8(const uint16_t *dacBuf, int n) {
  if (!g_ampDev) return;
  int16_t st[256];                              // up to 128 frames x2
  int i = 0;
  while (i < n) {
    int chunk = (n - i > 128) ? 128 : (n - i);
    for (int k = 0; k < chunk; k++) {
      int16_t s = (int16_t)((int)dacBuf[i + k] - 32768);   // 0x8000-centered unsigned -> signed
      st[k * 2] = s; st[k * 2 + 1] = s; audioTap(s);
    }
    ampBackpressure();
    SDL_QueueAudio(g_ampDev, st, chunk * 2 * sizeof(int16_t));
    i += chunk;
  }
}

void ampWriteMono(const int16_t *mono, int n) {
  if (!g_ampDev) return;
  int16_t st[256];
  int i = 0;
  while (i < n) {
    int chunk = (n - i > 128) ? 128 : (n - i);
    for (int k = 0; k < chunk; k++) { st[k * 2] = mono[i + k]; st[k * 2 + 1] = mono[i + k]; audioTap(mono[i + k]); }
    ampBackpressure();
    SDL_QueueAudio(g_ampDev, st, chunk * 2 * sizeof(int16_t));
    i += chunk;
  }
}

// ===================== Apple/PC 1-bit speaker =====================
// The SDL pull-callback fills a whole buffer in microseconds, so it CANNOT sample the live 1-bit
// speaker_state per output sample (every sample would read the same value -> the waveform collapses
// to the callback rate ~86 Hz -> aliased garbage). Instead the CPU records the micros() TIMESTAMP of
// every speaker_state flip (speakerToggle), and the callback reconstructs the square wave from that
// event timeline at the true 44100 Hz (consuming toggles up to each sample's time). This mirrors the
// device's hardware-timer sampling. (PC-XT stays frequency-synthesized; that path is already correct.)
#define SPK_FS 44100
static SDL_AudioDeviceID g_spkDev = 0;

static const int TOG_RING = 8192;          // speaker flip-timestamp ring (1-bit speaker is sub-kHz)
static volatile uint32_t g_togTs[TOG_RING];
static volatile uint32_t g_togW = 0, g_togR = 0;

static void speakerCallback(void * /*ud*/, Uint8 *stream, int len) {
  int16_t *out = (int16_t *)stream;
  int frames = len / (int)(2 * sizeof(int16_t));        // stereo
  static float dcInPrev = 0, dcOutPrev = 0, lp = 0;
  static uint32_t pcAcc = 0;
  static double playUs = 0; static bool anchored = false;
  static bool level = false;
  const double dt = 1.0e6 / (double)SPK_FS;             // 22.675 us per sample
  const uint32_t LAG = 35000;                           // render ~35 ms behind real time (toggles recorded)
  uint32_t now = (uint32_t)micros();
  if (!anchored) { playUs = (double)(now - LAG); anchored = true; }
  int32_t gap = (int32_t)(now - (uint32_t)playUs);      // resync on big drift (prevents divergence)
  if (gap < 0 || gap > (int32_t)(LAG * 4)) playUs = (double)(now - LAG);

  bool pcxt = (currentPlatform == PLATFORM_PCXT);
  for (int k = 0; k < frames; k++) {
    int amp = sound ? ((int)volume << 4) : 0;           // volume 0..0xF0 -> 0..~3840
    int16_t s;
    if (pcxt) {
      int f = g_pcSpkFreq;
      if (g_pcSpkOn && f > 0) {
        pcAcc += (uint32_t)f;
        if (pcAcc >= (uint32_t)SPK_FS) pcAcc -= (uint32_t)SPK_FS;
        s = (pcAcc < (uint32_t)(SPK_FS / 2)) ? (int16_t)amp : (int16_t)(-amp);
      } else s = 0;
    } else {
      uint32_t t = (uint32_t)playUs;                    // consume all flips up to this sample's time
      while (g_togR != g_togW && (int32_t)(g_togTs[g_togR & (TOG_RING - 1)] - t) <= 0) {
        level = !level; g_togR++;
      }
      s = level ? (int16_t)amp : (int16_t)(-amp);
    }
    float x  = (float)s;
    float hp = x - dcInPrev + 0.999f * dcOutPrev;       // DC blocker (~10 Hz)
    dcInPrev = x; dcOutPrev = hp;
    lp += 0.5f * (hp - lp);                             // gentle low-pass
    int v = (int)lp;
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    out[k * 2] = (int16_t)v; out[k * 2 + 1] = (int16_t)v; audioTap((int16_t)v);
    playUs += dt;
  }
}

void speakerSetup() {
  if (g_spkDev) return;
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = SPK_FS;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  want.callback = speakerCallback;
  g_spkDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!g_spkDev) { printLog("speaker: SDL_OpenAudioDevice failed"); return; }
  g_atapRate = SPK_FS;
  SDL_PauseAudioDevice(g_spkDev, 0);
  printLog("speaker: SDL 1-bit speaker on");
}

void speakerToggle() {
  speaker_state = !speaker_state;
  uint32_t w = g_togW;                                  // record the flip time for the callback to replay
  if ((uint32_t)(w - g_togR) < TOG_RING) { g_togTs[w & (TOG_RING - 1)] = (uint32_t)micros(); g_togW = w + 1; }
}

#endif // BOARD_DESKTOP
