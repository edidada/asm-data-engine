/*
 * benchmark.c - Compare scalar C (-O3) against SSE2 / AVX2 / AVX-512
 * assembly kernels on a deterministic in-memory dataset.
 */
#include "calc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double best_of(int repeat, double (*fn)(const double *, size_t),
                      const double *data, size_t n, double *out_result)
{
    double best = 1e30;
    double r = 0.0;
    for (int i = 0; i < repeat; i++) {
        double t0 = now_sec();
        r = fn(data, n);
        double dt = now_sec() - t0;
        if (dt < best)
            best = dt;
    }
    *out_result = r;
    return best;
}

static void generate(double *data, size_t n, unsigned seed)
{
    /* xorshift32 deterministic generator */
    unsigned x = seed;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        data[i] = (double)(x & 0xFFFFF) * 0.001 + 0.5; /* [0.5, ~1049] */
    }
}

static int has_avx2 = 0, has_avx512 = 0;

static void run_case(const char *name, double (*fn)(const double *, size_t),
                     const double *data, size_t n, int repeat,
                     double ref_sum, int *ok)
{
    double result;
    double secs = best_of(repeat, fn, data, n, &result);
    double gbps = (double)n * 8.0 / (secs * 1e9);
    double diff = result - ref_sum;
    if (diff < 0)
        diff = -diff;
    int match = diff <= 1e-6 * (ref_sum > 1.0 ? ref_sum : 1.0);
    if (!match)
        *ok = 0;
    printf("%-14s %10.6f s   %8.2f GB/s   SUM=%.4f %s\n",
           name, secs, gbps, result, match ? "" : "  <-- MISMATCH");
}

int main(int argc, char **argv)
{
    size_t count = 1u << 21; /* 2,097,152 by default */
    int repeat = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            count = (size_t)strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            repeat = atoi(argv[++i]);
        else {
            fprintf(stderr, "Usage: %s [--count N] [--repeat R]\n", argv[0]);
            return 2;
        }
    }

    calc_cpu_features cpu;
    calc_query_cpu(&cpu);
    has_avx2 = cpu.has_avx2;
    has_avx512 = cpu.has_avx512f;

    double *data = malloc(count * sizeof(double));
    if (!data) {
        fprintf(stderr, "error: out of memory (%zu doubles)\n", count);
        return 1;
    }
    generate(data, count, 12345u);

    double ref;
    best_of(repeat, calc_scalar_sum, data, count, &ref);

    int ok = 1;
    printf("== asm-data-engine benchmark ==\n");
    printf("elements: %zu (~%.1f MB)   repeats per kernel: %d\n", count,
           count * 8.0 / (1024.0 * 1024.0), repeat);
    printf("cpu: sse2=%d avx2=%d avx512f=%d\n\n",
           cpu.has_sse2, cpu.has_avx2, cpu.has_avx512f);
    printf("%-14s %12s   %11s   %s\n", "kernel", "best time", "throughput", "checksum");
    printf("----------------------------------------------------------------\n");

    run_case("C scalar -O3", calc_scalar_sum, data, count, repeat, ref, &ok);
    run_case("ASM SSE2", calc_sum_sse2, data, count, repeat, ref, &ok);
    if (has_avx2)
        run_case("ASM AVX2", calc_sum_avx2, data, count, repeat, ref, &ok);
    else
        printf("%-14s %12s   %11s   (skipped, no AVX2)\n", "ASM AVX2", "-", "-");
    if (has_avx512)
        run_case("ASM AVX-512", calc_sum_avx512, data, count, repeat, ref, &ok);
    else
        printf("%-14s %12s   %11s   (skipped, no AVX-512)\n", "ASM AVX-512", "-", "-");

    free(data);
    return ok ? 0 : 1;
}
