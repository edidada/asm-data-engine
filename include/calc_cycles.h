/*
 * calc_cycles.h - V7: fine-grained timing with the x86 TSC.
 *
 * `calc_cycles_begin` and `calc_cycles_end` are serializing RDTSC/RDTSCP
 * wrappers implemented in NASM (src/calc_rdtsc.asm). They measure the
 * calling thread's reference-cycle counter, which is invariant on modern
 * Intel/AMD CPUs and therefore usable as a high-resolution wall clock.
 *
 * For whole-process profiles including cache misses, IPC and branch
 * mispredictions, pair this with `perf stat` (see scripts/perf.sh).
 */
#ifndef ASM_DATA_ENGINE_CALC_CYCLES_H
#define ASM_DATA_ENGINE_CALC_CYCLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* lfence; rdtsc - prevents reordering with earlier stores/loads. */
uint64_t calc_cycles_begin(void);

/* rdtscp; lfence - waits for all prior instructions to retire. */
uint64_t calc_cycles_end(void);

/*
 * Calibrated TSC frequency in Hz (cached after the first call).
 * Uses CLOCK_MONOTONIC over a ~20 ms window, so it reflects the actual
 * invariant-TSC rate rather than the nominal CPU marketing frequency.
 */
double calc_cycle_hz(void);

static inline double calc_cycles_to_ns(uint64_t cycles, double hz)
{
    return hz > 0.0 ? (double)cycles * 1e9 / hz : 0.0;
}

#ifdef __cplusplus
}
#endif

#endif /* ASM_DATA_ENGINE_CALC_CYCLES_H */
