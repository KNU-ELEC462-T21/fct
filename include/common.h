#ifndef FCT_COMMON_H
#define FCT_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define FCT_MAGIC UINT32_C(0x46435431)
#define DEFAULT_CHUNK_SZ (2 * 1024 * 1024)
#define FCT_DEFAULT_PORT 9000

#define FCT_MAX_DATA_WORKERS 8
#define FCT_CHUNK_FLAG_COMPRESSED UINT32_C(1)
#define FCT_TRANSFER_TOKEN_SIZE 16
#define FCT_CONTROL_FRAME_WIRE_SIZE 24

#define FCT_CONNECT_REGISTER_TIMEOUT_MS 10000U
#define FCT_CONTROL_IO_TIMEOUT_MS 5000U
#define FCT_DATA_IDLE_TIMEOUT_MS 30000U
#define FCT_RESULT_TIMEOUT_MS 30000U

typedef enum {
    FCT_CONTROL_HELLO = 1,
    FCT_CONTROL_READY = 2,
    FCT_CONTROL_DATA = 3,
    FCT_CONTROL_RESULT = 4,
} fct_control_type_t;

typedef enum {
    FCT_RESULT_OK = 0,
    FCT_RESULT_PROTOCOL_ERROR = 1,
    FCT_RESULT_STORAGE_ERROR = 2,
    FCT_RESULT_TRANSFER_ERROR = 3,
} fct_result_t;

/* Host-order fields in wire order. Use FCT_MAGIC for magic and serialize
 * integers explicitly as big-endian.
 * HELLO and READY carry the actual worker count in arg. DATA carries a
 * zero-based connection index. RESULT carries an fct_result_t value.
 * Use an all-zero token in HELLO and pre-READY errors, and the token issued
 * in READY for DATA and subsequent RESULT frames. */
typedef struct {
    uint32_t magic;
    uint16_t type;
    uint16_t arg;
    uint8_t token[FCT_TRANSFER_TOKEN_SIZE];
} fct_control_frame_t;

/* Wire integers use big-endian byte order; filenames occupy exactly 256 bytes.
 * Metadata is 276 bytes and chunk headers are 24 bytes on the wire. */
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
    uint32_t payload_len; /* Bytes on the wire. Derive the raw length from metadata. */
    uint32_t flags;       /* Zero for raw data, FCT_CHUNK_FLAG_COMPRESSED for zlib data. */
} fct_chunk_header_t;
#pragma pack(pop)

/* Return 0 on success, or -1 with errno set; EOF is ECONNRESET. */
int send_exact(int sockfd, const void* buf, size_t len);
int recv_exact(int sockfd, void* buf, size_t len);

#endif
