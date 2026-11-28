#ifndef FCT_CHECKSUM_H
#define FCT_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

typedef struct {
    void* evp;
} fct_sha256_ctx_t;

/* Compute IEEE CRC32 over data[0..len). Returns 0 when len is 0. */
uint32_t fct_crc32(const void* data, size_t len);

/* Every fct_sha256_init() call must be paired with fct_sha256_final() to
 * release the underlying digest context. */
void fct_sha256_init(fct_sha256_ctx_t* ctx);
void fct_sha256_update(fct_sha256_ctx_t* ctx, const void* data, size_t len);
void fct_sha256_final(fct_sha256_ctx_t* ctx, uint8_t out_digest[FCT_SHA256_SIZE]);
void fct_sha256_bytes(const void* data, size_t len, uint8_t out_digest[FCT_SHA256_SIZE]);

/* Hash bytes [0, expected_size) from fd using pread() without changing the file
 * offset. Return 0 on success, or -1 with errno set (EIO on size mismatch). */
int fct_sha256_fd(int fd, uint64_t expected_size, uint8_t out_digest[FCT_SHA256_SIZE]);

/* Encode digest as 64 lowercase hex characters plus NUL terminator. */
void fct_sha256_to_hex(const uint8_t digest[FCT_SHA256_SIZE],
                       char out_hex[FCT_SHA256_SIZE * 2 + 1]);

#endif
