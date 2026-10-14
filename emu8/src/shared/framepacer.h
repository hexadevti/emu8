#pragma once
// framepacer.h - frame pacing for the SMS core: steady NORMAL speed, FAST = as fast as possible.
//
// Game music is ticked by the VBlank handler, so the music tempo IS the emulated frame rate. When the
// host cannot hold the real rate (the PicoCalc's M0+ in busy scenes), the old scheduler kept a debt
// of up to 100 ms and paid it back with back-to-back frames once a light scene came along: the tempo
// swung, slow then fast. This keeps it steady instead:
//  - frameskip: when frames cost more than the budget, the picture is drawn only every 2nd, then
//    3rd, then 4th frame
//    (vdpRender is a big share of a frame on the M0+). Drawing used to be skipped anyway whenever
//    the render core had not shown the previous frame yet -- so the CPU cost, and the speed, followed
//    how busy the panel was. Now the skip pattern is the pacer's, and regular.
//  - periodUs: if even that does not fit, the frame period itself is stretched to what the host
//    sustains. It only ever grows (until resync()): following each scene up and down was itself
//    a tempo swing. Frameskip, once on, also stays on. A little slow, but constant.
//  - up to 3 frames of debt are caught up -- gently, at the period's 6% headroom -- so a short
//    spike does not stretch the period; later than that just moves the schedule.
//
// Usage, per frame:   bool draw = pacer.begin(fast);  runFrame(draw);  ...;  pacer.end(fast);
// and pacer.resync() after a pause / the settings window / a reset.
// Needs micros() / vTaskDelay() / taskYIELD(), i.e. include it after emu.h.
#include <stdint.h>

struct FramePacer {
  uint32_t frameUs;              // real frame period
  uint32_t periodUs;             // current frame period, >= frameUs
  uint32_t nextUs = 0;           // absolute deadline of the next frame
  uint32_t workQ4;               // EMA of a frame's busy time (us, 28.4 fixed point)
  uint32_t t0 = 0;               // start of the current frame
  uint32_t lastYieldUs = 0;      // FAST: when the loop last slept
  int skip = 0, phase = 0, evalCnt = 0;
  static constexpr int MAX_SKIP = 3;   // at most 3 frames undrawn per drawn one

  explicit FramePacer(uint32_t us) : frameUs(us), periodUs(us), workQ4(us << 4) { resync(); }

  void resync() { nextUs = micros(); periodUs = frameUs; workQ4 = frameUs << 4; skip = phase = evalCnt = 0; }

  // Start of a frame. Returns whether this frame should draw its picture.
  bool begin(bool fast) {
    t0 = micros();
    if (fast) return true;       // the frame is drawn only if the display core is free anyway
    const bool draw = phase == 0;
    if (++phase > skip) phase = 0;
    return draw;
  }

  // End of a frame: adapt, then sleep until the next one is due.
  void end(bool fast) {
    if (fast) {
#if defined(BOARD_DESKTOP)
      taskYIELD();               // desktop: full host speed (render/input on other threads)
#else
      // Uncapped: sleep one tick only every 50 ms (lets the idle task / WDT run) rather than
      // after every frame, which cost ~1 ms of each.
      const uint32_t now = micros();
      if (now - lastYieldUs >= 50000) { lastYieldUs = now; vTaskDelay(1); }
#endif
      nextUs = micros();
      return;
    }

    // Busy time (+1 ms for the tick the wait below always costs). EMA over ~32 frames, so a
    // draw / no-draw pair and short spikes average out.
    const uint32_t work = (micros() - t0) + 1000;
    workQ4 += (int32_t)((work << 4) - workQ4) / 32;
    const uint32_t avg = workQ4 >> 4;

    // Frameskip, checked about once a second (one way: on stays on). On engaging it the cost drops,
    // so the average is scaled down rather than letting the old full-draw cost set the period.
    // Deeper frameskip first (draw 1 in 2, 3, then 4): the panel shows ~15-20 pictures a second
    // on the M0+ anyway, so pictures are the cheap thing to give up -- game speed is not.
    bool grow = false;
    if (++evalCnt >= 60) {
      evalCnt = 0;
      if (avg > frameUs * 95 / 100) {
        if (skip < MAX_SKIP) { skip++; phase = 0; workQ4 = frameUs << 4; }   // re-measure at the new depth
        else grow = true;
      }
    }

    // Period: only once frameskip is maxed out, 6% headroom over the average cost; never faster
    // than the real machine.
    uint32_t target = avg + avg / 16;
    if (target < frameUs) target = frameUs;
    if (grow && target > periodUs) periodUs = target;                  // only grows, once a second

    nextUs += periodUs;
    int32_t wait = (int32_t)(nextUs - micros());
    if (wait < -3 * (int32_t)periodUs) { nextUs = micros(); wait = 0; }  // too late: drop the debt
    if (wait > 0) {
      const uint32_t ms = (uint32_t)wait / 1000;
      vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));    // sleep the ms bulk; the sub-ms rest self-corrects
    } else {
      vTaskDelay(1);
    }
  }
};
