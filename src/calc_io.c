/*
 * calc_io.c - V6: mmap / chunked-read input for very large double streams.
 *
 * A 100M-record file is 800 MB; a billion records is 8 GB. Neither fits
 * comfortably in RAM together with the rest of the system, so the scanner
 * either maps the file and walks it chunk by chunk (the kernel handles
 * read-ahead and page eviction) or reads into one reusable 64 MiB buffer.
 *
 * Chunk sizes are page multiples, and the file size must be a multiple of
 * 8 bytes, so every chunk contains only complete doubles. No carry state is
 * needed across chunk boundaries.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "calc_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct calc_file {
    int fd;
    uint64_t size;  /* bytes */
    uint64_t pos;   /* bytes already handed out */
    calc_io_mode mode;
    unsigned char *map;
    size_t map_len;
    unsigned char *buf; /* read mode: page-aligned chunk buffer */
    size_t chunk_bytes;
    char err[256];
};

static _Thread_local char open_error[256];

const char *calc_io_last_error(void)
{
    return open_error[0] ? open_error : "no error";
}

static int set_open_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(open_error, sizeof open_error, fmt, ap);
    va_end(ap);
    return -1;
}

static int set_file_error(calc_file *file, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(file->err, sizeof file->err, fmt, ap);
    va_end(ap);
    return -1;
}

static size_t normalize_chunk(size_t chunk_bytes)
{
    if (chunk_bytes == 0)
        chunk_bytes = CALC_IO_DEFAULT_CHUNK;
    chunk_bytes &= ~(size_t)4095; /* whole pages */
    if (chunk_bytes < 4096)
        chunk_bytes = 4096;
    return chunk_bytes;
}

calc_file *calc_file_open(const char *path, calc_io_mode mode,
                          size_t chunk_bytes)
{
    open_error[0] = '\0';

    if (!path || path[0] == '\0') {
        set_open_error("empty input path");
        return NULL;
    }
    if (mode != CALC_IO_AUTO && mode != CALC_IO_MMAP &&
        mode != CALC_IO_READ) {
        set_open_error("unknown I/O mode");
        return NULL;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_open_error("cannot open '%s': %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        set_open_error("cannot stat '%s': %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    if (st.st_size < 0) {
        set_open_error("'%s' has an invalid size", path);
        close(fd);
        return NULL;
    }

    uint64_t size = (uint64_t)st.st_size;
    if (size % sizeof(double) != 0) {
        set_open_error("'%s' is not a multiple of 8 bytes "
                       "(raw double stream expected)", path);
        close(fd);
        return NULL;
    }

    calc_file *file = calloc(1, sizeof *file);
    if (!file) {
        set_open_error("out of memory");
        close(fd);
        return NULL;
    }
    file->fd = fd;
    file->size = size;
    file->chunk_bytes = normalize_chunk(chunk_bytes);

    /* Resolve AUTO -> mmap when the object supports it. */
    calc_io_mode actual = (mode == CALC_IO_AUTO) ? CALC_IO_MMAP : mode;
    if (actual == CALC_IO_MMAP && !S_ISREG(st.st_mode)) {
        if (mode == CALC_IO_MMAP) {
            set_open_error("'%s' is not a regular file; mmap is not possible",
                           path);
            goto fail;
        }
        actual = CALC_IO_READ;
    }

    if (actual == CALC_IO_MMAP && size > 0) {
        if (size > (uint64_t)SIZE_MAX) {
            if (mode == CALC_IO_MMAP) {
                set_open_error("'%s' is too large to map on this host", path);
                goto fail;
            }
            actual = CALC_IO_READ;
        } else {
            void *p = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                if (mode == CALC_IO_MMAP) {
                    set_open_error("cannot mmap '%s': %s", path,
                                   strerror(errno));
                    goto fail;
                }
                actual = CALC_IO_READ;
            } else {
                file->map = p;
                file->map_len = (size_t)size;
                /* The access pattern is a single forward sweep. */
                (void)madvise(p, (size_t)size, MADV_SEQUENTIAL);
            }
        }
    }

    if (actual == CALC_IO_READ) {
        void *p = NULL;
        if (posix_memalign(&p, 4096, file->chunk_bytes) != 0 || !p) {
            set_open_error("out of memory (chunk buffer)");
            goto fail;
        }
        file->buf = p;
    }

    file->mode = actual;
    return file;

fail:
    if (file->map)
        munmap(file->map, file->map_len);
    free(file);
    close(fd);
    return NULL;
}

void calc_file_close(calc_file *file)
{
    if (!file)
        return;
    if (file->map)
        munmap(file->map, file->map_len);
    free(file->buf);
    if (file->fd >= 0)
        close(file->fd);
    free(file);
}

size_t calc_file_next(calc_file *file, const double **data)
{
    if (data)
        *data = NULL;
    if (!file || file->err[0] || file->pos >= file->size)
        return 0;

    uint64_t remaining = file->size - file->pos;
    size_t want = (size_t)(remaining < file->chunk_bytes ? remaining
                                                         : file->chunk_bytes);

    if (file->mode == CALC_IO_MMAP) {
        const unsigned char *base = file->map;
        file->pos += want;
        if (data)
            *data = (const double *)(base + file->pos - want);
        return want / sizeof(double);
    }

    size_t got = 0;
    while (got < want) {
        ssize_t r = read(file->fd, file->buf + got, want - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            set_file_error(file, "read error: %s", strerror(errno));
            return 0;
        }
        if (r == 0) {
            set_file_error(file, "unexpected EOF (file truncated?)");
            return 0;
        }
        got += (size_t)r;
    }
    file->pos += got;
    if (data)
        *data = (const double *)file->buf;
    return got / sizeof(double);
}

int calc_file_rewind(calc_file *file)
{
    if (!file)
        return -1;
    if (file->mode == CALC_IO_READ &&
        lseek(file->fd, 0, SEEK_SET) != (off_t)0) {
        set_file_error(file, "cannot rewind: %s", strerror(errno));
        return -1;
    }
    file->pos = 0;
    file->err[0] = '\0';
    return 0;
}

const char *calc_file_error(const calc_file *file)
{
    if (!file || !file->err[0])
        return NULL;
    return file->err;
}

uint64_t calc_file_size(const calc_file *file)
{
    return file ? file->size : 0;
}

size_t calc_file_count(const calc_file *file)
{
    return file ? (size_t)(file->size / sizeof(double)) : 0;
}

calc_io_mode calc_file_mode(const calc_file *file)
{
    return file ? file->mode : CALC_IO_AUTO;
}

const char *calc_io_mode_name(calc_io_mode mode)
{
    switch (mode) {
    case CALC_IO_AUTO: return "auto";
    case CALC_IO_MMAP: return "mmap";
    case CALC_IO_READ: return "read";
    default:           return "unknown";
    }
}
