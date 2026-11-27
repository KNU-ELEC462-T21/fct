#ifndef FCT_WORKER_H
#define FCT_WORKER_H

#include <stdbool.h>
#include <stddef.h>

#include "bitmap.h"
#include "compress.h"
#include "file_reader.h"
#include "file_writer.h"
#include "network.h"

/* Called once per fully sent or successfully written chunk. payload_len
 * excludes the header. Calls may arrive concurrently or out of order. */
typedef void (*fct_progress_fn)(void* context, size_t raw_len, size_t payload_len);

typedef void (*fct_chunk_stat_fn)(void* context, size_t raw_len, size_t payload_len,
                                  bool compressed);

typedef struct {
    fct_progress_fn on_chunk;
    fct_chunk_stat_fn on_chunk_stat;
    void* context;
} fct_progress_t;

/* channel_count is the negotiated worker count. For empty or already-complete
 * files, channel_count is 0 and channels may be NULL; fct_send_parallel returns
 * 0 immediately, and fct_receive_parallel stores FCT_RESULT_OK in *result and
 * returns 0 immediately. For nonempty rounds, channels must contain
 * channel_count initialized channels, one per worker.
 * If progress is supplied, keep it and its context alive until the call returns.
 * Both functions join all started workers before returning. The caller owns
 * channel closure and file cleanup. */
/* Send one round of chunks not yet set in completed (or all chunks when
 * completed is NULL), appending an EOR chunk header on each channel without
 * shutting down the write side so channels can be reused for RETRY rounds.
 * When is_retry is true, progress receives raw_len=0 with wire payload_len.
 * Returns 0 on success or -1 with errno set. */
int fct_send_parallel(fct_channel_t* channels, size_t channel_count, file_reader_t* reader,
                      const fct_file_meta_t* meta, fct_compression_mode_t mode,
                      const fct_bitmap_t* completed, bool is_retry, const fct_progress_t* progress);

/* Receive one round of chunks until EOR on all channels, verifying raw CRC32
 * before writing and updating completed for each written chunk.
 * result is required. Return 0 when all total_chunks are complete, 1 when EOR
 * completed with CRC mismatches requiring RETRY (*out_crc_errors > 0), or -1 on
 * fatal error with *result and errno set. Preserve completed chunks on 1 or -1
 * return, and preserve the shared channel's write side in single-worker mode. */
int fct_receive_parallel(fct_channel_t* channels, size_t channel_count, file_writer_t* writer,
                         const fct_file_meta_t* meta, fct_bitmap_t* completed,
                         const fct_progress_t* progress, uint32_t* out_crc_errors,
                         fct_result_t* result);

#endif
