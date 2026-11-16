/*
 * calc_cycles.c - V7: calibrate the invariant TSC against CLOCK_MONOTONIC.
 *
 * The TSC frequency is not necessarily the nominal CPU frequency (turbo,
 * base clock and invariant-TSC divisors all matter), so measure it once
 * over a fixed wall-clock window instead of trusting CPUID or /proc.
 */
#define _POSIX_C_SOURCE 199309L

#include "calc_cycles.h"

#include <time.h>

#define CALIBRATION_NS 20000000L /* 20 ms */

static double cached_hz;

double calc_cycle_hz(void)
{
    if (cached_hz > 0.0)
        return cached_hz;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t c0 = calc_cycles_begin();
    long elapsed_ns;
    do {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L +
                     (t1.tv_nsec - t0.tv_nsec);
    } while (elapsed_ns < CALIBRATION_NS);
    uint64_t c1 = calc_cycles_end();

    double secs = (double)(t1.tv_sec - t0.tv_sec) +
                  (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    if (!(secs > 0.0) || c1 <= c0)
        return 1.0;

    cached_hz = (double)(c1 - c0) / secs;
    return cached_hz;
}
