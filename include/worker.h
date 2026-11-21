#ifndef FCT_WORKER_H
#define FCT_WORKER_H

#include <stddef.h>

#include "compress.h"
#include "file_reader.h"
#include "file_writer.h"
#include "network.h"

/* Called once per fully sent or successfully written chunk. payload_len
 * excludes the header. Calls may arrive concurrently or out of order. */
typedef void (*fct_progress_fn)(void* context, size_t raw_len, size_t payload_len);

typedef struct {
    fct_progress_fn on_chunk;
    void* context;
} fct_progress_t;

/* channel_count is the negotiated worker count. For empty files
 * (meta->total_chunks == 0), channel_count is 0 and channels may be NULL;
 * fct_send_parallel returns 0 immediately, and fct_receive_parallel stores
 * FCT_RESULT_OK in *result and returns 0 immediately. For nonempty files,
 * channels must contain channel_count initialized channels, one per worker.
 * If progress is supplied, keep it and its context alive until the call returns.
 * Both functions join all started workers before returning 0 on success or
 * -1 with errno set. The caller owns channel closure and file cleanup. */
/* On success, the sender shuts down the write side of each data channel.
 * The caller can still read RESULT from the shared channel in single-worker mode. */
int fct_send_parallel(fct_channel_t* channels, size_t channel_count, file_reader_t* reader,
                      const fct_file_meta_t* meta, fct_compression_mode_t mode,
                      const fct_progress_t* progress);

/* result is required. Store FCT_RESULT_OK on success or the first failure's
 * result code on error. Preserve the shared channel's write side so the
 * caller can send RESULT after a single-worker receive failure. */
int fct_receive_parallel(fct_channel_t* channels, size_t channel_count, file_writer_t* writer,
                         const fct_file_meta_t* meta, const fct_progress_t* progress,
                         fct_result_t* result);

#endif
