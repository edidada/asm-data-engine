/*
 * calc_scalar.c - Portable scalar C reference implementations.
 * Used as ground truth for testing and as "C -O3" baseline in benchmarks.
 */
#include "calc.h"

double calc_scalar_sum(const double *data, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++)
        acc += data[i];
    return acc;
}

double calc_scalar_max(const double *data, size_t n)
{
    if (n == 0)
        return 0.0;
    double acc = data[0];
    for (size_t i = 1; i < n; i++)
        if (data[i] > acc)
            acc = data[i];
    return acc;
}

double calc_scalar_min(const double *data, size_t n)
{
    if (n == 0)
        return 0.0;
    double acc = data[0];
    for (size_t i = 1; i < n; i++)
        if (data[i] < acc)
            acc = data[i];
    return acc;
}
