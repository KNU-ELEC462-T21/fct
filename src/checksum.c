#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "checksum.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* Read size for hashing a file; reused for every pread() call. */
#define FCT_SHA256_READ_BUF (64 * 1024)

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

/* SHA-256 as specified in FIPS 180-4. */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

/* Process one 64-byte block. */
static void sha256_block(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void fct_sha256_init(fct_sha256_ctx_t* ctx) {
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bit_len = 0;
    ctx->buffer_len = 0;
}

void fct_sha256_update(fct_sha256_ctx_t* ctx, const void* data, size_t len) {
    const uint8_t* p = data;

    if (len == 0)
        return;
    ctx->bit_len += (uint64_t)len * 8;

    /* Top up a partially filled block first. */
    if (ctx->buffer_len > 0) {
        size_t n = 64 - ctx->buffer_len;
        if (n > len)
            n = len;
        memcpy(ctx->buffer + ctx->buffer_len, p, n);
        ctx->buffer_len += n;
        p += n;
        len -= n;
        if (ctx->buffer_len < 64)
            return;
        sha256_block(ctx->state, ctx->buffer);
        ctx->buffer_len = 0;
    }
    /* Then hash whole blocks straight from the input. */
    while (len >= 64) {
        sha256_block(ctx->state, p);
        p += 64;
        len -= 64;
    }
    memcpy(ctx->buffer, p, len);
    ctx->buffer_len = len;
}

void fct_sha256_final(fct_sha256_ctx_t* ctx, uint8_t out_digest[FCT_SHA256_SIZE]) {
    uint64_t bit_len = ctx->bit_len;

    /* Padding: a 1 bit, zeros, then the message length as a 64-bit integer. */
    ctx->buffer[ctx->buffer_len++] = 0x80;
    if (ctx->buffer_len > 56) {
        memset(ctx->buffer + ctx->buffer_len, 0, 64 - ctx->buffer_len);
        sha256_block(ctx->state, ctx->buffer);
        ctx->buffer_len = 0;
    }
    memset(ctx->buffer + ctx->buffer_len, 0, 56 - ctx->buffer_len);
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)(bit_len >> (56 - 8 * i));
    sha256_block(ctx->state, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        out_digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out_digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out_digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out_digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
    /* Do not leave hashed data behind in the context. */
    memset(ctx, 0, sizeof(*ctx));
}

void fct_sha256_bytes(const void* data, size_t len, uint8_t out_digest[FCT_SHA256_SIZE]) {
    fct_sha256_ctx_t ctx;

    fct_sha256_init(&ctx);
    fct_sha256_update(&ctx, data, len);
    fct_sha256_final(&ctx, out_digest);
}

int fct_sha256_fd(int fd, uint64_t expected_size, uint8_t out_digest[FCT_SHA256_SIZE]) {
    uint8_t buf[FCT_SHA256_READ_BUF];
    fct_sha256_ctx_t ctx;
    struct stat st;
    uint64_t offset = 0;

    if (fd < 0 || !out_digest || expected_size > (uint64_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (fstat(fd, &st) < 0)
        return -1;
    if ((uint64_t)st.st_size != expected_size) {
        errno = EIO;
        return -1;
    }

    fct_sha256_init(&ctx);
    while (offset < expected_size) {
        uint64_t left = expected_size - offset;
        size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
        ssize_t n = pread(fd, buf, want, (off_t)offset);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) { /* the file shrank while hashing */
            errno = EIO;
            return -1;
        }
        fct_sha256_update(&ctx, buf, (size_t)n);
        offset += (uint64_t)n;
    }
    fct_sha256_final(&ctx, out_digest);
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
