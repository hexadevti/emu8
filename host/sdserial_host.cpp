// sdserial_host.cpp - desktop harness for the SD file manager server (src/shared/sdserial.cpp).
//
// Runs the real server against a host directory standing in for the SD card, with stdin/stdout as
// the serial port, so the clients in tools/sdmanager can be tested without a board:
//
//   ./sdserial_host <sd-dir> [--noise]
//
// --noise mimics a busy device: it prints log lines between frames and corrupts one reply byte in
// every ~40 writes, so the client's resync/retry path gets exercised.
//
// Build:
//   g++ -O2 -std=c++17 -DSDSERIAL_HOST -Isrc/desktop/arduino_shim -o sdserial_host
//       host/sdserial_host.cpp src/shared/sdserial.cpp

#include <Arduino.h>
#include "FS.h"
#include "SD.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

std::string g_sdRoot = "./sdcard";
std::string sdHostPath(const char *sdRelative) {
  std::string rel = sdRelative ? sdRelative : "";
  if (rel.empty()) return g_sdRoot;
  if (rel[0] != '/') rel = "/" + rel;
  return g_sdRoot + rel;
}
SDClass SD;

void sdSerialPoll();

static std::mutex g_mu;
static std::deque<uint8_t> g_in;
static bool g_eof = false;
static bool g_noise = false;
static auto g_t0 = std::chrono::steady_clock::now();

int sdsHostAvailable() { std::lock_guard<std::mutex> l(g_mu); return (int)g_in.size(); }
int sdsHostRead() {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_in.empty()) return -1;
  int c = g_in.front(); g_in.pop_front(); return c;
}
void sdsHostWrite(const uint8_t *b, size_t n) {
  static unsigned count = 0;
  if (g_noise && ++count % 40 == 0 && n > 8) {
    std::string bad((const char *)b, n);
    bad[n / 2] ^= 0x5A;                       // corrupt the frame: the client must retry
    fwrite(bad.data(), 1, n, stdout);
  } else {
    fwrite(b, 1, n, stdout);
  }
  if (g_noise && count % 7 == 0) fputs("I (1234) emu8: some log line\r\n", stdout);
  fflush(stdout);
}
uint32_t sdsHostMillis() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - g_t0).count();
}
void sdsHostIdle() { std::this_thread::sleep_for(std::chrono::microseconds(200)); }

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <sd-dir> [--noise]\n", argv[0]); return 1; }
  g_sdRoot = argv[1];
  g_noise = argc > 2 && !strcmp(argv[2], "--noise");
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::thread reader([] {
    uint8_t buf[4096];
    for (;;) {
      size_t n = fread(buf, 1, 1, stdin);     // 1 byte at a time: fread blocks until it fills
      if (n == 0) break;
      std::lock_guard<std::mutex> l(g_mu);
      g_in.insert(g_in.end(), buf, buf + n);
    }
    std::lock_guard<std::mutex> l(g_mu);
    g_eof = true;
  });
  reader.detach();
  for (;;) {
    sdSerialPoll();
    {
      std::lock_guard<std::mutex> l(g_mu);
      if (g_eof && g_in.empty()) break;
    }
    sdsHostIdle();
  }
  return 0;
}
