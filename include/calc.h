/*
 * calc.h - Public API of asm-data-engine.
 *
 * All kernels take a raw array of little-endian IEEE-754 doubles and a
 * element count, and follow the System V AMD64 calling convention
 * (implemented in NASM assembly files under src/).
 *
 * Semantics:
 *   - calc_*_sum:  n == 0 returns 0.0.
 *   - calc_*_max / calc_*_min: n == 0 returns 0.0.
 */
#ifndef ASM_DATA_ENGINE_CALC_H
#define ASM_DATA_ENGINE_CALC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------- scalar C reference -------------------------- */
double calc_scalar_sum(const double *data, size_t n);
double calc_scalar_max(const double *data, size_t n);
double calc_scalar_min(const double *data, size_t n);

/* --------------------------- SSE2 (2 doubles) --------------------------- */
double calc_sum_sse2(const double *data, size_t n);
double calc_max_sse2(const double *data, size_t n);
double calc_min_sse2(const double *data, size_t n);

/* --------------------------- AVX2 (4 doubles) --------------------------- */
double calc_sum_avx2(const double *data, size_t n);
double calc_max_avx2(const double *data, size_t n);
double calc_min_avx2(const double *data, size_t n);

/* -------------------------- AVX-512 (8 doubles) ------------------------- */
double calc_sum_avx512(const double *data, size_t n);
double calc_max_avx512(const double *data, size_t n);
double calc_min_avx512(const double *data, size_t n);

/* ------------------------------ CPU detect ------------------------------ */
typedef struct {
    int has_sse2;    /* always 1 on x86-64 */
    int has_avx2;
    int has_avx512f;
} calc_cpu_features;

/* Fills features via CPUID. Only supported on x86-64 Linux builds. */
void calc_query_cpu(calc_cpu_features *features);

/* Engine identifiers for dispatch helpers. */
typedef enum {
    CALC_ENGINE_SCALAR = 0,
    CALC_ENGINE_SSE2,
    CALC_ENGINE_AVX2,
    CALC_ENGINE_AVX512
} calc_engine;

/* Best engine the current CPU supports (scalar -> sse2 -> avx2 -> avx512). */
calc_engine calc_pick_engine(const calc_cpu_features *features);

/* Downgrade `engine` until it is runnable on `features` (never fails). */
calc_engine calc_engine_resolve(calc_engine engine,
                                const calc_cpu_features *features);

const char *calc_engine_name(calc_engine e);

/* --------------------------- aggregate result --------------------------- */
/*
 * Result of one full SUM/MIN/MAX pass. `count` is the number of elements
 * aggregated, so a zeroed result is a valid "empty" accumulator.
 */
typedef struct {
    size_t count;
    double sum;
    double min;
    double max;
} calc_result;

/* Aggregate one contiguous range with `engine`. Pass the result of
 * calc_engine_resolve() when the engine came from user input. Empty input
 * yields a zeroed result. */
void calc_engine_aggregate(const double *data, size_t n, calc_engine engine,
                           calc_result *out);

/* Merge `part` into `acc`. Merging in chunk order keeps results stable. */
void calc_result_merge(calc_result *acc, const calc_result *part);

/* Online CPU count (>= 1). */
int calc_cpu_count(void);

/* ---------------------- V5: pthread chunked aggregation ----------------- */
/*
 * Split [data, data+n) into cache-line-aligned chunks, aggregate them in
 * parallel with `engine`, and merge partial results in chunk order.
 *
 * threads <= 0  -> auto (online CPUs).
 * Small inputs run on the calling thread to avoid spawn overhead.
 *
 * Returns 0 on success, -1 on invalid arguments.
 */
int calc_aggregate_mt(const double *data, size_t n, calc_engine engine,
                      int threads, calc_result *out);

#ifdef __cplusplus
}
#endif

#endif /* ASM_DATA_ENGINE_CALC_H */
