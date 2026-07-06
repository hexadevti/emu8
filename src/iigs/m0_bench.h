// m0_bench.h - Apple IIGS feasibility gate (M0): PSRAM/SRAM memory-latency benchmark.
//
// This is a THROWAWAY validation experiment, NOT part of the emulator. It only compiles
// when -DIIGS_M0_BENCH is passed to the build (see .vscode/tasks.json). Without that flag
// the whole translation unit (src/iigs/m0_bench.cpp) is empty and this declares nothing.
//
// Purpose: before writing any IIGS (65C816) core, measure whether the planned memory
// architecture - "hot banks in internal SRAM, expansion RAM in Octal PSRAM" - can service
// the ~2.8M accesses/sec a 65C816 @ 2.8MHz demands, with host-cycle headroom left for the
// interpreter. See C:\Users\lucia\.claude\plans\sim-iterative-rivest.md for the full design.
//
// Call runIIgsM0Bench() at the very top of setup() (before any task/canvas/IRQ init), then
// halt, so a single power-cycle-synced serial capture gets all results.

#pragma once

#ifdef IIGS_M0_BENCH
void runIIgsM0Bench();    // worst-case latency battery (B1..B9)
#endif
#ifdef IIGS_M05_BENCH
void runIIgsM05Bench();   // realistic mix + scatter-fraction sweep + bank-reload throughput
#endif
