// Arduino.h — desktop (Windows/Linux, SDL2) shim of the Arduino-core + ESP-IDF + FreeRTOS API.
//
// This is the heart of the BOARD_DESKTOP HAL: it lets the UNMODIFIED emulator/shared code compile
// off-device by providing host implementations of every Arduino/ESP/FreeRTOS symbol the codebase
// touches (inventoried from the tree). It is pulled in only on the desktop build (this directory is
// prepended to the include path; on the device the real <Arduino.h> wins). Trivial things are inline
// here; instances and the FreeRTOS-on-std::thread layer live in src/desktop/hal.cpp.
//
// Fidelity notes: built 32-bit (-m32 / ILP32) so long/pointer widths match the ESP32 (Xtensa LX).
// millis/micros/getCycleCount come from a steady clock so the cores' real-time pacing still throttles.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>

// ---------------------------------------------------------------------------
// Arduino integer/bool aliases
// ---------------------------------------------------------------------------
typedef uint8_t  boolean;
typedef uint8_t  byte;
typedef uint16_t word;
#ifndef _SYS_TYPES_H            // POSIX <sys/types.h> may already define these on Linux
  #ifndef __ushort_defined
typedef unsigned short ushort;
  #endif
#endif

// ---------------------------------------------------------------------------
// GPIO / digital / analog — all no-ops on the desktop (no real pins)
// ---------------------------------------------------------------------------
#define LOW            0x0
#define HIGH           0x1
#define INPUT          0x0
#define OUTPUT         0x1
#define INPUT_PULLUP   0x2
#define INPUT_PULLDOWN 0x3
#define RISING         0x1
#define FALLING        0x2
#define CHANGE         0x3

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int  digitalRead(int) { return 0; }
inline int  analogRead(int) { return 0; }       // touch/joystick analog paths are compiled out anyway
inline void analogWrite(int, int) {}
inline void dacWrite(int, int) {}
inline int  digitalPinToInterrupt(int p) { return p; }
inline void attachInterrupt(int, void (*)(), int) {}
inline void detachInterrupt(int) {}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
unsigned long millis();                 // ms since first call to the time base (hal.cpp)
unsigned long micros();                 // us since the time base (wraps at 2^32 like the ESP)
int64_t       esp_timer_get_time();     // us, 64-bit
inline void   delay(unsigned long ms)         { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void   delayMicroseconds(unsigned int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
inline void   yield()                         { std::this_thread::yield(); }

// map()/constrain() (Arduino macros). min/max are NOT redefined here so <algorithm>'s std::min/max
// keep working; the codebase includes <algorithm>.
long map(long x, long in_min, long in_max, long out_min, long out_max);
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif

// random() — Arduino style
long random(long howbig);
long random(long howsmall, long howbig);
void randomSeed(unsigned long);

// ---------------------------------------------------------------------------
// PROGMEM / attributes / flash helpers — all transparent on a flat address space
// ---------------------------------------------------------------------------
#define PROGMEM
#define PGM_P            const char *
#define PGM_VOID_P       const void *
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define ICACHE_RAM_ATTR
#ifndef F
#define F(x) (x)
#endif
#define FPSTR(x) (x)
#define PSTR(x)  (x)
#define pgm_read_byte(addr)   (*(const uint8_t  *)(addr))
#define pgm_read_word(addr)   (*(const uint16_t *)(addr))
#define pgm_read_dword(addr)  (*(const uint32_t *)(addr))
#define pgm_read_float(addr)  (*(const float    *)(addr))
#define pgm_read_ptr(addr)    (*(void * const *)(addr))
#define pgm_read_byte_near(addr) pgm_read_byte(addr)
#define pgm_read_word_near(addr) pgm_read_word(addr)
#define memcpy_P  memcpy
#define memcmp_P  memcmp
#define strcpy_P  strcpy
#define strncpy_P strncpy
#define strcmp_P  strcmp
#define strncmp_P strncmp
#define strlen_P  strlen

// Number bases for String/Serial print
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

// ---------------------------------------------------------------------------
// String — wrapper over std::string covering the method surface the codebase uses
// ---------------------------------------------------------------------------
class String {
  std::string s;
public:
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &x) : s(x) {}
  String(char c) : s(1, c) {}
  String(unsigned char v) : s(std::to_string((unsigned)v)) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned int v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(float v) : s(std::to_string(v)) {}
  String(double v) : s(std::to_string(v)) {}
  String(long v, int base) { char b[40]; fmtBase(b, (long)v, base); s = b; }
  String(int v, int base)  { char b[40]; fmtBase(b, (long)v, base); s = b; }

  const char  *c_str() const { return s.c_str(); }
  unsigned     length() const { return (unsigned)s.size(); }
  bool         isEmpty() const { return s.empty(); }
  char         charAt(unsigned i) const { return i < s.size() ? s[i] : 0; }
  char         operator[](int i) const { return (i >= 0 && (size_t)i < s.size()) ? s[i] : 0; }
  char        &operator[](int i) { return s[i]; }

  String substring(unsigned from) const { return from <= s.size() ? String(s.substr(from)) : String(); }
  String substring(unsigned from, unsigned to) const {           // Arduino: [from, to)
    if (from > s.size()) return String();
    if (to > s.size()) to = (unsigned)s.size();
    if (to < from) to = from;
    return String(s.substr(from, to - from));
  }
  int indexOf(char c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(char c, unsigned from) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const String &o) const { auto p = s.find(o.s); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const String &o, unsigned from) const { auto p = s.find(o.s, from); return p == std::string::npos ? -1 : (int)p; }
  int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  bool startsWith(const String &p) const { return s.rfind(p.s, 0) == 0; }
  bool endsWith(const String &suf) const {
    return suf.s.size() <= s.size() && s.compare(s.size() - suf.s.size(), suf.s.size(), suf.s) == 0;
  }
  void remove(unsigned idx) { if (idx < s.size()) s.erase(idx); }
  void remove(unsigned idx, unsigned count) { if (idx < s.size()) s.erase(idx, count); }
  void toUpperCase() { for (auto &c : s) c = (char)toupper((unsigned char)c); }
  void toLowerCase() { for (auto &c : s) c = (char)tolower((unsigned char)c); }
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
  }
  long   toInt() const { return strtol(s.c_str(), nullptr, 10); }
  double toFloat() const { return strtod(s.c_str(), nullptr); }

  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *o)   { if (o) s += o; return *this; }
  String &operator+=(char c)          { s += c; return *this; }
  String &operator+=(int v)           { s += std::to_string(v); return *this; }

  bool operator==(const String &o) const { return s == o.s; }
  bool operator==(const char *o)   const { return o && s == o; }
  bool operator!=(const String &o) const { return s != o.s; }
  bool operator!=(const char *o)   const { return !o || s != o; }
  bool operator< (const String &o) const { return s < o.s; }     // enables std::sort / std::map keys

  const std::string &str() const { return s; }

  friend String operator+(const String &a, const String &b) { return String(a.s + b.s); }
  friend String operator+(const String &a, const char *b)   { return String(a.s + (b ? b : "")); }
  friend String operator+(const char *a, const String &b)   { return String(std::string(a ? a : "") + b.s); }
  friend String operator+(const String &a, char b)          { return String(a.s + b); }

private:
  static void fmtBase(char *out, long v, int base) {
    if (base == 16)      sprintf(out, "%lX", (unsigned long)v);
    else if (base == 8)  sprintf(out, "%lo", (unsigned long)v);
    else if (base == 2)  { // binary
      char tmp[40]; int n = 0; unsigned long u = (unsigned long)v;
      if (!u) tmp[n++] = '0';
      while (u) { tmp[n++] = '0' + (u & 1); u >>= 1; }
      for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
      out[n] = '\0';
    } else               sprintf(out, "%ld", v);
  }
};

// ---------------------------------------------------------------------------
// Serial — writes to stdout/stderr
// ---------------------------------------------------------------------------
class HardwareSerial {
public:
  void   begin(unsigned long = 0) {}
  void   end() {}
  void   setDebugOutput(bool) {}
  void   flush() { fflush(stdout); }
  int    available() { return 0; }
  int    read() { return -1; }
  size_t write(uint8_t c) { putchar(c); return 1; }
  size_t write(const uint8_t *b, size_t n) { fwrite(b, 1, n, stdout); return n; }
  operator bool() const { return true; }

  size_t print(const char *s) { return s ? fputs(s, stdout), strlen(s) : 0; }
  size_t print(const String &s) { return print(s.c_str()); }
  size_t print(char c) { putchar(c); return 1; }
  size_t print(int v, int base = DEC) { return printNum((long)v, base, false); }
  size_t print(unsigned v, int base = DEC) { return printNum((long)v, base, true); }
  size_t print(long v, int base = DEC) { return printNum(v, base, false); }
  size_t print(unsigned long v, int base = DEC) { return printNum((long)v, base, true); }
  size_t print(double v) { return printf("%g", v); }

  size_t println() { putchar('\n'); return 1; }
  size_t println(const char *s) { size_t n = print(s); putchar('\n'); return n + 1; }
  size_t println(const String &s) { return println(s.c_str()); }
  size_t println(char c) { putchar(c); putchar('\n'); return 2; }
  size_t println(int v, int base = DEC) { size_t n = print(v, base); putchar('\n'); return n + 1; }
  size_t println(unsigned v, int base = DEC) { size_t n = print(v, base); putchar('\n'); return n + 1; }
  size_t println(long v, int base = DEC) { size_t n = print(v, base); putchar('\n'); return n + 1; }
  size_t println(unsigned long v, int base = DEC) { size_t n = print(v, base); putchar('\n'); return n + 1; }
  size_t println(double v) { size_t n = print(v); putchar('\n'); return n + 1; }

  int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

private:
  size_t printNum(long v, int base, bool uns) {
    if (base == 16) return printf(uns ? "%lX" : "%lX", (unsigned long)v);
    if (base == 8)  return printf("%lo", (unsigned long)v);
    if (base == 2)  { String t((long)v, 2); return print(t.c_str()); }
    return printf(uns ? "%lu" : "%ld", v);
  }
};
extern HardwareSerial Serial;

// ---------------------------------------------------------------------------
// ESP system object + heap/PSRAM/cycle-count helpers
// ---------------------------------------------------------------------------
class EspClass {
public:
  uint32_t getFreeHeap()    { return 200u * 1024; }
  uint32_t getMinFreeHeap() { return 150u * 1024; }
  uint32_t getHeapSize()    { return 300u * 1024; }
  uint32_t getFreePsram()   { return 4u * 1024 * 1024; }
  uint32_t getPsramSize()   { return 8u * 1024 * 1024; }
  uint32_t getCpuFreqMHz()  { return 240; }
  uint32_t getCycleCount(); // steady clock scaled to 240 MHz (hal.cpp) — drives Apple/C64 busy-wait pacing
  void     restart();       // hal.cpp: requests app shutdown
};
extern EspClass ESP;

// PSRAM / heap_caps allocators -> plain malloc on the host
#define MALLOC_CAP_EXEC      (1 << 0)
#define MALLOC_CAP_32BIT     (1 << 1)
#define MALLOC_CAP_8BIT      (1 << 2)
#define MALLOC_CAP_DMA       (1 << 3)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_INTERNAL  (1 << 11)
#define MALLOC_CAP_DEFAULT   (1 << 12)
inline void  *ps_malloc(size_t n)            { return malloc(n); }
inline void  *ps_calloc(size_t c, size_t n)  { return calloc(c, n); }
inline void  *ps_realloc(void *p, size_t n)  { return realloc(p, n); }
inline void  *heap_caps_malloc(size_t n, uint32_t)              { return malloc(n); }
inline void  *heap_caps_calloc(size_t c, size_t n, uint32_t)    { return calloc(c, n); }
inline void  *heap_caps_realloc(void *p, size_t n, uint32_t)    { return realloc(p, n); }
inline void   heap_caps_free(void *p)                           { free(p); }
inline size_t heap_caps_get_free_size(uint32_t)                 { return 4u * 1024 * 1024; }
inline size_t heap_caps_get_largest_free_block(uint32_t)        { return 4u * 1024 * 1024; }

// ---------------------------------------------------------------------------
// FreeRTOS — each task() becomes a std::thread (faithful multi-thread model).
// Declarations here; the thread layer + registry lives in hal.cpp.
// ---------------------------------------------------------------------------
typedef int           BaseType_t;
typedef unsigned int  UBaseType_t;
typedef uint32_t      TickType_t;
typedef void         *TaskHandle_t;
typedef void         *QueueHandle_t;
typedef void         *SemaphoreHandle_t;
typedef uint8_t       StackType_t;
typedef struct { char _opaque[128]; } StaticTask_t;
typedef void (*TaskFunction_t)(void *);

#define pdPASS          1
#define pdFAIL          0
#define pdTRUE          1
#define pdFALSE         0
#define portMAX_DELAY   ((TickType_t)0xffffffffUL)
#define tskNO_AFFINITY  0x7fffffff
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))     // 1 tick == 1 ms in this shim
#define portTICK_PERIOD_MS 1

BaseType_t   xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *param,
                         UBaseType_t prio, TaskHandle_t *handle);
BaseType_t   xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *param,
                                     UBaseType_t prio, TaskHandle_t *handle, BaseType_t core);
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *param,
                                           UBaseType_t prio, StackType_t *stackBuf, StaticTask_t *tcb,
                                           BaseType_t core);
void         vTaskDelete(TaskHandle_t handle);   // NULL = current task -> its thread returns
void         vTaskDelay(TickType_t ticks);       // sleep `ticks` ms
void         vTaskSuspend(TaskHandle_t);
void         vTaskResume(TaskHandle_t);
void         taskYIELD();
void         disableCore0WDT();
void         disableLoopWDT();
void         enableCore0WDT();
void         enableLoopWDT();

SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t        xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t);

// ESP hardware timer API (only speaker.cpp used it; replaced on desktop, so these are harmless stubs)
typedef struct hw_timer_s hw_timer_t;
hw_timer_t *timerBegin(uint8_t num, uint16_t divider, bool countUp);
void        timerAttachInterrupt(hw_timer_t *t, void (*fn)(), bool edge);
void        timerAlarmWrite(hw_timer_t *t, uint64_t alarm, bool autoreload);
void        timerAlarmEnable(hw_timer_t *t);

// ---------------------------------------------------------------------------
// esp_reset_reason / esp_random / esp_restart
// ---------------------------------------------------------------------------
typedef enum {
  ESP_RST_UNKNOWN = 0, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
  ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO
} esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason();   // returns ESP_RST_POWERON on desktop -> boot splash shows
uint32_t           esp_random();
void               esp_restart();

typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL -1
