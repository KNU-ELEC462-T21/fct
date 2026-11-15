#include "network.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void pair(fct_channel_t *a, fct_channel_t *b) {
    int fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    *a = (fct_channel_t){.sockfd = fd[0]};
    *b = (fct_channel_t){.sockfd = fd[1]};
}

static void test_wire_and_roundtrip(void) {
    fct_channel_t a, b;
    unsigned char wire[276];
    const unsigned char prefix[] = {0x46, 0x43, 0x54, 0x31, 1, 2, 3, 4, 5, 6,
                                    7,    8,    0,    0,    0, 4, 0, 0, 0, 2};
    fct_file_meta_t meta = {.magic = FCT_MAGIC,
                            .file_size = UINT64_C(0x0102030405060708),
                            .chunk_size = 4,
                            .total_chunks = 2,
                            .filename = "test.bin"};
    fct_file_meta_t received;
    fct_chunk_header_t hdr = {.magic = FCT_MAGIC,
                              .chunk_id = 0x01020304,
                              .offset = UINT64_C(0x0102030405060708),
                              .payload_len = 3};
    fct_chunk_header_t got;
    char payload[3];
    const unsigned char chunk_wire[] = {
        0x46, 0x43, 0x54, 0x31, 1, 2, 3, 4, 1, 2, 3,   4,   5,  6,
        7,    8,    0,    0,    0, 3, 0, 0, 0, 0, 'a', 'b', 'c'};
    pair(&a, &b);
    assert(fct_channel_send_meta(&a, &meta) == 0);
    assert(recv_exact(b.sockfd, wire, sizeof(wire)) == 0);
    assert(memcmp(wire, prefix, sizeof(prefix)) == 0);
    assert(memcmp(wire + 20, meta.filename, 256) == 0);
    assert(send_exact(a.sockfd, wire, sizeof(wire)) == 0);
    assert(fct_channel_recv_meta(&b, &received) == 0);
    assert(received.file_size == meta.file_size && received.chunk_size == 4 &&
           received.total_chunks == 2 &&
           memcmp(received.filename, meta.filename, 256) == 0);
    assert(fct_channel_send_chunk(&a, &hdr, "abc") == 0);
    assert(recv_exact(b.sockfd, wire, sizeof(chunk_wire)) == 0);
    assert(memcmp(wire, chunk_wire, sizeof(chunk_wire)) == 0);
    assert(send_exact(a.sockfd, chunk_wire, sizeof(chunk_wire)) == 0);
    assert(fct_channel_recv_chunk(&b, &got, payload, sizeof(payload)) == 0);
    assert(got.chunk_id == hdr.chunk_id && got.offset == hdr.offset &&
           got.payload_len == 3 && memcmp(payload, "abc", 3) == 0);
    assert(a.bytes_sent == 276 + 27 && b.bytes_recv == 276 + 27);
    hdr.payload_len = 0;
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
    unsigned char invalid[276] = {0};
    pair(&a, &b);
    assert(fct_channel_send_chunk(&a, &hdr, "abc") == 0);
    errno = 0;
    assert(fct_channel_recv_chunk(&b, &got, buf, sizeof(buf)) == -1 &&
           errno == EMSGSIZE);
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
    invalid[23] = 1;
    assert(send_exact(a.sockfd, invalid, 24) == 0);
    assert(fct_channel_recv_chunk(&b, &got, buf, sizeof(buf)) == -1 &&
           errno == EPROTO);
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
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0);
    fct_channel_close(&b);
}

static void test_tcp(void) {
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(addr);
    fct_channel_t client, server;
    int probe = socket(AF_INET, SOCK_STREAM, 0), listener, port;
    assert(probe >= 0);
    assert(bind(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(getsockname(probe, (struct sockaddr *)&addr, &len) == 0);
    port = ntohs(addr.sin_port);
    close(probe);
    listener = fct_server_listen(port);
    assert(listener >= 0);
    assert(fct_channel_connect(&client, "127.0.0.1", port) == 0);
    assert(fct_channel_accept(&server, listener) == 0);
    assert(strcmp(client.peer_ip, "127.0.0.1") == 0 &&
           client.peer_port == port);
    assert(strcmp(server.peer_ip, "127.0.0.1") == 0 && server.peer_port > 0);
    fct_channel_close(&client);
    fct_channel_close(&server);
    close(listener);
}

int main(void) {
    signal(SIGPIPE, SIG_DFL);
    test_wire_and_roundtrip();
    test_errors();
    test_fragmentation();
    test_tcp();
    puts("network tests passed");
    return 0;
}
