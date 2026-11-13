#ifndef FILE_WRITER_H
#define FILE_WRITER_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    int fd;
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t total_chunks;
    char filepath[512];
} file_writer_t;

int fw_init(file_writer_t *writer,
            const char *filepath,
            uint64_t total_size,
            uint32_t chunk_size);
ssize_t fw_write_chunk(file_writer_t *writer,
                       uint32_t chunk_id,
                       const void *buf,
                       size_t len);
ssize_t fw_write_at(file_writer_t *writer,
                    uint64_t offset,
                    const void *buf,
                    size_t len);
void fw_close(file_writer_t *writer);
#endif
