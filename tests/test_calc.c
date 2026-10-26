/*
 * test_calc.c - Correctness tests: every SIMD engine must agree with the
 * scalar C reference on a deterministic dataset, including awkward sizes
 * (0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 1000, 4097) that exercise the
 * vector loops, the scalar tail, and empty-input semantics.
 */
#include "calc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

static void expect_close(const char *what, double got, double ref)
{
    double diff = got - ref;
    if (diff < 0)
        diff = -diff;
    double tol = 1e-9 * (fabs(ref) > 1.0 ? fabs(ref) : 1.0);
    if (!(diff <= tol)) {
        fprintf(stderr, "FAIL %-10s got=%.17g ref=%.17g\n", what, got, ref);
        failures++;
    }
}

static void run_size(const double *data, size_t n)
{
    char tag[64];
    double rs = calc_scalar_sum(data, n);
    double rmn = calc_scalar_min(data, n);
    double rmx = calc_scalar_max(data, n);

    snprintf(tag, sizeof tag, "sum/sse2/n=%zu", n);
    expect_close(tag, calc_sum_sse2(data, n), rs);
    snprintf(tag, sizeof tag, "min/sse2/n=%zu", n);
    expect_close(tag, calc_min_sse2(data, n), rmn);
    snprintf(tag, sizeof tag, "max/sse2/n=%zu", n);
    expect_close(tag, calc_max_sse2(data, n), rmx);

    calc_cpu_features cpu;
    calc_query_cpu(&cpu);

    if (cpu.has_avx2) {
        snprintf(tag, sizeof tag, "sum/avx2/n=%zu", n);
        expect_close(tag, calc_sum_avx2(data, n), rs);
        snprintf(tag, sizeof tag, "min/avx2/n=%zu", n);
        expect_close(tag, calc_min_avx2(data, n), rmn);
        snprintf(tag, sizeof tag, "max/avx2/n=%zu", n);
        expect_close(tag, calc_max_avx2(data, n), rmx);
    }
    if (cpu.has_avx512f) {
        snprintf(tag, sizeof tag, "sum/avx512/n=%zu", n);
        expect_close(tag, calc_sum_avx512(data, n), rs);
        snprintf(tag, sizeof tag, "min/avx512/n=%zu", n);
        expect_close(tag, calc_min_avx512(data, n), rmn);
        snprintf(tag, sizeof tag, "max/avx512/n=%zu", n);
        expect_close(tag, calc_max_avx512(data, n), rmx);
    }
}

int main(void)
{
    /* deterministic dataset: positives, negatives, zeros */
    static double data[5000];
    unsigned x = 987u;
    for (size_t i = 0; i < sizeof data / sizeof data[0]; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        data[i] = ((double)(x & 0x7FF) - 1024.0) * 0.1; /* around -102..102 */
    }

    static const size_t sizes[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 1000, 4999, 5000};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        run_size(data, sizes[i]);

    /* empty-input semantics */
    expect_close("sum empty", calc_sum_sse2(data, 0), 0.0);
    expect_close("max empty", calc_max_sse2(data, 0), 0.0);
    expect_close("scalar max empty", calc_scalar_max(data, 0), 0.0);

    /* all-negative dataset exercises the broadcast-seed trick */
    double neg[9] = {-7.5, -3.0, -100.25, -1.0, -2.0, -0.5, -8.0, -64.0, -12.5};
    run_size(neg, 9);
    expect_close("max all-negative sse2", calc_max_sse2(neg, 9), -0.5);

    if (failures == 0) {
        printf("calc-tests: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "calc-tests: %d failure(s)\n", failures);
    return 1;
}
