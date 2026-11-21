#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define META_SIZE 276u
#define CHUNK_SIZE 24u

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static uint32_t get32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get64(const unsigned char *p) {
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

int send_exact(int sockfd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    if (len && !buf) {
        errno = EINVAL;
        return -1;
    }
    while (len) {
        ssize_t n = send(sockfd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int recv_exact(int sockfd, void *buf, size_t len) {
    unsigned char *p = buf;
    if (len && !buf) {
        errno = EINVAL;
        return -1;
    }
    while (len) {
        ssize_t n = recv(sockfd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int
channel_init(fct_channel_t *ch, int fd, const struct sockaddr_in *peer) {
    memset(ch, 0, sizeof(*ch));
    ch->sockfd = -1;
    if (!inet_ntop(AF_INET, &peer->sin_addr, ch->peer_ip, sizeof(ch->peer_ip)))
        return -1;
    ch->peer_port = ntohs(peer->sin_port);
    ch->sockfd = fd;
    return 0;
}

int fct_channel_connect(fct_channel_t *ch, const char *server_ip, int port) {
    struct sockaddr_in addr = {0};
    int fd, result, saved;
    if (!ch || !server_ip || port < 1 || port > 65535) {
        errno = EINVAL;
        return -1;
    }
    memset(ch, 0, sizeof(*ch));
    ch->sockfd = -1;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    result = inet_pton(AF_INET, server_ip, &addr.sin_addr);
    if (result != 1) {
        if (result == 0)
            errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        channel_init(ch, fd, &addr) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return 0;
}

int fct_server_listen(int port) {
    struct sockaddr_in addr = {0};
    int fd, enabled = 1, saved;
    if (port < 1 || port > 65535) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) <
            0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 16) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int fct_channel_accept(fct_channel_t *ch, int listen_fd) {
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int fd, saved;
    if (!ch || listen_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    memset(ch, 0, sizeof(*ch));
    ch->sockfd = -1;
    do {
        fd = accept(listen_fd, (struct sockaddr *)&peer, &len);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;
    if (channel_init(ch, fd, &peer) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return 0;
}

int fct_channel_send_meta(fct_channel_t *ch, const fct_file_meta_t *meta) {
    unsigned char wire[META_SIZE];
    if (!ch || !meta || ch->sockfd < 0 || meta->magic != FCT_MAGIC) {
        errno = EINVAL;
        return -1;
    }
    put32(wire, meta->magic);
    put64(wire + 4, meta->file_size);
    put32(wire + 12, meta->chunk_size);
    put32(wire + 16, meta->total_chunks);
    memcpy(wire + 20, meta->filename, sizeof(meta->filename));
    if (send_exact(ch->sockfd, wire, sizeof(wire)) < 0)
        return -1;
    ch->bytes_sent += sizeof(wire);
    return 0;
}

int fct_channel_recv_meta(fct_channel_t *ch, fct_file_meta_t *out_meta) {
    unsigned char wire[META_SIZE];
    fct_file_meta_t meta;
    if (!ch || !out_meta || ch->sockfd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact(ch->sockfd, wire, sizeof(wire)) < 0)
        return -1;
    meta.magic = get32(wire);
    if (meta.magic != FCT_MAGIC) {
        errno = EPROTO;
        return -1;
    }
    meta.file_size = get64(wire + 4);
    meta.chunk_size = get32(wire + 12);
    meta.total_chunks = get32(wire + 16);
    memcpy(meta.filename, wire + 20, sizeof(meta.filename));
    *out_meta = meta;
    ch->bytes_recv += sizeof(wire);
    return 0;
}

int fct_channel_send_chunk(fct_channel_t *ch,
                           const fct_chunk_header_t *hdr,
                           const void *payload) {
    unsigned char wire[CHUNK_SIZE];
    if (!ch || !hdr || ch->sockfd < 0 || hdr->magic != FCT_MAGIC ||
        hdr->flags != 0 || (hdr->payload_len && !payload)) {
        errno = EINVAL;
        return -1;
    }
    put32(wire, hdr->magic);
    put32(wire + 4, hdr->chunk_id);
    put64(wire + 8, hdr->offset);
    put32(wire + 16, hdr->payload_len);
    put32(wire + 20, hdr->flags);
    if (send_exact(ch->sockfd, wire, sizeof(wire)) < 0 ||
        send_exact(ch->sockfd, payload, hdr->payload_len) < 0)
        return -1;
    ch->bytes_sent += sizeof(wire) + (uint64_t)hdr->payload_len;
    return 0;
}

int fct_channel_recv_chunk(fct_channel_t *ch,
                           fct_chunk_header_t *out_hdr,
                           void *payload_buf,
                           size_t payload_capacity) {
    unsigned char wire[CHUNK_SIZE];
    fct_chunk_header_t hdr;
    if (!ch || !out_hdr || ch->sockfd < 0 ||
        (!payload_buf && payload_capacity)) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact(ch->sockfd, wire, sizeof(wire)) < 0)
        return -1;
    hdr.magic = get32(wire);
    hdr.chunk_id = get32(wire + 4);
    hdr.offset = get64(wire + 8);
    hdr.payload_len = get32(wire + 16);
    hdr.flags = get32(wire + 20);
    if (hdr.magic != FCT_MAGIC || hdr.flags != 0) {
        errno = EPROTO;
        return -1;
    }
    if (hdr.payload_len > payload_capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    if (recv_exact(ch->sockfd, payload_buf, hdr.payload_len) < 0)
        return -1;
    *out_hdr = hdr;
    ch->bytes_recv += sizeof(wire) + (uint64_t)hdr.payload_len;
    return 0;
}

void fct_channel_close(fct_channel_t *ch) {
    if (ch && ch->sockfd >= 0) {
        close(ch->sockfd);
        ch->sockfd = -1;
    }
}
