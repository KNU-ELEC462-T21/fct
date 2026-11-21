#include "file_reader.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Unlike assert(), CHECK always runs its expression, even with -DNDEBUG. */
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* Write len pseudo-random bytes to a new temp file; return its path. */
static char *make_file(size_t len, unsigned char **out_data) {
    char *path = strdup("/tmp/fr_testXXXXXX");
    unsigned char *data = malloc(len ? len : 1);
    int fd;

    CHECK(path != NULL && data != NULL);
    for (size_t i = 0; i < len; i++)
        data[i] = (unsigned char)(rand() & 0xff);
    fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, data, len) == (ssize_t)len);
    close(fd);
    *out_data = data;
    return path;
}

/* Read every chunk in reverse order and compare with the original bytes. */
static void check_roundtrip(size_t len, uint32_t chunk_size) {
    unsigned char *data, *buf;
    char *path = make_file(len, &data);
    file_reader_t r;
    uint32_t expect = (uint32_t)((len + chunk_size - 1) / chunk_size);

    CHECK(fr_init(&r, path, chunk_size) == 0);
    CHECK(r.file_size == len);
    CHECK(r.total_chunks == expect);

    buf = malloc(chunk_size);
    CHECK(buf != NULL);
    for (uint32_t id = r.total_chunks; id-- > 0;) {
        size_t off = (size_t)id * chunk_size;
        size_t want = len - off < chunk_size ? len - off : chunk_size;
        ssize_t n = fr_read_chunk(&r, id, buf);
        CHECK(n == (ssize_t)want);
        CHECK(memcmp(buf, data + off, want) == 0);
    }

    /* Out-of-range chunk id is rejected. */
    errno = 0;
    CHECK(fr_read_chunk(&r, r.total_chunks, buf) == -1 && errno == EINVAL);

    fr_close(&r);
    CHECK(r.fd == -1);
    fr_close(&r); /* closing twice is harmless */

    unlink(path);
    free(path);
    free(data);
    free(buf);
}

static void test_errors(void) {
    file_reader_t r;

    errno = 0;
    CHECK(fr_init(&r, "/nonexistent/fct_file", 16) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(fr_init(&r, "/tmp", 16) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(fr_init(&r, "/tmp", 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(fr_init(NULL, "/tmp", 16) == -1 && errno == EINVAL);
}

/* A sparse 5 GiB file read in 1-byte chunks needs more than 2^32 chunk ids. */
static void test_too_many_chunks(void) {
    char path[] = "/tmp/fr_bigXXXXXX";
    int fd = mkstemp(path);
    file_reader_t r;

    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)5 << 30) == 0);
    close(fd);
    errno = 0;
    CHECK(fr_init(&r, path, 1) == -1 && errno == EFBIG);
    CHECK(fr_init(&r, path, 2097152) == 0 && r.total_chunks == 2560);
    fr_close(&r);
    unlink(path);
}

int main(void) {
    srand(462);
    check_roundtrip(0, 4096);           /* empty file: 0 chunks */
    check_roundtrip(5, 4096);           /* smaller than one chunk */
    check_roundtrip(4096, 4096);        /* exactly one chunk */
    check_roundtrip(4097, 4096);        /* one byte into a second chunk */
    check_roundtrip(5000000, 2097152);  /* multi-chunk with a short tail */
    check_roundtrip(100000, 1000);      /* many small chunks */
    test_errors();
    test_too_many_chunks();
    printf("file_reader_test: all tests passed\n");
    return 0;
}
