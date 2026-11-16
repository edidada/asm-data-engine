/*
 * calc_threads.c - V5: pthread-based chunked aggregation.
 *
 * Design notes:
 *   - The input is split on 64-byte (8 double) boundaries so no chunk
 *     straddles a cache line and every worker sees whole SIMD vectors.
 *   - A small persistent worker pool is created lazily and reused across
 *     calls. This matters for V6: a 1 GB file is scanned in many chunks,
 *     and spawning 16 threads per chunk would dominate the runtime.
 *   - Each worker owns a 64-byte aligned result slot. Slots are padded to
 *     one cache line to avoid false sharing between threads.
 *   - The calling thread is worker 0. Partial results are merged in chunk
 *     order, so the floating-point reduction order is deterministic for a
 *     fixed thread count.
 *   - Very small inputs stay on the calling thread: waking the pool costs
 *     more than aggregating a few thousand doubles.
 */
#define _POSIX_C_SOURCE 200809L

#include "calc.h"

#if defined(__unix__) || defined(__APPLE__)
#define CALC_HAVE_PTHREAD 1
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef CALC_HAVE_PTHREAD
#include <pthread.h>
#include <unistd.h>
#endif

/* Minimum doubles per worker (64 KiB of input) before going parallel. */
#define CALC_MT_MIN_ELEMS ((size_t)8192)
#define CALC_MT_MAX_THREADS 256
#define CALC_CACHE_LINE 64

int calc_cpu_count(void)
{
#ifdef CALC_HAVE_PTHREAD
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0)
        return (int)n;
#endif
    return 1;
}

/* One cache line per worker result: no false sharing between slots. */
typedef union {
    calc_result result;
    unsigned char line[CALC_CACHE_LINE];
} worker_slot;

typedef struct {
    const double *data;
    size_t start;
    size_t end;
    calc_engine engine;
    worker_slot *slot;
} worker_arg;

static void worker_run(worker_arg *arg)
{
    calc_engine_aggregate(arg->data + arg->start, arg->end - arg->start,
                          arg->engine, &arg->slot->result);
}

#ifdef CALC_HAVE_PTHREAD

static void *worker_entry(void *opaque)
{
    worker_run(opaque);
    return NULL;
}

/* --------------------------- persistent pool --------------------------- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t work_cv;
    pthread_cond_t done_cv;
    int nworkers;            /* worker threads created so far */
    int active;              /* workers taking part in the current job */
    int pending;             /* active workers not finished yet */
    unsigned long generation;
    int disabled;            /* creation failed: use per-call threads */
    worker_arg *job_args;    /* caller-owned array; [0] is the caller */
} thread_pool;

static thread_pool g_pool = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .work_cv = PTHREAD_COND_INITIALIZER,
    .done_cv = PTHREAD_COND_INITIALIZER,
};

static void *pool_worker(void *opaque)
{
    int index = (int)(intptr_t)opaque;
    unsigned long seen = 0;

    pthread_mutex_lock(&g_pool.mu);
    for (;;) {
        while (!g_pool.disabled &&
               (g_pool.generation == seen || index >= g_pool.active))
            pthread_cond_wait(&g_pool.work_cv, &g_pool.mu);
        if (g_pool.disabled)
            break;

        seen = g_pool.generation;
        /* Copy the descriptor while locked; the caller frees it only after
         * pending drops to zero. */
        worker_arg arg = g_pool.job_args[index + 1];
        pthread_mutex_unlock(&g_pool.mu);

        worker_run(&arg);

        pthread_mutex_lock(&g_pool.mu);
        g_pool.pending--;
        if (g_pool.pending == 0)
            pthread_cond_signal(&g_pool.done_cv);
    }
    pthread_mutex_unlock(&g_pool.mu);
    return NULL;
}

/* Called with g_pool.mu held. Returns 0 when the pool can run `threads-1`
 * workers, -1 when creation failed (pool is then disabled for good). */
static int pool_ensure_locked(int need)
{
    if (g_pool.disabled)
        return -1;

    while (g_pool.nworkers < need) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, pool_worker,
                           (void *)(intptr_t)g_pool.nworkers) != 0) {
            g_pool.disabled = 1;
            pthread_cond_broadcast(&g_pool.work_cv);
            return -1;
        }
        g_pool.nworkers++;
    }
    return 0;
}

/* Returns 1 when the pool executed the job, 0 when the caller must fall
 * back to spawning threads for this call. */
static int pool_run(worker_arg *args, int threads)
{
    pthread_mutex_lock(&g_pool.mu);
    if (pool_ensure_locked(threads - 1) != 0) {
        pthread_mutex_unlock(&g_pool.mu);
        return 0;
    }

    g_pool.job_args = args;
    g_pool.active = threads - 1;
    g_pool.pending = threads - 1;
    g_pool.generation++;
    pthread_cond_broadcast(&g_pool.work_cv);
    pthread_mutex_unlock(&g_pool.mu);

    /* The calling thread does its own share while workers run. */
    worker_run(&args[0]);

    pthread_mutex_lock(&g_pool.mu);
    while (g_pool.pending > 0)
        pthread_cond_wait(&g_pool.done_cv, &g_pool.mu);
    pthread_mutex_unlock(&g_pool.mu);
    return 1;
}

/* Fallback used only if the pool cannot be created (e.g. EAGAIN). */
static void spawn_run(worker_arg *args, int threads)
{
    pthread_t *tids = calloc((size_t)threads, sizeof *tids);
    unsigned char *started = calloc((size_t)threads, sizeof *started);
    if (!tids || !started) {
        free(tids);
        free(started);
        for (int i = 0; i < threads; i++)
            worker_run(&args[i]);
        return;
    }

    for (int i = 1; i < threads; i++) {
        if (pthread_create(&tids[i], NULL, worker_entry, &args[i]) == 0)
            started[i] = 1;
        else
            worker_run(&args[i]);
    }
    worker_run(&args[0]);

    for (int i = 1; i < threads; i++) {
        if (started[i])
            pthread_join(tids[i], NULL);
    }
    free(tids);
    free(started);
}

int calc_aggregate_mt(const double *data, size_t n, calc_engine engine,
                      int threads, calc_result *out)
{
    memset(out, 0, sizeof *out);
    if (n == 0)
        return 0;
    if (!data)
        return -1;

    if (threads <= 0)
        threads = calc_cpu_count();
    if (threads > CALC_MT_MAX_THREADS)
        threads = CALC_MT_MAX_THREADS;

    /* Keep enough work per worker and avoid pool wake-ups for tiny inputs. */
    size_t max_threads = n / CALC_MT_MIN_ELEMS;
    if (max_threads == 0)
        max_threads = 1;
    if ((size_t)threads > max_threads)
        threads = (int)max_threads;

    if (threads <= 1) {
        calc_engine_aggregate(data, n, engine, out);
        return 0;
    }

    worker_slot *slots = NULL;
    worker_arg *args = calloc((size_t)threads, sizeof *args);
    if (!args ||
        posix_memalign((void **)&slots, CALC_CACHE_LINE,
                       (size_t)threads * sizeof *slots) != 0) {
        free(args);
        free(slots);
        calc_engine_aggregate(data, n, engine, out);
        return 0;
    }
    memset(slots, 0, (size_t)threads * sizeof *slots);

    /* Split on 8-double boundaries; the last worker takes the remainder. */
    size_t base = (n / (size_t)threads) & ~(size_t)7;
    if (base == 0)
        base = 8;
    size_t start = 0;
    int used = 0;
    for (int i = 0; i < threads && start < n; i++) {
        size_t end = (i == threads - 1) ? n : start + base;
        if (end > n)
            end = n;

        args[used].data = data;
        args[used].start = start;
        args[used].end = end;
        args[used].engine = engine;
        args[used].slot = &slots[used];
        used++;
        start = end;
    }

    if (used <= 1) {
        worker_run(&args[0]);
    } else if (!pool_run(args, used)) {
        spawn_run(args, used);
    }

    for (int i = 0; i < used; i++)
        calc_result_merge(out, &slots[i].result);

    free(slots);
    free(args);
    return 0;
}

#else /* no pthreads: graceful single-thread fallback */

int calc_aggregate_mt(const double *data, size_t n, calc_engine engine,
                      int threads, calc_result *out)
{
    (void)threads;
    if (!data && n > 0) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    calc_engine_aggregate(data, n, engine, out);
    return 0;
}

#endif
