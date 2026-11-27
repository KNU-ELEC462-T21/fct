#include "bitmap.h"
#include "checksum.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

uint32_t fct_crc32(const void* data, size_t len) {
    uLong crc = crc32(0L, Z_NULL, 0);
    const unsigned char* p = data;
    if (len == 0)
        return 0;
    if (!data)
        return 0;
    while (len > 0) {
        uInt step = len > (size_t)UINT_MAX ? UINT_MAX : (uInt)len;
        crc = crc32(crc, p, step);
        p += step;
        len -= step;
    }
    return (uint32_t)crc;
}

static uint8_t last_byte_mask(uint32_t total_chunks) {
    unsigned used = total_chunks % 8U;
    return used == 0 ? 0xffU : (uint8_t)((1U << used) - 1U);
}

static bool trailing_bits_clear(const uint8_t* bits, size_t byte_len, uint32_t total_chunks) {
    return byte_len == 0 || (bits[byte_len - 1] & (uint8_t)~last_byte_mask(total_chunks)) == 0;
}

static uint32_t count_bits(const uint8_t* bits, size_t byte_len) {
    uint32_t n = 0;
    for (size_t i = 0; i < byte_len; ++i)
        n += (uint32_t)__builtin_popcount(bits[i]);
    return n;
}

size_t fct_bitmap_byte_len(uint32_t total_chunks) {
    return ((size_t)total_chunks + 7U) / 8U;
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
    if (!bm || chunk_id >= bm->total_chunks || !bm->bits)
        return false;
    return ((bm->bits[chunk_id / 8U] >> (chunk_id % 8U)) & 1U) != 0;
}

int fct_bitmap_set(fct_bitmap_t* bm, uint32_t chunk_id) {
    if (!bm || chunk_id >= bm->total_chunks || !bm->bits) {
        errno = EINVAL;
        return -1;
    }
    if (!fct_bitmap_get(bm, chunk_id)) {
        bm->bits[chunk_id / 8U] |= (uint8_t)(1U << (chunk_id % 8U));
        bm->completed_chunks++;
    }
    return 0;
}

int fct_bitmap_clear(fct_bitmap_t* bm, uint32_t chunk_id) {
    if (!bm || chunk_id >= bm->total_chunks || !bm->bits) {
        errno = EINVAL;
        return -1;
    }
    if (fct_bitmap_get(bm, chunk_id)) {
        bm->bits[chunk_id / 8U] &= (uint8_t) ~(1U << (chunk_id % 8U));
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
    bm->completed_chunks = count_bits(bm->bits, byte_len);
    return 0;
}

bool fct_bitmap_is_superset(const fct_bitmap_t* newer, const fct_bitmap_t* older) {
    if (!newer || !older || newer->total_chunks != older->total_chunks ||
        newer->byte_len != older->byte_len) {
        return false;
    }
    for (size_t i = 0; i < newer->byte_len; ++i) {
        if ((newer->bits[i] & older->bits[i]) != older->bits[i])
            return false;
    }
    return true;
}
