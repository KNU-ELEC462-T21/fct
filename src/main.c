#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    if (expected > UINT32_MAX || meta->total_chunks != (uint32_t)expected)
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
                          const fct_file_meta_t* meta, uint16_t worker_count,
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
        .arg = worker_count,
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

    if (resp.type != FCT_CONTROL_READY || resp.arg != worker_count || is_zero_token(resp.token)) {
        errno = EPROTO;
        return -1;
    }

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

static int send_session(const fct_options_t* options, file_reader_t* reader,
                        const fct_file_meta_t* meta, fct_compression_mode_t mode) {
    uint16_t worker_count = compute_worker_count(options->worker_count, meta->total_chunks);
    fct_channel_t control = {.sockfd = -1};
    uint8_t token[FCT_TRANSFER_TOKEN_SIZE] = {0};

    if (send_handshake(&control, options, meta, worker_count, token) < 0) {
        int saved = errno;
        fct_channel_close(&control);
        errno = saved;
        return -1;
    }

    fct_progress_state_t progress;
    if (fct_progress_init(&progress, meta->file_size, stderr) < 0) {
        int saved = errno;
        fct_channel_close(&control);
        errno = saved;
        return -1;
    }
    fct_progress_t cb = {.on_chunk = fct_progress_on_chunk, .context = &progress};

    int send_rc = -1;
    if (worker_count == 0) {
        send_rc = fct_send_parallel(NULL, 0, reader, meta, mode, NULL, false, &cb);
    } else if (worker_count == 1) {
        send_rc = fct_send_parallel(&control, 1, reader, meta, mode, NULL, false, &cb);
    } else {
        fct_channel_t data_channels[FCT_MAX_DATA_WORKERS];
        if (open_data_channels(options, worker_count, token, data_channels) == 0) {
            send_rc = fct_send_parallel(data_channels, worker_count, reader, meta, mode, NULL,
                                        false, &cb);
            int saved = errno;
            close_channels(data_channels, worker_count);
            errno = saved;
        }
    }

    int result = -1;
    int saved = errno;
    uint16_t code = FCT_RESULT_TRANSFER_ERROR;

    if (send_rc == 0) {
        if (wait_sender_result(&control, token, FCT_RESULT_TIMEOUT_MS, &code) < 0) {
            saved = errno;
        } else if (code == FCT_RESULT_OK) {
            fct_progress_finish(&progress);
            result = 0;
            saved = 0;
        } else {
            saved = result_to_errno(code);
        }
    } else if (saved != ETIMEDOUT) {
        if (wait_sender_result(&control, token, FCT_FAILURE_RESULT_WAIT_MS, &code) == 0 &&
            code != FCT_RESULT_OK) {
            saved = result_to_errno(code);
        }
    }

    if (result != 0)
        fct_progress_abort(&progress);
    fct_progress_destroy(&progress);
    fct_channel_close(&control);
    errno = saved;
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
    if (determine_compression_mode(&reader, &meta, options->force_compression, &mode) == 0)
        result = send_session(options, &reader, &meta, mode);

    int saved = errno;
    fr_close(&reader);
    errno = saved;
    return result;
}

static char* join_path(const char* directory, const char* name) {
    size_t directory_length = strlen(directory);
    size_t name_size = strlen(name) + 1;
    char* path = malloc(directory_length + 1 + name_size);
    if (!path)
        return NULL;
    memcpy(path, directory, directory_length);
    path[directory_length] = '/';
    memcpy(path + directory_length + 1, name, name_size);
    return path;
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

static int receive_payload_and_publish(fct_channel_t* control, int* listener,
                                       const fct_file_meta_t* meta, uint16_t worker_count,
                                       file_writer_t* writer, const char* temporary_file,
                                       int output_fd) {
    uint8_t session_token[FCT_TRANSFER_TOKEN_SIZE] = {0};
    if (generate_token(session_token) < 0) {
        int saved = errno;
        (void)send_result_frame(control, FCT_RESULT_TRANSFER_ERROR, NULL);
        errno = saved;
        return -1;
    }

    struct timespec deadline;
    fct_control_frame_t ready = {
        .magic = FCT_MAGIC,
        .type = FCT_CONTROL_READY,
        .arg = worker_count,
    };
    memcpy(ready.token, session_token, FCT_TRANSFER_TOKEN_SIZE);
    if (make_deadline(FCT_CONTROL_IO_TIMEOUT_MS, &deadline) < 0 ||
        fct_channel_send_control_until(control, &ready, &deadline) < 0) {
        return -1;
    }

    fct_channel_t data_channels[FCT_MAX_DATA_WORKERS];
    if (worker_count >= 2) {
        fct_result_t accept_code = FCT_RESULT_TRANSFER_ERROR;
        int accept_rc = accept_data_channels(*listener, worker_count, session_token, data_channels,
                                             &accept_code);
        int accept_errno = errno;
        if (*listener >= 0) {
            close(*listener);
            *listener = -1;
        }
        if (accept_rc < 0) {
            (void)send_result_frame(control, accept_code, session_token);
            errno = accept_errno;
            return -1;
        }
    }

    fct_result_t recv_result = FCT_RESULT_PROTOCOL_ERROR;
    int recv_rc = -1;
    if (worker_count == 0) {
        recv_rc = fct_receive_parallel(NULL, 0, writer, meta, NULL, NULL, NULL, &recv_result);
    } else if (worker_count == 1) {
        recv_rc = fct_receive_parallel(control, 1, writer, meta, NULL, NULL, NULL, &recv_result);
    } else {
        recv_rc = fct_receive_parallel(data_channels, worker_count, writer, meta, NULL, NULL, NULL,
                                       &recv_result);
        int saved = errno;
        close_channels(data_channels, worker_count);
        errno = saved;
    }

    if (recv_rc < 0) {
        int saved = errno;
        (void)send_result_frame(control, recv_result, session_token);
        errno = saved;
        return -1;
    }

    if (fsync(writer->fd) == 0 &&
        linkat(AT_FDCWD, temporary_file, output_fd, meta->filename, 0) == 0 &&
        fsync(output_fd) == 0) {
        (void)send_result_frame(control, FCT_RESULT_OK, session_token);
        return 0;
    }

    int saved = errno;
    (void)send_result_frame(control, FCT_RESULT_STORAGE_ERROR, session_token);
    errno = saved;
    return -1;
}

static int receive_file(fct_channel_t* control, int* listener, const char* directory,
                        int output_fd) {
    fct_file_meta_t meta;
    uint16_t worker_count = 0;
    if (receive_handshake(control, &meta, &worker_count) < 0)
        return -1;

    if (worker_count <= 1 && *listener >= 0) {
        close(*listener);
        *listener = -1;
    }

    file_writer_t writer = {.fd = -1};
    char* temporary_dir = join_path(directory, ".fct-XXXXXX");
    char* temporary_file = NULL;
    char* created_dir = temporary_dir ? mkdtemp(temporary_dir) : NULL;
    int result = -1;
    int saved = 0;

    if (created_dir) {
        temporary_file = join_path(temporary_dir, "data");
        if (temporary_file &&
            fw_init(&writer, temporary_file, meta.file_size, meta.chunk_size) == 0) {
            result = receive_payload_and_publish(control, listener, &meta, worker_count, &writer,
                                                 temporary_file, output_fd);
        } else {
            saved = errno;
            (void)send_result_frame(control, FCT_RESULT_STORAGE_ERROR, NULL);
            errno = saved;
        }
    } else {
        saved = errno;
        (void)send_result_frame(control, FCT_RESULT_STORAGE_ERROR, NULL);
        errno = saved;
    }

    saved = errno;
    fw_close(&writer);
    if (temporary_file)
        unlink(temporary_file);
    if (created_dir)
        rmdir(created_dir);
    free(temporary_file);
    free(temporary_dir);
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
        result = receive_file(&channel, &listener, options->output_dir, output_fd);
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
