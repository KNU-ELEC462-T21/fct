#define _POSIX_C_SOURCE 200809L

#include "compress.h"

#include "common.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

/* Level 1 favors speed: the bottleneck is the network, not the ratio. */
#define FCT_ZLIB_LEVEL Z_BEST_SPEED

/* AUTO is chosen when the trial chunk shrinks to at most 95% of its size. */
#define FCT_AUTO_RATIO_PERCENT 95

/* Map a zlib status code to an errno value. */
static int zlib_errno(int status) {
    switch (status) {
    case Z_MEM_ERROR:
        return ENOMEM;
    case Z_BUF_ERROR:
        return ENOBUFS;
    default:
        return EIO;
    }
}

/* Compress src into scratch and store the length in *out_len.
 * Return 0 on success, or -1 with errno set. */
static int deflate_into(const void* src, size_t raw_len, void* scratch, size_t capacity,
                        size_t* out_len) {
    size_t bound = fct_compress_bound(raw_len);

    if (bound == 0)
        return -1;
    if (!scratch || capacity < bound) {
        errno = EINVAL;
        return -1;
    }

    uLongf len = (uLongf)bound;
    int status =
        compress2((Bytef*)scratch, &len, (const Bytef*)src, (uLong)raw_len, FCT_ZLIB_LEVEL);
    if (status != Z_OK) {
        errno = zlib_errno(status);
        return -1;
    }
    *out_len = (size_t)len;
    return 0;
}

/* True when the name ends in an extension of an already compressed format. */
static bool has_compressed_extension(const char* filename) {
    static const char* const exts[] = {".zip", ".gz", ".tgz", ".mp4"};
    const char* slash;
    size_t name_len;

    if (!filename)
        return false;
    slash = strrchr(filename, '/');
    if (slash)
        filename = slash + 1;
    name_len = strlen(filename);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t ext_len = strlen(exts[i]);
        if (name_len > ext_len && strcasecmp(filename + name_len - ext_len, exts[i]) == 0)
            return true;
    }
    return false;
}

/* True when the data starts with a ZIP, GZIP or MP4 signature. */
static bool has_compressed_signature(const unsigned char* data, size_t len) {
    if (len >= 4 && data[0] == 'P' && data[1] == 'K' &&
        ((data[2] == 0x03 && data[3] == 0x04) || (data[2] == 0x05 && data[3] == 0x06) ||
         (data[2] == 0x07 && data[3] == 0x08)))
        return true; /* ZIP */
    if (len >= 2 && data[0] == 0x1f && data[1] == 0x8b)
        return true; /* GZIP */
    if (len >= 8 && memcmp(data + 4, "ftyp", 4) == 0)
        return true; /* MP4 and other ISO media files */
    return false;
}

size_t fct_compress_bound(size_t raw_len) {
    if (raw_len > DEFAULT_CHUNK_SZ) {
        errno = EINVAL;
        return 0;
    }
    return (size_t)compressBound((uLong)raw_len);
}

int fct_compression_mode_ex(const char* filename, const void* first_chunk, size_t len, bool force,
                            void* scratch, size_t capacity, fct_compression_mode_t* mode,
                            fct_compression_reason_t* reason) {
    size_t compressed_len;

    if (!mode || (len > 0 && !first_chunk) || len > DEFAULT_CHUNK_SZ) {
        errno = EINVAL;
        return -1;
    }

    if (force) {
        *mode = FCT_COMPRESS_FORCE;
        if (reason)
            *reason = FCT_COMPRESS_REASON_FORCE;
        return 0;
    }
    if (len == 0) {
        *mode = FCT_COMPRESS_RAW;
        if (reason)
            *reason = FCT_COMPRESS_REASON_EMPTY;
        return 0;
    }
    if (has_compressed_extension(filename) || has_compressed_signature(first_chunk, len)) {
        *mode = FCT_COMPRESS_RAW;
        if (reason)
            *reason = FCT_COMPRESS_REASON_COMPRESSED_FORMAT;
        return 0;
    }

    /* Trial-compress the first chunk to estimate how compressible the file is. */
    if (deflate_into(first_chunk, len, scratch, capacity, &compressed_len) < 0)
        return -1;
    if (compressed_len * 100 <= len * FCT_AUTO_RATIO_PERCENT) {
        *mode = FCT_COMPRESS_AUTO;
        if (reason)
            *reason = FCT_COMPRESS_REASON_AUTO;
    } else {
        *mode = FCT_COMPRESS_RAW;
        if (reason)
            *reason = FCT_COMPRESS_REASON_LOW_RATIO;
    }
    return 0;
}

int fct_compression_mode(const char* filename, const void* first_chunk, size_t len, bool force,
                         void* scratch, size_t capacity, fct_compression_mode_t* mode) {
    return fct_compression_mode_ex(filename, first_chunk, len, force, scratch, capacity, mode,
                                   NULL);
}

const char* fct_compression_reason_str(fct_compression_reason_t reason) {
    switch (reason) {
    case FCT_COMPRESS_REASON_AUTO:
        return "ON (auto)";
    case FCT_COMPRESS_REASON_FORCE:
        return "ON (force)";
    case FCT_COMPRESS_REASON_EMPTY:
        return "OFF (empty)";
    case FCT_COMPRESS_REASON_COMPRESSED_FORMAT:
        return "OFF (compressed format)";
    case FCT_COMPRESS_REASON_LOW_RATIO:
        return "OFF (low ratio)";
    default:
        return "unknown";
    }
}

int fct_encode_chunk(fct_compression_mode_t mode, const void* src, size_t raw_len, void* scratch,
                     size_t capacity, const void** payload, size_t* payload_len, uint32_t* flags) {
    size_t compressed_len;

    if (!payload || !payload_len || !flags || (raw_len > 0 && !src) || raw_len > DEFAULT_CHUNK_SZ) {
        errno = EINVAL;
        return -1;
    }

    switch (mode) {
    case FCT_COMPRESS_RAW:
        break;
    case FCT_COMPRESS_AUTO:
    case FCT_COMPRESS_FORCE:
        if (deflate_into(src, raw_len, scratch, capacity, &compressed_len) < 0)
            return -1;
        /* AUTO keeps compressed data only when it actually saves bytes. */
        if (mode == FCT_COMPRESS_FORCE || compressed_len < raw_len) {
            *payload = scratch;
            *payload_len = compressed_len;
            *flags = FCT_CHUNK_FLAG_COMPRESSED;
            return 0;
        }
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    *payload = src;
    *payload_len = raw_len;
    *flags = 0;
    return 0;
}

int fct_decode_chunk(uint32_t flags, const void* payload, size_t payload_len, void* dst,
                     size_t expected_len) {
    if ((payload_len > 0 && !payload) || (expected_len > 0 && !dst) ||
        expected_len > DEFAULT_CHUNK_SZ) {
        errno = EINVAL;
        return -1;
    }

    /* dst and payload must not overlap. */
    if (payload_len > 0 && expected_len > 0) {
        uintptr_t p = (uintptr_t)payload, d = (uintptr_t)dst;
        if (p < d ? d - p < payload_len : p - d < expected_len) {
            errno = EINVAL;
            return -1;
        }
    }

    if (flags == 0) {
        if (payload_len != expected_len) {
            errno = EPROTO;
            return -1;
        }
        if (expected_len > 0)
            memcpy(dst, payload, expected_len);
        return 0;
    }
    if (flags != FCT_CHUNK_FLAG_COMPRESSED) {
        errno = EPROTO;
        return -1;
    }

    /* A compressed payload is 1..compressBound(raw_len) bytes long. */
    if (payload_len == 0 || payload_len > fct_compress_bound(expected_len)) {
        errno = EPROTO;
        return -1;
    }

    uLongf out_len = (uLongf)expected_len;
    uLong in_len = (uLong)payload_len;
    int status = uncompress2((Bytef*)dst, &out_len, (const Bytef*)payload, &in_len);

    if (status == Z_MEM_ERROR) {
        errno = ENOMEM;
        return -1;
    }
    /* Corrupt, truncated, oversized or trailing data are all protocol errors. */
    if (status != Z_OK || out_len != expected_len || in_len != payload_len) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}
