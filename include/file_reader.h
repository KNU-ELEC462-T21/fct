#ifndef FILE_READER_H
#define FILE_READER_H
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    int fd;
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t total_chunks;
} file_reader_t;

int fr_init(file_reader_t *reader, const char *filepath, uint32_t chunk_size);
/* buf must have room for at least reader->chunk_size bytes. */
ssize_t fr_read_chunk(file_reader_t *reader, uint32_t chunk_id, void *buf);
void fr_close(file_reader_t *reader);
#endif
