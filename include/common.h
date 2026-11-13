#ifndef FCT_COMMON_H
#define FCT_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define FCT_MAGIC UINT32_C(0x46435431)
#define DEFAULT_CHUNK_SZ (2 * 1024 * 1024)
#define FCT_DEFAULT_PORT 9000

/* Protocol fields; transport must explicitly convert integer byte order. */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t total_chunks;
    char filename[256];
} fct_file_meta_t;

typedef struct {
    uint32_t magic;
    uint32_t chunk_id;
    uint64_t offset;
    uint32_t payload_len;
    uint32_t flags;
} fct_chunk_header_t;
#pragma pack(pop)

int send_exact(int sockfd, const void *buf, size_t len);
int recv_exact(int sockfd, void *buf, size_t len);

#endif
