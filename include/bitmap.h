#ifndef FCT_BITMAP_H
#define FCT_BITMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

#define FCT_BITMAP_MAGIC UINT32_C(0x46435442) /* 'FCTB' */
#define FCT_BITMAP_VERSION UINT32_C(1)

typedef struct {
    uint32_t total_chunks;
    uint32_t completed_chunks;
    size_t byte_len;
    uint8_t* bits;
} fct_bitmap_t;

size_t fct_bitmap_byte_len(uint32_t total_chunks);
int fct_bitmap_init(fct_bitmap_t* bm, uint32_t total_chunks);
void fct_bitmap_free(fct_bitmap_t* bm);

bool fct_bitmap_get(const fct_bitmap_t* bm, uint32_t chunk_id);
int fct_bitmap_set(fct_bitmap_t* bm, uint32_t chunk_id);
int fct_bitmap_clear(fct_bitmap_t* bm, uint32_t chunk_id);
uint32_t fct_bitmap_completed(const fct_bitmap_t* bm);
uint32_t fct_bitmap_missing(const fct_bitmap_t* bm);
bool fct_bitmap_is_complete(const fct_bitmap_t* bm);

/* Validate byte_len and zero unused trailing bits, copy bits into bm, and
 * recompute completed_chunks. Return 0 on success, or -1 with errno set. */
int fct_bitmap_load_bytes(fct_bitmap_t* bm, const uint8_t* src, size_t byte_len);

/* Return true when newer has every bit set in older and matching total_chunks. */
bool fct_bitmap_is_superset(const fct_bitmap_t* newer, const fct_bitmap_t* older);

/* Atomically persist bm + meta to path (mkstemp + fsync + rename), or load and
 * validate magic, version, CRC32, trailing zero bits, and expected_meta. */
int fct_bitmap_save_file(const fct_bitmap_t* bm, const fct_file_meta_t* meta, const char* path);
int fct_bitmap_load_file(fct_bitmap_t* bm, const fct_file_meta_t* expected_meta, const char* path);

#endif
