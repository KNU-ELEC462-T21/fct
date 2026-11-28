#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "checksum.h"

#include <errno.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* Read size for hashing a file; reused for every pread() call. */
#define FCT_SHA256_READ_BUF (256 * 1024)

uint32_t fct_crc32(const void* data, size_t len) {
    const unsigned char* p = data;
    uLong crc = crc32(0L, Z_NULL, 0);

    if (!data)
        return (uint32_t)crc;
    /* zlib takes a uInt length, so feed very large buffers in pieces. */
    while (len > 0) {
        uInt n = len > UINT_MAX ? UINT_MAX : (uInt)len;
        crc = crc32(crc, p, n);
        p += n;
        len -= n;
    }
    return (uint32_t)crc;
}

static void abort_on_crypto_error(EVP_MD_CTX* md) {
    EVP_MD_CTX_free(md);
    abort();
}

void fct_sha256_init(fct_sha256_ctx_t* ctx) {
    if (!ctx)
        abort();

    EVP_MD_CTX* md = EVP_MD_CTX_new();
    if (!md || EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1)
        abort_on_crypto_error(md);
    ctx->evp = md;
}

void fct_sha256_update(fct_sha256_ctx_t* ctx, const void* data, size_t len) {
    if (len == 0)
        return;
    if (!ctx || !ctx->evp || !data)
        abort();
    if (EVP_DigestUpdate((EVP_MD_CTX*)ctx->evp, data, len) != 1)
        abort();
}

void fct_sha256_final(fct_sha256_ctx_t* ctx, uint8_t out_digest[FCT_SHA256_SIZE]) {
    if (!ctx || !ctx->evp || !out_digest)
        abort();

    EVP_MD_CTX* md = (EVP_MD_CTX*)ctx->evp;
    ctx->evp = NULL;

    unsigned int out_len = 0;
    int ok = EVP_DigestFinal_ex(md, out_digest, &out_len);
    EVP_MD_CTX_free(md);
    if (ok != 1 || out_len != FCT_SHA256_SIZE)
        abort();
}

void fct_sha256_bytes(const void* data, size_t len, uint8_t out_digest[FCT_SHA256_SIZE]) {
    if (!out_digest || (len > 0 && !data))
        abort();

    unsigned int out_len = 0;
    if (EVP_Digest(data ? data : "", len, out_digest, &out_len, EVP_sha256(), NULL) != 1 ||
        out_len != FCT_SHA256_SIZE)
        abort();
}

int fct_sha256_fd(int fd, uint64_t expected_size, uint8_t out_digest[FCT_SHA256_SIZE]) {
    struct stat st;
    uint64_t offset = 0;

    if (fd < 0 || !out_digest || expected_size > (uint64_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (fstat(fd, &st) < 0)
        return -1;
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size != expected_size) {
        errno = EIO;
        return -1;
    }

    uint8_t* buf = expected_size > 0 ? malloc(FCT_SHA256_READ_BUF) : NULL;
    if (expected_size > 0 && !buf) {
        errno = ENOMEM;
        return -1;
    }

    EVP_MD_CTX* md = EVP_MD_CTX_new();
    if (!md) {
        free(buf);
        errno = ENOMEM;
        return -1;
    }

    int err = 0;
    if (EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1) {
        err = EIO;
    } else {
        while (offset < expected_size) {
            uint64_t left = expected_size - offset;
            size_t want = left < FCT_SHA256_READ_BUF ? (size_t)left : FCT_SHA256_READ_BUF;
            ssize_t n = pread(fd, buf, want, (off_t)offset);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                err = errno;
                break;
            }
            if (n == 0) { /* the file shrank while hashing */
                err = EIO;
                break;
            }
            if (EVP_DigestUpdate(md, buf, (size_t)n) != 1) {
                err = EIO;
                break;
            }
            offset += (uint64_t)n;
        }
    }

    if (err == 0) {
        unsigned int out_len = 0;
        if (EVP_DigestFinal_ex(md, out_digest, &out_len) != 1 || out_len != FCT_SHA256_SIZE)
            err = EIO;
    }

    free(buf);
    EVP_MD_CTX_free(md);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

void fct_sha256_to_hex(const uint8_t digest[FCT_SHA256_SIZE],
                       char out_hex[FCT_SHA256_SIZE * 2 + 1]) {
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < FCT_SHA256_SIZE; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[FCT_SHA256_SIZE * 2] = '\0';
}
