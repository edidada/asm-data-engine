/*
 * calc_engine.c - Engine dispatch for full aggregate passes (SUM/MIN/MAX).
 *
 * The SIMD files expose one function per statistic; this file groups them
 * into a single pass descriptor so the threaded driver (V5) and the file
 * scanner (V6) can treat every engine uniformly.
 */
#include "calc.h"

#include <string.h>

void calc_result_merge(calc_result *acc, const calc_result *part)
{
    if (part->count == 0)
        return;
    if (acc->count == 0) {
        *acc = *part;
        return;
    }
    acc->sum += part->sum;
    if (part->min < acc->min)
        acc->min = part->min;
    if (part->max > acc->max)
        acc->max = part->max;
    acc->count += part->count;
}

void calc_engine_aggregate(const double *data, size_t n, calc_engine engine,
                           calc_result *out)
{
    memset(out, 0, sizeof *out);
    if (n == 0)
        return;

    out->count = n;
    switch (engine) {
    case CALC_ENGINE_SCALAR:
        out->sum = calc_scalar_sum(data, n);
        out->min = calc_scalar_min(data, n);
        out->max = calc_scalar_max(data, n);
        break;
    case CALC_ENGINE_SSE2:
        out->sum = calc_sum_sse2(data, n);
        out->min = calc_min_sse2(data, n);
        out->max = calc_max_sse2(data, n);
        break;
    case CALC_ENGINE_AVX2:
        out->sum = calc_sum_avx2(data, n);
        out->min = calc_min_avx2(data, n);
        out->max = calc_max_avx2(data, n);
        break;
    case CALC_ENGINE_AVX512:
        out->sum = calc_sum_avx512(data, n);
        out->min = calc_min_avx512(data, n);
        out->max = calc_max_avx512(data, n);
        break;
    default:
        out->count = 0;
        break;
    }
}
