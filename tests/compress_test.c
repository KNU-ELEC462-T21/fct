#include "common.h"
#include "compress.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unlike assert(), CHECK always runs its expression, even with -DNDEBUG. */
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define CHECK_FAILS(call, err)                                                                     \
    do {                                                                                           \
        errno = 0;                                                                                 \
        CHECK((call) == -1 && errno == (err));                                                     \
    } while (0)

static unsigned char* make_text(size_t len) {
    static const char words[] = "the quick brown fox jumps over the lazy dog ";
    unsigned char* buf = malloc(len ? len : 1);

    CHECK(buf != NULL);
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)words[i % (sizeof(words) - 1)];
    return buf;
}

static unsigned char* make_random(size_t len) {
    unsigned char* buf = malloc(len ? len : 1);

    CHECK(buf != NULL);
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)(rand() & 0xff);
    return buf;
}

/* Encode with mode, check the expected flag, then decode and compare. */
static void check_roundtrip(fct_compression_mode_t mode, const unsigned char* src, size_t len,
                            uint32_t expect_flags) {
    size_t cap = fct_compress_bound(len);
    unsigned char* scratch = malloc(cap);
    unsigned char* out = malloc(len ? len : 1);
    const void* payload;
    size_t payload_len;
    uint32_t flags;

    CHECK(cap > 0 && scratch != NULL && out != NULL);
    CHECK(fct_encode_chunk(mode, src, len, scratch, cap, &payload, &payload_len, &flags) == 0);
    CHECK(flags == expect_flags);
    if (flags == 0) {
        CHECK(payload == src && payload_len == len);
    } else {
        CHECK(payload == scratch);
        if (mode == FCT_COMPRESS_AUTO)
            CHECK(payload_len < len);
    }
    CHECK(fct_decode_chunk(flags, payload, payload_len, out, len) == 0);
    CHECK(len == 0 || memcmp(out, src, len) == 0);

    free(scratch);
    free(out);
}

static void test_bound(void) {
    CHECK(fct_compress_bound(0) > 0);
    CHECK(fct_compress_bound(DEFAULT_CHUNK_SZ) >= DEFAULT_CHUNK_SZ);
    errno = 0;
    CHECK(fct_compress_bound(DEFAULT_CHUNK_SZ + 1) == 0 && errno == EINVAL);
}

static void test_roundtrips(void) {
    static const size_t sizes[] = {0, 1, 4096, 100000, DEFAULT_CHUNK_SZ};
    const uint32_t z = FCT_CHUNK_FLAG_COMPRESSED;

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t len = sizes[i];
        unsigned char* text = make_text(len);
        unsigned char* noise = make_random(len);

        check_roundtrip(FCT_COMPRESS_RAW, text, len, 0);
        check_roundtrip(FCT_COMPRESS_FORCE, text, len, z);
        check_roundtrip(FCT_COMPRESS_FORCE, noise, len, z);
        /* AUTO compresses text, but falls back to raw when nothing is saved. */
        check_roundtrip(FCT_COMPRESS_AUTO, text, len, len >= 4096 ? z : 0);
        check_roundtrip(FCT_COMPRESS_AUTO, noise, len, 0);

        free(text);
        free(noise);
    }
}

static void test_mode(void) {
    size_t len = 65536;
    size_t cap = fct_compress_bound(len);
    unsigned char* text = make_text(len);
    unsigned char* noise = make_random(len);
    unsigned char* scratch = malloc(cap);
    unsigned char buf[16] = {0};
    fct_compression_mode_t m;

    CHECK(scratch != NULL);
    CHECK(fct_compression_mode("a.txt", noise, len, true, NULL, 0, &m) == 0 &&
          m == FCT_COMPRESS_FORCE);
    CHECK(fct_compression_mode("empty.txt", NULL, 0, false, NULL, 0, &m) == 0 &&
          m == FCT_COMPRESS_RAW);
    CHECK(fct_compression_mode("a.txt", text, len, false, scratch, cap, &m) == 0 &&
          m == FCT_COMPRESS_AUTO);
    CHECK(fct_compression_mode("a.bin", noise, len, false, scratch, cap, &m) == 0 &&
          m == FCT_COMPRESS_RAW);

    /* Known formats skip the trial compression, so no scratch is needed. */
    CHECK(fct_compression_mode("dir/Movie.MP4", text, len, false, NULL, 0, &m) == 0 &&
          m == FCT_COMPRESS_RAW);
    CHECK(fct_compression_mode("a.zip", text, len, false, NULL, 0, &m) == 0 &&
          m == FCT_COMPRESS_RAW);
    /* A dotfile named ".zip" has no stem, so it is not treated as a ZIP extension. */
    CHECK(fct_compression_mode("dir/.zip", text, len, false, scratch, cap, &m) == 0 &&
          m == FCT_COMPRESS_AUTO);
    memcpy(buf, "PK\x03\x04", 4);
    CHECK(fct_compression_mode("a", buf, sizeof(buf), false, NULL, 0, &m) == 0 &&
          m == FCT_COMPRESS_RAW);
    memcpy(buf, "PK\x05\x06", 4);
    CHECK(fct_compression_mode("a", buf, sizeof(buf), false, NULL, 0, &m) == 0 &&
          m == FCT_COMPRESS_RAW);
    /* Plain text starting with "PK " must not be misidentified as a ZIP header. */
    unsigned char* pk_text = make_text(len);
    memcpy(pk_text, "PK ", 3);
    CHECK(fct_compression_mode("a.txt", pk_text, len, false, scratch, cap, &m) == 0 &&
          m == FCT_COMPRESS_AUTO);
    free(pk_text);
    memcpy(buf, "\x1f\x8b", 2);
    CHECK(fct_compression_mode("a", buf, sizeof(buf), false, NULL, 0, &m) == 0 &&
          m == FCT_COMPRESS_RAW);
    memset(buf, 0, sizeof(buf));
    memcpy(buf + 4, "ftyp", 4);
    CHECK(fct_compression_mode("a", buf, sizeof(buf), false, NULL, 0, &m) == 0 &&
          m == FCT_COMPRESS_RAW);

    CHECK_FAILS(fct_compression_mode("a.txt", text, len, false, scratch, cap, NULL), EINVAL);
    CHECK_FAILS(fct_compression_mode("a.txt", NULL, len, false, scratch, cap, &m), EINVAL);
    CHECK_FAILS(fct_compression_mode("a.txt", text, len, false, scratch, cap - 1, &m), EINVAL);
    CHECK_FAILS(fct_compression_mode("a.txt", text, DEFAULT_CHUNK_SZ + 1, false, scratch, cap, &m),
                EINVAL);

    free(text);
    free(noise);
    free(scratch);
}

/* Size of src compressed with FORCE. */
static size_t forced_size(const unsigned char* src, size_t len, unsigned char* scratch,
                          size_t cap) {
    const void* p;
    size_t n;
    uint32_t f;

    CHECK(fct_encode_chunk(FCT_COMPRESS_FORCE, src, len, scratch, cap, &p, &n, &f) == 0);
    return n;
}

/* Grow a random prefix inside zero bytes until the ratio crosses 95%, then check
 * that the mode flips from AUTO to RAW exactly where the ratio crosses. */
static void test_ratio_boundary(void) {
    size_t len = 4096;
    size_t cap = fct_compress_bound(len);
    unsigned char* noise = make_random(len);
    unsigned char* buf = calloc(len, 1);
    unsigned char* scratch = malloc(cap);
    fct_compression_mode_t m;
    size_t k = 0;

    CHECK(buf != NULL && scratch != NULL);
    while (k < len) {
        memcpy(buf, noise, k);
        if (forced_size(buf, len, scratch, cap) * 100 > len * 95)
            break;
        k++;
    }
    CHECK(k > 0 && k < len);

    /* k random bytes: just over 95%, so RAW. */
    CHECK(fct_compression_mode("a.bin", buf, len, false, scratch, cap, &m) == 0 &&
          m == FCT_COMPRESS_RAW);
    /* k - 1 random bytes: at most 95%, so AUTO. */
    buf[k - 1] = 0;
    CHECK(forced_size(buf, len, scratch, cap) * 100 <= len * 95);
    CHECK(fct_compression_mode("a.bin", buf, len, false, scratch, cap, &m) == 0 &&
          m == FCT_COMPRESS_AUTO);

    free(noise);
    free(buf);
    free(scratch);
}

static void test_encode_errors(void) {
    unsigned char src[64] = {0};
    unsigned char scratch[128];
    const void* p;
    size_t n;
    uint32_t f;

    CHECK_FAILS(fct_encode_chunk(FCT_COMPRESS_AUTO, src, sizeof(src), NULL, 0, &p, &n, &f), EINVAL);
    CHECK_FAILS(fct_encode_chunk(FCT_COMPRESS_FORCE, src, sizeof(src), scratch, 8, &p, &n, &f),
                EINVAL);
    CHECK_FAILS(fct_encode_chunk((fct_compression_mode_t)7, src, sizeof(src), scratch,
                                 sizeof(scratch), &p, &n, &f),
                EINVAL);
    CHECK_FAILS(fct_encode_chunk(FCT_COMPRESS_RAW, NULL, 1, NULL, 0, &p, &n, &f), EINVAL);
    CHECK_FAILS(fct_encode_chunk(FCT_COMPRESS_RAW, src, sizeof(src), NULL, 0, NULL, &n, &f),
                EINVAL);
}

/* Damaged or mismatched payloads must be rejected, never half-decoded. */
static void test_decode_errors(void) {
    size_t len = 10000;
    size_t cap = fct_compress_bound(len) + 1;
    unsigned char* text = make_text(len);
    unsigned char* scratch = malloc(cap);
    unsigned char* out = malloc(len + 1);
    const void* p;
    size_t n;
    uint32_t f;

    CHECK(scratch != NULL && out != NULL);
    CHECK(fct_encode_chunk(FCT_COMPRESS_FORCE, text, len, scratch, cap, &p, &n, &f) == 0);

    CHECK_FAILS(fct_decode_chunk(f, p, 0, out, len), EPROTO);     /* empty payload */
    CHECK_FAILS(fct_decode_chunk(f, p, n - 1, out, len), EPROTO); /* truncated */
    CHECK_FAILS(fct_decode_chunk(f, p, n, out, len - 1), EPROTO); /* raw data too long */
    CHECK_FAILS(fct_decode_chunk(f, p, n, out, len + 1), EPROTO); /* raw data too short */
    scratch[n] = 0x55;
    CHECK_FAILS(fct_decode_chunk(f, p, n + 1, out, len), EPROTO); /* trailing garbage */
    scratch[n / 2] ^= 0xff;
    CHECK_FAILS(fct_decode_chunk(f, p, n, out, len), EPROTO); /* corrupted byte */

    CHECK_FAILS(fct_decode_chunk(0, text, len - 1, out, len), EPROTO);  /* raw length mismatch */
    CHECK_FAILS(fct_decode_chunk(2, text, len, out, len), EPROTO);      /* unknown flag */
    CHECK_FAILS(fct_decode_chunk(0, text, len, text + 1, len), EINVAL); /* overlapping buffers */
    CHECK_FAILS(fct_decode_chunk(0, text, SIZE_MAX, text, 1), EINVAL);
    CHECK_FAILS(fct_decode_chunk(0, text, SIZE_MAX, text + 1, 1), EINVAL);
    CHECK_FAILS(fct_decode_chunk(0, NULL, 1, out, 1), EINVAL);

    free(text);
    free(scratch);
    free(out);
}

int main(void) {
    srand(462);
    test_bound();
    test_roundtrips();
    test_mode();
    test_ratio_boundary();
    test_encode_errors();
    test_decode_errors();
    printf("compress_test: all tests passed\n");
    return 0;
}
