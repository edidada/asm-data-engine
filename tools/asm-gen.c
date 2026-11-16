/*
 * asm-gen.c - Fast raw-double generator for large-scale tests (V6).
 *
 * scripts/convert.py is fine for real Parquet input, but generating a
 * billion records with Python takes minutes. This tool writes the same
 * format (little-endian doubles, 8 bytes each) at disk speed, which makes
 * it easy to exercise mmap / chunked I/O on 100M+ record files.
 *
 * Usage:
 *   ./asm-gen --out big.bin --count 100000000 [--seed 42]
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_DOUBLES 65536

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --out FILE --count N [--seed S]\n"
            "\n"
            "Writes N taxi-fare-like doubles in [2.00, 249.99] as a raw\n"
            "little-endian double stream (8 bytes per record).\n",
            prog);
}

static uint64_t next_u64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    uint64_t count = 0;
    uint64_t seed = 42;
    int have_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            count = strtoull(argv[++i], &end, 10);
            if (errno != 0 || !end || *end != '\0') {
                fprintf(stderr, "error: invalid --count '%s'\n", argv[i]);
                return 2;
            }
            have_count = 1;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 10);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!out || !have_count) {
        usage(argv[0]);
        return 2;
    }

    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot create '%s': %s\n", out,
                strerror(errno));
        return 1;
    }

    static double block[BLOCK_DOUBLES];
    uint64_t state = seed ? seed : 1;
    uint64_t written = 0;

    while (written < count) {
        size_t n = (size_t)((count - written < BLOCK_DOUBLES)
                                ? (count - written)
                                : BLOCK_DOUBLES);
        for (size_t i = 0; i < n; i++) {
            /* 200..24999 cents -> 2.00 .. 249.99 */
            uint64_t cents = 200 + next_u64(&state) % 24800;
            block[i] = (double)cents / 100.0;
        }
        if (fwrite(block, sizeof(double), n, f) != n) {
            fprintf(stderr, "error: write failed on '%s': %s\n", out,
                    strerror(errno));
            fclose(f);
            return 1;
        }
        written += n;
    }

    if (fclose(f) != 0) {
        fprintf(stderr, "error: close failed on '%s': %s\n", out,
                strerror(errno));
        return 1;
    }

    printf("wrote %llu doubles (%.2f MB) -> %s\n",
           (unsigned long long)count, (double)count * 8.0 / (1024.0 * 1024.0),
           out);
    return 0;
}
