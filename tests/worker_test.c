#define _POSIX_C_SOURCE 200809L

#include "compress_stub.h"
#include "file_reader.h"
#include "file_writer.h"
#include "network.h"
#include "worker.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static struct timespec deadline_after_ms(unsigned ms) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    ts.tv_sec += (time_t)(ms / 1000U);
    ts.tv_nsec += (long)((ms % 1000U) * 1000000UL);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec += 1;
    }
    return ts;
}

static void make_tcp_pairs(size_t count, fct_channel_t* clients, fct_channel_t* servers) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t len = sizeof(addr);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int enabled = 1;
    assert(listener >= 0);
    assert(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0);
    assert(bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 16) == 0);
    assert(getsockname(listener, (struct sockaddr*)&addr, &len) == 0);
    int port = ntohs(addr.sin_port);

    for (size_t i = 0; i < count; ++i)
        assert(fct_channel_connect(&clients[i], "127.0.0.1", port) == 0);
    for (size_t i = 0; i < count; ++i)
        assert(fct_channel_accept(&servers[i], listener) == 0);

    close(listener);
}

static void close_channels(size_t count, fct_channel_t* channels) {
    for (size_t i = 0; i < count; ++i)
        fct_channel_close(&channels[i]);
}

static void write_mixed_pattern_file(const char* path, uint64_t total_size, uint32_t chunk_size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);

    unsigned char* buf = malloc(chunk_size > 0 ? chunk_size : 1);
    assert(buf != NULL);

    uint64_t offset = 0;
    uint32_t chunk_id = 0;
    while (offset < total_size) {
        size_t len =
            (total_size - offset) < chunk_size ? (size_t)(total_size - offset) : chunk_size;
        if (chunk_id % 2 == 0) {
            /* Uniform bytes -> AUTO compresses to 7 bytes via compress_stub. */
            memset(buf, (int)(0x40 + (chunk_id % 19)), len);
        } else {
            /* Non-uniform bytes -> AUTO sends RAW, FORCE expands to len + 2. */
            for (size_t i = 0; i < len; ++i)
                buf[i] = (unsigned char)(((chunk_id * 37U) + i * 13U + (i >> 8)) & 0xFFU);
        }
        size_t written = 0;
        while (written < len) {
            ssize_t n = write(fd, buf + written, len - written);
            assert(n > 0);
            written += (size_t)n;
        }
        offset += len;
        chunk_id++;
    }

    free(buf);
    close(fd);
}

static void assert_files_equal(const char* a_path, const char* b_path, uint64_t expected_size) {
    FILE* fa = fopen(a_path, "rb");
    FILE* fb = fopen(b_path, "rb");
    assert(fa != NULL && fb != NULL);

    unsigned char* ba = malloc(65536);
    unsigned char* bb = malloc(65536);
    assert(ba != NULL && bb != NULL);

    uint64_t total = 0;
    for (;;) {
        size_t na = fread(ba, 1, 65536, fa);
        size_t nb = fread(bb, 1, 65536, fb);
        assert(na == nb);
        if (na == 0)
            break;
        assert(memcmp(ba, bb, na) == 0);
        total += na;
    }
    assert(total == expected_size);
    assert(feof(fa) && feof(fb));

    free(ba);
    free(bb);
    fclose(fa);
    fclose(fb);
}

typedef struct {
    pthread_mutex_t mu;
    size_t calls;
    uint64_t total_raw;
    uint64_t total_payload;
} test_progress_t;

static void progress_init(test_progress_t* p) {
    assert(pthread_mutex_init(&p->mu, NULL) == 0);
    p->calls = 0;
    p->total_raw = 0;
    p->total_payload = 0;
}

static void progress_destroy(test_progress_t* p) {
    pthread_mutex_destroy(&p->mu);
}

static void progress_cb(void* ctx, size_t raw_len, size_t payload_len) {
    test_progress_t* p = ctx;
    pthread_mutex_lock(&p->mu);
    p->calls++;
    p->total_raw += raw_len;
    p->total_payload += payload_len;
    pthread_mutex_unlock(&p->mu);
}

typedef struct {
    fct_channel_t* channels;
    size_t channel_count;
    file_reader_t* reader;
    const fct_file_meta_t* meta;
    fct_compression_mode_t mode;
    const fct_progress_t* progress;
    int rc;
    int err;
} send_thread_args_t;

static void* send_thread_entry(void* arg) {
    send_thread_args_t* a = arg;
    errno = 0;
    a->rc =
        fct_send_parallel(a->channels, a->channel_count, a->reader, a->meta, a->mode, a->progress);
    a->err = errno;
    return NULL;
}

static fct_file_meta_t make_meta(uint64_t file_size, uint32_t chunk_size, const char* name) {
    uint64_t chunks = file_size / chunk_size + (file_size % chunk_size != 0);
    fct_file_meta_t meta = {
        .magic = FCT_MAGIC,
        .file_size = file_size,
        .chunk_size = chunk_size,
        .total_chunks = (uint32_t)chunks,
    };
    snprintf(meta.filename, sizeof(meta.filename), "%s", name);
    return meta;
}

static void run_parallel_roundtrip(const char* dir, uint64_t file_size, uint32_t chunk_size,
                                   size_t n_workers, fct_compression_mode_t mode) {
    char src_path[256];
    char dst_path[256];
    snprintf(src_path, sizeof(src_path), "%s/src.bin", dir);
    snprintf(dst_path, sizeof(dst_path), "%s/dst.bin", dir);

    write_mixed_pattern_file(src_path, file_size, chunk_size);

    file_reader_t reader;
    file_writer_t writer;
    assert(fr_init(&reader, src_path, chunk_size) == 0);
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    fct_file_meta_t meta = make_meta(file_size, chunk_size, "src.bin");

    fct_channel_t clients[FCT_MAX_DATA_WORKERS];
    fct_channel_t servers[FCT_MAX_DATA_WORKERS];
    make_tcp_pairs(n_workers, clients, servers);

    test_progress_t send_prog_ctx;
    test_progress_t recv_prog_ctx;
    progress_init(&send_prog_ctx);
    progress_init(&recv_prog_ctx);
    fct_progress_t send_prog = {.on_chunk = progress_cb, .context = &send_prog_ctx};
    fct_progress_t recv_prog = {.on_chunk = progress_cb, .context = &recv_prog_ctx};

    send_thread_args_t sargs = {
        .channels = clients,
        .channel_count = n_workers,
        .reader = &reader,
        .meta = &meta,
        .mode = mode,
        .progress = &send_prog,
    };
    pthread_t sthread;
    assert(pthread_create(&sthread, NULL, send_thread_entry, &sargs) == 0);

    fct_result_t result = FCT_RESULT_TRANSFER_ERROR;
    int rrc = fct_receive_parallel(servers, n_workers, &writer, &meta, &recv_prog, &result);
    assert(pthread_join(sthread, NULL) == 0);

    assert(sargs.rc == 0);
    assert(rrc == 0);
    assert(result == FCT_RESULT_OK);

    assert(send_prog_ctx.calls == meta.total_chunks);
    assert(recv_prog_ctx.calls == meta.total_chunks);
    assert(send_prog_ctx.total_raw == file_size);
    assert(recv_prog_ctx.total_raw == file_size);
    assert(send_prog_ctx.total_payload == recv_prog_ctx.total_payload);

    if (mode == FCT_COMPRESS_RAW) {
        assert(recv_prog_ctx.total_payload == file_size);
    } else if (mode == FCT_COMPRESS_AUTO && meta.total_chunks >= 1) {
        /* Chunk 0 is uniform and shrinks to 7 bytes (< chunk_size >= 8 or similar). */
        if (chunk_size >= 8)
            assert(recv_prog_ctx.total_payload < file_size);
    } else if (mode == FCT_COMPRESS_FORCE && meta.total_chunks >= 2) {
        assert(recv_prog_ctx.total_payload != file_size);
    }

    if (n_workers == 1) {
        /* Verify RESULT frame can still be exchanged on the single shared channel. */
        struct timespec dl = deadline_after_ms(2000);
        fct_control_frame_t out_res = {
            .magic = FCT_MAGIC,
            .type = FCT_CONTROL_RESULT,
            .arg = FCT_RESULT_OK,
            .token = {1, 2, 3},
        };
        fct_control_frame_t in_res = {0};
        assert(fct_channel_send_control_until(&servers[0], &out_res, &dl) == 0);
        assert(fct_channel_recv_control_until(&clients[0], &in_res, &dl) == 0);
        assert(in_res.type == FCT_CONTROL_RESULT && in_res.arg == FCT_RESULT_OK &&
               in_res.token[0] == 1 && in_res.token[2] == 3);
    }

    progress_destroy(&send_prog_ctx);
    progress_destroy(&recv_prog_ctx);
    close_channels(n_workers, clients);
    close_channels(n_workers, servers);
    fr_close(&reader);
    fw_close(&writer);

    assert_files_equal(src_path, dst_path, file_size);
    unlink(src_path);
    unlink(dst_path);
}

static void test_empty_file_n0(const char* dir) {
    char src_path[256];
    char dst_path[256];
    snprintf(src_path, sizeof(src_path), "%s/empty_src.bin", dir);
    snprintf(dst_path, sizeof(dst_path), "%s/empty_dst.bin", dir);

    write_mixed_pattern_file(src_path, 0, DEFAULT_CHUNK_SZ);

    file_reader_t reader;
    file_writer_t writer;
    assert(fr_init(&reader, src_path, DEFAULT_CHUNK_SZ) == 0);
    assert(fw_init(&writer, dst_path, 0, DEFAULT_CHUNK_SZ) == 0);

    fct_file_meta_t meta = make_meta(0, DEFAULT_CHUNK_SZ, "empty.bin");
    fct_result_t result = FCT_RESULT_PROTOCOL_ERROR;

    assert(fct_send_parallel(NULL, 0, &reader, &meta, FCT_COMPRESS_RAW, NULL) == 0);
    assert(fct_receive_parallel(NULL, 0, &writer, &meta, NULL, &result) == 0);
    assert(result == FCT_RESULT_OK);

    /* Non-zero channel_count must be rejected for empty files. */
    fct_channel_t dummy = {.sockfd = 0};
    errno = 0;
    assert(fct_send_parallel(&dummy, 1, &reader, &meta, FCT_COMPRESS_RAW, NULL) == -1 &&
           errno == EINVAL);
    errno = 0;
    assert(fct_receive_parallel(&dummy, 1, &writer, &meta, NULL, &result) == -1 &&
           errno == EINVAL && result == FCT_RESULT_PROTOCOL_ERROR);

    fr_close(&reader);
    fw_close(&writer);
    assert_files_equal(src_path, dst_path, 0);
    unlink(src_path);
    unlink(dst_path);
}

static void test_single_and_multi_channel_transfers(const char* dir) {
    /* N = 1 across RAW, AUTO, FORCE with short last chunk + shared RESULT exchange */
    run_parallel_roundtrip(dir, 5U * 4096U + 123U, 4096U, 1, FCT_COMPRESS_RAW);
    run_parallel_roundtrip(dir, 5U * 4096U + 123U, 4096U, 1, FCT_COMPRESS_AUTO);
    run_parallel_roundtrip(dir, 5U * 4096U + 123U, 4096U, 1, FCT_COMPRESS_FORCE);

    /* Exact single-chunk file with N = 1 */
    run_parallel_roundtrip(dir, 4096U, 4096U, 1, FCT_COMPRESS_AUTO);

    /* Multi-chunk with short last chunk and N = 4 */
    run_parallel_roundtrip(dir, 10U * 4096U + 777U, 4096U, 4, FCT_COMPRESS_RAW);
    run_parallel_roundtrip(dir, 10U * 4096U + 777U, 4096U, 4, FCT_COMPRESS_AUTO);
    run_parallel_roundtrip(dir, 10U * 4096U + 777U, 4096U, 4, FCT_COMPRESS_FORCE);

    /* Full 2 MiB DEFAULT_CHUNK_SZ multi-chunk transfer */
    run_parallel_roundtrip(dir, 2ULL * DEFAULT_CHUNK_SZ + 12345ULL, DEFAULT_CHUNK_SZ, 3,
                           FCT_COMPRESS_AUTO);
    run_parallel_roundtrip(dir, 2ULL * DEFAULT_CHUNK_SZ + 12345ULL, DEFAULT_CHUNK_SZ, 3,
                           FCT_COMPRESS_FORCE);
}

static void test_out_of_order_assembly(const char* dir) {
    char src_path[256];
    char dst_path[256];
    snprintf(src_path, sizeof(src_path), "%s/ooo_src.bin", dir);
    snprintf(dst_path, sizeof(dst_path), "%s/ooo_dst.bin", dir);

    const uint32_t chunk_size = 4096U;
    const uint64_t file_size = 7U * chunk_size + 500U;
    write_mixed_pattern_file(src_path, file_size, chunk_size);

    file_reader_t reader;
    file_writer_t writer;
    assert(fr_init(&reader, src_path, chunk_size) == 0);
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    fct_file_meta_t meta = make_meta(file_size, chunk_size, "ooo.bin");

    fct_channel_t clients[4];
    fct_channel_t servers[4];
    make_tcp_pairs(4, clients, servers);

    /* Send chunks intentionally out of order across 4 connections. */
    const uint32_t order[8] = {7, 2, 5, 0, 6, 1, 4, 3};
    unsigned char raw[4096];
    unsigned char scratch[4096 + 16];
    for (size_t i = 0; i < 8; ++i) {
        uint32_t id = order[i];
        ssize_t raw_len = fr_read_chunk(&reader, id, raw);
        assert(raw_len > 0);
        const void* payload = NULL;
        size_t payload_len = 0;
        uint32_t flags = 0;
        assert(fct_encode_chunk(FCT_COMPRESS_AUTO, raw, (size_t)raw_len, scratch, sizeof(scratch),
                                &payload, &payload_len, &flags) == 0);
        fct_chunk_header_t hdr = {
            .magic = FCT_MAGIC,
            .chunk_id = id,
            .offset = (uint64_t)id * chunk_size,
            .payload_len = (uint32_t)payload_len,
            .flags = flags,
        };
        assert(fct_channel_send_chunk_idle(&clients[i % 4], &hdr, payload, 2000) == 0);
    }
    for (size_t i = 0; i < 4; ++i)
        assert(shutdown(clients[i].sockfd, SHUT_WR) == 0);

    fct_result_t result = FCT_RESULT_TRANSFER_ERROR;
    assert(fct_receive_parallel(servers, 4, &writer, &meta, NULL, &result) == 0);
    assert(result == FCT_RESULT_OK);

    close_channels(4, clients);
    close_channels(4, servers);
    fr_close(&reader);
    fw_close(&writer);
    assert_files_equal(src_path, dst_path, file_size);
    unlink(src_path);
    unlink(dst_path);
}

static void send_raw_chunk_manual(fct_channel_t* ch, uint32_t id, uint64_t offset, uint32_t len,
                                  uint8_t fill) {
    unsigned char* buf = malloc(len > 0 ? len : 1);
    assert(buf != NULL);
    memset(buf, fill, len);
    fct_chunk_header_t hdr = {
        .magic = FCT_MAGIC,
        .chunk_id = id,
        .offset = offset,
        .payload_len = len,
        .flags = 0,
    };
    assert(fct_channel_send_chunk_idle(ch, &hdr, len > 0 ? buf : NULL, 2000) == 0);
    free(buf);
}

static void test_duplicate_and_missing_chunks(const char* dir) {
    char dst_path[256];
    snprintf(dst_path, sizeof(dst_path), "%s/dup_miss.bin", dir);

    const uint32_t chunk_size = 1024U;
    const uint64_t file_size = 4U * chunk_size;
    fct_file_meta_t meta = make_meta(file_size, chunk_size, "dup.bin");

    /* 1. Duplicate chunk_id across two channels. */
    file_writer_t writer;
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    fct_channel_t clients[2], servers[2];
    make_tcp_pairs(2, clients, servers);

    send_raw_chunk_manual(&clients[0], 0, 0, chunk_size, 'A');
    send_raw_chunk_manual(&clients[0], 1, chunk_size, chunk_size, 'B');
    send_raw_chunk_manual(&clients[1], 1, chunk_size, chunk_size, 'B');
    send_raw_chunk_manual(&clients[1], 2, 2U * chunk_size, chunk_size, 'C');
    send_raw_chunk_manual(&clients[1], 3, 3U * chunk_size, chunk_size, 'D');
    shutdown(clients[0].sockfd, SHUT_WR);
    shutdown(clients[1].sockfd, SHUT_WR);

    fct_result_t result = FCT_RESULT_OK;
    errno = 0;
    assert(fct_receive_parallel(servers, 2, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR);
    assert(errno == EPROTO);

    close_channels(2, clients);
    close_channels(2, servers);
    fw_close(&writer);
    unlink(dst_path);

    /* 2. Missing chunk_id despite clean EOF on all channels. */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(2, clients, servers);

    send_raw_chunk_manual(&clients[0], 0, 0, chunk_size, 'A');
    send_raw_chunk_manual(&clients[0], 1, chunk_size, chunk_size, 'B');
    send_raw_chunk_manual(&clients[1], 3, 3U * chunk_size, chunk_size, 'D');
    shutdown(clients[0].sockfd, SHUT_WR);
    shutdown(clients[1].sockfd, SHUT_WR);

    result = FCT_RESULT_OK;
    errno = 0;
    assert(fct_receive_parallel(servers, 2, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR);
    assert(errno == EPROTO);

    close_channels(2, clients);
    close_channels(2, servers);
    fw_close(&writer);
    unlink(dst_path);
}

static void test_protocol_errors_and_decode_failures(const char* dir) {
    char dst_path[256];
    snprintf(dst_path, sizeof(dst_path), "%s/proto.bin", dir);

    const uint32_t chunk_size = 1024U;
    const uint64_t file_size = 2U * chunk_size + 100U; /* total_chunks = 3 */
    fct_file_meta_t meta = make_meta(file_size, chunk_size, "proto.bin");
    file_writer_t writer;
    fct_channel_t client, server;
    fct_result_t result;

    /* Case 1: chunk_id out of range */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(1, &client, &server);
    send_raw_chunk_manual(&client, 3, 3U * chunk_size, 100U, 'X');
    shutdown(client.sockfd, SHUT_WR);
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR && errno == EPROTO);
    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(dst_path);

    /* Case 2: wrong offset */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(1, &client, &server);
    send_raw_chunk_manual(&client, 1, 0, chunk_size, 'X');
    shutdown(client.sockfd, SHUT_WR);
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR && errno == EPROTO);
    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(dst_path);

    /* Case 3: wrong raw length on last short chunk */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(1, &client, &server);
    send_raw_chunk_manual(&client, 2, 2U * chunk_size, 200U, 'X');
    shutdown(client.sockfd, SHUT_WR);
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR && errno == EPROTO);
    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(dst_path);

    /* Case 4: oversized compressed payload_len (> fct_compress_bound(raw_len)) */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(1, &client, &server);
    size_t bad_len = fct_compress_bound(100U) + 1U;
    unsigned char* bad_buf = calloc(1, bad_len);
    assert(bad_buf != NULL);
    fct_chunk_header_t hdr = {
        .magic = FCT_MAGIC,
        .chunk_id = 2,
        .offset = 2U * chunk_size,
        .payload_len = (uint32_t)bad_len,
        .flags = FCT_CHUNK_FLAG_COMPRESSED,
    };
    assert(fct_channel_send_chunk_idle(&client, &hdr, bad_buf, 2000) == 0);
    free(bad_buf);
    shutdown(client.sockfd, SHUT_WR);
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR && errno == EPROTO);
    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(dst_path);

    /* Case 5: corrupt compressed payload -> decode failure -> PROTOCOL_ERROR / EBADMSG */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(1, &client, &server);
    const unsigned char corrupt[7] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 'A'};
    hdr = (fct_chunk_header_t){
        .magic = FCT_MAGIC,
        .chunk_id = 2,
        .offset = 2U * chunk_size,
        .payload_len = sizeof(corrupt),
        .flags = FCT_CHUNK_FLAG_COMPRESSED,
    };
    assert(fct_channel_send_chunk_idle(&client, &hdr, corrupt, 2000) == 0);
    shutdown(client.sockfd, SHUT_WR);
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR && errno == EBADMSG);
    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(dst_path);

    /* Case 6: ENOMEM during decode -> FCT_RESULT_TRANSFER_ERROR / ENOMEM */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(1, &client, &server);
    unsigned char raw_small[100];
    unsigned char scratch_small[116];
    memset(raw_small, 'Z', sizeof(raw_small));
    const void* enc_payload = NULL;
    size_t enc_len = 0;
    uint32_t enc_flags = 0;
    assert(fct_encode_chunk(FCT_COMPRESS_AUTO, raw_small, sizeof(raw_small), scratch_small,
                            sizeof(scratch_small), &enc_payload, &enc_len, &enc_flags) == 0);
    assert(enc_flags == FCT_CHUNK_FLAG_COMPRESSED);
    hdr = (fct_chunk_header_t){
        .magic = FCT_MAGIC,
        .chunk_id = 2,
        .offset = 2U * chunk_size,
        .payload_len = (uint32_t)enc_len,
        .flags = enc_flags,
    };
    assert(fct_channel_send_chunk_idle(&client, &hdr, enc_payload, 2000) == 0);
    shutdown(client.sockfd, SHUT_WR);
    compress_stub_set_decode_errno(ENOMEM);
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    compress_stub_reset();
    assert(result == FCT_RESULT_TRANSFER_ERROR && errno == ENOMEM);
    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(dst_path);

    /* Case 7: truncated chunk frame mid-payload -> PROTOCOL_ERROR / EPROTO */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(1, &client, &server);
    unsigned char partial_wire[24 + 5] = {
        0x46, 0x43, 0x54, 0x31, 0,   0, 0, 2, 0, 0,   0,   0,   0,   0,   0x08,
        0x00, 0,    0,    0,    100, 0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e',
    };
    assert(send_exact(client.sockfd, partial_wire, sizeof(partial_wire)) == 0);
    shutdown(client.sockfd, SHUT_WR);
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR && errno == EPROTO);
    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(dst_path);
}

static void test_storage_error(const char* dir) {
    char ro_path[256];
    snprintf(ro_path, sizeof(ro_path), "%s/readonly.bin", dir);
    write_mixed_pattern_file(ro_path, 1024U, 1024U);

    int ro_fd = open(ro_path, O_RDONLY);
    assert(ro_fd >= 0);

    fct_file_meta_t meta = make_meta(1024U, 1024U, "readonly.bin");
    file_writer_t writer = {
        .fd = ro_fd,
        .file_size = 1024U,
        .chunk_size = 1024U,
        .total_chunks = 1,
    };
    fct_channel_t client, server;
    make_tcp_pairs(1, &client, &server);
    send_raw_chunk_manual(&client, 0, 0, 1024U, 'S');
    shutdown(client.sockfd, SHUT_WR);

    fct_result_t result = FCT_RESULT_OK;
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_STORAGE_ERROR && errno == EBADF);

    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(ro_path);
}

static void test_multi_worker_wakeup_and_transfer_error(const char* dir) {
    char dst_path[256];
    char src_path[256];
    snprintf(dst_path, sizeof(dst_path), "%s/wake_dst.bin", dir);
    snprintf(src_path, sizeof(src_path), "%s/wake_src.bin", dir);

    const uint32_t chunk_size = 4096U;
    const uint64_t file_size = 8U * chunk_size;
    fct_file_meta_t meta = make_meta(file_size, chunk_size, "wake.bin");

    /* 1. Receiver wakeup on protocol error while 3 sibling channels stay open and idle. */
    file_writer_t writer;
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    fct_channel_t clients[4], servers[4];
    make_tcp_pairs(4, clients, servers);

    /* Send bad chunk on channel 0, keep channels 1..3 completely idle & open. */
    send_raw_chunk_manual(&clients[0], 99, 0, chunk_size, 'X');

    uint64_t start_ms = monotonic_ms();
    fct_result_t result = FCT_RESULT_OK;
    errno = 0;
    assert(fct_receive_parallel(servers, 4, &writer, &meta, NULL, &result) == -1);
    uint64_t elapsed_ms = monotonic_ms() - start_ms;
    assert(result == FCT_RESULT_PROTOCOL_ERROR && errno == EPROTO);
    assert(elapsed_ms < 2000ULL);

    close_channels(4, clients);
    close_channels(4, servers);
    fw_close(&writer);
    unlink(dst_path);

    /* 2. Receiver wakeup on TCP RST (transfer error) while 3 sibling channels stay open & idle. */
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);
    make_tcp_pairs(4, clients, servers);

    struct linger rst_linger = {.l_onoff = 1, .l_linger = 0};
    assert(setsockopt(clients[0].sockfd, SOL_SOCKET, SO_LINGER, &rst_linger, sizeof(rst_linger)) ==
           0);
    fct_channel_close(&clients[0]);

    start_ms = monotonic_ms();
    result = FCT_RESULT_OK;
    errno = 0;
    assert(fct_receive_parallel(servers, 4, &writer, &meta, NULL, &result) == -1);
    elapsed_ms = monotonic_ms() - start_ms;
    assert(result == FCT_RESULT_TRANSFER_ERROR && errno == ECONNRESET);
    assert(elapsed_ms < 2000ULL);

    close_channels(4, clients);
    close_channels(4, servers);
    fw_close(&writer);
    unlink(dst_path);

    /* 3. Sender wakeup and error propagation on encode failure across N = 4 workers. */
    write_mixed_pattern_file(src_path, file_size, chunk_size);
    file_reader_t reader;
    assert(fr_init(&reader, src_path, chunk_size) == 0);
    make_tcp_pairs(4, clients, servers);

    compress_stub_set_encode_errno(EIO);
    start_ms = monotonic_ms();
    errno = 0;
    assert(fct_send_parallel(clients, 4, &reader, &meta, FCT_COMPRESS_AUTO, NULL) == -1);
    elapsed_ms = monotonic_ms() - start_ms;
    compress_stub_reset();
    assert(errno == EIO);
    assert(elapsed_ms < 2000ULL);

    close_channels(4, clients);
    close_channels(4, servers);
    fr_close(&reader);
    unlink(src_path);
}

static void* client_fail_result_exchange_thread(void* arg) {
    fct_channel_t* client = arg;
    /* Send chunk 0 with invalid offset so server fails with FCT_RESULT_PROTOCOL_ERROR. */
    send_raw_chunk_manual(client, 0, 1234ULL, 1024U, 'F');
    shutdown(client->sockfd, SHUT_WR);

    struct timespec dl = deadline_after_ms(3000);
    fct_control_frame_t frame = {0};
    assert(fct_channel_recv_control_until(client, &frame, &dl) == 0);
    assert(frame.magic == FCT_MAGIC);
    assert(frame.type == FCT_CONTROL_RESULT);
    assert(frame.arg == FCT_RESULT_PROTOCOL_ERROR);
    assert(frame.token[0] == 0xAB && frame.token[15] == 0xCD);
    return NULL;
}

static void test_single_channel_failure_result_exchange(const char* dir) {
    char dst_path[256];
    snprintf(dst_path, sizeof(dst_path), "%s/n1_fail.bin", dir);

    const uint32_t chunk_size = 1024U;
    const uint64_t file_size = 2U * chunk_size;
    fct_file_meta_t meta = make_meta(file_size, chunk_size, "n1_fail.bin");

    file_writer_t writer;
    assert(fw_init(&writer, dst_path, file_size, chunk_size) == 0);

    fct_channel_t client, server;
    make_tcp_pairs(1, &client, &server);

    pthread_t cthread;
    assert(pthread_create(&cthread, NULL, client_fail_result_exchange_thread, &client) == 0);

    fct_result_t result = FCT_RESULT_OK;
    errno = 0;
    assert(fct_receive_parallel(&server, 1, &writer, &meta, NULL, &result) == -1);
    assert(result == FCT_RESULT_PROTOCOL_ERROR && errno == EPROTO);

    /* Server must still be able to write RESULT on the shared N=1 channel. */
    fct_control_frame_t res_frame = {
        .magic = FCT_MAGIC,
        .type = FCT_CONTROL_RESULT,
        .arg = (uint16_t)result,
    };
    res_frame.token[0] = 0xAB;
    res_frame.token[15] = 0xCD;
    struct timespec dl = deadline_after_ms(3000);
    assert(fct_channel_send_control_until(&server, &res_frame, &dl) == 0);

    assert(pthread_join(cthread, NULL) == 0);

    close_channels(1, &client);
    close_channels(1, &server);
    fw_close(&writer);
    unlink(dst_path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    compress_stub_reset();

    char dir[] = "/tmp/fct-worker-test-XXXXXX";
    assert(mkdtemp(dir) != NULL);

    test_empty_file_n0(dir);
    test_single_and_multi_channel_transfers(dir);
    test_out_of_order_assembly(dir);
    test_duplicate_and_missing_chunks(dir);
    test_protocol_errors_and_decode_failures(dir);
    test_storage_error(dir);
    test_multi_worker_wakeup_and_transfer_error(dir);
    test_single_channel_failure_result_exchange(dir);

    assert(rmdir(dir) == 0);
    puts("worker tests passed");
    return 0;
}
