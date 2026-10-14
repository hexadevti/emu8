// hal.cpp — desktop HAL implementation for the BOARD_DESKTOP build.
//
// Provides the non-inline parts of the Arduino/ESP/FreeRTOS shim declared in arduino_shim/Arduino.h:
//   * the time base (millis/micros/esp_timer_get_time/ESP.getCycleCount) off a steady clock,
//   * the FreeRTOS task model mapped 1:1 onto std::thread (faithful multi-thread, as chosen),
//   * mutex semaphores over std::mutex,
//   * the Serial / ESP / EEPROM global instances.
//
// Only compiled on desktop (CMake). The cores/shared code call these exactly as on the device.
#if defined(BOARD_DESKTOP)

#include <Arduino.h>
#include <EEPROM.h>
#include <SDL.h>          // SDL_GetBasePath: anchor the persistence files next to the .exe
#include <string>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_WIN32)
  #include <process.h>     // _execv
#else
  #include <unistd.h>      // execv
#endif

// Set by main() so ESP.restart() can re-exec this same binary (a true "reboot").
char **g_emuArgv = nullptr;

// Directory of the running .exe (with trailing slash). All persistence files (eeprom.bin, imgui.ini,
// emu8.cfg) are anchored here so the saved session is found no matter what the working directory
// is when the app is launched. SDL must be initialised first (main() does SDL_Init before setup()).
const char *desktopBaseDir() {
  static std::string dir;
  if (dir.empty()) {
    if (char *b = SDL_GetBasePath()) { dir = b; SDL_free(b); }
    else dir = "";   // fall back to cwd-relative
  }
  return dir.c_str();
}

// ---------------------------------------------------------------------------
// Time base — first call to now() seeds t0; everything is relative to it so the
// cores' real-time pacing (micros()/millis()/getCycleCount busy-waits) still throttles.
// ---------------------------------------------------------------------------
static std::chrono::steady_clock::time_point timeBase() {
  static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  return t0;
}
static int64_t elapsedUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now() - timeBase()).count();
}

unsigned long millis() { return (unsigned long)(elapsedUs() / 1000); }
unsigned long micros() { return (unsigned long)elapsedUs(); }          // wraps at 2^32 like the ESP
int64_t       esp_timer_get_time() { return elapsedUs(); }

uint32_t EspClass::getCycleCount() {
  // Scale real time to a nominal 240 MHz cycle counter so Apple/C64 cycle-count busy-waits pace
  // to real time. 240 cycles per microsecond.
  return (uint32_t)((uint64_t)elapsedUs() * 240ULL);
}
// Reboot = re-exec this binary: a fresh process is the closest desktop analog to an ESP hardware
// reset (re-runs setup(), re-reads the saved eeprom.bin so a mount+reboot / platform switch / settings
// change takes effect). Used by the options-UI "Reboot" + mount-and-reboot buttons. (Under gdb/F5 the
// debug session ends here, since the old process exits and the fresh one runs un-attached.)
static void doReboot() {
  fflush(nullptr);
  if (g_emuArgv && g_emuArgv[0]) {
#if defined(_WIN32)
    _execv(g_emuArgv[0], (const char *const *)g_emuArgv);
#else
    execv(g_emuArgv[0], g_emuArgv);
#endif
  }
  std::exit(0);   // fallback if re-exec failed
}
void EspClass::restart() { doReboot(); }

EspClass       ESP;
HardwareSerial Serial;

int HardwareSerial::printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vprintf(fmt, ap);
  va_end(ap);
  return n;
}

// ---------------------------------------------------------------------------
// map / random / esp_random / esp_reset_reason
// ---------------------------------------------------------------------------
long map(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
long random(long howbig) { return howbig <= 0 ? 0 : (long)((unsigned long)rand() % (unsigned long)howbig); }
long random(long howsmall, long howbig) { return howsmall >= howbig ? howsmall : howsmall + random(howbig - howsmall); }
void randomSeed(unsigned long seed) { srand((unsigned)seed); }
uint32_t esp_random() { return ((uint32_t)rand() << 16) ^ (uint32_t)rand(); }
// EMU_PLATFORM set -> report a soft reboot so videoSetup() skips the splash and boots straight into
// the chosen platform (deterministic for debugging). Otherwise POWERON -> the platform-picker splash.
esp_reset_reason_t esp_reset_reason() { return getenv("EMU_PLATFORM") ? ESP_RST_SW : ESP_RST_POWERON; }
void esp_restart() { doReboot(); }

// ---------------------------------------------------------------------------
// FreeRTOS tasks -> std::thread (faithful multi-thread model)
// ---------------------------------------------------------------------------
namespace {
struct TaskRec { std::thread th; };
std::mutex          g_taskMx;
std::vector<TaskRec*> g_tasks;

TaskHandle_t spawn(TaskFunction_t fn, void *param) {
  auto *rec = new TaskRec();
  {
    std::lock_guard<std::mutex> lk(g_taskMx);
    g_tasks.push_back(rec);
  }
  rec->th = std::thread([fn, param]() { fn(param); });   // run the task body; returns -> thread ends
  return (TaskHandle_t)rec;
}
} // namespace

BaseType_t xTaskCreate(TaskFunction_t fn, const char *, uint32_t, void *param,
                       UBaseType_t, TaskHandle_t *handle) {
  TaskHandle_t h = spawn(fn, param);
  if (handle) *handle = h;
  return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *, uint32_t, void *param,
                                   UBaseType_t, TaskHandle_t *handle, BaseType_t) {
  TaskHandle_t h = spawn(fn, param);
  if (handle) *handle = h;
  return pdPASS;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *, uint32_t, void *param,
                                           UBaseType_t, StackType_t *, StaticTask_t *, BaseType_t) {
  return spawn(fn, param);   // (not used on desktop: renderLoop runs on main thread — see video.cpp)
}

void vTaskDelete(TaskHandle_t) {
  // NULL == current task: its function is about to return anyway, so nothing to do. A non-NULL
  // handle (rare; not used by the desktop-compiled tasks) is left to exit via its own `running` loop.
}
void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
void vTaskSuspend(TaskHandle_t) {}
void vTaskResume(TaskHandle_t) {}
void taskYIELD() { std::this_thread::yield(); }
void disableCore0WDT() {}
void disableLoopWDT() {}
void enableCore0WDT() {}
void enableLoopWDT() {}

// Join every spawned task thread. Call from main() after setting `running=false` (and after the
// audio backend has unblocked its producer) for a clean, TSan-friendly shutdown.
void halJoinAllTasks() {
  std::vector<TaskRec*> copy;
  { std::lock_guard<std::mutex> lk(g_taskMx); copy = g_tasks; }
  for (auto *r : copy) if (r->th.joinable()) r->th.join();
}

// ---------------------------------------------------------------------------
// Semaphores (mutex only — the codebase uses a single mutex semaphore, gBusLock)
// ---------------------------------------------------------------------------
SemaphoreHandle_t xSemaphoreCreateMutex() { return (SemaphoreHandle_t)new std::mutex(); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) { if (s) ((std::mutex*)s)->lock(); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { if (s) ((std::mutex*)s)->unlock(); return pdTRUE; }

// ESP hardware timer — speaker.cpp is replaced on desktop, so these are harmless no-ops.
struct hw_timer_s { int dummy; };
static hw_timer_s g_dummyTimer;
hw_timer_t *timerBegin(uint8_t, uint16_t, bool) { return &g_dummyTimer; }
void timerAttachInterrupt(hw_timer_t *, void (*)(), bool) {}
void timerAlarmWrite(hw_timer_t *, uint64_t, bool) {}
void timerAlarmEnable(hw_timer_t *) {}

// ---------------------------------------------------------------------------
// EEPROM — file-backed (eeprom.bin in the working directory)
// ---------------------------------------------------------------------------
EEPROMClass EEPROM;
namespace {
std::vector<uint8_t> g_ee;
std::string          g_eePath;            // resolved to <exe dir>/eeprom.bin on first begin()
uint8_t              g_eeScratch = 0xFF;
}
bool EEPROMClass::begin(size_t size) {
  if (g_eePath.empty()) g_eePath = std::string(desktopBaseDir()) + "eeprom.bin";
  g_ee.assign(size, 0xFF);
  FILE *f = fopen(g_eePath.c_str(), "rb");
  if (f) {
    fread(g_ee.data(), 1, size, f);   // load whatever fits; rest stays 0xFF
    fclose(f);
  }
  return true;
}
uint8_t EEPROMClass::read(int addr) {
  return (addr >= 0 && (size_t)addr < g_ee.size()) ? g_ee[addr] : 0xFF;
}
void EEPROMClass::write(int addr, uint8_t val) {
  if (addr >= 0 && (size_t)addr < g_ee.size()) g_ee[addr] = val;
}
bool EEPROMClass::commit() {
  FILE *f = fopen(g_eePath.c_str(), "wb");
  if (!f) return false;
  fwrite(g_ee.data(), 1, g_ee.size(), f);
  fclose(f);
  return true;
}
uint8_t &EEPROMClass::operator[](int addr) {
  if (addr >= 0 && (size_t)addr < g_ee.size()) return g_ee[addr];
  return g_eeScratch;
}
size_t EEPROMClass::length() { return g_ee.size(); }

#endif // BOARD_DESKTOP
