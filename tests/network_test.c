#define _POSIX_C_SOURCE 200809L

#include "network.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void pair(fct_channel_t* a, fct_channel_t* b) {
    int fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    *a = (fct_channel_t){.sockfd = fd[0]};
    *b = (fct_channel_t){.sockfd = fd[1]};
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

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void sleep_ms(unsigned ms) {
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)((ms % 1000U) * 1000000UL),
    };
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
        ;
}

static int open_ephemeral_listener(int* out_port) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t len = sizeof(addr);
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    assert(probe >= 0);
    assert(bind(probe, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(getsockname(probe, (struct sockaddr*)&addr, &len) == 0);
    *out_port = ntohs(addr.sin_port);
    close(probe);
    int listener = fct_server_listen(*out_port);
    assert(listener >= 0);
    return listener;
}

static void test_wire_and_roundtrip(void) {
    fct_channel_t a, b;
    unsigned char wire[308];
    const unsigned char prefix[] = {0x46, 0x43, 0x54, 0x31, 1, 2, 3, 4, 5, 6,
                                    7,    8,    0,    0,    0, 4, 0, 0, 0, 2};
    fct_file_meta_t meta = {.magic = FCT_MAGIC,
                            .file_size = UINT64_C(0x0102030405060708),
                            .chunk_size = 4,
                            .total_chunks = 2,
                            .filename = "test.bin"};
    for (size_t i = 0; i < FCT_SHA256_SIZE; ++i)
        meta.file_sha256[i] = (uint8_t)(0xA0U + i);
    fct_file_meta_t received;
    fct_chunk_header_t hdr = {.magic = FCT_MAGIC,
                              .chunk_id = 0x01020304,
                              .offset = UINT64_C(0x0102030405060708),
                              .payload_len = 3,
                              .crc32 = 0x11223344U};
    fct_chunk_header_t got;
    char payload[3];
    const unsigned char chunk_wire[] = {0x46, 0x43, 0x54, 0x31, 1,    2,    3,   4,   1,  2, 3,
                                        4,    5,    6,    7,    8,    0,    0,   0,   3,  0, 0,
                                        0,    0,    0x11, 0x22, 0x33, 0x44, 'a', 'b', 'c'};
    pair(&a, &b);
    assert(fct_channel_send_meta(&a, &meta) == 0);
    assert(recv_exact(b.sockfd, wire, sizeof(wire)) == 0);
    assert(memcmp(wire, prefix, sizeof(prefix)) == 0);
    assert(memcmp(wire + 20, meta.filename, 256) == 0);
    assert(memcmp(wire + 276, meta.file_sha256, FCT_SHA256_SIZE) == 0);
    assert(send_exact(a.sockfd, wire, sizeof(wire)) == 0);
    assert(fct_channel_recv_meta(&b, &received) == 0);
    assert(received.file_size == meta.file_size && received.chunk_size == 4 &&
           received.total_chunks == 2 && memcmp(received.filename, meta.filename, 256) == 0 &&
           memcmp(received.file_sha256, meta.file_sha256, FCT_SHA256_SIZE) == 0);
    assert(fct_channel_send_chunk(&a, &hdr, "abc") == 0);
    assert(recv_exact(b.sockfd, wire, sizeof(chunk_wire)) == 0);
    assert(memcmp(wire, chunk_wire, sizeof(chunk_wire)) == 0);
    assert(send_exact(a.sockfd, chunk_wire, sizeof(chunk_wire)) == 0);
    assert(fct_channel_recv_chunk(&b, &got, payload, sizeof(payload)) == 0);
    assert(got.chunk_id == hdr.chunk_id && got.offset == hdr.offset && got.payload_len == 3 &&
           got.crc32 == hdr.crc32 && memcmp(payload, "abc", 3) == 0);
    assert(a.bytes_sent == 308 + 31 && b.bytes_recv == 308 + 31);
    hdr.payload_len = 0;
    hdr.crc32 = 0;
    assert(fct_channel_send_chunk(&a, &hdr, NULL) == 0);
    assert(fct_channel_recv_chunk(&b, &got, NULL, 0) == 0);
    fct_channel_close(&a);
    fct_channel_close(&a);
    fct_channel_close(&b);
}

static void test_errors(void) {
    fct_channel_t a, b;
    fct_chunk_header_t hdr = {.magic = FCT_MAGIC, .payload_len = 3}, got;
    fct_file_meta_t meta;
    char buf[2];
    unsigned char invalid[308] = {0};
    pair(&a, &b);
    assert(fct_channel_send_chunk(&a, &hdr, "abc") == 0);
    errno = 0;
    assert(fct_channel_recv_chunk(&b, &got, buf, sizeof(buf)) == -1 && errno == EMSGSIZE);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);
    pair(&a, &b);
    assert(send_exact(a.sockfd, invalid, sizeof(invalid)) == 0);
    assert(fct_channel_recv_meta(&b, &meta) == -1 && errno == EPROTO);
    fct_channel_close(&a);
    fct_channel_close(&b);
    pair(&a, &b);
    invalid[0] = 0x46;
    invalid[1] = 0x43;
    invalid[2] = 0x54;
    invalid[3] = 0x31;
    invalid[23] = 4; /* undefined flag bit */
    assert(send_exact(a.sockfd, invalid, 28) == 0);
    assert(fct_channel_recv_chunk(&b, &got, buf, sizeof(buf)) == -1 && errno == EPROTO);
    fct_channel_close(&a);
    fct_channel_close(&b);
    pair(&a, &b);
    assert(send_exact(a.sockfd, "a", 1) == 0);
    fct_channel_close(&a);
    assert(recv_exact(b.sockfd, buf, sizeof(buf)) == -1 && errno == ECONNRESET);
    assert(send_exact(b.sockfd, "x", 1) == -1);
    fct_channel_close(&b);
    assert(fct_channel_connect(&a, "invalid", 9000) == -1 && errno == EINVAL);
    assert(fct_server_listen(0) == -1 && errno == EINVAL);
}

static void test_fragmentation(void) {
    fct_channel_t a, b;
    pid_t pid;
    int status;
    char buf[5];
    pair(&a, &b);
    pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(b.sockfd);
        for (size_t i = 0; i < 5; ++i)
            if (send_exact(a.sockfd, "hello" + i, 1) < 0)
                _exit(1);
        close(a.sockfd);
        _exit(0);
    }
    close(a.sockfd);
    assert(recv_exact(b.sockfd, buf, sizeof(buf)) == 0);
    assert(memcmp(buf, "hello", sizeof(buf)) == 0);
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    fct_channel_close(&b);
}

static void test_tcp(void) {
    fct_channel_t client, server;
    int port = 0;
    int listener = open_ephemeral_listener(&port);
    assert(fct_channel_connect(&client, "127.0.0.1", port) == 0);
    assert(fct_channel_accept(&server, listener) == 0);
    assert(strcmp(client.peer_ip, "127.0.0.1") == 0 && client.peer_port == port);
    assert(strcmp(server.peer_ip, "127.0.0.1") == 0 && server.peer_port > 0);
    fct_channel_close(&client);
    fct_channel_close(&server);
    close(listener);
}

static void test_control_frame_wire_and_errors(void) {
    fct_channel_t a, b;
    struct timespec dl = deadline_after_ms(2000);
    const uint16_t types[5] = {
        FCT_CONTROL_HELLO,  FCT_CONTROL_READY, FCT_CONTROL_DATA,
        FCT_CONTROL_RESULT, FCT_CONTROL_RETRY,
    };

    /* 1. Big-endian wire layout and round-trip for all 5 control frame types. */
    pair(&a, &b);
    for (size_t i = 0; i < 5; ++i) {
        fct_control_frame_t frame = {
            .magic = FCT_MAGIC,
            .type = types[i],
            .arg = (uint16_t)(0x1230U + i),
        };
        for (size_t j = 0; j < FCT_TRANSFER_TOKEN_SIZE; ++j)
            frame.token[j] = (uint8_t)(i * 16U + j);

        assert(fct_channel_send_control_until(&a, &frame, &dl) == 0);
        unsigned char wire[FCT_CONTROL_FRAME_WIRE_SIZE];
        assert(recv_exact(b.sockfd, wire, sizeof(wire)) == 0);
        assert(wire[0] == 0x46 && wire[1] == 0x43 && wire[2] == 0x54 && wire[3] == 0x31);
        assert(wire[4] == 0 && wire[5] == (unsigned char)types[i]);
        assert(wire[6] == 0x12 && wire[7] == (unsigned char)(0x30U + i));
        assert(memcmp(wire + 8, frame.token, FCT_TRANSFER_TOKEN_SIZE) == 0);

        assert(send_exact(a.sockfd, wire, sizeof(wire)) == 0);
        fct_control_frame_t got = {0};
        assert(fct_channel_recv_control_until(&b, &got, &dl) == 0);
        assert(got.magic == FCT_MAGIC && got.type == frame.type && got.arg == frame.arg);
        assert(memcmp(got.token, frame.token, FCT_TRANSFER_TOKEN_SIZE) == 0);
    }
    assert(a.bytes_sent == 5U * FCT_CONTROL_FRAME_WIRE_SIZE);
    assert(b.bytes_recv == 5U * FCT_CONTROL_FRAME_WIRE_SIZE);
    fct_channel_close(&a);
    fct_channel_close(&b);

    /* 2. Reject bad magic or unknown control type on both send and recv. */
    pair(&a, &b);
    fct_control_frame_t bad = {.magic = 0x12345678U, .type = FCT_CONTROL_HELLO};
    errno = 0;
    assert(fct_channel_send_control_until(&a, &bad, &dl) == -1 && errno == EINVAL);
    bad.magic = FCT_MAGIC;
    bad.type = 0;
    errno = 0;
    assert(fct_channel_send_control_until(&a, &bad, &dl) == -1 && errno == EINVAL);
    bad.type = 6;
    errno = 0;
    assert(fct_channel_send_control_until(&a, &bad, &dl) == -1 && errno == EINVAL);

    unsigned char raw_ctrl[FCT_CONTROL_FRAME_WIRE_SIZE] = {0x46, 0x43, 0x54, 0x31, 0, 6};
    assert(send_exact(a.sockfd, raw_ctrl, sizeof(raw_ctrl)) == 0);
    fct_control_frame_t out = {0};
    errno = 0;
    assert(fct_channel_recv_control_until(&b, &out, &dl) == -1 && errno == EPROTO);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);

    /* 3. Unified ECONNRESET for 0-byte EOF and partial EOF in *_until recv calls. */
    pair(&a, &b);
    fct_channel_close(&a);
    errno = 0;
    assert(fct_channel_recv_control_until(&b, &out, &dl) == -1 && errno == ECONNRESET);
    fct_channel_close(&b);

    pair(&a, &b);
    assert(send_exact(a.sockfd, raw_ctrl, 10) == 0);
    fct_channel_close(&a);
    errno = 0;
    assert(fct_channel_recv_control_until(&b, &out, &dl) == -1 && errno == ECONNRESET);
    fct_channel_close(&b);

    pair(&a, &b);
    fct_file_meta_t meta_out = {0};
    assert(send_exact(a.sockfd, raw_ctrl, 10) == 0);
    fct_channel_close(&a);
    errno = 0;
    assert(fct_channel_recv_meta_until(&b, &meta_out, &dl) == -1 && errno == ECONNRESET);
    assert(b.bytes_recv == 0);
    fct_channel_close(&b);
}

static void test_compressed_chunk_flag(void) {
    fct_channel_t a, b;
    const unsigned char payload[5] = {0x78, 0x9C, 0x01, 0x02, 0x03};
    unsigned char buf[16];
    fct_chunk_header_t hdr = {
        .magic = FCT_MAGIC,
        .chunk_id = 7,
        .offset = 14336,
        .payload_len = sizeof(payload),
        .flags = FCT_CHUNK_FLAG_COMPRESSED,
        .crc32 = 0xCAFEBABEU,
    };
    fct_chunk_header_t got = {0};

    pair(&a, &b);
    assert(fct_channel_send_chunk(&a, &hdr, payload) == 0);
    assert(fct_channel_recv_chunk(&b, &got, buf, sizeof(buf)) == 0);
    assert(got.chunk_id == 7 && got.offset == 14336 && got.payload_len == sizeof(payload) &&
           got.flags == FCT_CHUNK_FLAG_COMPRESSED && got.crc32 == 0xCAFEBABEU);
    assert(memcmp(buf, payload, sizeof(payload)) == 0);

    assert(fct_channel_send_chunk_idle(&a, &hdr, payload, 1000) == 0);
    memset(&got, 0, sizeof(got));
    memset(buf, 0, sizeof(buf));
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 1000) == 0);
    assert(got.flags == FCT_CHUNK_FLAG_COMPRESSED && got.crc32 == 0xCAFEBABEU &&
           memcmp(buf, payload, sizeof(payload)) == 0);

    /* Reject COMPRESSED flag when payload_len == 0. */
    hdr.payload_len = 0;
    errno = 0;
    assert(fct_channel_send_chunk(&a, &hdr, NULL) == -1 && errno == EINVAL);
    errno = 0;
    assert(fct_channel_send_chunk_idle(&a, &hdr, NULL, 1000) == -1 && errno == EINVAL);

    /* Reject COMPRESSED|EOR mutual exclusion and undefined flag bits on send & recv. */
    hdr.payload_len = 3;
    hdr.flags = FCT_CHUNK_FLAG_COMPRESSED | FCT_CHUNK_FLAG_EOR;
    errno = 0;
    assert(fct_channel_send_chunk_idle(&a, &hdr, "abc", 1000) == -1 && errno == EINVAL);

    hdr.flags = FCT_CHUNK_FLAG_COMPRESSED | 4U;
    errno = 0;
    assert(fct_channel_send_chunk_idle(&a, &hdr, "abc", 1000) == -1 && errno == EINVAL);

    unsigned char wire_bad[28] = {0x46, 0x43, 0x54, 0x31};
    wire_bad[23] = FCT_CHUNK_FLAG_COMPRESSED; /* payload_len == 0 with compressed flag */
    assert(send_exact(a.sockfd, wire_bad, sizeof(wire_bad)) == 0);
    errno = 0;
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 1000) == -1 && errno == EPROTO);

    unsigned char wire_both[28 + 1] = {0x46, 0x43, 0x54, 0x31};
    wire_both[19] = 1;
    wire_both[23] = FCT_CHUNK_FLAG_COMPRESSED | FCT_CHUNK_FLAG_EOR;
    assert(send_exact(a.sockfd, wire_both, sizeof(wire_both)) == 0);
    errno = 0;
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 1000) == -1 && errno == EPROTO);

    fct_channel_close(&a);
    fct_channel_close(&b);
}

static void test_deadline_connect_accept_and_io(void) {
    int port = 0;
    int listener = open_ephemeral_listener(&port);
    fct_channel_t client, server;

    /* 1. accept_until expires with ETIMEDOUT and restores listen_fd blocking mode. */
    struct timespec short_dl = deadline_after_ms(50);
    errno = 0;
    assert(fct_channel_accept_until(&server, listener, &short_dl) == -1 && errno == ETIMEDOUT);
    assert((fcntl(listener, F_GETFL, 0) & O_NONBLOCK) == 0);

    /* 2. Already expired deadline fails immediately on connect_until. */
    struct timespec past_dl = {0, 0};
    errno = 0;
    assert(fct_channel_connect_until(&client, "127.0.0.1", port, &past_dl) == -1 &&
           errno == ETIMEDOUT);

    /* 3. Valid deadline connect_until + accept_until + meta_until round-trip. */
    struct timespec valid_dl = deadline_after_ms(2000);
    assert(fct_channel_connect_until(&client, "127.0.0.1", port, &valid_dl) == 0);
    assert(fct_channel_accept_until(&server, listener, &valid_dl) == 0);
    assert((fcntl(listener, F_GETFL, 0) & O_NONBLOCK) == 0);
    assert((fcntl(server.sockfd, F_GETFL, 0) & O_NONBLOCK) == 0);
    assert((fcntl(client.sockfd, F_GETFL, 0) & O_NONBLOCK) == 0);

    fct_file_meta_t meta = {
        .magic = FCT_MAGIC,
        .file_size = 42,
        .chunk_size = DEFAULT_CHUNK_SZ,
        .total_chunks = 1,
        .filename = "until.bin",
    };
    fct_file_meta_t got_meta = {0};
    assert(fct_channel_send_meta_until(&client, &meta, &valid_dl) == 0);
    assert(fct_channel_recv_meta_until(&server, &got_meta, &valid_dl) == 0);
    assert(got_meta.file_size == 42 && strcmp(got_meta.filename, "until.bin") == 0);

    /* 4. Silent peer triggers ETIMEDOUT on recv_control_until & recv_meta_until. */
    fct_control_frame_t ctrl = {0};
    short_dl = deadline_after_ms(50);
    errno = 0;
    assert(fct_channel_recv_control_until(&server, &ctrl, &short_dl) == -1 && errno == ETIMEDOUT);

    short_dl = deadline_after_ms(50);
    errno = 0;
    assert(fct_channel_recv_meta_until(&server, &got_meta, &short_dl) == -1 && errno == ETIMEDOUT);

    fct_channel_close(&client);
    fct_channel_close(&server);
    close(listener);
}

static volatile sig_atomic_t g_alarm_hits = 0;

static void alarm_handler(int signo) {
    (void)signo;
    g_alarm_hits++;
}

static void test_chunk_idle_eof_and_timeout(void) {
    fct_channel_t a, b;
    fct_chunk_header_t hdr = {.magic = FCT_MAGIC, .chunk_id = 1, .offset = 100, .payload_len = 16};
    fct_chunk_header_t got = {0};
    unsigned char buf[32];

    /* 1. idle_timeout_ms == 0 expires immediately with ETIMEDOUT. */
    pair(&a, &b);
    errno = 0;
    assert(fct_channel_send_chunk_idle(&a, &hdr, "0123456789abcdef", 0) == -1 &&
           errno == ETIMEDOUT);
    assert(a.bytes_sent == 0);
    errno = 0;
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 0) == -1 && errno == ETIMEDOUT);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);

    /* 2. Clean EOF before header returns 1; mid-header and mid-payload EOF return -1/EPROTO. */
    pair(&a, &b);
    shutdown(a.sockfd, SHUT_WR);
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 1000) == 1);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);

    pair(&a, &b);
    const unsigned char half_hdr[10] = {0x46, 0x43, 0x54, 0x31, 0, 0, 0, 1, 0, 0};
    assert(send_exact(a.sockfd, half_hdr, sizeof(half_hdr)) == 0);
    shutdown(a.sockfd, SHUT_WR);
    errno = 0;
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 1000) == -1 && errno == EPROTO);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);

    pair(&a, &b);
    const unsigned char partial_frame[28 + 5] = {
        0x46, 0x43, 0x54, 0x31, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,    0,    16,   0,    0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5,
    };
    assert(send_exact(a.sockfd, partial_frame, sizeof(partial_frame)) == 0);
    shutdown(a.sockfd, SHUT_WR);
    errno = 0;
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 1000) == -1 && errno == EPROTO);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);

    /* 3. Trickling bytes faster than idle_timeout_ms resets timer and succeeds. */
    pair(&a, &b);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        fct_channel_close(&b);
        const unsigned char full_frame[28 + 20] = {
            0x46, 0x43, 0x54, 0x31, 0,   0,   0,   3,   0,   0,   0,   0,   0,   0,   0,   64,
            0,    0,    0,    20,   0,   0,   0,   0,   0xA, 0xB, 0xC, 0xD, 'a', 'b', 'c', 'd',
            'e',  'f',  'g',  'h',  'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
        };
        for (size_t off = 0; off < sizeof(full_frame); off += 4) {
            if (send_exact(a.sockfd, full_frame + off, 4) < 0)
                _exit(1);
            sleep_ms(30);
        }
        fct_channel_close(&a);
        _exit(0);
    }
    fct_channel_close(&a);
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 150) == 0);
    assert(got.chunk_id == 3 && got.offset == 64 && got.payload_len == 20 &&
           got.crc32 == 0x0A0B0C0DU);
    assert(memcmp(buf, "abcdefghijklmnopqrst", 20) == 0);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    fct_channel_close(&b);

    /* 4. Repeated EINTR (SIGALRM) without transferred bytes must NOT reset idle timeout. */
    pair(&a, &b);
    struct sigaction sa = {0};
    struct sigaction old_sa = {0};
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* Ensure poll() gets EINTR. */
    g_alarm_hits = 0;
    assert(sigaction(SIGALRM, &sa, &old_sa) == 0);

    struct itimerval it = {
        .it_interval = {.tv_sec = 0, .tv_usec = 15000},
        .it_value = {.tv_sec = 0, .tv_usec = 15000},
    };
    struct itimerval stop_it = {0};
    assert(setitimer(ITIMER_REAL, &it, NULL) == 0);

    uint64_t t0 = monotonic_ms();
    errno = 0;
    assert(fct_channel_recv_chunk_idle(&b, &got, buf, sizeof(buf), 80) == -1 && errno == ETIMEDOUT);
    uint64_t elapsed = monotonic_ms() - t0;

    assert(setitimer(ITIMER_REAL, &stop_it, NULL) == 0);
    assert(sigaction(SIGALRM, &old_sa, NULL) == 0);
    assert(g_alarm_hits >= 2);
    assert(elapsed >= 70 && elapsed < 500);

    fct_channel_close(&a);
    fct_channel_close(&b);
}

static void test_eor_chunk_header(void) {
    fct_channel_t a, b;
    fct_chunk_header_t eor = {.magic = FCT_MAGIC, .flags = FCT_CHUNK_FLAG_EOR};
    fct_chunk_header_t got = {0};
    unsigned char wire[28];
    const unsigned char expected_wire[28] = {
        0x46, 0x43, 0x54, 0x31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,    0,    0,    0,    0, 0, 0, 0, 0, 2, 0, 0, 0, 0,
    };

    pair(&a, &b);
    assert(fct_channel_send_chunk(&a, &eor, NULL) == 0);
    assert(recv_exact(b.sockfd, wire, sizeof(wire)) == 0);
    assert(memcmp(wire, expected_wire, sizeof(expected_wire)) == 0);

    assert(send_exact(a.sockfd, wire, sizeof(wire)) == 0);
    assert(fct_channel_recv_chunk(&b, &got, NULL, 0) == 0);
    assert(got.magic == FCT_MAGIC && got.flags == FCT_CHUNK_FLAG_EOR && got.chunk_id == 0 &&
           got.offset == 0 && got.payload_len == 0 && got.crc32 == 0);

    memset(&got, 0xFF, sizeof(got));
    assert(fct_channel_send_chunk_idle(&a, &eor, NULL, 1000) == 0);
    assert(fct_channel_recv_chunk_idle(&b, &got, NULL, 0, 1000) == 0);
    assert(got.flags == FCT_CHUNK_FLAG_EOR && got.chunk_id == 0 && got.offset == 0 &&
           got.payload_len == 0 && got.crc32 == 0);
    assert(a.bytes_sent == 56 && b.bytes_recv == 56);

    /* Reject non-zero fields with EOR on send (EINVAL). */
    fct_chunk_header_t bad = eor;
    bad.chunk_id = 1;
    errno = 0;
    assert(fct_channel_send_chunk(&a, &bad, NULL) == -1 && errno == EINVAL);
    bad = eor;
    bad.offset = 1;
    errno = 0;
    assert(fct_channel_send_chunk_idle(&a, &bad, NULL, 1000) == -1 && errno == EINVAL);
    bad = eor;
    bad.payload_len = 1;
    errno = 0;
    assert(fct_channel_send_chunk_idle(&a, &bad, "x", 1000) == -1 && errno == EINVAL);
    bad = eor;
    bad.crc32 = 1;
    errno = 0;
    assert(fct_channel_send_chunk_idle(&a, &bad, NULL, 1000) == -1 && errno == EINVAL);

    /* Reject malformed EOR headers on recv (EPROTO) without advancing bytes_recv. */
    const size_t nonzero_offsets[4] = {7, 15, 19, 27};
    for (size_t i = 0; i < 4; ++i) {
        unsigned char raw[28];
        unsigned char dummy = 0;
        memcpy(raw, expected_wire, sizeof(raw));
        raw[nonzero_offsets[i]] = 1;
        assert(send_exact(a.sockfd, raw, sizeof(raw)) == 0);
        if (nonzero_offsets[i] == 19)
            assert(send_exact(a.sockfd, &dummy, 1) == 0);
        errno = 0;
        assert(fct_channel_recv_chunk_idle(&b, &got, &dummy, 1, 1000) == -1 && errno == EPROTO);
        if (nonzero_offsets[i] == 19)
            assert(recv_exact(b.sockfd, &dummy, 1) == 0);
    }
    assert(b.bytes_recv == 56);

    fct_channel_close(&a);
    fct_channel_close(&b);
}

static void test_bitmap_frame_wire_and_errors(void) {
    fct_channel_t a, b;
    struct timespec dl = deadline_after_ms(2000);

    /* 1. Non-empty bitmap big-endian wire layout and round-trip. */
    pair(&a, &b);
    const uint8_t bits[5] = {0xA5, 0x01, 0x80, 0x00, 0x3F};
    uint8_t recv_bits[5] = {0};
    unsigned char wire_hdr[FCT_BITMAP_HEADER_WIRE_SIZE];
    assert(fct_channel_send_bitmap_until(&a, bits, sizeof(bits), &dl) == 0);
    assert(recv_exact(b.sockfd, wire_hdr, sizeof(wire_hdr)) == 0);
    assert(wire_hdr[0] == 0x46 && wire_hdr[1] == 0x43 && wire_hdr[2] == 0x54 &&
           wire_hdr[3] == 0x31);
    assert(wire_hdr[4] == 0 && wire_hdr[5] == 0 && wire_hdr[6] == 0 && wire_hdr[7] == 5);
    assert(recv_exact(b.sockfd, recv_bits, sizeof(recv_bits)) == 0);
    assert(memcmp(recv_bits, bits, sizeof(bits)) == 0);

    memset(recv_bits, 0, sizeof(recv_bits));
    assert(fct_channel_send_bitmap_until(&a, bits, sizeof(bits), &dl) == 0);
    assert(fct_channel_recv_bitmap_until(&b, recv_bits, sizeof(recv_bits), &dl) == 0);
    assert(memcmp(recv_bits, bits, sizeof(bits)) == 0);
    assert(a.bytes_sent == 2U * (8U + 5U));
    assert(b.bytes_recv == 8U + 5U);
    fct_channel_close(&a);
    fct_channel_close(&b);

    /* 2. Empty file bitmap (byte_len == 0, bits == NULL) round-trip. */
    pair(&a, &b);
    assert(fct_channel_send_bitmap_until(&a, NULL, 0, &dl) == 0);
    assert(fct_channel_recv_bitmap_until(&b, NULL, 0, &dl) == 0);
    assert(a.bytes_sent == 8 && b.bytes_recv == 8);
    fct_channel_close(&a);
    fct_channel_close(&b);

    /* 3. Parameter validation (EINVAL). */
    pair(&a, &b);
    errno = 0;
    assert(fct_channel_send_bitmap_until(&a, bits, FCT_MAX_BITMAP_BYTES + 1U, &dl) == -1 &&
           errno == EINVAL);
    errno = 0;
    assert(fct_channel_send_bitmap_until(&a, NULL, 1, &dl) == -1 && errno == EINVAL);
    errno = 0;
    assert(fct_channel_recv_bitmap_until(&b, recv_bits, FCT_MAX_BITMAP_BYTES + 1U, &dl) == -1 &&
           errno == EINVAL);
    errno = 0;
    assert(fct_channel_recv_bitmap_until(&b, NULL, 1, &dl) == -1 && errno == EINVAL);
    fct_channel_close(&a);
    fct_channel_close(&b);

    /* 4. Protocol errors (bad magic, mismatched length, oversized wire length) -> EPROTO. */
    pair(&a, &b);
    const unsigned char bad_magic[8] = {0x00, 0x43, 0x54, 0x31, 0, 0, 0, 1};
    assert(send_exact(a.sockfd, bad_magic, sizeof(bad_magic)) == 0);
    errno = 0;
    assert(fct_channel_recv_bitmap_until(&b, recv_bits, 1, &dl) == -1 && errno == EPROTO);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);

    pair(&a, &b);
    assert(fct_channel_send_bitmap_until(&a, bits, 5, &dl) == 0);
    errno = 0;
    assert(fct_channel_recv_bitmap_until(&b, recv_bits, 4, &dl) == -1 && errno == EPROTO);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);

    pair(&a, &b);
    const unsigned char too_big[8] = {0x46, 0x43, 0x54, 0x31, 0x00, 0x40, 0x00, 0x01};
    assert(send_exact(a.sockfd, too_big, sizeof(too_big)) == 0);
    errno = 0;
    assert(fct_channel_recv_bitmap_until(&b, recv_bits, 1, &dl) == -1 && errno == EPROTO);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);

    /* 5. EOF / partial truncation -> ECONNRESET, silent peer -> ETIMEDOUT. */
    pair(&a, &b);
    fct_channel_close(&a);
    errno = 0;
    assert(fct_channel_recv_bitmap_until(&b, recv_bits, 2, &dl) == -1 && errno == ECONNRESET);
    assert(b.bytes_recv == 0);
    fct_channel_close(&b);

    pair(&a, &b);
    const unsigned char hdr_only[8] = {0x46, 0x43, 0x54, 0x31, 0, 0, 0, 4};
    assert(send_exact(a.sockfd, hdr_only, sizeof(hdr_only)) == 0);
    assert(send_exact(a.sockfd, "ab", 2) == 0);
    fct_channel_close(&a);
    errno = 0;
    assert(fct_channel_recv_bitmap_until(&b, recv_bits, 4, &dl) == -1 && errno == ECONNRESET);
    assert(b.bytes_recv == 0);
    fct_channel_close(&b);

    pair(&a, &b);
    struct timespec short_dl = deadline_after_ms(50);
    errno = 0;
    assert(fct_channel_recv_bitmap_until(&b, recv_bits, 1, &short_dl) == -1 && errno == ETIMEDOUT);
    assert(b.bytes_recv == 0);
    fct_channel_close(&a);
    fct_channel_close(&b);
}

int main(void) {
    signal(SIGPIPE, SIG_DFL);
    test_wire_and_roundtrip();
    test_errors();
    test_fragmentation();
    test_tcp();
    test_control_frame_wire_and_errors();
    test_compressed_chunk_flag();
    test_eor_chunk_header();
    test_bitmap_frame_wire_and_errors();
    test_deadline_connect_accept_and_io();
    test_chunk_idle_eof_and_timeout();
    puts("network tests passed");
    return 0;
}
