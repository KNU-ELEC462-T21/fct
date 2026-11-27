#define _POSIX_C_SOURCE 200809L

#include "worker.h"

#include "checksum.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

enum {
    CHUNK_MISSING = 0,
    CHUNK_IN_PROGRESS = 1,
    CHUNK_WRITTEN = 2,
};

static bool validate_meta(const fct_file_meta_t* meta) {
    if (!meta || meta->chunk_size == 0 || meta->chunk_size > DEFAULT_CHUNK_SZ)
        return false;
    if (meta->file_size > (uint64_t)INT64_MAX)
        return false;
    uint64_t expected =
        meta->file_size / meta->chunk_size + (meta->file_size % meta->chunk_size != 0);
    if (expected > UINT32_MAX || meta->total_chunks != (uint32_t)expected)
        return false;
    return true;
}

static bool is_valid_compression_mode(fct_compression_mode_t mode) {
    return mode == FCT_COMPRESS_RAW || mode == FCT_COMPRESS_AUTO || mode == FCT_COMPRESS_FORCE;
}

static bool validate_bitmap(const fct_bitmap_t* bm, const fct_file_meta_t* meta) {
    if (!bm)
        return true;
    if (bm->total_chunks != meta->total_chunks)
        return false;
    if (bm->byte_len != fct_bitmap_byte_len(meta->total_chunks))
        return false;
    if (bm->byte_len > 0 && !bm->bits)
        return false;
    if (bm->completed_chunks > bm->total_chunks)
        return false;
    return true;
}

static void shutdown_dedicated_sockets(const int* fds, size_t count) {
    if (count < 2)
        return;
    for (size_t i = 0; i < count; ++i) {
        if (fds[i] >= 0)
            shutdown(fds[i], SHUT_RDWR);
    }
}

typedef struct {
    file_reader_t* reader;
    const fct_file_meta_t* meta;
    fct_compression_mode_t mode;
    const fct_progress_t* progress;
    bool is_retry;
    size_t scratch_cap;
    size_t channel_count;
    int fds[FCT_MAX_DATA_WORKERS];
    uint32_t* slots;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    bool eof_produced;
    bool failed;
    int first_errno;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} send_state_t;

typedef struct {
    send_state_t* state;
    fct_channel_t* ch;
} send_worker_ctx_t;

static void send_record_failure(send_state_t* state, int err) {
    bool first = false;
    pthread_mutex_lock(&state->mu);
    if (!state->failed) {
        state->failed = true;
        state->first_errno = err ? err : EIO;
        first = true;
    }
    pthread_cond_broadcast(&state->not_empty);
    pthread_cond_broadcast(&state->not_full);
    pthread_mutex_unlock(&state->mu);
    if (first)
        shutdown_dedicated_sockets(state->fds, state->channel_count);
}

static void* send_worker_main(void* arg) {
    send_worker_ctx_t* ctx = arg;
    send_state_t* state = ctx->state;
    void* raw_buf = malloc(state->meta->chunk_size);
    void* scratch_buf = state->scratch_cap > 0 ? malloc(state->scratch_cap) : NULL;

    if (!raw_buf || (state->scratch_cap > 0 && !scratch_buf)) {
        send_record_failure(state, ENOMEM);
        free(raw_buf);
        free(scratch_buf);
        return NULL;
    }

    for (;;) {
        uint32_t chunk_id = 0;
        bool have_chunk = false;

        pthread_mutex_lock(&state->mu);
        while (state->count == 0 && !state->eof_produced && !state->failed)
            pthread_cond_wait(&state->not_empty, &state->mu);
        if (!state->failed && state->count > 0) {
            chunk_id = state->slots[state->head];
            state->head = (state->head + 1) % state->capacity;
            state->count--;
            have_chunk = true;
            pthread_cond_signal(&state->not_full);
        }
        pthread_mutex_unlock(&state->mu);

        if (!have_chunk)
            break;

        ssize_t raw_len = fr_read_chunk(state->reader, chunk_id, raw_buf);
        if (raw_len < 0) {
            send_record_failure(state, errno);
            break;
        }

        uint32_t crc = fct_crc32(raw_buf, (size_t)raw_len);
        const void* payload = NULL;
        size_t payload_len = 0;
        uint32_t flags = 0;
        if (fct_encode_chunk(state->mode, raw_buf, (size_t)raw_len, scratch_buf, state->scratch_cap,
                             &payload, &payload_len, &flags) < 0) {
            send_record_failure(state, errno);
            break;
        }
        if (payload_len > UINT32_MAX) {
            send_record_failure(state, EOVERFLOW);
            break;
        }

        fct_chunk_header_t hdr = {
            .magic = FCT_MAGIC,
            .chunk_id = chunk_id,
            .offset = (uint64_t)chunk_id * state->meta->chunk_size,
            .payload_len = (uint32_t)payload_len,
            .flags = flags,
            .crc32 = crc,
        };
        if (fct_channel_send_chunk_idle(ctx->ch, &hdr, payload, FCT_DATA_IDLE_TIMEOUT_MS) < 0) {
            send_record_failure(state, errno);
            break;
        }
        if (state->progress) {
            size_t reported_raw = state->is_retry ? 0 : (size_t)raw_len;
            if (state->progress->on_chunk)
                state->progress->on_chunk(state->progress->context, reported_raw, payload_len);
            if (state->progress->on_chunk_stat) {
                state->progress->on_chunk_stat(state->progress->context, reported_raw, payload_len,
                                               flags == FCT_CHUNK_FLAG_COMPRESSED);
            }
        }
    }

    pthread_mutex_lock(&state->mu);
    bool failed = state->failed;
    pthread_mutex_unlock(&state->mu);
    if (!failed) {
        fct_chunk_header_t eor = {
            .magic = FCT_MAGIC,
            .chunk_id = 0,
            .offset = 0,
            .payload_len = 0,
            .flags = FCT_CHUNK_FLAG_EOR,
            .crc32 = 0,
        };
        if (fct_channel_send_chunk_idle(ctx->ch, &eor, NULL, FCT_DATA_IDLE_TIMEOUT_MS) < 0)
            send_record_failure(state, errno);
    }

    free(raw_buf);
    free(scratch_buf);
    return NULL;
}

int fct_send_parallel(fct_channel_t* channels, size_t channel_count, file_reader_t* reader,
                      const fct_file_meta_t* meta, fct_compression_mode_t mode,
                      const fct_bitmap_t* completed, bool is_retry,
                      const fct_progress_t* progress) {
    if (!validate_meta(meta) || !reader || reader->fd < 0 || reader->file_size != meta->file_size ||
        reader->chunk_size != meta->chunk_size || reader->total_chunks != meta->total_chunks ||
        !is_valid_compression_mode(mode) || !validate_bitmap(completed, meta)) {
        errno = EINVAL;
        return -1;
    }

    if (meta->total_chunks == 0 || (completed != NULL && fct_bitmap_is_complete(completed))) {
        if (channel_count != 0) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }

    if (!channels || channel_count == 0 || channel_count > FCT_MAX_DATA_WORKERS ||
        channel_count > meta->total_chunks) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < channel_count; ++i) {
        if (channels[i].sockfd < 0) {
            errno = EINVAL;
            return -1;
        }
    }

    size_t scratch_cap = 0;
    if (mode != FCT_COMPRESS_RAW) {
        scratch_cap = fct_compress_bound(meta->chunk_size);
        if (scratch_cap == 0)
            return -1;
    }

    send_state_t state = {
        .reader = reader,
        .meta = meta,
        .mode = mode,
        .progress = progress,
        .is_retry = is_retry,
        .scratch_cap = scratch_cap,
        .channel_count = channel_count,
        .capacity = 2 * channel_count,
    };
    for (size_t i = 0; i < channel_count; ++i)
        state.fds[i] = channels[i].sockfd;

    state.slots = calloc(state.capacity, sizeof(*state.slots));
    if (!state.slots)
        return -1;

    int rc = pthread_mutex_init(&state.mu, NULL);
    if (rc != 0) {
        free(state.slots);
        errno = rc;
        return -1;
    }
    rc = pthread_cond_init(&state.not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&state.mu);
        free(state.slots);
        errno = rc;
        return -1;
    }
    rc = pthread_cond_init(&state.not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&state.not_empty);
        pthread_mutex_destroy(&state.mu);
        free(state.slots);
        errno = rc;
        return -1;
    }

    pthread_t threads[FCT_MAX_DATA_WORKERS];
    send_worker_ctx_t ctxs[FCT_MAX_DATA_WORKERS];
    size_t started = 0;

    for (size_t i = 0; i < channel_count; ++i) {
        ctxs[i].state = &state;
        ctxs[i].ch = &channels[i];
        rc = pthread_create(&threads[i], NULL, send_worker_main, &ctxs[i]);
        if (rc != 0) {
            send_record_failure(&state, rc);
            break;
        }
        started++;
    }

    if (started == channel_count) {
        for (uint32_t id = 0; id < meta->total_chunks; ++id) {
            if (completed && fct_bitmap_get(completed, id))
                continue;
            pthread_mutex_lock(&state.mu);
            while (state.count == state.capacity && !state.failed)
                pthread_cond_wait(&state.not_full, &state.mu);
            if (state.failed) {
                pthread_mutex_unlock(&state.mu);
                break;
            }
            state.slots[state.tail] = id;
            state.tail = (state.tail + 1) % state.capacity;
            state.count++;
            pthread_cond_signal(&state.not_empty);
            pthread_mutex_unlock(&state.mu);
        }

        pthread_mutex_lock(&state.mu);
        state.eof_produced = true;
        pthread_cond_broadcast(&state.not_empty);
        pthread_mutex_unlock(&state.mu);
    }

    for (size_t i = 0; i < started; ++i)
        pthread_join(threads[i], NULL);

    bool failed = state.failed;
    int first_errno = state.first_errno;

    pthread_cond_destroy(&state.not_full);
    pthread_cond_destroy(&state.not_empty);
    pthread_mutex_destroy(&state.mu);
    free(state.slots);

    if (failed) {
        errno = first_errno;
        return -1;
    }
    return 0;
}

typedef struct {
    file_writer_t* writer;
    const fct_file_meta_t* meta;
    fct_bitmap_t* completed;
    const fct_progress_t* progress;
    size_t max_payload_cap;
    size_t channel_count;
    int fds[FCT_MAX_DATA_WORKERS];
    uint8_t* states;
    uint32_t written_count;
    uint32_t crc_error_count;
    bool failed;
    fct_result_t first_result;
    int first_errno;
    pthread_mutex_t mu;
} recv_state_t;

typedef struct {
    recv_state_t* state;
    fct_channel_t* ch;
} recv_worker_ctx_t;

static void recv_record_failure(recv_state_t* state, fct_result_t code, int err) {
    bool first = false;
    pthread_mutex_lock(&state->mu);
    if (!state->failed) {
        state->failed = true;
        state->first_result = code;
        state->first_errno = err ? err : EIO;
        first = true;
    }
    pthread_mutex_unlock(&state->mu);
    if (first)
        shutdown_dedicated_sockets(state->fds, state->channel_count);
}

static void* recv_worker_main(void* arg) {
    recv_worker_ctx_t* ctx = arg;
    recv_state_t* state = ctx->state;
    void* recv_buf = malloc(state->max_payload_cap);
    void* decode_buf = malloc(state->meta->chunk_size);

    if (!recv_buf || !decode_buf) {
        recv_record_failure(state, FCT_RESULT_TRANSFER_ERROR, ENOMEM);
        free(recv_buf);
        free(decode_buf);
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&state->mu);
        bool failed = state->failed;
        pthread_mutex_unlock(&state->mu);
        if (failed)
            break;

        fct_chunk_header_t hdr;
        int rc = fct_channel_recv_chunk_idle(ctx->ch, &hdr, recv_buf, state->max_payload_cap,
                                             FCT_DATA_IDLE_TIMEOUT_MS);
        if (rc == 1) {
            recv_record_failure(state, FCT_RESULT_TRANSFER_ERROR, ECONNRESET);
            break;
        }
        if (rc < 0) {
            int err = errno;
            fct_result_t code = (err == EPROTO || err == EMSGSIZE) ? FCT_RESULT_PROTOCOL_ERROR
                                                                   : FCT_RESULT_TRANSFER_ERROR;
            recv_record_failure(state, code, err);
            break;
        }
        if (hdr.flags == FCT_CHUNK_FLAG_EOR)
            break;

        if (hdr.chunk_id >= state->meta->total_chunks) {
            recv_record_failure(state, FCT_RESULT_PROTOCOL_ERROR, EPROTO);
            break;
        }
        uint64_t expected_offset = (uint64_t)hdr.chunk_id * state->meta->chunk_size;
        uint64_t remaining = state->meta->file_size - expected_offset;
        size_t raw_len =
            remaining < state->meta->chunk_size ? (size_t)remaining : state->meta->chunk_size;
        if (hdr.offset != expected_offset) {
            recv_record_failure(state, FCT_RESULT_PROTOCOL_ERROR, EPROTO);
            break;
        }
        if (hdr.flags == 0) {
            if (hdr.payload_len != raw_len) {
                recv_record_failure(state, FCT_RESULT_PROTOCOL_ERROR, EPROTO);
                break;
            }
        } else if (hdr.flags == FCT_CHUNK_FLAG_COMPRESSED) {
            size_t bound = fct_compress_bound(raw_len);
            if (bound == 0 || hdr.payload_len < 1 || hdr.payload_len > bound) {
                recv_record_failure(state, FCT_RESULT_PROTOCOL_ERROR, EPROTO);
                break;
            }
        } else {
            recv_record_failure(state, FCT_RESULT_PROTOCOL_ERROR, EPROTO);
            break;
        }

        bool claimed = false;
        bool first_fail = false;
        pthread_mutex_lock(&state->mu);
        if (!state->failed) {
            if (state->states[hdr.chunk_id] != CHUNK_MISSING) {
                state->failed = true;
                state->first_result = FCT_RESULT_PROTOCOL_ERROR;
                state->first_errno = EPROTO;
                first_fail = true;
            } else {
                state->states[hdr.chunk_id] = CHUNK_IN_PROGRESS;
                claimed = true;
            }
        }
        pthread_mutex_unlock(&state->mu);
        if (first_fail)
            shutdown_dedicated_sockets(state->fds, state->channel_count);
        if (!claimed)
            break;

        const void* write_buf = recv_buf;
        if (hdr.flags == FCT_CHUNK_FLAG_COMPRESSED) {
            if (fct_decode_chunk(hdr.flags, recv_buf, hdr.payload_len, decode_buf, raw_len) < 0) {
                int err = errno;
                fct_result_t code =
                    (err == ENOMEM) ? FCT_RESULT_TRANSFER_ERROR : FCT_RESULT_PROTOCOL_ERROR;
                recv_record_failure(state, code, err ? err : EPROTO);
                break;
            }
            write_buf = decode_buf;
        }

        if (fct_crc32(write_buf, raw_len) != hdr.crc32) {
            pthread_mutex_lock(&state->mu);
            state->states[hdr.chunk_id] = CHUNK_MISSING;
            state->crc_error_count++;
            pthread_mutex_unlock(&state->mu);
            continue;
        }

        if (fw_write_chunk(state->writer, hdr.chunk_id, write_buf, raw_len) < 0) {
            int err = errno;
            pthread_mutex_lock(&state->mu);
            state->states[hdr.chunk_id] = CHUNK_MISSING;
            pthread_mutex_unlock(&state->mu);
            recv_record_failure(state, FCT_RESULT_STORAGE_ERROR, err);
            break;
        }

        pthread_mutex_lock(&state->mu);
        state->states[hdr.chunk_id] = CHUNK_WRITTEN;
        if (state->completed)
            (void)fct_bitmap_set(state->completed, hdr.chunk_id);
        state->written_count++;
        pthread_mutex_unlock(&state->mu);

        if (state->progress) {
            if (state->progress->on_chunk)
                state->progress->on_chunk(state->progress->context, raw_len, hdr.payload_len);
            if (state->progress->on_chunk_stat) {
                state->progress->on_chunk_stat(state->progress->context, raw_len, hdr.payload_len,
                                               hdr.flags == FCT_CHUNK_FLAG_COMPRESSED);
            }
        }
    }

    free(recv_buf);
    free(decode_buf);
    return NULL;
}

int fct_receive_parallel(fct_channel_t* channels, size_t channel_count, file_writer_t* writer,
                         const fct_file_meta_t* meta, fct_bitmap_t* completed,
                         const fct_progress_t* progress, uint32_t* out_crc_errors,
                         fct_result_t* result) {
    if (out_crc_errors)
        *out_crc_errors = 0;
    if (!result) {
        errno = EINVAL;
        return -1;
    }
    *result = FCT_RESULT_PROTOCOL_ERROR;

    if (!validate_meta(meta) || !validate_bitmap(completed, meta)) {
        errno = EINVAL;
        return -1;
    }
    if (!writer || writer->fd < 0 || writer->file_size != meta->file_size ||
        writer->chunk_size != meta->chunk_size || writer->total_chunks != meta->total_chunks) {
        *result = FCT_RESULT_STORAGE_ERROR;
        errno = EINVAL;
        return -1;
    }

    if (meta->total_chunks == 0 || (completed != NULL && fct_bitmap_is_complete(completed))) {
        if (channel_count != 0) {
            *result = FCT_RESULT_PROTOCOL_ERROR;
            errno = EINVAL;
            return -1;
        }
        *result = FCT_RESULT_OK;
        return 0;
    }

    if (!channels || channel_count == 0 || channel_count > FCT_MAX_DATA_WORKERS ||
        channel_count > meta->total_chunks) {
        *result = FCT_RESULT_PROTOCOL_ERROR;
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < channel_count; ++i) {
        if (channels[i].sockfd < 0) {
            *result = FCT_RESULT_PROTOCOL_ERROR;
            errno = EINVAL;
            return -1;
        }
    }

    size_t max_payload_cap = fct_compress_bound(meta->chunk_size);
    if (max_payload_cap == 0) {
        *result = FCT_RESULT_TRANSFER_ERROR;
        return -1;
    }
    if (max_payload_cap < meta->chunk_size)
        max_payload_cap = meta->chunk_size;

    recv_state_t state = {
        .writer = writer,
        .meta = meta,
        .completed = completed,
        .progress = progress,
        .max_payload_cap = max_payload_cap,
        .channel_count = channel_count,
        .first_result = FCT_RESULT_OK,
    };
    for (size_t i = 0; i < channel_count; ++i)
        state.fds[i] = channels[i].sockfd;

    state.states = calloc(meta->total_chunks, sizeof(*state.states));
    if (!state.states) {
        *result = FCT_RESULT_TRANSFER_ERROR;
        errno = ENOMEM;
        return -1;
    }
    if (completed) {
        for (uint32_t id = 0; id < meta->total_chunks; ++id) {
            if (fct_bitmap_get(completed, id)) {
                state.states[id] = CHUNK_WRITTEN;
                state.written_count++;
            }
        }
    }

    int rc = pthread_mutex_init(&state.mu, NULL);
    if (rc != 0) {
        free(state.states);
        *result = FCT_RESULT_TRANSFER_ERROR;
        errno = rc;
        return -1;
    }

    pthread_t threads[FCT_MAX_DATA_WORKERS];
    recv_worker_ctx_t ctxs[FCT_MAX_DATA_WORKERS];
    size_t started = 0;

    for (size_t i = 0; i < channel_count; ++i) {
        ctxs[i].state = &state;
        ctxs[i].ch = &channels[i];
        rc = pthread_create(&threads[i], NULL, recv_worker_main, &ctxs[i]);
        if (rc != 0) {
            recv_record_failure(&state, FCT_RESULT_TRANSFER_ERROR, rc);
            break;
        }
        started++;
    }

    for (size_t i = 0; i < started; ++i)
        pthread_join(threads[i], NULL);

    if (out_crc_errors)
        *out_crc_errors = state.crc_error_count;

    bool failed = state.failed;
    fct_result_t first_result = state.first_result;
    int first_errno = state.first_errno;
    uint32_t written_count = state.written_count;
    uint32_t crc_error_count = state.crc_error_count;

    pthread_mutex_destroy(&state.mu);
    free(state.states);

    if (failed) {
        *result = first_result;
        errno = first_errno;
        return -1;
    }
    if (written_count == meta->total_chunks) {
        *result = FCT_RESULT_OK;
        return 0;
    }
    if (crc_error_count > 0) {
        *result = FCT_RESULT_CHECKSUM_ERROR;
        return 1;
    }

    *result = FCT_RESULT_PROTOCOL_ERROR;
    errno = EPROTO;
    return -1;
}
