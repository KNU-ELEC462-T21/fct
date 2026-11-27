#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "checksum.h"
#include "cli.h"
#include "compress.h"
#include "file_reader.h"
#include "file_writer.h"
#include "network.h"
#include "progress.h"
#include "worker.h"

#define FCT_FAILURE_RESULT_WAIT_MS 1000U

static int make_deadline(unsigned timeout_ms, struct timespec* out) {
    if (clock_gettime(CLOCK_MONOTONIC, out) < 0)
        return -1;
    time_t add_sec = (time_t)(timeout_ms / 1000U);
    long add_nsec = (long)((timeout_ms % 1000U) * 1000000UL);
    out->tv_nsec += add_nsec;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_nsec -= 1000000000L;
        add_sec += 1;
    }
    out->tv_sec += add_sec;
    return 0;
}

static struct timespec earlier_deadline(struct timespec a, struct timespec b) {
    if (a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec <= b.tv_nsec))
        return a;
    return b;
}

static bool is_zero_token(const uint8_t token[FCT_TRANSFER_TOKEN_SIZE]) {
    for (size_t i = 0; i < FCT_TRANSFER_TOKEN_SIZE; ++i) {
        if (token[i] != 0)
            return false;
    }
    return true;
}

static bool tokens_equal(const uint8_t a[FCT_TRANSFER_TOKEN_SIZE],
                         const uint8_t b[FCT_TRANSFER_TOKEN_SIZE]) {
    return memcmp(a, b, FCT_TRANSFER_TOKEN_SIZE) == 0;
}

static int generate_token(uint8_t token[FCT_TRANSFER_TOKEN_SIZE]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    size_t done = 0;
    int result = 0;
    while (done < FCT_TRANSFER_TOKEN_SIZE) {
        ssize_t n = read(fd, token + done, FCT_TRANSFER_TOKEN_SIZE - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            result = -1;
            break;
        }
        if (n == 0) {
            errno = EIO;
            result = -1;
            break;
        }
        done += (size_t)n;
    }
    int saved = errno;
    close(fd);
    errno = saved;
    if (result == 0 && is_zero_token(token))
        token[0] = 1;
    return result;
}

static bool is_valid_result_code(uint16_t arg) {
    return arg <= FCT_RESULT_CHECKSUM_ERROR;
}

static int result_to_errno(uint16_t code) {
    switch (code) {
    case FCT_RESULT_OK:
        return 0;
    case FCT_RESULT_PROTOCOL_ERROR:
        return EPROTO;
    case FCT_RESULT_STORAGE_ERROR:
        return EIO;
    case FCT_RESULT_TRANSFER_ERROR:
        return ECONNABORTED;
    case FCT_RESULT_CHECKSUM_ERROR:
        return EBADMSG;
    default:
        return EPROTO;
    }
}

static int send_result_frame(fct_channel_t* ch, fct_result_t code,
                             const uint8_t token[FCT_TRANSFER_TOKEN_SIZE]) {
    struct timespec deadline;
    if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &deadline) < 0)
        return -1;
    fct_control_frame_t frame = {
        .magic = FCT_MAGIC,
        .type = FCT_CONTROL_RESULT,
        .arg = (uint16_t)code,
    };
    if (token)
        memcpy(frame.token, token, FCT_TRANSFER_TOKEN_SIZE);
    return fct_channel_send_control_until(ch, &frame, &deadline);
}

static bool is_valid_filename(const char* name) {
    const char* hex = "0123456789abcdef";
    if (strncmp(name, ".fct-", 5) == 0 && strlen(name + 5) == FCT_SHA256_SIZE * 2 &&
        strspn(name + 5, hex) == FCT_SHA256_SIZE * 2)
        return false; /* Private session directory names. */
    return name[0] && !strchr(name, '/') && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static bool validate_meta_and_workers(const fct_file_meta_t* meta, uint16_t workers) {
    if (!meta || !memchr(meta->filename, '\0', sizeof(meta->filename)) ||
        !is_valid_filename(meta->filename) || meta->chunk_size == 0 ||
        meta->chunk_size > DEFAULT_CHUNK_SZ || meta->file_size > (uint64_t)INT64_MAX) {
        return false;
    }
    uint64_t expected =
        meta->file_size / meta->chunk_size + (meta->file_size % meta->chunk_size != 0);
    if (expected > UINT32_MAX || meta->total_chunks != (uint32_t)expected ||
        fct_bitmap_byte_len(meta->total_chunks) > FCT_MAX_BITMAP_BYTES)
        return false;
    if (meta->total_chunks == 0)
        return workers == 0;
    return workers >= 1 && workers <= FCT_MAX_DATA_WORKERS && workers <= meta->total_chunks;
}

static void init_channels(fct_channel_t* channels, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        memset(&channels[i], 0, sizeof(channels[i]));
        channels[i].sockfd = -1;
    }
}

static void close_channels(fct_channel_t* channels, size_t count) {
    for (size_t i = 0; i < count; ++i)
        fct_channel_close(&channels[i]);
}

static int determine_compression_mode(file_reader_t* reader, const fct_file_meta_t* meta,
                                      bool force, fct_compression_mode_t* mode) {
    if (meta->total_chunks == 0)
        return fct_compression_mode(meta->filename, NULL, 0, force, NULL, 0, mode);

    void* first_chunk = malloc(meta->chunk_size);
    if (!first_chunk)
        return -1;

    int result = -1;
    ssize_t first_len = fr_read_chunk(reader, 0, first_chunk);
    if (first_len >= 0) {
        if (force) {
            result = fct_compression_mode(meta->filename, first_chunk, (size_t)first_len, true,
                                          NULL, 0, mode);
        } else {
            size_t cap = fct_compress_bound((size_t)first_len);
            void* scratch = cap > 0 ? malloc(cap) : NULL;
            if (cap > 0 && scratch != NULL) {
                result = fct_compression_mode(meta->filename, first_chunk, (size_t)first_len, false,
                                              scratch, cap, mode);
            }
            int saved = errno;
            free(scratch);
            errno = saved;
        }
    }

    int saved = errno;
    free(first_chunk);
    errno = saved;
    return result;
}

static uint16_t compute_worker_count(int requested, uint32_t total_chunks) {
    if (total_chunks == 0)
        return 0;
    uint32_t capped = requested < 1 ? 1U : (uint32_t)requested;
    if (capped > total_chunks)
        capped = total_chunks;
    if (capped > FCT_MAX_DATA_WORKERS)
        capped = FCT_MAX_DATA_WORKERS;
    return (uint16_t)capped;
}

static int open_data_channels(const fct_options_t* options, uint16_t worker_count,
                              const uint8_t token[FCT_TRANSFER_TOKEN_SIZE],
                              fct_channel_t* channels) {
    init_channels(channels, worker_count);

    struct timespec deadline;
    if (make_deadline(FCT_CONNECT_REGISTER_TIMEOUT_MS, &deadline) < 0)
        return -1;

    for (uint16_t i = 0; i < worker_count; ++i) {
        if (fct_channel_connect_until(&channels[i], options->server_ip, options->port, &deadline) <
            0) {
            int saved = errno;
            close_channels(channels, worker_count);
            errno = saved;
            return -1;
        }

        fct_control_frame_t frame = {
            .magic = FCT_MAGIC,
            .type = FCT_CONTROL_DATA,
            .arg = i,
        };
        memcpy(frame.token, token, FCT_TRANSFER_TOKEN_SIZE);
        if (fct_channel_send_control_until(&channels[i], &frame, &deadline) < 0) {
            int saved = errno;
            close_channels(channels, worker_count);
            errno = saved;
            return -1;
        }
    }
    return 0;
}

static int send_handshake(fct_channel_t* control, const fct_options_t* options,
                          const fct_file_meta_t* meta, uint16_t* worker_count,
                          uint8_t out_token[FCT_TRANSFER_TOKEN_SIZE]) {
    struct timespec deadline;
    if (make_deadline(FCT_CONNECT_REGISTER_TIMEOUT_MS, &deadline) < 0 ||
        fct_channel_connect_until(control, options->server_ip, options->port, &deadline) < 0) {
        return -1;
    }

    if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &deadline) < 0)
        return -1;
    fct_control_frame_t hello = {
        .magic = FCT_MAGIC,
        .type = FCT_CONTROL_HELLO,
        .arg = *worker_count,
    };
    if (fct_channel_send_control_until(control, &hello, &deadline) < 0 ||
        fct_channel_send_meta_until(control, meta, &deadline) < 0) {
        return -1;
    }

    fct_control_frame_t resp;
    if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &deadline) < 0 ||
        fct_channel_recv_control_until(control, &resp, &deadline) < 0) {
        return -1;
    }

    if (resp.type == FCT_CONTROL_RESULT) {
        if (is_zero_token(resp.token) && is_valid_result_code(resp.arg) &&
            resp.arg != FCT_RESULT_OK) {
            errno = result_to_errno(resp.arg);
        } else {
            errno = EPROTO;
        }
        return -1;
    }

    if (resp.type != FCT_CONTROL_READY || resp.arg > *worker_count || is_zero_token(resp.token)) {
        errno = EPROTO;
        return -1;
    }

    *worker_count = resp.arg;
    memcpy(out_token, resp.token, FCT_TRANSFER_TOKEN_SIZE);
    return 0;
}

static int wait_sender_result(fct_channel_t* control, const uint8_t token[FCT_TRANSFER_TOKEN_SIZE],
                              unsigned timeout_ms, uint16_t* out_code) {
    struct timespec deadline;
    if (make_deadline(timeout_ms, &deadline) < 0)
        return -1;

    fct_control_frame_t frame;
    if (fct_channel_recv_control_until(control, &frame, &deadline) < 0)
        return -1;

    if (frame.type != FCT_CONTROL_RESULT || !tokens_equal(frame.token, token) ||
        !is_valid_result_code(frame.arg)) {
        errno = EPROTO;
        return -1;
    }

    *out_code = frame.arg;
    return 0;
}

static int receive_bitmap(fct_channel_t* control, fct_bitmap_t* bitmap) {
    size_t len = bitmap->byte_len;
    uint8_t* bits = len ? malloc(len) : NULL;
    if (len && !bits)
        return -1;

    struct timespec deadline;
    int rc = -1;
    if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &deadline) == 0 &&
        fct_channel_recv_bitmap_until(control, bits, len, &deadline) == 0)
        rc = fct_bitmap_load_bytes(bitmap, bits, len);
    int saved = errno;
    free(bits);
    errno = saved;
    return rc;
}

static uint64_t resumed_bytes(const fct_file_meta_t* meta, const fct_bitmap_t* bitmap) {
    uint64_t bytes = (uint64_t)fct_bitmap_completed(bitmap) * meta->chunk_size;
    uint32_t tail = meta->file_size % meta->chunk_size;
    if (tail && fct_bitmap_get(bitmap, meta->total_chunks - 1))
        bytes -= meta->chunk_size - tail;
    return bytes;
}

static int send_rounds(fct_channel_t* control, fct_channel_t* channels, uint16_t worker_count,
                       const uint8_t token[FCT_TRANSFER_TOKEN_SIZE], file_reader_t* reader,
                       const fct_file_meta_t* meta, fct_compression_mode_t mode,
                       fct_bitmap_t* completed, const fct_progress_t* progress) {
    for (unsigned round = 0;; ++round) {
        if (fct_send_parallel(channels, worker_count, reader, meta, mode, completed, round != 0,
                              progress) < 0) {
            int saved = errno;
            uint16_t code;
            if (saved != ETIMEDOUT &&
                wait_sender_result(control, token, FCT_FAILURE_RESULT_WAIT_MS, &code) == 0 &&
                code != FCT_RESULT_OK)
                saved = result_to_errno(code);
            errno = saved;
            return -1;
        }

        struct timespec deadline;
        fct_control_frame_t frame;
        if (make_deadline(FCT_RESULT_TIMEOUT_MS, &deadline) < 0 ||
            fct_channel_recv_control_until(control, &frame, &deadline) < 0)
            return -1;
        if (!tokens_equal(frame.token, token))
            break;
        if (frame.type == FCT_CONTROL_RESULT && is_valid_result_code(frame.arg)) {
            errno = result_to_errno(frame.arg);
            return errno ? -1 : 0;
        }
        if (frame.type != FCT_CONTROL_RETRY || !worker_count || frame.arg != round + 1 ||
            frame.arg > FCT_MAX_CHUNK_RETRIES)
            break;

        fct_bitmap_t updated;
        if (fct_bitmap_init(&updated, meta->total_chunks) < 0)
            return -1;
        if (receive_bitmap(control, &updated) < 0) {
            fct_bitmap_free(&updated);
            return -1;
        }
        if (!fct_bitmap_is_superset(&updated, completed) || fct_bitmap_is_complete(&updated)) {
            fct_bitmap_free(&updated);
            break;
        }
        fct_bitmap_free(completed);
        *completed = updated;
    }
    errno = EPROTO;
    return -1;
}

static int send_session(const fct_options_t* options, file_reader_t* reader,
                        const fct_file_meta_t* meta, fct_compression_mode_t mode) {
    uint16_t requested = compute_worker_count(options->worker_count, meta->total_chunks);
    uint16_t worker_count = requested;
    fct_channel_t control = {.sockfd = -1};
    fct_channel_t data_channels[FCT_MAX_DATA_WORKERS];
    init_channels(data_channels, FCT_MAX_DATA_WORKERS);
    uint8_t token[FCT_TRANSFER_TOKEN_SIZE] = {0};
    fct_bitmap_t completed;
    if (fct_bitmap_init(&completed, meta->total_chunks) < 0)
        return -1;

    int result = -1;
    if (send_handshake(&control, options, meta, &worker_count, token) < 0 ||
        receive_bitmap(&control, &completed) < 0)
        goto done;
    if (worker_count > fct_bitmap_missing(&completed) ||
        ((worker_count == 0) != fct_bitmap_is_complete(&completed))) {
        errno = EPROTO;
        goto done;
    }

    fct_progress_state_t progress;
    if (fct_progress_init(&progress, meta->file_size, stderr) < 0)
        goto done;
    fct_progress_set_resumed(&progress, resumed_bytes(meta, &completed));
    fct_progress_t cb = {.on_chunk = fct_progress_on_chunk, .context = &progress};

    if (worker_count < 2 || open_data_channels(options, worker_count, token, data_channels) == 0) {
        fct_channel_t* channels = worker_count < 2 ? &control : data_channels;
        result = send_rounds(&control, channels, worker_count, token, reader, meta, mode,
                             &completed, &cb);
    }

    int saved = errno;
    if (result == 0)
        fct_progress_finish(&progress);
    else
        fct_progress_abort(&progress);
    fct_progress_destroy(&progress);
    errno = saved;

done:;
    int cleanup_errno = errno;
    close_channels(data_channels, FCT_MAX_DATA_WORKERS);
    fct_channel_close(&control);
    fct_bitmap_free(&completed);
    errno = cleanup_errno;
    return result;
}

static int send_file(const fct_options_t* options) {
    const char* name = strrchr(options->file_path, '/');
    name = name ? name + 1 : options->file_path;
    fct_file_meta_t meta = {.magic = FCT_MAGIC};
    if (strlen(name) >= sizeof(meta.filename)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!is_valid_filename(name)) {
        errno = EINVAL;
        return -1;
    }
    strcpy(meta.filename, name);

    file_reader_t reader;
    if (fr_init(&reader, options->file_path, DEFAULT_CHUNK_SZ) < 0)
        return -1;

    meta.file_size = reader.file_size;
    meta.chunk_size = reader.chunk_size;
    meta.total_chunks = reader.total_chunks;

    fct_compression_mode_t mode = FCT_COMPRESS_RAW;
    int result = -1;
    if (fct_bitmap_byte_len(meta.total_chunks) > FCT_MAX_BITMAP_BYTES) {
        errno = EFBIG;
    } else if (fct_sha256_fd(reader.fd, meta.file_size, meta.file_sha256) == 0 &&
               determine_compression_mode(&reader, &meta, options->force_compression, &mode) == 0)
        result = send_session(options, &reader, &meta, mode);

    int saved = errno;
    fr_close(&reader);
    errno = saved;
    return result;
}

static int accept_data_channels(int listener, uint16_t worker_count,
                                const uint8_t token[FCT_TRANSFER_TOKEN_SIZE],
                                fct_channel_t* channels, fct_result_t* out_code) {
    init_channels(channels, worker_count);
    *out_code = FCT_RESULT_TRANSFER_ERROR;

    struct timespec reg_deadline;
    if (make_deadline(FCT_CONNECT_REGISTER_TIMEOUT_MS, &reg_deadline) < 0)
        return -1;

    bool seen[FCT_MAX_DATA_WORKERS] = {false};
    uint16_t registered = 0;

    while (registered < worker_count) {
        fct_channel_t incoming = {.sockfd = -1};
        if (fct_channel_accept_until(&incoming, listener, &reg_deadline) < 0) {
            int saved = errno;
            close_channels(channels, worker_count);
            errno = saved;
            *out_code = FCT_RESULT_TRANSFER_ERROR;
            return -1;
        }

        struct timespec ctrl_deadline;
        if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &ctrl_deadline) < 0) {
            int saved = errno;
            fct_channel_close(&incoming);
            close_channels(channels, worker_count);
            errno = saved;
            *out_code = FCT_RESULT_TRANSFER_ERROR;
            return -1;
        }

        struct timespec io_deadline = earlier_deadline(ctrl_deadline, reg_deadline);
        fct_control_frame_t frame;
        if (fct_channel_recv_control_until(&incoming, &frame, &io_deadline) < 0) {
            int saved = errno;
            fct_channel_close(&incoming);
            close_channels(channels, worker_count);
            errno = saved;
            *out_code = (saved == EPROTO) ? FCT_RESULT_PROTOCOL_ERROR : FCT_RESULT_TRANSFER_ERROR;
            return -1;
        }

        if (frame.type != FCT_CONTROL_DATA || !tokens_equal(frame.token, token) ||
            frame.arg >= worker_count || seen[frame.arg]) {
            fct_channel_close(&incoming);
            close_channels(channels, worker_count);
            errno = EPROTO;
            *out_code = FCT_RESULT_PROTOCOL_ERROR;
            return -1;
        }

        channels[frame.arg] = incoming;
        seen[frame.arg] = true;
        registered++;
    }

    *out_code = FCT_RESULT_OK;
    return 0;
}

static int receive_handshake(fct_channel_t* control, fct_file_meta_t* out_meta,
                             uint16_t* out_workers) {
    struct timespec deadline;
    fct_control_frame_t hello;
    if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &deadline) < 0 ||
        fct_channel_recv_control_until(control, &hello, &deadline) < 0) {
        if (errno == EPROTO)
            (void)send_result_frame(control, FCT_RESULT_PROTOCOL_ERROR, NULL);
        return -1;
    }

    if (hello.type != FCT_CONTROL_HELLO || !is_zero_token(hello.token)) {
        (void)send_result_frame(control, FCT_RESULT_PROTOCOL_ERROR, NULL);
        errno = EPROTO;
        return -1;
    }

    if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &deadline) < 0 ||
        fct_channel_recv_meta_until(control, out_meta, &deadline) < 0) {
        if (errno == EPROTO)
            (void)send_result_frame(control, FCT_RESULT_PROTOCOL_ERROR, NULL);
        return -1;
    }

    if (!validate_meta_and_workers(out_meta, hello.arg)) {
        (void)send_result_frame(control, FCT_RESULT_PROTOCOL_ERROR, NULL);
        errno = EPROTO;
        return -1;
    }

    *out_workers = hello.arg;
    return 0;
}

#define PART_NAME "data.part"
#define CHECKPOINT_NAME "data.bitmap"

typedef struct {
    int dir_fd;
    char name[5 + FCT_SHA256_SIZE * 2 + 1];
    char part_path[64];
    char checkpoint_path[64];
    file_writer_t writer;
    fct_bitmap_t completed;
} receive_session_t;

static int open_receive_session(receive_session_t* session, const fct_file_meta_t* meta,
                                int output_fd) {
    /* Keep different versions of the same filename in independent sessions. */
    char geometry[64];
    int len = snprintf(geometry, sizeof(geometry), "%llu/%u/",
                       (unsigned long long)meta->file_size, meta->chunk_size);
    fct_sha256_ctx_t hash;
    uint8_t digest[FCT_SHA256_SIZE];
    fct_sha256_init(&hash);
    fct_sha256_update(&hash, geometry, (size_t)len);
    fct_sha256_update(&hash, meta->file_sha256, sizeof(meta->file_sha256));
    fct_sha256_update(&hash, meta->filename, strlen(meta->filename));
    fct_sha256_final(&hash, digest);
    memcpy(session->name, ".fct-", 5);
    fct_sha256_to_hex(digest, session->name + 5);

    if (mkdirat(output_fd, session->name, 0700) < 0 && errno != EEXIST)
        return -1;
    session->dir_fd = openat(output_fd, session->name,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (session->dir_fd < 0 || flock(session->dir_fd, LOCK_EX | LOCK_NB) < 0)
        return -1;

    struct stat opened, named;
    if (fstat(session->dir_fd, &opened) < 0 ||
        fstatat(output_fd, session->name, &named, AT_SYMLINK_NOFOLLOW) < 0)
        return -1;
    if (opened.st_uid != geteuid() || (opened.st_mode & 077) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) {
        errno = EACCES;
        return -1;
    }

    /* Anchor the path-only writer and bitmap APIs to the opened directory. */
    snprintf(session->part_path, sizeof(session->part_path), "/proc/self/fd/%d/" PART_NAME,
             session->dir_fd);
    snprintf(session->checkpoint_path, sizeof(session->checkpoint_path),
             "/proc/self/fd/%d/" CHECKPOINT_NAME, session->dir_fd);

    if (fstatat(session->dir_fd, CHECKPOINT_NAME, &named, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(named.st_mode)) {
            errno = EINVAL;
            return -1;
        }
        if (fct_bitmap_load_file(&session->completed, meta, session->checkpoint_path) == 0) {
            if (fw_open_existing(&session->writer, session->part_path, meta->file_size,
                                 meta->chunk_size) == 0) {
                if (fstat(session->writer.fd, &opened) < 0)
                    return -1;
                if (opened.st_nlink == 1)
                    return 0;
                errno = EMLINK;
                return -1;
            }
            if (errno != ENOENT && errno != EIO)
                return -1;
            fct_bitmap_free(&session->completed);
        } else if (errno != EPROTO) {
            return -1;
        }

        /* Invalidate the old bitmap durably before replacing its data. */
        if (unlinkat(session->dir_fd, CHECKPOINT_NAME, 0) < 0 || fsync(session->dir_fd) < 0)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }

    if (fct_bitmap_init(&session->completed, meta->total_chunks) < 0)
        return -1;
    return fw_init(&session->writer, session->part_path, meta->file_size, meta->chunk_size);
}

static int checkpoint_session(receive_session_t* session, const fct_file_meta_t* meta,
                              int output_fd) {
    if (fsync(session->writer.fd) < 0 ||
        fct_bitmap_save_file(&session->completed, meta, session->checkpoint_path) < 0 ||
        fsync(session->dir_fd) < 0 || fsync(output_fd) < 0)
        return -1;
    return 0;
}

static int send_bitmap_frame(fct_channel_t* control, const fct_control_frame_t* frame,
                             const fct_bitmap_t* completed) {
    struct timespec deadline;
    if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &deadline) < 0 ||
        fct_channel_send_control_until(control, frame, &deadline) < 0)
        return -1;
    return fct_channel_send_bitmap_until(control, completed->bits, completed->byte_len,
                                         &deadline);
}

static int receive_rounds(fct_channel_t* control, fct_channel_t* channels, uint16_t worker_count,
                          fct_control_frame_t frame, const fct_file_meta_t* meta,
                          receive_session_t* session, int output_fd, fct_result_t* result) {
    for (unsigned round = 0;; ++round) {
        uint32_t before = fct_bitmap_completed(&session->completed);
        int rc = fct_receive_parallel(channels, worker_count, &session->writer, meta,
                                      &session->completed, NULL, NULL, result);
        int saved = errno;

        /* Workers have joined, so the bitmap only describes complete writes. */
        if (before != fct_bitmap_completed(&session->completed) &&
            checkpoint_session(session, meta, output_fd) < 0) {
            *result = FCT_RESULT_STORAGE_ERROR;
            return -1;
        }
        if (rc <= 0) {
            errno = saved;
            return rc;
        }
        if (round == FCT_MAX_CHUNK_RETRIES) {
            *result = FCT_RESULT_CHECKSUM_ERROR;
            errno = EBADMSG;
            return -1;
        }
        frame.type = FCT_CONTROL_RETRY;
        frame.arg = (uint16_t)(round + 1);
        if (send_bitmap_frame(control, &frame, &session->completed) < 0) {
            *result = FCT_RESULT_TRANSFER_ERROR;
            return -1;
        }
    }
}

static int receive_payload_and_publish(fct_channel_t* control, int* listener,
                                       const fct_file_meta_t* meta, uint16_t worker_count,
                                       receive_session_t* session, int output_fd,
                                       bool* discard_session) {
    uint32_t missing = fct_bitmap_missing(&session->completed);
    if (worker_count > missing)
        worker_count = (uint16_t)missing;
    fct_control_frame_t ready = {.magic = FCT_MAGIC, .type = FCT_CONTROL_READY, .arg = worker_count};
    if (generate_token(ready.token) < 0) {
        int saved = errno;
        (void)send_result_frame(control, FCT_RESULT_TRANSFER_ERROR, NULL);
        errno = saved;
        return -1;
    }
    if (send_bitmap_frame(control, &ready, &session->completed) < 0)
        return -1;

    fct_channel_t data_channels[FCT_MAX_DATA_WORKERS];
    init_channels(data_channels, FCT_MAX_DATA_WORKERS);
    fct_result_t code = FCT_RESULT_TRANSFER_ERROR;
    int rc = 0;
    if (worker_count >= 2)
        rc = accept_data_channels(*listener, worker_count, ready.token, data_channels, &code);
    close(*listener);
    *listener = -1;

    if (rc == 0) {
        fct_channel_t* channels = worker_count < 2 ? control : data_channels;
        rc = receive_rounds(control, channels, worker_count, ready, meta, session, output_fd, &code);
    }

    int saved = errno;
    close_channels(data_channels, FCT_MAX_DATA_WORKERS);
    errno = saved;
    if (rc == 0) {
        uint8_t digest[FCT_SHA256_SIZE];
        code = FCT_RESULT_STORAGE_ERROR;
        rc = -1;
        if (fct_sha256_fd(session->writer.fd, meta->file_size, digest) == 0) {
            if (memcmp(digest, meta->file_sha256, sizeof(digest)) != 0) {
                *discard_session = true;
                code = FCT_RESULT_CHECKSUM_ERROR;
                errno = EBADMSG;
            } else if (fsync(session->writer.fd) == 0 &&
                       linkat(session->dir_fd, PART_NAME, output_fd, meta->filename, 0) == 0) {
                *discard_session = true;
                if (fsync(output_fd) == 0) {
                    code = FCT_RESULT_OK;
                    rc = 0;
                }
            }
        }
    }

    saved = errno;
    (void)send_result_frame(control, code, ready.token);
    errno = saved;
    return rc;
}

static int receive_file(fct_channel_t* control, int* listener, int output_fd) {
    fct_file_meta_t meta;
    uint16_t worker_count = 0;
    if (receive_handshake(control, &meta, &worker_count) < 0)
        return -1;

    receive_session_t session = {.dir_fd = -1, .writer = {.fd = -1}};
    bool discard_session = false;
    struct stat destination;
    int result = -1;
    if (fstatat(output_fd, meta.filename, &destination, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
    } else if (errno == ENOENT && open_receive_session(&session, &meta, output_fd) == 0) {
        result = receive_payload_and_publish(control, listener, &meta, worker_count, &session,
                                             output_fd, &discard_session);
        discard_session |= fct_bitmap_completed(&session.completed) == 0;
        goto done;
    }

    int setup_errno = errno;
    (void)send_result_frame(control, FCT_RESULT_STORAGE_ERROR, NULL);
    errno = setup_errno;

done:;
    int saved = errno;
    fw_close(&session.writer);
    fct_bitmap_free(&session.completed);
    if (discard_session) {
        (void)unlinkat(session.dir_fd, CHECKPOINT_NAME, 0);
        (void)unlinkat(session.dir_fd, PART_NAME, 0);
        (void)unlinkat(output_fd, session.name, AT_REMOVEDIR);
        (void)fsync(output_fd);
    }
    if (session.dir_fd >= 0)
        close(session.dir_fd);
    errno = saved;
    return result;
}

static int run_server(const fct_options_t* options) {
    int output_fd = open(options->output_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (output_fd < 0)
        return -1;

    int listener = fct_server_listen(options->port);
    if (listener < 0) {
        int saved = errno;
        close(output_fd);
        errno = saved;
        return -1;
    }
    fprintf(stderr, "Listening on port %d\n", options->port);
    fflush(stderr);

    fct_channel_t channel = {.sockfd = -1};
    int result = fct_channel_accept(&channel, listener);
    int saved = errno;
    if (result == 0) {
        result = receive_file(&channel, &listener, output_fd);
        saved = errno;
    }

    if (listener >= 0)
        close(listener);
    fct_channel_close(&channel);
    close(output_fd);
    errno = saved;
    return result;
}

int main(int argc, char** argv) {
    fct_options_t options;
    int result = fct_parse_options(argc, argv, &options, stderr);

    if (result != 0) {
        fct_print_usage(result > 0 ? stdout : stderr, argv[0]);
        return result > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (options.mode == FCT_MODE_SERVER) {
        if (run_server(&options) == 0)
            return EXIT_SUCCESS;
        fprintf(stderr, "Receive into '%s': %s\n", options.output_dir, strerror(errno));
    } else {
        if (send_file(&options) == 0)
            return EXIT_SUCCESS;
        fprintf(stderr, "Send '%s' to %s:%d: %s\n", options.file_path, options.server_ip,
                options.port, strerror(errno));
    }
    return EXIT_FAILURE;
}
