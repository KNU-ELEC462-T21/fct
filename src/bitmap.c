#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "bitmap.h"

#include "checksum.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* .bitmap file layout (big-endian):
 *   magic(4) version(4) file_size(8) chunk_size(4) total_chunks(4)
 *   file_sha256(32) filename(256) bitmap_bytes(4) bits[bitmap_bytes]
 *   file_crc32(4) = CRC32 of every byte before it. */
#define FILENAME_SIZE 256
#define HEADER_SIZE (4 + 4 + 8 + 4 + 4 + FCT_SHA256_SIZE + FILENAME_SIZE + 4)
#define TRAILER_SIZE 4

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put64(uint8_t* p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t get64(const uint8_t* p) {
    return (uint64_t)get32(p) << 32 | get32(p + 4);
}

/* Mask of the bits that are in use in the last byte (bit i lives at
 * bits[i / 8], value 1 << (i % 8)). */
static uint8_t last_byte_mask(uint32_t total_chunks) {
    unsigned used = total_chunks % 8;
    return used == 0 ? 0xff : (uint8_t)((1u << used) - 1);
}

/* True when every bit past total_chunks is zero. */
static bool trailing_bits_clear(const uint8_t* bits, size_t byte_len, uint32_t total_chunks) {
    return byte_len == 0 || (bits[byte_len - 1] & ~last_byte_mask(total_chunks)) == 0;
}

static uint32_t count_bits(const uint8_t* bits, size_t byte_len) {
    uint32_t n = 0;

    for (size_t i = 0; i < byte_len; i++)
        n += (uint32_t)__builtin_popcount(bits[i]);
    return n;
}

/* Copy a 256-byte filename field up to its first NUL and zero the rest, so bytes
 * after the terminator never affect what is stored or compared. */
static void normalize_filename(uint8_t dst[FILENAME_SIZE], const char* src) {
    size_t n = strnlen(src, FILENAME_SIZE);

    memset(dst, 0, FILENAME_SIZE);
    memcpy(dst, src, n);
}

size_t fct_bitmap_byte_len(uint32_t total_chunks) {
    return ((size_t)total_chunks + 7) / 8;
}

int fct_bitmap_init(fct_bitmap_t* bm, uint32_t total_chunks) {
    size_t len = fct_bitmap_byte_len(total_chunks);

    if (!bm || len > FCT_MAX_BITMAP_BYTES) {
        errno = EINVAL;
        return -1;
    }
    memset(bm, 0, sizeof(*bm));
    if (len > 0) {
        bm->bits = calloc(len, 1);
        if (!bm->bits)
            return -1;
    }
    bm->total_chunks = total_chunks;
    bm->byte_len = len;
    return 0;
}

void fct_bitmap_free(fct_bitmap_t* bm) {
    if (!bm)
        return;
    free(bm->bits);
    memset(bm, 0, sizeof(*bm));
}

bool fct_bitmap_get(const fct_bitmap_t* bm, uint32_t chunk_id) {
    if (!bm || chunk_id >= bm->total_chunks)
        return false;
    return (bm->bits[chunk_id / 8] >> (chunk_id % 8)) & 1;
}

int fct_bitmap_set(fct_bitmap_t* bm, uint32_t chunk_id) {
    if (!bm || chunk_id >= bm->total_chunks) {
        errno = EINVAL;
        return -1;
    }
    if (!fct_bitmap_get(bm, chunk_id)) {
        bm->bits[chunk_id / 8] |= (uint8_t)(1u << (chunk_id % 8));
        bm->completed_chunks++;
    }
    return 0;
}

int fct_bitmap_clear(fct_bitmap_t* bm, uint32_t chunk_id) {
    if (!bm || chunk_id >= bm->total_chunks) {
        errno = EINVAL;
        return -1;
    }
    if (fct_bitmap_get(bm, chunk_id)) {
        bm->bits[chunk_id / 8] &= (uint8_t)~(1u << (chunk_id % 8));
        bm->completed_chunks--;
    }
    return 0;
}

uint32_t fct_bitmap_completed(const fct_bitmap_t* bm) {
    return bm ? bm->completed_chunks : 0;
}

uint32_t fct_bitmap_missing(const fct_bitmap_t* bm) {
    return bm ? bm->total_chunks - bm->completed_chunks : 0;
}

bool fct_bitmap_is_complete(const fct_bitmap_t* bm) {
    return bm && bm->completed_chunks == bm->total_chunks;
}

int fct_bitmap_load_bytes(fct_bitmap_t* bm, const uint8_t* src, size_t byte_len) {
    if (!bm || (byte_len > 0 && !src)) {
        errno = EINVAL;
        return -1;
    }
    if (byte_len != bm->byte_len || !trailing_bits_clear(src, byte_len, bm->total_chunks)) {
        errno = EPROTO;
        return -1;
    }
    if (byte_len > 0)
        memcpy(bm->bits, src, byte_len);
    bm->completed_chunks = count_bits(bm->bits, bm->byte_len);
    return 0;
}

bool fct_bitmap_is_superset(const fct_bitmap_t* newer, const fct_bitmap_t* older) {
    if (!newer || !older || newer->total_chunks != older->total_chunks)
        return false;
    for (size_t i = 0; i < newer->byte_len; i++)
        if ((older->bits[i] & ~newer->bits[i]) != 0)
            return false;
    return true;
}

/* Write all of buf, retrying short writes and interruptions. */
static int write_all(int fd, const uint8_t* buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read exactly len bytes from offset 0; a short file is EPROTO. */
static int read_all(int fd, uint8_t* buf, size_t len) {
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(fd, buf + done, len - done, (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EPROTO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int fct_bitmap_save_file(const fct_bitmap_t* bm, const fct_file_meta_t* meta, const char* path) {
    if (!bm || !meta || !path || !*path || meta->total_chunks != bm->total_chunks ||
        (bm->byte_len > 0 && !bm->bits)) {
        errno = EINVAL;
        return -1;
    }

    /* Serialize the whole file in memory. */
    size_t total = HEADER_SIZE + bm->byte_len + TRAILER_SIZE;
    uint8_t* buf = malloc(total);
    if (!buf)
        return -1;

    uint8_t* p = buf;
    put32(p, FCT_BITMAP_MAGIC);
    put32(p + 4, FCT_BITMAP_VERSION);
    put64(p + 8, meta->file_size);
    put32(p + 16, meta->chunk_size);
    put32(p + 20, meta->total_chunks);
    p += 24;
    memcpy(p, meta->file_sha256, FCT_SHA256_SIZE);
    p += FCT_SHA256_SIZE;
    normalize_filename(p, meta->filename);
    p += FILENAME_SIZE;
    put32(p, (uint32_t)bm->byte_len);
    p += 4;
    if (bm->byte_len > 0)
        memcpy(p, bm->bits, bm->byte_len);
    p += bm->byte_len;
    put32(p, fct_crc32(buf, (size_t)(p - buf)));

    /* Write a temporary file next to path, flush it, then rename over path so a
     * crash leaves either the old or the new bitmap, never a partial one. */
    static const char suffix[] = ".XXXXXX";
    size_t path_len = strlen(path);
    char* tmp = malloc(path_len + sizeof(suffix));
    if (!tmp) {
        free(buf);
        return -1;
    }
    memcpy(tmp, path, path_len);
    memcpy(tmp + path_len, suffix, sizeof(suffix));

    int fd = mkstemp(tmp);
    int err = 0;
    if (fd < 0) {
        err = errno;
    } else {
        if (write_all(fd, buf, total) < 0 || fsync(fd) < 0)
            err = errno;
        if (close(fd) < 0 && err == 0)
            err = errno;
        if (err == 0 && rename(tmp, path) < 0)
            err = errno;
        if (err != 0)
            unlink(tmp); /* keep any existing bitmap untouched */
    }

    free(tmp);
    free(buf);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

int fct_bitmap_load_file(fct_bitmap_t* bm, const fct_file_meta_t* expected_meta, const char* path) {
    if (!bm || !expected_meta || !path) {
        errno = EINVAL;
        return -1;
    }
    memset(bm, 0, sizeof(*bm));

    size_t byte_len = fct_bitmap_byte_len(expected_meta->total_chunks);
    if (byte_len > FCT_MAX_BITMAP_BYTES) {
        errno = EPROTO;
        return -1;
    }
    size_t total = HEADER_SIZE + byte_len + TRAILER_SIZE;

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    struct stat st;
    uint8_t* buf = NULL;
    int err = 0;

    if (fstat(fd, &st) < 0) {
        err = errno;
    } else if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != total) {
        err = EPROTO;
    } else if (!(buf = malloc(total))) {
        err = errno;
    } else if (read_all(fd, buf, total) < 0) {
        err = errno;
    }
    close(fd);

    if (err == 0) {
        const uint8_t* p = buf;
        const uint8_t* bits = buf + HEADER_SIZE;
        uint8_t stored_name[FILENAME_SIZE], expected_name[FILENAME_SIZE];

        normalize_filename(stored_name, (const char*)(p + 24 + FCT_SHA256_SIZE));
        normalize_filename(expected_name, expected_meta->filename);

        /* Check integrity first, then that the file belongs to this transfer. */
        if (get32(buf + total - TRAILER_SIZE) != fct_crc32(buf, total - TRAILER_SIZE) ||
            get32(p) != FCT_BITMAP_MAGIC || get32(p + 4) != FCT_BITMAP_VERSION ||
            get64(p + 8) != expected_meta->file_size ||
            get32(p + 16) != expected_meta->chunk_size ||
            get32(p + 20) != expected_meta->total_chunks ||
            memcmp(p + 24, expected_meta->file_sha256, FCT_SHA256_SIZE) != 0 ||
            memcmp(stored_name, expected_name, FILENAME_SIZE) != 0 ||
            get32(p + HEADER_SIZE - 4) != byte_len ||
            !trailing_bits_clear(bits, byte_len, expected_meta->total_chunks)) {
            err = EPROTO;
        } else if (fct_bitmap_init(bm, expected_meta->total_chunks) < 0 ||
                   fct_bitmap_load_bytes(bm, bits, byte_len) < 0) {
            err = errno;
            fct_bitmap_free(bm);
        }
    }

    free(buf);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}
