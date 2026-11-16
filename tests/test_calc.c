/*
 * test_calc.c - Correctness tests.
 *
 *   - every SIMD engine must agree with the scalar C reference on a
 *     deterministic dataset, including awkward sizes that exercise the
 *     vector loops, the scalar tail and empty-input semantics;
 *   - V5: pthread aggregation (1..16 threads, tiny and large inputs);
 *   - V6: mmap and chunked-read file iterators, multiple chunk sizes,
 *     rewind, empty files and malformed files;
 *   - V7: TSC calibration and monotonicity.
 */
#define _POSIX_C_SOURCE 200809L

#include "calc.h"
#include "calc_cycles.h"
#include "calc_io.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DATA_CAP 100000

static int failures = 0;

static void expect_close(const char *what, double got, double ref)
{
    double diff = got - ref;
    if (diff < 0)
        diff = -diff;
    double tol = 1e-9 * (fabs(ref) > 1.0 ? fabs(ref) : 1.0);
    if (!(diff <= tol)) {
        fprintf(stderr, "FAIL %-26s got=%.17g ref=%.17g\n", what, got, ref);
        failures++;
    }
}

static void expect_size(const char *what, size_t got, size_t ref)
{
    if (got != ref) {
        fprintf(stderr, "FAIL %-26s got=%zu ref=%zu\n", what, got, ref);
        failures++;
    }
}

static void expect_aggregate(const char *tag, const calc_result *got,
                             const calc_result *ref)
{
    char label[96];
    snprintf(label, sizeof label, "count/%s", tag);
    expect_size(label, got->count, ref->count);
    snprintf(label, sizeof label, "sum/%s", tag);
    expect_close(label, got->sum, ref->sum);
    snprintf(label, sizeof label, "min/%s", tag);
    expect_close(label, got->min, ref->min);
    snprintf(label, sizeof label, "max/%s", tag);
    expect_close(label, got->max, ref->max);
}

/* --------------------------- kernel coverage --------------------------- */

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

    /* calc_engine_aggregate must expose exactly the same numbers. */
    calc_result ref = {n, rs, rmn, rmx};
    calc_result agg;
    calc_engine_aggregate(data, n, CALC_ENGINE_SCALAR, &agg);
    snprintf(tag, sizeof tag, "agg/scalar/n=%zu", n);
    expect_aggregate(tag, &agg, &ref);
    calc_engine_aggregate(data, n, CALC_ENGINE_SSE2, &agg);
    snprintf(tag, sizeof tag, "agg/sse2/n=%zu", n);
    expect_aggregate(tag, &agg, &ref);
    if (cpu.has_avx2) {
        calc_engine_aggregate(data, n, CALC_ENGINE_AVX2, &agg);
        snprintf(tag, sizeof tag, "agg/avx2/n=%zu", n);
        expect_aggregate(tag, &agg, &ref);
    }
    if (cpu.has_avx512f) {
        calc_engine_aggregate(data, n, CALC_ENGINE_AVX512, &agg);
        snprintf(tag, sizeof tag, "agg/avx512/n=%zu", n);
        expect_aggregate(tag, &agg, &ref);
    }
}

/* ----------------------------- V5: threads ----------------------------- */

static void run_threaded(const double *data, size_t n)
{
    calc_cpu_features cpu;
    calc_query_cpu(&cpu);
    calc_engine engine = calc_engine_resolve(calc_pick_engine(&cpu), &cpu);

    calc_result ref;
    calc_engine_aggregate(data, n, CALC_ENGINE_SCALAR, &ref);

    static const int thread_counts[] = {1, 2, 3, 4, 8, 16};
    for (size_t t = 0; t < sizeof thread_counts / sizeof thread_counts[0]; t++) {
        calc_result got;
        int rc = calc_aggregate_mt(data, n, engine, thread_counts[t], &got);
        char tag[64];
        snprintf(tag, sizeof tag, "mt/%d/n=%zu", thread_counts[t], n);
        expect_size(tag, (size_t)(rc == 0), 1);
        if (rc == 0)
            expect_aggregate(tag, &got, &ref);
    }

    /* auto thread count */
    calc_result got;
    if (calc_aggregate_mt(data, n, engine, 0, &got) == 0) {
        char tag[64];
        snprintf(tag, sizeof tag, "mt/auto/n=%zu", n);
        expect_aggregate(tag, &got, &ref);
    } else {
        fprintf(stderr, "FAIL mt/auto/n=%zu returned error\n", n);
        failures++;
    }
}

static void test_threads(const double *data)
{
    static const size_t sizes[] = {0, 1, 7, 8, 9, 63, 64, 65,
                                   1000, 4099, 8191, 8192, 100000};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        run_threaded(data, sizes[i]);

    /* NULL data with a non-zero length is a hard error. */
    calc_result got;
    expect_size("mt/null", (size_t)(calc_aggregate_mt(NULL, 10, CALC_ENGINE_SSE2,
                                                      2, &got) == -1),
                1);

    /* Merge semantics, including empty partials. */
    calc_result acc = {0, 0.0, 0.0, 0.0};
    calc_result part = {2, 5.0, -1.0, 3.0};
    calc_result empty = {0, 0.0, 0.0, 0.0};
    calc_result_merge(&acc, &empty);
    expect_size("merge/empty-count", acc.count, 0);
    calc_result_merge(&acc, &part);
    calc_result_merge(&acc, &empty);
    calc_result more = {3, 6.0, -4.0, 10.0};
    calc_result_merge(&acc, &more);
    calc_result merged_ref = {5, 11.0, -4.0, 10.0};
    expect_aggregate("merge", &acc, &merged_ref);

    /* Engine downgrade rules. */
    calc_cpu_features cpu;
    calc_query_cpu(&cpu);
    expect_size("resolve/avx512",
                (size_t)calc_engine_resolve(CALC_ENGINE_AVX512, &cpu),
                (size_t)(cpu.has_avx512f
                             ? CALC_ENGINE_AVX512
                             : (cpu.has_avx2 ? CALC_ENGINE_AVX2
                                             : CALC_ENGINE_SSE2)));
    calc_cpu_features no_simd = {0, 0, 0};
    expect_size("resolve/scalar",
                (size_t)calc_engine_resolve(CALC_ENGINE_AVX512, &no_simd),
                (size_t)CALC_ENGINE_SCALAR);
}

/* ------------------------------- V6: I/O ------------------------------- */

static int write_all(int fd, const void *buf, size_t bytes)
{
    const unsigned char *p = buf;
    while (bytes > 0) {
        ssize_t w = write(fd, p, bytes);
        if (w < 0)
            return -1;
        p += (size_t)w;
        bytes -= (size_t)w;
    }
    return 0;
}

static int scan_io(calc_file *file, size_t *out_chunks, calc_result *out)
{
    calc_result acc;
    memset(&acc, 0, sizeof acc);
    size_t chunks = 0;
    const double *chunk;
    size_t count;

    while ((count = calc_file_next(file, &chunk)) > 0) {
        calc_result part;
        calc_engine_aggregate(chunk, count, CALC_ENGINE_SSE2, &part);
        calc_result_merge(&acc, &part);
        chunks++;
    }
    if (calc_file_error(file))
        return -1;

    *out = acc;
    *out_chunks = chunks;
    return 0;
}

static void run_io(const double *data, size_t n)
{
    char path[] = "/tmp/asm-data-engine-io-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "FAIL mkstemp\n");
        failures++;
        return;
    }
    if (write_all(fd, data, n * sizeof(double)) != 0) {
        fprintf(stderr, "FAIL write temp file\n");
        failures++;
        close(fd);
        unlink(path);
        return;
    }
    close(fd);

    calc_result ref;
    calc_engine_aggregate(data, n, CALC_ENGINE_SSE2, &ref);

    static const calc_io_mode modes[] = {CALC_IO_AUTO, CALC_IO_MMAP,
                                         CALC_IO_READ};
    static const size_t chunks[] = {4096, 8192, 1u << 20};

    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
            char tag[96];
            calc_file *file = calc_file_open(path, modes[m], chunks[c]);
            if (!file) {
                fprintf(stderr, "FAIL open %s chunk=%zu: %s\n",
                        calc_io_mode_name(modes[m]), chunks[c],
                        calc_io_last_error());
                failures++;
                continue;
            }

            size_t nchunks = 0;
            calc_result got;
            snprintf(tag, sizeof tag, "io/%s/%zu/n=%zu",
                     calc_io_mode_name(modes[m]), chunks[c], n);
            if (scan_io(file, &nchunks, &got) != 0) {
                fprintf(stderr, "FAIL scan %s: %s\n", tag,
                        calc_file_error(file));
                failures++;
            } else {
                expect_aggregate(tag, &got, &ref);
                size_t expected_chunks =
                    n == 0 ? 0 : (n * sizeof(double) + chunks[c] - 1) / chunks[c];
                snprintf(tag, sizeof tag, "io/chunks/%s/%zu/n=%zu",
                         calc_io_mode_name(modes[m]), chunks[c], n);
                expect_size(tag, nchunks, expected_chunks);
            }

            /* Rewind and scan again: must be repeatable. */
            if (calc_file_rewind(file) != 0) {
                fprintf(stderr, "FAIL rewind %s: %s\n", tag,
                        calc_file_error(file));
                failures++;
            } else if (scan_io(file, &nchunks, &got) == 0) {
                snprintf(tag, sizeof tag, "io/rewind/%s/%zu/n=%zu",
                         calc_io_mode_name(modes[m]), chunks[c], n);
                expect_aggregate(tag, &got, &ref);
            } else {
                fprintf(stderr, "FAIL rescan %s: %s\n", tag,
                        calc_file_error(file));
                failures++;
            }

            calc_file_close(file);
        }
    }

    unlink(path);
}

static void test_io(const double *data)
{
    static const size_t sizes[] = {0, 1, 7, 8, 512, 1000, 4096, 5000};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        run_io(data, sizes[i]);

    /* Missing file and malformed (not a multiple of 8 bytes) input. */
    char missing[128];
    snprintf(missing, sizeof missing, "/tmp/asm-data-engine-missing-%ld",
             (long)getpid());
    expect_size("io/missing",
                calc_file_open(missing, CALC_IO_MMAP, 0) == NULL, 1);

    char path[] = "/tmp/asm-data-engine-bad-XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        (void)write_all(fd, "abc", 3);
        close(fd);
        expect_size("io/bad-size",
                    calc_file_open(path, CALC_IO_READ, 0) == NULL, 1);
        expect_size("io/bad-size-msg",
                    (size_t)(strstr(calc_io_last_error(), "multiple of 8") !=
                             NULL),
                    1);
        unlink(path);
    }

    /* Mode names are part of the CLI contract. */
    expect_size("io/name-mmap", strcmp(calc_io_mode_name(CALC_IO_MMAP),
                                       "mmap") == 0, 1);
    expect_size("io/name-read", strcmp(calc_io_mode_name(CALC_IO_READ),
                                       "read") == 0, 1);
}

/* ------------------------------ V7: cycles ----------------------------- */

static void test_cycles(void)
{
    double hz = calc_cycle_hz();
    if (!(hz > 1e6)) {
        fprintf(stderr, "FAIL cycle-hz got=%.1f\n", hz);
        failures++;
    }

    volatile double acc = 0.0;
    uint64_t c0 = calc_cycles_begin();
    for (int i = 0; i < 10000; i++)
        acc += (double)i;
    uint64_t c1 = calc_cycles_end();

    if (c1 <= c0) {
        fprintf(stderr, "FAIL cycle-monotonic c0=%llu c1=%llu\n",
                (unsigned long long)c0, (unsigned long long)c1);
        failures++;
    }
    if (!(calc_cycles_to_ns(c1 - c0, hz) > 0.0)) {
        fprintf(stderr, "FAIL cycle-to-ns\n");
        failures++;
    }
    (void)acc;
}

/* ---------------------------------------------------------------------- */

int main(void)
{
    static double data[DATA_CAP];
    unsigned x = 987u;
    for (size_t i = 0; i < DATA_CAP; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        /* taxi-like positive values in [0.50, 1049.00] (cents) */
        data[i] = (double)(500u + x % 104851u) / 100.0;
    }

    static const size_t sizes[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17,
                                   1000, 4999, 5000};
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

    test_threads(data);
    test_io(data);
    test_cycles();

    if (failures == 0) {
        printf("calc-tests: all tests passed\n");
        return 0;
    }
    fprintf(stderr, "calc-tests: %d failure(s)\n", failures);
    return 1;
}
