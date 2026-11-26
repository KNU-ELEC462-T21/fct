#define _POSIX_C_SOURCE 200809L

#include "checksum.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Unlike assert(), CHECK always runs its expression, even with -DNDEBUG. */
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void check_hex(const uint8_t digest[FCT_SHA256_SIZE], const char* expect) {
    char hex[FCT_SHA256_SIZE * 2 + 1];

    fct_sha256_to_hex(digest, hex);
    CHECK(strcmp(hex, expect) == 0);
}

static void test_crc32(void) {
    unsigned char big[100000];

    CHECK(fct_crc32("", 0) == 0);
    CHECK(fct_crc32(NULL, 0) == 0);
    CHECK(fct_crc32("a", 1) == UINT32_C(0xe8b7be43));
    CHECK(fct_crc32("123456789", 9) == UINT32_C(0xcbf43926)); /* standard check value */

    /* One flipped bit must change the checksum. */
    memset(big, 0x5a, sizeof(big));
    uint32_t before = fct_crc32(big, sizeof(big));
    big[sizeof(big) / 2] ^= 1;
    CHECK(fct_crc32(big, sizeof(big)) != before);
}

/* FIPS 180-4 / NIST test vectors. */
static void test_sha256_vectors(void) {
    uint8_t d[FCT_SHA256_SIZE];
    const char* two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    fct_sha256_bytes("", 0, d);
    check_hex(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    fct_sha256_bytes("abc", 3, d);
    check_hex(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fct_sha256_bytes(two_blocks, strlen(two_blocks), d);
    check_hex(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* One million 'a' fed in uneven pieces. */
    fct_sha256_ctx_t ctx;
    char piece[1000];
    memset(piece, 'a', sizeof(piece));
    fct_sha256_init(&ctx);
    for (size_t done = 0, step = 1; done < 1000000; step = step % 997 + 1) {
        size_t n = 1000000 - done < step ? 1000000 - done : step;
        fct_sha256_update(&ctx, piece, n);
        done += n;
    }
    fct_sha256_final(&ctx, d);
    check_hex(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* Padding edge cases: lengths around the 56- and 64-byte block boundaries
 * must hash the same whether fed at once or one byte at a time. */
static void test_sha256_boundaries(void) {
    unsigned char data[200];

    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (unsigned char)(i * 7 + 3);
    for (size_t len = 0; len <= sizeof(data); len++) {
        uint8_t a[FCT_SHA256_SIZE], b[FCT_SHA256_SIZE];
        fct_sha256_ctx_t ctx;

        fct_sha256_bytes(data, len, a);
        fct_sha256_init(&ctx);
        for (size_t i = 0; i < len; i++)
            fct_sha256_update(&ctx, data + i, 1);
        fct_sha256_final(&ctx, b);
        CHECK(memcmp(a, b, sizeof(a)) == 0);
    }
}

static void test_sha256_fd(void) {
    char path[] = "/tmp/fct_shaXXXXXX";
    int fd = mkstemp(path);
    size_t len = 1000000;
    char* data = malloc(len);
    uint8_t d[FCT_SHA256_SIZE];

    CHECK(fd >= 0 && data != NULL);
    memset(data, 'a', len);
    CHECK(write(fd, data, len) == (ssize_t)len);

    /* pread() hashes from offset 0 and leaves the file offset alone. */
    off_t pos = lseek(fd, 0, SEEK_CUR);
    CHECK(fct_sha256_fd(fd, len, d) == 0);
    check_hex(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(lseek(fd, 0, SEEK_CUR) == pos);

    errno = 0;
    CHECK(fct_sha256_fd(fd, len + 1, d) == -1 && errno == EIO); /* size mismatch */
    errno = 0;
    CHECK(fct_sha256_fd(fd, len - 1, d) == -1 && errno == EIO);
    errno = 0;
    CHECK(fct_sha256_fd(-1, len, d) == -1 && errno == EINVAL);

    CHECK(ftruncate(fd, 0) == 0);
    CHECK(fct_sha256_fd(fd, 0, d) == 0);
    check_hex(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    close(fd);
    unlink(path);
    free(data);
}

int main(void) {
    test_crc32();
    test_sha256_vectors();
    test_sha256_boundaries();
    test_sha256_fd();
    printf("checksum_test: all tests passed\n");
    return 0;
}
