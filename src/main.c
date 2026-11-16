/*
 * main.c - asm-stat CLI.
 *
 * Scans a raw little-endian binary file of doubles (amounts.bin, produced
 * by scripts/convert.py or tools/asm-gen.c) and prints a statistics report,
 * mirroring this SQL:
 *
 *   SELECT COUNT(*), SUM(x), AVG(x), MIN(x), MAX(x)
 *   FROM records;
 *
 * V5: the scan is split across pthreads.
 * V6: input comes from mmap or a reusable chunked-read buffer.
 * V7: wall time plus invariant-TSC cycles per element.
 *
 * Usage:
 *   ./asm-stat --input FILE [--engine auto|scalar|sse2|avx2|avx512]
 *              [--threads N] [--io auto|mmap|read] [--chunk-mb N]
 *              [--repeat R]
 */
#define _POSIX_C_SOURCE 200809L

#include "calc.h"
#include "calc_cycles.h"
#include "calc_io.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --input FILE [options]\n"
        "\n"
        "  --engine  auto|scalar|sse2|avx2|avx512   (default: auto)\n"
        "  --threads N       worker threads, 0 = all online CPUs (default: 0)\n"
        "  --io      auto|mmap|read                 (default: auto)\n"
        "  --chunk-mb N      I/O chunk size in MiB (default: 64)\n"
        "  --repeat  R       scan the file R times, report the best (default: 1)\n"
        "\n"
        "FILE must contain raw little-endian doubles (see scripts/convert.py).\n",
        prog);
    exit(2);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct {
    calc_result result;
    size_t chunks;
} scan_result;

static int scan_file(calc_file *file, calc_engine engine, int threads,
                     scan_result *out)
{
    calc_result acc;
    memset(&acc, 0, sizeof acc);
    size_t chunks = 0;

    const double *data = NULL;
    size_t count;
    while ((count = calc_file_next(file, &data)) > 0) {
        calc_result part;
        if (calc_aggregate_mt(data, count, engine, threads, &part) != 0) {
            fprintf(stderr, "error: aggregation failed\n");
            return -1;
        }
        calc_result_merge(&acc, &part);
        chunks++;
    }

    const char *err = calc_file_error(file);
    if (err) {
        fprintf(stderr, "error: %s\n", err);
        return -1;
    }

    out->result = acc;
    out->chunks = chunks;
    return 0;
}

static calc_io_mode parse_io_mode(const char *name)
{
    if (strcmp(name, "auto") == 0)
        return CALC_IO_AUTO;
    if (strcmp(name, "mmap") == 0)
        return CALC_IO_MMAP;
    if (strcmp(name, "read") == 0)
        return CALC_IO_READ;
    fprintf(stderr, "error: unknown I/O mode '%s'\n", name);
    exit(2);
}

static calc_engine parse_engine(const char *name, const calc_cpu_features *cpu)
{
    calc_engine requested;
    if (strcmp(name, "scalar") == 0)      requested = CALC_ENGINE_SCALAR;
    else if (strcmp(name, "sse2") == 0)   requested = CALC_ENGINE_SSE2;
    else if (strcmp(name, "avx2") == 0)   requested = CALC_ENGINE_AVX2;
    else if (strcmp(name, "avx512") == 0) requested = CALC_ENGINE_AVX512;
    else if (strcmp(name, "auto") == 0)   requested = calc_pick_engine(cpu);
    else {
        fprintf(stderr, "error: unknown engine '%s'\n", name);
        exit(2);
    }

    calc_engine resolved = calc_engine_resolve(requested, cpu);
    if (resolved != requested) {
        fprintf(stderr, "warning: %s requested but CPU does not support it; "
                        "using %s\n",
                calc_engine_name(requested), calc_engine_name(resolved));
    }
    return resolved;
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *engine_arg = "auto";
    const char *io_arg = "auto";
    int threads = 0;
    int repeat = 1;
    size_t chunk_bytes = 0;
    int chunk_given = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input = argv[++i];
        } else if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
            engine_arg = argv[++i];
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || v < 0 || v > 1024) {
                fprintf(stderr, "error: invalid --threads '%s'\n", argv[i]);
                return 2;
            }
            threads = (int)v;
        } else if (strcmp(argv[i], "--io") == 0 && i + 1 < argc) {
            io_arg = argv[++i];
        } else if (strcmp(argv[i], "--chunk-mb") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            unsigned long long mb = strtoull(argv[++i], &end, 10);
            if (errno != 0 || !end || *end != '\0' || mb == 0 ||
                mb > (unsigned long long)(SIZE_MAX >> 20)) {
                fprintf(stderr, "error: invalid --chunk-mb '%s'\n", argv[i]);
                return 2;
            }
            chunk_bytes = (size_t)mb << 20;
            chunk_given = 1;
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || v < 1 || v > 1000) {
                fprintf(stderr, "error: invalid --repeat '%s'\n", argv[i]);
                return 2;
            }
            repeat = (int)v;
        } else {
            usage(argv[0]);
        }
    }
    if (!input)
        usage(argv[0]);

    calc_cpu_features cpu;
    calc_query_cpu(&cpu);
    calc_engine engine = parse_engine(engine_arg, &cpu);
    calc_io_mode io_mode = parse_io_mode(io_arg);

    if (threads <= 0)
        threads = calc_cpu_count();

    calc_file *file = calc_file_open(input, io_mode,
                                     chunk_given ? chunk_bytes : 0);
    if (!file) {
        fprintf(stderr, "error: %s\n", calc_io_last_error());
        return 1;
    }

    double best_secs = 1e300;
    uint64_t best_cycles = UINT64_MAX;
    scan_result best;
    memset(&best, 0, sizeof best);

    for (int r = 0; r < repeat; r++) {
        if (r > 0 && calc_file_rewind(file) != 0) {
            fprintf(stderr, "error: %s\n", calc_file_error(file));
            calc_file_close(file);
            return 1;
        }

        uint64_t c0 = calc_cycles_begin();
        double t0 = now_sec();
        scan_result cur;
        if (scan_file(file, engine, threads, &cur) != 0) {
            calc_file_close(file);
            return 1;
        }
        double t1 = now_sec();
        uint64_t c1 = calc_cycles_end();

        double secs = t1 - t0;
        if (secs < best_secs) {
            best_secs = secs;
            best_cycles = c1 - c0;
            best = cur;
        }
    }

    size_t n = best.result.count;
    double bytes = (double)calc_file_size(file);
    double mb = bytes / (1024.0 * 1024.0);
    double chunk_mb = (double)CALC_IO_DEFAULT_CHUNK / (1024.0 * 1024.0);
    if (chunk_given)
        chunk_mb = (double)chunk_bytes / (1024.0 * 1024.0);

    printf("== asm-data-engine report ==\n");
    printf("input        : %s (%.2f MB)\n", input, mb);
    printf("records      : %zu\n", n);
    printf("engine       : %s\n", calc_engine_name(engine));
    printf("threads      : %d (%d online)\n", threads, calc_cpu_count());
    printf("io           : %s (chunk %.1f MiB, %zu chunk(s))\n",
           calc_io_mode_name(calc_file_mode(file)), chunk_mb, best.chunks);
    printf("%-12s : %.6f s (%d scan%s)\n", repeat > 1 ? "best time" : "time",
           best_secs, repeat, repeat > 1 ? "s" : "");

    if (n > 0) {
        double gbps = (double)n * sizeof(double) / (best_secs * 1e9);
        double melem = (double)n / (best_secs * 1e6);
        printf("throughput   : %.2f GB/s, %.2f M elem/s\n", gbps, melem);

        double hz = calc_cycle_hz();
        if (hz > 0.0) {
            printf("tsc          : %.2f cycles/elem @ %.2f GHz "
                   "(wall, includes I/O)\n",
                   (double)best_cycles / (double)n, hz / 1e9);
        }
    }

    if (n == 0) {
        printf("no data      : input file contains no doubles\n");
    } else {
        double avg = best.result.sum / (double)n;
        printf("SUM          : %.4f\n", best.result.sum);
        printf("AVG          : %.4f\n", avg);
        printf("MIN          : %.4f\n", best.result.min);
        printf("MAX          : %.4f\n", best.result.max);
    }

    calc_file_close(file);
    return 0;
}
