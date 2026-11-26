#define _POSIX_C_SOURCE 200809L

#include "file_writer.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_open_existing(const char* directory)
{
    file_writer_t writer = {.fd = -1};
    char path[256];
    char other[256];
    snprintf(path, sizeof(path), "%s/partial.bin", directory);
    snprintf(other, sizeof(other), "%s/other", directory);

    assert(fw_open_existing(&writer, path, 10, 4) == -1);
    assert(writer.fd == -1 && access(path, F_OK) == -1);
    assert(fw_init(&writer, path, 10, 4) == 0);
    assert(fw_write_chunk(&writer, 0, "ABCD", 4) == 4);
    assert(fw_write_chunk(&writer, 2, "IJ", 2) == 2);
    fw_close(&writer);

    assert(fw_open_existing(&writer, path, 9, 4) == -1 && writer.fd == -1);
    assert(fw_open_existing(&writer, path, 11, 4) == -1 && writer.fd == -1);
    assert(fw_open_existing(&writer, path, 10, 0) == -1 && writer.fd == -1);
    assert(fw_open_existing(&writer, path, UINT64_MAX, 4) == -1 && writer.fd == -1);
    assert(fw_open_existing(&writer, directory, 10, 4) == -1 && writer.fd == -1);
    assert(mkfifo(other, 0600) == 0);
    assert(fw_open_existing(&writer, other, 0, 4) == -1 && writer.fd == -1);
    assert(unlink(other) == 0);
    assert(symlink(path, other) == 0);
    assert(fw_open_existing(&writer, other, 10, 4) == -1 && writer.fd == -1);
    assert(unlink(other) == 0);

    assert(fw_open_existing(&writer, path, 10, 4) == 0);
    assert(writer.file_size == 10 && writer.chunk_size == 4 && writer.total_chunks == 3);
    assert(fw_write_chunk(&writer, 1, "EFGH", 4) == 4);
    char buf[10];
    assert(pread(writer.fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
    assert(memcmp(buf, "ABCDEFGHIJ", sizeof(buf)) == 0);
    fw_close(&writer);
    assert(unlink(path) == 0);

    assert(fw_init(&writer, path, 0, 4) == 0);
    fw_close(&writer);
    assert(fw_open_existing(&writer, path, 0, 4) == 0);
    assert(writer.total_chunks == 0);
    fw_close(&writer);
    assert(unlink(path) == 0);
}

int main(void)
{
    file_writer_t writer;
    char directory[] = "/tmp/fct-writer-test-XXXXXX";
    char path[sizeof(directory) + sizeof("/output.bin")];

    assert(mkdtemp(directory) != NULL);
    test_open_existing(directory);
    snprintf(path, sizeof(path), "%s/output.bin", directory);
    assert(fw_init(&writer, path, 10, 4) == 0);

    /* Write chunks out of order. */
    assert(fw_write_chunk(&writer, 2, "IJ", 2) == 2);
    assert(fw_write_chunk(&writer, 0, "ABCD", 4) == 4);
    assert(fw_write_chunk(&writer, 1, "EFGH", 4) == 4);

    /* Test invalid inputs. */
    assert(fw_write_chunk(&writer, 3, "X", 1) == -1);
    assert(fw_write_chunk(&writer, 0, "AB", 2) == -1);
    assert(fw_write_at(&writer, 9, "XY", 2) == -1);

    fw_close(&writer);

    /* Force allocation to fail after the temporary file has been opened. */
    struct rlimit original_limit, limited;
    assert(getrlimit(RLIMIT_FSIZE, &original_limit) == 0);
    limited = original_limit;
    limited.rlim_cur = 0;
    void (*previous_handler)(int) = signal(SIGXFSZ, SIG_IGN);
    assert(previous_handler != SIG_ERR);
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    assert(fw_init(&writer, path, 10, 4) == -1);
    assert(setrlimit(RLIMIT_FSIZE, &original_limit) == 0);
    assert(signal(SIGXFSZ, previous_handler) != SIG_ERR);
    fw_close(&writer);

    /* Verify output file. */
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);

    char buf[11] = {0};
    assert(fread(buf, 1, 10, fp) == 10);
    assert(strcmp(buf, "ABCDEFGHIJ") == 0);

    /* Cleanup after either validation or open failure must leave other fds open. */
    int unrelated = fileno(fp);
    writer.fd = unrelated;
    assert(fw_init(&writer, path, 10, 0) == -1);
    assert(writer.fd == -1);
    fw_close(&writer);
    assert(fcntl(unrelated, F_GETFD) >= 0);

    char invalid[sizeof(path) + sizeof("/child")];
    snprintf(invalid, sizeof(invalid), "%s/child", path);
    writer.fd = unrelated;
    assert(fw_init(&writer, invalid, 10, 4) == -1);
    assert(writer.fd == -1);
    fw_close(&writer);
    assert(fcntl(unrelated, F_GETFD) >= 0);

    assert(fclose(fp) == 0);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);

    printf("All File Writer tests passed!\n");

    return 0;
}
