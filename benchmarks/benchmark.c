/*
 * benchmark.c - V7 benchmark: scalar C (-O3) vs SSE2 / AVX2 / AVX-512,
 * including a pthread chunked run (V5).
 *
 * Every kernel is measured with both CLOCK_MONOTONIC and serializing
 * RDTSC/RDTSCP reads, so the report shows wall time, throughput, cycles per
 * element and nanoseconds per element. For whole-process counters (cache
 * misses, IPC, branch misses) run scripts/perf.sh.
 */
#define _POSIX_C_SOURCE 200809L

#include "calc.h"
#include "calc_cycles.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef double (*sum_fn)(const double *, size_t);

typedef struct {
    double secs;
    uint64_t cycles;
    double result;
} measurement;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static measurement measure_sum(sum_fn fn, const double *data, size_t n,
                               int repeat)
{
    measurement best = {1e300, UINT64_MAX, 0.0};
    for (int i = 0; i < repeat; i++) {
        uint64_t c0 = calc_cycles_begin();
        double t0 = now_sec();
        double r = fn(data, n);
        double t1 = now_sec();
        uint64_t c1 = calc_cycles_end();
        if (t1 - t0 < best.secs) {
            best.secs = t1 - t0;
            best.cycles = c1 - c0;
            best.result = r;
        }
    }
    return best;
}

typedef struct {
    const double *data;
    size_t n;
    int threads;
    calc_engine engine;
} mt_ctx;

static measurement measure_mt(const mt_ctx *ctx, int repeat)
{
    measurement best = {1e300, UINT64_MAX, 0.0};
    for (int i = 0; i < repeat; i++) {
        calc_result r;
        uint64_t c0 = calc_cycles_begin();
        double t0 = now_sec();
        int rc = calc_aggregate_mt(ctx->data, ctx->n, ctx->engine,
                                   ctx->threads, &r);
        double t1 = now_sec();
        uint64_t c1 = calc_cycles_end();
        if (rc != 0) {
            best.result = 0.0;
            continue;
        }
        if (t1 - t0 < best.secs) {
            best.secs = t1 - t0;
            best.cycles = c1 - c0;
            best.result = r.sum;
        }
    }
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

static int check_sum(double got, double ref)
{
    double diff = got - ref;
    if (diff < 0)
        diff = -diff;
    return diff <= 1e-6 * (ref > 1.0 ? ref : 1.0);
}

static void report(const char *name, const measurement *m, size_t n,
                   double hz, double ref_sum, double bytes_per_elem, int *ok)
{
    double gbps = (double)n * 8.0 * bytes_per_elem / (m->secs * 1e9);
    double cycles_per_elem = n ? (double)m->cycles / (double)n : 0.0;
    double ns_per_elem = n && hz > 0.0
                             ? (double)m->cycles / (double)n * 1e9 / hz
                             : 0.0;
    int match = check_sum(m->result, ref_sum);
    if (!match)
        *ok = 0;
    printf("%-27s %11.6f s  %9.2f GB/s  %10.2f  %9.2f  %.4f%s\n",
           name, m->secs, gbps, cycles_per_elem, ns_per_elem, m->result,
           match ? "" : "  <-- MISMATCH");
}

int main(int argc, char **argv)
{
    size_t count = 1u << 21; /* 2,097,152 by default */
    int repeat = 5;
    int threads = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = (size_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else {
            fprintf(stderr,
                    "Usage: %s [--count N] [--repeat R] [--threads N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (count == 0 || repeat < 1 || threads < 0) {
        fprintf(stderr, "error: count >= 1, repeat >= 1, threads >= 0\n");
        return 2;
    }
    if (threads == 0)
        threads = calc_cpu_count();

    calc_cpu_features cpu;
    calc_query_cpu(&cpu);
    calc_engine best_engine = calc_engine_resolve(calc_pick_engine(&cpu), &cpu);

    double *data = malloc(count * sizeof(double));
    if (!data) {
        fprintf(stderr, "error: out of memory (%zu doubles)\n", count);
        return 1;
    }
    generate(data, count, 12345u);

    double ref = calc_scalar_sum(data, count);
    double hz = calc_cycle_hz();

    int ok = 1;
    printf("== asm-data-engine benchmark ==\n");
    printf("elements: %zu (~%.1f MB)   repeats per kernel: %d\n", count,
           (double)count * 8.0 / (1024.0 * 1024.0), repeat);
    printf("cpu: sse2=%d avx2=%d avx512f=%d   cpu threads=%d   "
           "tsc=%.2f GHz\n\n",
           cpu.has_sse2, cpu.has_avx2, cpu.has_avx512f, calc_cpu_count(),
           hz / 1e9);
    printf("%-27s %13s  %13s  %10s  %9s  %s\n", "kernel", "best time",
           "throughput", "cycles/elem", "ns/elem", "checksum");
    printf("-------------------------------------------------------------------"
           "------------------------------------\n");

    measurement m;

    m = measure_sum(calc_scalar_sum, data, count, repeat);
    report("C scalar -O3 (sum)", &m, count, hz, ref, 1, &ok);

    m = measure_sum(calc_sum_sse2, data, count, repeat);
    report("ASM SSE2 (sum)", &m, count, hz, ref, 1, &ok);

    if (cpu.has_avx2) {
        m = measure_sum(calc_sum_avx2, data, count, repeat);
        report("ASM AVX2 (sum)", &m, count, hz, ref, 1, &ok);
    } else {
        printf("%-27s %11s    %9s      %10s  %9s  (skipped, no AVX2)\n",
               "ASM AVX2 (sum)", "-", "-", "-", "-");
    }

    if (cpu.has_avx512f) {
        m = measure_sum(calc_sum_avx512, data, count, repeat);
        report("ASM AVX-512 (sum)", &m, count, hz, ref, 1, &ok);
    } else {
        printf("%-27s %11s    %9s      %10s  %9s  (skipped, no AVX-512)\n",
               "ASM AVX-512 (sum)", "-", "-", "-", "-");
    }

    if (threads > 1) {
        /* Fair comparison: same three-pass aggregate, one thread vs N. */
        mt_ctx single = {data, count, 1, best_engine};
        m = measure_mt(&single, repeat);
        char name[64];
        snprintf(name, sizeof name, "ASM %s aggregate x1",
                 calc_engine_name(best_engine));
        report(name, &m, count, hz, ref, 3, &ok);

        mt_ctx ctx = {data, count, threads, best_engine};
        m = measure_mt(&ctx, repeat);
        snprintf(name, sizeof name, "ASM %s aggregate x%d",
                 calc_engine_name(best_engine), threads);
        report(name, &m, count, hz, ref, 3, &ok);
    }

    printf("\nnote: \"(sum)\" rows are one SIMD pass over the data; "
           "\"aggregate\" rows compute\n"
           "      SUM+MIN+MAX (three passes) and GB/s counts all three "
           "passes, so the\n"
           "      columns are comparable. cycles/elem for pthread rows is "
           "wall-clock TSC\n"
           "      per element, not per-core cycles.\n");

    free(data);
    return ok ? 0 : 1;
}
