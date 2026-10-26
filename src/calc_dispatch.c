/*
 * calc_dispatch.c - Runtime CPU feature detection and engine selection.
 * Uses CPUID leaf 7 (structured extended feature flags) via cpuid.h.
 */
#include "calc.h"

#if defined(__x86_64__)
#include <cpuid.h>

void calc_query_cpu(calc_cpu_features *features)
{
    unsigned int eax, ebx, ecx, edx;

    features->has_sse2 = 1; /* guaranteed on x86-64 */
    features->has_avx2 = 0;
    features->has_avx512f = 0;

    /* OSXSAVE must be set so YMM/ZMM state is enabled by the OS */
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return;
    if (!(ecx & bit_OSXSAVE))
        return;

    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        return;

    if (ebx & bit_AVX2)
        features->has_avx2 = 1;
    if (ebx & bit_AVX512F)
        features->has_avx512f = 1;
}

#else /* non-x86-64: scalar-only fallback */

void calc_query_cpu(calc_cpu_features *features)
{
    features->has_sse2 = 0;
    features->has_avx2 = 0;
    features->has_avx512f = 0;
}

#endif

calc_engine calc_pick_engine(const calc_cpu_features *features)
{
    if (features->has_avx512f)
        return CALC_ENGINE_AVX512;
    if (features->has_avx2)
        return CALC_ENGINE_AVX2;
    return CALC_ENGINE_SSE2;
}

const char *calc_engine_name(calc_engine e)
{
    switch (e) {
    case CALC_ENGINE_SCALAR: return "scalar";
    case CALC_ENGINE_SSE2:   return "sse2";
    case CALC_ENGINE_AVX2:   return "avx2";
    case CALC_ENGINE_AVX512: return "avx512";
    default:                 return "unknown";
    }
}
