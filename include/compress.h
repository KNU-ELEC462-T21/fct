#ifndef FCT_COMPRESS_H
#define FCT_COMPRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FCT_COMPRESS_RAW,
    FCT_COMPRESS_AUTO,
    FCT_COMPRESS_FORCE,
} fct_compression_mode_t;

typedef enum {
    FCT_COMPRESS_REASON_AUTO,
    FCT_COMPRESS_REASON_FORCE,
    FCT_COMPRESS_REASON_EMPTY,
    FCT_COMPRESS_REASON_COMPRESSED_FORMAT,
    FCT_COMPRESS_REASON_LOW_RATIO,
} fct_compression_reason_t;

/* Maximum zlib payload for a raw length in [0, DEFAULT_CHUNK_SZ].
 * Returns 0 with errno set for an out-of-range length. */
size_t fct_compress_bound(size_t raw_len);

/* Returns 0 on success or -1 with errno set. Force selects FORCE. Otherwise,
 * a known compressed format or an empty file selects RAW. For other inputs,
 * select AUTO when the first chunk compresses to at most 95% of its raw size.
 * The first chunk is NULL when len is zero. When !force and len > 0, scratch
 * must hold fct_compress_bound(len) bytes. */
int fct_compression_mode(const char* filename, const void* first_chunk, size_t len, bool force,
                         void* scratch, size_t capacity, fct_compression_mode_t* mode);

/* Same as fct_compression_mode, and optionally records the selection reason
 * in *reason when reason is non-NULL. */
int fct_compression_mode_ex(const char* filename, const void* first_chunk, size_t len, bool force,
                            void* scratch, size_t capacity, fct_compression_mode_t* mode,
                            fct_compression_reason_t* reason);

/* Human-readable compression status string for fct_compression_reason_t. */
const char* fct_compression_reason_str(fct_compression_reason_t reason);

/* Returns 0 on success or -1 with errno set. For AUTO or FORCE, scratch
 * must hold fct_compress_bound(raw_len) bytes. RAW can use NULL scratch.
 * The returned payload borrows src or scratch until the caller finishes
 * sending it. AUTO uses compressed data only if it is smaller than src.
 * flags is zero for raw data or FCT_CHUNK_FLAG_COMPRESSED. */
int fct_encode_chunk(fct_compression_mode_t mode, const void* src, size_t raw_len, void* scratch,
                     size_t capacity, const void** payload, size_t* payload_len, uint32_t* flags);

/* Returns 0 on success or -1 with errno set. dst must hold expected_len
 * bytes and must not overlap payload. Success consumes the entire payload
 * and produces exactly expected_len bytes for either supported flag value. */
int fct_decode_chunk(uint32_t flags, const void* payload, size_t payload_len, void* dst,
                     size_t expected_len);

#endif
