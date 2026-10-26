/*
 * main.c - asm-stat CLI.
 *
 * Reads a raw little-endian binary file of doubles (amounts.bin, produced
 * by scripts/convert.py) and prints a statistics report using the best
 * SIMD engine the host CPU supports, mirroring this SQL:
 *
 *   SELECT COUNT(*), SUM(x), AVG(x), MIN(x), MAX(x)
 *   FROM records;
 *
 * Usage:
 *   ./asm-stat --input FILE [--engine auto|scalar|sse2|avx2|avx512]
 */
#include "calc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --input FILE [--engine auto|scalar|sse2|avx2|avx512]\n"
        "\n"
        "FILE must contain raw little-endian doubles (see scripts/convert.py).\n",
        prog);
    exit(2);
}

static double *load_file(const char *path, size_t *out_n, double *out_mb)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    if (bytes < 0 || (bytes % (long)sizeof(double)) != 0) {
        fprintf(stderr, "error: '%s' is not a multiple of 8 bytes "
                        "(raw double stream expected)\n", path);
        fclose(f);
        exit(1);
    }
    rewind(f);

    size_t n = (size_t)(bytes / (long)sizeof(double));
    double *data = malloc(n ? n * sizeof(double) : sizeof(double));
    if (!data) {
        fprintf(stderr, "error: out of memory\n");
        fclose(f);
        exit(1);
    }
    if (bytes > 0 && fread(data, sizeof(double), n, f) != n) {
        fprintf(stderr, "error: short read on '%s'\n", path);
        fclose(f);
        free(data);
        exit(1);
    }
    fclose(f);
    *out_n = n;
    *out_mb = (double)bytes / (1024.0 * 1024.0);
    return data;
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *engine_arg = "auto";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input = argv[++i];
        } else if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
            engine_arg = argv[++i];
        } else {
            usage(argv[0]);
        }
    }
    if (!input)
        usage(argv[0]);

    calc_cpu_features cpu;
    calc_query_cpu(&cpu);

    calc_engine engine = CALC_ENGINE_SSE2;
    if (strcmp(engine_arg, "scalar") == 0)      engine = CALC_ENGINE_SCALAR;
    else if (strcmp(engine_arg, "sse2") == 0)   engine = CALC_ENGINE_SSE2;
    else if (strcmp(engine_arg, "avx2") == 0)   engine = CALC_ENGINE_AVX2;
    else if (strcmp(engine_arg, "avx512") == 0) engine = CALC_ENGINE_AVX512;
    else if (strcmp(engine_arg, "auto") == 0)   engine = calc_pick_engine(&cpu);
    else {
        fprintf(stderr, "error: unknown engine '%s'\n", engine_arg);
        usage(argv[0]);
    }

    double mb = 0.0;
    size_t n = 0;
    double *data = load_file(input, &n, &mb);

    double sum, mn, mx;
    switch (engine) {
    case CALC_ENGINE_SCALAR:
        sum = calc_scalar_sum(data, n);
        mn = calc_scalar_min(data, n);
        mx = calc_scalar_max(data, n);
        break;
    case CALC_ENGINE_SSE2:
        sum = calc_sum_sse2(data, n);
        mn = calc_min_sse2(data, n);
        mx = calc_max_sse2(data, n);
        break;
    case CALC_ENGINE_AVX2:
        if (!cpu.has_avx2) {
            fprintf(stderr, "warning: avx2 requested but CPU does not support it; "
                            "falling back to sse2\n");
            engine = CALC_ENGINE_SSE2;
            sum = calc_sum_sse2(data, n);
            mn = calc_min_sse2(data, n);
            mx = calc_max_sse2(data, n);
            break;
        }
        sum = calc_sum_avx2(data, n);
        mn = calc_min_avx2(data, n);
        mx = calc_max_avx2(data, n);
        break;
    case CALC_ENGINE_AVX512:
        if (!cpu.has_avx512f) {
            fprintf(stderr, "warning: avx512 requested but CPU does not support it; "
                            "falling back to %s\n",
                    cpu.has_avx2 ? "avx2" : "sse2");
            engine = cpu.has_avx2 ? CALC_ENGINE_AVX2 : CALC_ENGINE_SSE2;
            if (engine == CALC_ENGINE_AVX2) {
                sum = calc_sum_avx2(data, n);
                mn = calc_min_avx2(data, n);
                mx = calc_max_avx2(data, n);
            } else {
                sum = calc_sum_sse2(data, n);
                mn = calc_min_sse2(data, n);
                mx = calc_max_sse2(data, n);
            }
            break;
        }
        sum = calc_sum_avx512(data, n);
        mn = calc_min_avx512(data, n);
        mx = calc_max_avx512(data, n);
        break;
    default:
        abort();
    }

    double avg = (n > 0) ? sum / (double)n : 0.0;

    printf("== asm-data-engine report ==\n");
    printf("input        : %s (%.2f MB)\n", input, mb);
    printf("engine       : %s\n", calc_engine_name(engine));
    printf("cpu          : sse2=%d avx2=%d avx512f=%d\n",
           cpu.has_sse2, cpu.has_avx2, cpu.has_avx512f);
    if (n == 0) {
        printf("records      : 0 (no data)\n");
    } else {
        printf("records      : %zu\n", n);
        printf("SUM          : %.4f\n", sum);
        printf("AVG          : %.4f\n", avg);
        printf("MIN          : %.4f\n", mn);
        printf("MAX          : %.4f\n", mx);
    }

    free(data);
    return 0;
}
