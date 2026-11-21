#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "file_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int fw_init(file_writer_t *writer, const char *filepath,
            uint64_t total_size, uint32_t chunk_size)
{
    if (!writer) {
        errno = EINVAL;
        return -1;
    }
    writer->fd = -1;
    if (!filepath || !*filepath || chunk_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (strlen(filepath) >= sizeof(writer->filepath)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    uint64_t chunks = total_size / chunk_size +
                      (total_size % chunk_size != 0);

    if (total_size > INT64_MAX || chunks > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    /* Allocate in the same directory so the final rename is atomic. */
    char temporary[sizeof(writer->filepath) + sizeof(".fct-XXXXXX")];
    const char *slash = strrchr(filepath, '/');
    size_t prefix = slash ? (size_t)(slash - filepath) + 1 : 0;
    memcpy(temporary, filepath, prefix);
    memcpy(temporary + prefix, ".fct-XXXXXX", sizeof(".fct-XXXXXX"));
    int fd = mkstemp(temporary);

    if (fd == -1)
        return -1;

    /* Reserve disk space for non-empty output files. */
    int err = total_size ? posix_fallocate(fd, 0, (off_t)total_size) : 0;
    if (err == 0 && rename(temporary, filepath) < 0)
        err = errno;
    if (err != 0) {
        close(fd);
        unlink(temporary);
        errno = err;
        return -1;
    }

    writer->fd = fd;
    writer->file_size = total_size;
    writer->chunk_size = chunk_size;
    writer->total_chunks = (uint32_t)chunks;
    strcpy(writer->filepath, filepath);

    return 0;
}

ssize_t fw_write_at(file_writer_t *writer, uint64_t offset,
                    const void *buf, size_t len)
{
    if (!writer || (!buf && len != 0)) {
        errno = EINVAL;
        return -1;
    }

    if (writer->fd < 0) {
        errno = EBADF;
        return -1;
    }

    if (offset > writer->file_size ||
        len > writer->file_size - offset || len > SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }

    const unsigned char *data = buf;
    size_t written = 0;

    /* Retry interruptions and partial writes. */
    while (written < len) {
        ssize_t n = pwrite(writer->fd, data + written,
                           len - written, (off_t)(offset + written));

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        written += (size_t)n;
    }

    return (ssize_t)written;
}

ssize_t fw_write_chunk(file_writer_t *writer, uint32_t chunk_id,
                       const void *buf, size_t len)
{
    if (!writer || writer->chunk_size == 0 ||
        chunk_id >= writer->total_chunks) {
        errno = EINVAL;
        return -1;
    }

    uint64_t offset = (uint64_t)chunk_id * writer->chunk_size;
    uint64_t remaining = writer->file_size - offset;

    size_t expected = remaining < writer->chunk_size
                          ? (size_t)remaining : writer->chunk_size;

    if (len != expected) {
        errno = EINVAL;
        return -1;
    }

    return fw_write_at(writer, offset, buf, len);
}

void fw_close(file_writer_t *writer)
{
    if (writer && writer->fd >= 0) {
        close(writer->fd);
        writer->fd = -1;
    }
}
