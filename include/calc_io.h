/*
 * calc_io.h - V6: chunked file input for raw double streams.
 *
 * The reader hides the two ways of getting bytes off disk:
 *
 *   CALC_IO_MMAP  map the whole file and hand out slices (zero copy).
 *   CALC_IO_READ  read fixed-size chunks into an aligned buffer.
 *   CALC_IO_AUTO  try mmap, fall back to read if mapping is not possible.
 *
 * The API is a forward iterator: `calc_file_next` returns the next chunk of
 * complete doubles, so a caller can aggregate hundreds of millions of
 * records with constant memory (even in read mode).
 */
#ifndef ASM_DATA_ENGINE_CALC_IO_H
#define ASM_DATA_ENGINE_CALC_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CALC_IO_AUTO = 0, /* mmap when possible, otherwise chunked read */
    CALC_IO_MMAP,     /* force mmap (fails for non-regular files) */
    CALC_IO_READ      /* force chunked read() */
} calc_io_mode;

typedef struct calc_file calc_file;

/* Default chunk size: 64 MiB, a good balance between syscall count and L3. */
#define CALC_IO_DEFAULT_CHUNK ((size_t)64 << 20)

/*
 * Open a file of little-endian doubles. `chunk_bytes` is rounded down to a
 * 4096-byte multiple and defaults to CALC_IO_DEFAULT_CHUNK when 0.
 * Returns NULL on error; calc_io_last_error() then describes the failure.
 */
calc_file *calc_file_open(const char *path, calc_io_mode mode,
                          size_t chunk_bytes);

/* Message for the most recent failed open (thread-local, never NULL). */
const char *calc_io_last_error(void);

void calc_file_close(calc_file *file);

/*
 * Return the next chunk: sets *data to the first double and returns the
 * element count, or returns 0 at EOF / on error. `*data` may be NULL.
 */
size_t calc_file_next(calc_file *file, const double **data);

/* Rewind to the start so the same handle can be scanned again. */
int calc_file_rewind(calc_file *file);

/* Last error message, or NULL when no error occurred. */
const char *calc_file_error(const calc_file *file);

/* File size in bytes, file size in doubles, and the mode actually used. */
uint64_t calc_file_size(const calc_file *file);
size_t calc_file_count(const calc_file *file);
calc_io_mode calc_file_mode(const calc_file *file);

const char *calc_io_mode_name(calc_io_mode mode);

#ifdef __cplusplus
}
#endif

#endif /* ASM_DATA_ENGINE_CALC_IO_H */
