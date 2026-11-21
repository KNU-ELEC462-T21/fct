#include "file_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Open a regular file and compute its chunk count.
 * Return 0 on success, or -1 with errno set. */
int fr_init(file_reader_t* reader, const char* filepath, uint32_t chunk_size) {
    struct stat st;
    int fd, saved;

    if (reader == NULL || filepath == NULL || chunk_size == 0) {
        errno = EINVAL;
        return -1;
    }

    reader->fd = -1;
    reader->file_size = 0;
    reader->chunk_size = chunk_size;
    reader->total_chunks = 0;

    /* A FIFO must reach the file type check without waiting for a writer. */
    fd = open(filepath, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    /* fstat on the opened fd avoids a race between stat() and open(). */
    if (fstat(fd, &st) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    /* Only regular files can be transferred. */
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    /* Ceiling division: a short last chunk still counts; empty file -> 0. */
    uint64_t chunks = ((uint64_t)st.st_size + chunk_size - 1) / chunk_size;
    if (chunks > UINT32_MAX) { /* chunk ids are 32-bit on the wire */
        close(fd);
        errno = EFBIG;
        return -1;
    }

    reader->fd = fd;
    reader->file_size = (uint64_t)st.st_size;
    reader->total_chunks = (uint32_t)chunks;
    return 0;
}

/* Read chunk chunk_id into buf (at least reader->chunk_size bytes).
 * Return the chunk length (only the last chunk may be shorter), or -1 with
 * errno set (EIO if the file shrank since fr_init()).
 * pread() does not share a file offset, so several threads may read different
 * chunks through the same reader concurrently. */
ssize_t fr_read_chunk(file_reader_t* reader, uint32_t chunk_id, void* buf) {
    uint64_t offset, remain;
    size_t want, done = 0;

    if (reader == NULL || buf == NULL || reader->fd < 0 || chunk_id >= reader->total_chunks) {
        errno = EINVAL;
        return -1;
    }

    offset = (uint64_t)chunk_id * reader->chunk_size;
    remain = reader->file_size - offset;
    want = remain < reader->chunk_size ? (size_t)remain : (size_t)reader->chunk_size;

    /* pread() may return fewer bytes than requested, so loop until full. */
    while (done < want) {
        ssize_t n = pread(reader->fd, (char*)buf + done, want - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) { /* the file shrank after fr_init() */
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

void fr_close(file_reader_t* reader) {
    if (reader == NULL)
        return;
    if (reader->fd >= 0)
        close(reader->fd);
    reader->fd = -1;
}
