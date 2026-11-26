#define _POSIX_C_SOURCE 200809L

#include "compress_stub.h"
#include "common.h"
#include "compress.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

static _Atomic int g_encode_errno = 0;
static _Atomic int g_decode_errno = 0;

void compress_stub_reset(void) {
    atomic_store(&g_encode_errno, 0);
    atomic_store(&g_decode_errno, 0);
}

void compress_stub_set_encode_errno(int err) {
    atomic_store(&g_encode_errno, err);
}

void compress_stub_set_decode_errno(int err) {
    atomic_store(&g_decode_errno, err);
}

size_t fct_compress_bound(size_t raw_len) {
    if (raw_len > DEFAULT_CHUNK_SZ) {
        errno = EINVAL;
        return 0;
    }
    return raw_len + 16U;
}

int fct_compression_mode(const char* filename, const void* first_chunk, size_t len, bool force,
                         void* scratch, size_t capacity, fct_compression_mode_t* mode) {
    (void)filename;
    if (!mode || len > DEFAULT_CHUNK_SZ) {
        errno = EINVAL;
        return -1;
    }
    if (force) {
        *mode = FCT_COMPRESS_FORCE;
        return 0;
    }
    if (len == 0) {
        *mode = FCT_COMPRESS_RAW;
        return 0;
    }
    if (!first_chunk || !scratch || capacity < fct_compress_bound(len)) {
        errno = EINVAL;
        return -1;
    }
    *mode = FCT_COMPRESS_AUTO;
    return 0;
}

static bool is_uniform_bytes(const unsigned char* data, size_t len) {
    if (len < 8)
        return false;
    unsigned char first = data[0];
    for (size_t i = 1; i < len; ++i) {
        if (data[i] != first)
            return false;
    }
    return true;
}

int fct_encode_chunk(fct_compression_mode_t mode, const void* src, size_t raw_len, void* scratch,
                     size_t capacity, const void** payload, size_t* payload_len, uint32_t* flags) {
    int injected = atomic_load(&g_encode_errno);
    if (injected != 0) {
        errno = injected;
        return -1;
    }
    if (!payload || !payload_len || !flags || raw_len > DEFAULT_CHUNK_SZ || (raw_len > 0 && !src)) {
        errno = EINVAL;
        return -1;
    }
    if (mode == FCT_COMPRESS_RAW) {
        *payload = src;
        *payload_len = raw_len;
        *flags = 0;
        return 0;
    }
    if (mode != FCT_COMPRESS_AUTO && mode != FCT_COMPRESS_FORCE) {
        errno = EINVAL;
        return -1;
    }
    size_t bound = fct_compress_bound(raw_len);
    if (bound == 0 || !scratch || capacity < bound) {
        errno = EINVAL;
        return -1;
    }

    const unsigned char* in = src;
    unsigned char* out = scratch;
    if (is_uniform_bytes(in, raw_len)) {
        /* Compact 7-byte encoding: always smaller than raw_len (>= 8). */
        uint32_t len32 = (uint32_t)raw_len;
        out[0] = 0xFC;
        out[1] = 0x01;
        out[2] = (unsigned char)(len32 >> 24);
        out[3] = (unsigned char)(len32 >> 16);
        out[4] = (unsigned char)(len32 >> 8);
        out[5] = (unsigned char)len32;
        out[6] = in[0];
        *payload = scratch;
        *payload_len = 7;
        *flags = FCT_CHUNK_FLAG_COMPRESSED;
        return 0;
    }

    /* Expanded encoding: 2-byte header + bitwise-inverted payload. */
    out[0] = 0xFC;
    out[1] = 0x02;
    for (size_t i = 0; i < raw_len; ++i)
        out[2 + i] = (unsigned char)(~in[i]);

    if (mode == FCT_COMPRESS_AUTO) {
        *payload = src;
        *payload_len = raw_len;
        *flags = 0;
        return 0;
    }

    *payload = scratch;
    *payload_len = raw_len + 2U;
    *flags = FCT_CHUNK_FLAG_COMPRESSED;
    return 0;
}

int fct_decode_chunk(uint32_t flags, const void* payload, size_t payload_len, void* dst,
                     size_t expected_len) {
    int injected = atomic_load(&g_decode_errno);
    if (injected != 0) {
        errno = injected;
        return -1;
    }
    if (expected_len > DEFAULT_CHUNK_SZ || (expected_len > 0 && !dst) ||
        (payload_len > 0 && !payload)) {
        errno = EINVAL;
        return -1;
    }
    if (flags == 0) {
        if (payload_len != expected_len) {
            errno = EBADMSG;
            return -1;
        }
        if (expected_len > 0)
            memcpy(dst, payload, expected_len);
        return 0;
    }
    if (flags != FCT_CHUNK_FLAG_COMPRESSED) {
        errno = EINVAL;
        return -1;
    }
    const unsigned char* in = payload;
    if (payload_len < 2 || in[0] != 0xFC) {
        errno = EBADMSG;
        return -1;
    }
    if (in[1] == 0x01) {
        if (payload_len != 7 || expected_len < 8) {
            errno = EBADMSG;
            return -1;
        }
        uint32_t orig_len = ((uint32_t)in[2] << 24) | ((uint32_t)in[3] << 16) |
                            ((uint32_t)in[4] << 8) | (uint32_t)in[5];
        if (orig_len != expected_len) {
            errno = EBADMSG;
            return -1;
        }
        memset(dst, in[6], expected_len);
        return 0;
    }
    if (in[1] == 0x02) {
        if (payload_len != expected_len + 2U) {
            errno = EBADMSG;
            return -1;
        }
        unsigned char* out = dst;
        for (size_t i = 0; i < expected_len; ++i)
            out[i] = (unsigned char)(~in[2 + i]);
        return 0;
    }
    errno = EBADMSG;
    return -1;
}
