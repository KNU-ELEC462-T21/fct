#define _POSIX_C_SOURCE 200809L

#include "bitmap.h"
#include "checksum.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static char dir[] = "/tmp/fct_bmXXXXXX";
static char path[64];

static fct_file_meta_t make_meta(uint32_t total_chunks) {
    fct_file_meta_t m;

    memset(&m, 0, sizeof(m));
    m.magic = 0x46435431;
    m.chunk_size = 4096;
    m.total_chunks = total_chunks;
    m.file_size = (uint64_t)total_chunks * m.chunk_size;
    snprintf(m.filename, sizeof(m.filename), "movie.mp4");
    fct_sha256_bytes("contents", 8, m.file_sha256);
    return m;
}

static void test_basic(void) {
    static const uint32_t counts[] = {0, 1, 7, 8, 9, 1023, 1024, 1025};
    static const size_t bytes[] = {0, 1, 1, 1, 2, 128, 128, 129};

    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        uint32_t n = counts[i];
        fct_bitmap_t bm;

        CHECK(fct_bitmap_byte_len(n) == bytes[i]);
        CHECK(fct_bitmap_init(&bm, n) == 0);
        CHECK(bm.byte_len == bytes[i] && fct_bitmap_completed(&bm) == 0);
        CHECK(fct_bitmap_missing(&bm) == n);
        CHECK(fct_bitmap_is_complete(&bm) == (n == 0));

        /* Set every other chunk, twice, and the count still moves once. */
        for (uint32_t id = 0; id < n; id += 2) {
            CHECK(fct_bitmap_set(&bm, id) == 0);
            CHECK(fct_bitmap_set(&bm, id) == 0);
        }
        CHECK(fct_bitmap_completed(&bm) == (n + 1) / 2);
        for (uint32_t id = 0; id < n; id++)
            CHECK(fct_bitmap_get(&bm, id) == (id % 2 == 0));

        for (uint32_t id = 0; id < n; id++)
            CHECK(fct_bitmap_set(&bm, id) == 0);
        CHECK(fct_bitmap_is_complete(&bm) && fct_bitmap_missing(&bm) == 0);
        /* Bits past total_chunks stay zero. */
        CHECK(n % 8 == 0 || (bm.bits[bm.byte_len - 1] >> (n % 8)) == 0);

        if (n > 0) {
            CHECK(fct_bitmap_clear(&bm, n - 1) == 0);
            CHECK(fct_bitmap_clear(&bm, n - 1) == 0);
            CHECK(!fct_bitmap_get(&bm, n - 1) && fct_bitmap_missing(&bm) == 1);
        }
        CHECK(!fct_bitmap_get(&bm, n));
        CHECK_FAILS(fct_bitmap_set(&bm, n), EINVAL);
        CHECK_FAILS(fct_bitmap_clear(&bm, n), EINVAL);
        fct_bitmap_free(&bm);
        CHECK(bm.bits == NULL && bm.total_chunks == 0);
    }
    CHECK_FAILS(fct_bitmap_init(NULL, 1), EINVAL);
}

static void test_load_bytes_and_superset(void) {
    fct_bitmap_t a, b;
    uint8_t raw[2] = {0x05, 0x01}; /* chunks 0, 2 and 8 */

    CHECK(fct_bitmap_init(&a, 9) == 0 && fct_bitmap_init(&b, 9) == 0);
    CHECK(fct_bitmap_load_bytes(&a, raw, 2) == 0);
    CHECK(fct_bitmap_completed(&a) == 3 && fct_bitmap_get(&a, 8) && !fct_bitmap_get(&a, 1));

    CHECK_FAILS(fct_bitmap_load_bytes(&a, raw, 1), EPROTO); /* wrong length */
    raw[1] = 0x03;                                          /* bit 9 is past the end */
    CHECK_FAILS(fct_bitmap_load_bytes(&a, raw, 2), EPROTO);
    CHECK(fct_bitmap_completed(&a) == 3); /* unchanged after a rejected load */

    CHECK(fct_bitmap_set(&b, 0) == 0 && fct_bitmap_set(&b, 2) == 0);
    CHECK(fct_bitmap_is_superset(&a, &b));  /* a has every bit of b */
    CHECK(!fct_bitmap_is_superset(&b, &a)); /* b lacks chunk 8 */
    CHECK(fct_bitmap_is_superset(&a, &a));

    fct_bitmap_t c;
    CHECK(fct_bitmap_init(&c, 10) == 0);
    CHECK(!fct_bitmap_is_superset(&c, &b)); /* different chunk counts */

    fct_bitmap_free(&a);
    fct_bitmap_free(&b);
    fct_bitmap_free(&c);
}

/* Read the saved file, let edit() damage it, write it back, then expect EPROTO. */
static void check_rejects(const fct_file_meta_t* meta, void (*edit)(uint8_t*, size_t*),
                          bool fix_crc) {
    struct stat st;
    CHECK(stat(path, &st) == 0);
    size_t len = (size_t)st.st_size;
    uint8_t* buf = malloc(len + 16);
    int fd = open(path, O_RDWR);

    CHECK(buf != NULL && fd >= 0);
    CHECK(read(fd, buf, len) == (ssize_t)len);
    uint8_t* saved = malloc(len);
    CHECK(saved != NULL);
    memcpy(saved, buf, len);

    size_t new_len = len;
    edit(buf, &new_len);
    if (fix_crc) { /* make the damage pass the CRC so a later check must catch it */
        uint32_t crc = fct_crc32(buf, new_len - 4);
        buf[new_len - 4] = (uint8_t)(crc >> 24);
        buf[new_len - 3] = (uint8_t)(crc >> 16);
        buf[new_len - 2] = (uint8_t)(crc >> 8);
        buf[new_len - 1] = (uint8_t)crc;
    }
    CHECK(ftruncate(fd, 0) == 0 && pwrite(fd, buf, new_len, 0) == (ssize_t)new_len);

    fct_bitmap_t bm;
    CHECK_FAILS(fct_bitmap_load_file(&bm, meta, path), EPROTO);
    CHECK(bm.bits == NULL);

    /* Restore the original file for the next case. */
    CHECK(ftruncate(fd, 0) == 0 && pwrite(fd, saved, len, 0) == (ssize_t)len);
    close(fd);
    free(buf);
    free(saved);
}

static void flip_bit_byte(uint8_t* b, size_t* len) {
    b[*len - 5] ^= 0x01; /* last bitmap byte, CRC left stale */
}
static void truncate_one(uint8_t* b, size_t* len) {
    (void)b;
    (*len)--;
}
static void bad_magic(uint8_t* b, size_t* len) {
    (void)len;
    b[0] ^= 0xff;
}
static void bad_version(uint8_t* b, size_t* len) {
    (void)len;
    b[7] = 2;
}
static void set_trailing_bit(uint8_t* b, size_t* len) {
    b[*len - 5] |= 0x80; /* 9 chunks: bits 1..7 of the last byte are unused */
}

static void test_file_roundtrip(void) {
    fct_file_meta_t meta = make_meta(9);
    fct_bitmap_t bm, loaded;

    CHECK(fct_bitmap_init(&bm, 9) == 0);
    CHECK(fct_bitmap_set(&bm, 0) == 0 && fct_bitmap_set(&bm, 5) == 0 &&
          fct_bitmap_set(&bm, 8) == 0);
    CHECK(fct_bitmap_save_file(&bm, &meta, path) == 0);

    struct stat st;
    CHECK(stat(path, &st) == 0 && st.st_size == 316 + 2 + 4);

    CHECK(fct_bitmap_load_file(&loaded, &meta, path) == 0);
    CHECK(loaded.total_chunks == 9 && fct_bitmap_completed(&loaded) == 3);
    CHECK(memcmp(loaded.bits, bm.bits, bm.byte_len) == 0);
    fct_bitmap_free(&loaded);

    /* Saving again replaces the file atomically. */
    CHECK(fct_bitmap_set(&bm, 1) == 0);
    CHECK(fct_bitmap_save_file(&bm, &meta, path) == 0);
    CHECK(fct_bitmap_load_file(&loaded, &meta, path) == 0 && fct_bitmap_completed(&loaded) == 4);
    fct_bitmap_free(&loaded);

    /* Damaged files are rejected. */
    check_rejects(&meta, flip_bit_byte, false);
    check_rejects(&meta, truncate_one, false);
    check_rejects(&meta, bad_magic, true);
    check_rejects(&meta, bad_version, true);
    check_rejects(&meta, set_trailing_bit, true);

    /* A bitmap from a different transfer is rejected. */
    fct_file_meta_t other = meta;
    other.file_size++;
    CHECK_FAILS(fct_bitmap_load_file(&loaded, &other, path), EPROTO);
    other = meta;
    other.chunk_size *= 2;
    CHECK_FAILS(fct_bitmap_load_file(&loaded, &other, path), EPROTO);
    other = meta;
    other.file_sha256[31] ^= 1;
    CHECK_FAILS(fct_bitmap_load_file(&loaded, &other, path), EPROTO);
    other = meta;
    other.filename[0] = 'M';
    CHECK_FAILS(fct_bitmap_load_file(&loaded, &other, path), EPROTO);
    other = make_meta(10);
    other.file_size = meta.file_size;
    CHECK_FAILS(fct_bitmap_load_file(&loaded, &other, path), EPROTO);

    /* A failed save leaves the existing bitmap untouched. */
    char bad_path[96];
    snprintf(bad_path, sizeof(bad_path), "%s/missing/x.bitmap", dir);
    CHECK_FAILS(fct_bitmap_save_file(&bm, &meta, bad_path), ENOENT);
    fct_file_meta_t wrong = make_meta(8);
    CHECK_FAILS(fct_bitmap_save_file(&bm, &wrong, path), EINVAL);
    CHECK(fct_bitmap_load_file(&loaded, &meta, path) == 0 && fct_bitmap_completed(&loaded) == 4);
    fct_bitmap_free(&loaded);

    char missing[96];
    snprintf(missing, sizeof(missing), "%s/none.bitmap", dir);
    CHECK_FAILS(fct_bitmap_load_file(&loaded, &meta, missing), ENOENT);

    fct_bitmap_free(&bm);
    unlink(path);
}

static void test_empty_file(void) {
    fct_file_meta_t meta = make_meta(0);
    fct_bitmap_t bm, loaded;

    CHECK(fct_bitmap_init(&bm, 0) == 0);
    CHECK(fct_bitmap_save_file(&bm, &meta, path) == 0);
    CHECK(fct_bitmap_load_file(&loaded, &meta, path) == 0);
    CHECK(loaded.total_chunks == 0 && fct_bitmap_is_complete(&loaded));
    fct_bitmap_free(&loaded);
    fct_bitmap_free(&bm);
    unlink(path);
}

int main(void) {
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/t.bitmap", dir);

    test_basic();
    test_load_bytes_and_superset();
    test_file_roundtrip();
    test_empty_file();

    CHECK(rmdir(dir) == 0); /* also proves no temporary files were left behind */
    printf("bitmap_test: all tests passed\n");
    return 0;
}
