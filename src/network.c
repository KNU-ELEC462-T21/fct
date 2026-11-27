#define _POSIX_C_SOURCE 200809L

#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define META_SIZE 308u
#define CHUNK_SIZE 28u
#define CONTROL_SIZE FCT_CONTROL_FRAME_WIRE_SIZE
#define BITMAP_HEADER_SIZE FCT_BITMAP_HEADER_WIRE_SIZE

_Static_assert(sizeof(fct_file_meta_t) == META_SIZE, "fct_file_meta_t must be 308 bytes");
_Static_assert(sizeof(fct_chunk_header_t) == CHUNK_SIZE, "fct_chunk_header_t must be 28 bytes");

static void put16(unsigned char* p, uint16_t v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void put64(unsigned char* p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static uint16_t get16(const unsigned char* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t get32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get64(const unsigned char* p) {
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

static bool is_valid_control_type(uint16_t type) {
    return type >= FCT_CONTROL_HELLO && type <= FCT_CONTROL_RETRY;
}

static bool is_valid_chunk_header(const fct_chunk_header_t* hdr) {
    if (!hdr || hdr->magic != FCT_MAGIC)
        return false;
    if (hdr->flags == 0)
        return true;
    if (hdr->flags == FCT_CHUNK_FLAG_COMPRESSED)
        return hdr->payload_len > 0;
    if (hdr->flags == FCT_CHUNK_FLAG_EOR) {
        return hdr->chunk_id == 0 && hdr->offset == 0 && hdr->payload_len == 0 && hdr->crc32 == 0;
    }
    return false;
}

static void encode_control(unsigned char wire[CONTROL_SIZE], const fct_control_frame_t* frame) {
    put32(wire, frame->magic);
    put16(wire + 4, frame->type);
    put16(wire + 6, frame->arg);
    memcpy(wire + 8, frame->token, FCT_TRANSFER_TOKEN_SIZE);
}

static void decode_control(const unsigned char wire[CONTROL_SIZE], fct_control_frame_t* frame) {
    frame->magic = get32(wire);
    frame->type = get16(wire + 4);
    frame->arg = get16(wire + 6);
    memcpy(frame->token, wire + 8, FCT_TRANSFER_TOKEN_SIZE);
}

static void encode_meta(unsigned char wire[META_SIZE], const fct_file_meta_t* meta) {
    put32(wire, meta->magic);
    put64(wire + 4, meta->file_size);
    put32(wire + 12, meta->chunk_size);
    put32(wire + 16, meta->total_chunks);
    memcpy(wire + 20, meta->filename, sizeof(meta->filename));
    memcpy(wire + 20 + sizeof(meta->filename), meta->file_sha256, sizeof(meta->file_sha256));
}

static void decode_meta(const unsigned char wire[META_SIZE], fct_file_meta_t* meta) {
    meta->magic = get32(wire);
    meta->file_size = get64(wire + 4);
    meta->chunk_size = get32(wire + 12);
    meta->total_chunks = get32(wire + 16);
    memcpy(meta->filename, wire + 20, sizeof(meta->filename));
    memcpy(meta->file_sha256, wire + 20 + sizeof(meta->filename), sizeof(meta->file_sha256));
}

static void encode_chunk_header(unsigned char wire[CHUNK_SIZE], const fct_chunk_header_t* hdr) {
    put32(wire, hdr->magic);
    put32(wire + 4, hdr->chunk_id);
    put64(wire + 8, hdr->offset);
    put32(wire + 16, hdr->payload_len);
    put32(wire + 20, hdr->flags);
    put32(wire + 24, hdr->crc32);
}

static void decode_chunk_header(const unsigned char wire[CHUNK_SIZE], fct_chunk_header_t* hdr) {
    hdr->magic = get32(wire);
    hdr->chunk_id = get32(wire + 4);
    hdr->offset = get64(wire + 8);
    hdr->payload_len = get32(wire + 16);
    hdr->flags = get32(wire + 20);
    hdr->crc32 = get32(wire + 24);
}

static bool is_valid_deadline(const struct timespec* deadline) {
    return deadline != NULL && deadline->tv_nsec >= 0 && deadline->tv_nsec < 1000000000L;
}

static struct timespec add_timeout_ms(const struct timespec* base, unsigned timeout_ms) {
    struct timespec result = *base;
    time_t add_sec = (time_t)(timeout_ms / 1000U);
    long add_nsec = (long)((timeout_ms % 1000U) * 1000000UL);
    result.tv_nsec += add_nsec;
    if (result.tv_nsec >= 1000000000L) {
        result.tv_nsec -= 1000000000L;
        add_sec += 1;
    }
    result.tv_sec += add_sec;
    return result;
}

static int remaining_ms_until(const struct timespec* deadline, int* out_ms) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    if (now.tv_sec > deadline->tv_sec ||
        (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
        errno = ETIMEDOUT;
        return -1;
    }
    int64_t sec_diff = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    int64_t nsec_diff = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    if (nsec_diff < 0) {
        sec_diff -= 1;
        nsec_diff += 1000000000LL;
    }
    int64_t ms_from_nsec = (nsec_diff + 999999LL) / 1000000LL;
    int64_t max_sec = (INT64_MAX - ms_from_nsec) / 1000LL;
    int64_t total_ms = sec_diff > max_sec ? (int64_t)INT_MAX : sec_diff * 1000LL + ms_from_nsec;
    if (total_ms > INT_MAX)
        total_ms = INT_MAX;
    if (total_ms <= 0)
        total_ms = 1;
    *out_ms = (int)total_ms;
    return 0;
}

static int poll_fd_until(int fd, short events, const struct timespec* deadline) {
    for (;;) {
        int timeout_ms = 0;
        if (remaining_ms_until(deadline, &timeout_ms) < 0)
            return -1;
        struct pollfd pfd = {
            .fd = fd,
            .events = events,
            .revents = 0,
        };
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            continue;
        if (pfd.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        return 0;
    }
}

int send_exact(int sockfd, const void* buf, size_t len) {
    const unsigned char* p = buf;
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

int recv_exact(int sockfd, void* buf, size_t len) {
    unsigned char* p = buf;
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

static int send_exact_until(int sockfd, const void* buf, size_t len,
                            const struct timespec* deadline) {
    const unsigned char* p = buf;
    size_t rem = len;
    if (rem && !buf) {
        errno = EINVAL;
        return -1;
    }
    while (rem > 0) {
        int dummy = 0;
        if (remaining_ms_until(deadline, &dummy) < 0)
            return -1;
        ssize_t n = send(sockfd, p, rem, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            p += n;
            rem -= (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (poll_fd_until(sockfd, POLLOUT, deadline) < 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int recv_exact_until(int sockfd, void* buf, size_t len, const struct timespec* deadline) {
    unsigned char* p = buf;
    size_t rem = len;
    if (rem && !buf) {
        errno = EINVAL;
        return -1;
    }
    while (rem > 0) {
        int dummy = 0;
        if (remaining_ms_until(deadline, &dummy) < 0)
            return -1;
        ssize_t n = recv(sockfd, p, rem, MSG_DONTWAIT);
        if (n > 0) {
            p += n;
            rem -= (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (poll_fd_until(sockfd, POLLIN, deadline) < 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int send_exact_idle(int sockfd, const void* buf, size_t len, unsigned idle_timeout_ms,
                           struct timespec* deadline) {
    const unsigned char* p = buf;
    size_t rem = len;
    if (rem && !buf) {
        errno = EINVAL;
        return -1;
    }
    while (rem > 0) {
        int dummy = 0;
        if (remaining_ms_until(deadline, &dummy) < 0)
            return -1;
        ssize_t n = send(sockfd, p, rem, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            struct timespec now;
            p += n;
            rem -= (size_t)n;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
                return -1;
            *deadline = add_timeout_ms(&now, idle_timeout_ms);
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (poll_fd_until(sockfd, POLLOUT, deadline) < 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int recv_exact_idle(int sockfd, void* buf, size_t len, unsigned idle_timeout_ms,
                           struct timespec* deadline, bool allow_clean_eof) {
    unsigned char* p = buf;
    size_t total = 0;
    if (len && !buf) {
        errno = EINVAL;
        return -1;
    }
    while (total < len) {
        int dummy = 0;
        if (remaining_ms_until(deadline, &dummy) < 0)
            return -1;
        ssize_t n = recv(sockfd, p + total, len - total, MSG_DONTWAIT);
        if (n > 0) {
            struct timespec now;
            total += (size_t)n;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
                return -1;
            *deadline = add_timeout_ms(&now, idle_timeout_ms);
            continue;
        }
        if (n == 0) {
            if (allow_clean_eof && total == 0)
                return 1;
            errno = EPROTO;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (poll_fd_until(sockfd, POLLIN, deadline) < 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int channel_init(fct_channel_t* ch, int fd, const struct sockaddr_in* peer) {
    memset(ch, 0, sizeof(*ch));
    ch->sockfd = -1;
    if (!inet_ntop(AF_INET, &peer->sin_addr, ch->peer_ip, sizeof(ch->peer_ip)))
        return -1;
    ch->peer_port = ntohs(peer->sin_port);
    ch->sockfd = fd;
    return 0;
}

int fct_channel_connect(fct_channel_t* ch, const char* server_ip, int port) {
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
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || channel_init(ch, fd, &addr) < 0) {
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
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int fct_channel_accept(fct_channel_t* ch, int listen_fd) {
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
        fd = accept(listen_fd, (struct sockaddr*)&peer, &len);
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

int fct_channel_send_meta(fct_channel_t* ch, const fct_file_meta_t* meta) {
    unsigned char wire[META_SIZE];
    if (!ch || !meta || ch->sockfd < 0 || meta->magic != FCT_MAGIC) {
        errno = EINVAL;
        return -1;
    }
    encode_meta(wire, meta);
    if (send_exact(ch->sockfd, wire, sizeof(wire)) < 0)
        return -1;
    ch->bytes_sent += sizeof(wire);
    return 0;
}

int fct_channel_recv_meta(fct_channel_t* ch, fct_file_meta_t* out_meta) {
    unsigned char wire[META_SIZE];
    fct_file_meta_t meta;
    if (!ch || !out_meta || ch->sockfd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact(ch->sockfd, wire, sizeof(wire)) < 0)
        return -1;
    decode_meta(wire, &meta);
    if (meta.magic != FCT_MAGIC) {
        errno = EPROTO;
        return -1;
    }
    *out_meta = meta;
    ch->bytes_recv += sizeof(wire);
    return 0;
}

int fct_channel_send_chunk(fct_channel_t* ch, const fct_chunk_header_t* hdr, const void* payload) {
    unsigned char wire[CHUNK_SIZE];
    if (!ch || !hdr || ch->sockfd < 0 || !is_valid_chunk_header(hdr) ||
        (hdr->payload_len && !payload)) {
        errno = EINVAL;
        return -1;
    }
    encode_chunk_header(wire, hdr);
    if (send_exact(ch->sockfd, wire, sizeof(wire)) < 0 ||
        send_exact(ch->sockfd, payload, hdr->payload_len) < 0)
        return -1;
    ch->bytes_sent += sizeof(wire) + (uint64_t)hdr->payload_len;
    return 0;
}

int fct_channel_recv_chunk(fct_channel_t* ch, fct_chunk_header_t* out_hdr, void* payload_buf,
                           size_t payload_capacity) {
    unsigned char wire[CHUNK_SIZE];
    fct_chunk_header_t hdr;
    if (!ch || !out_hdr || ch->sockfd < 0 || (!payload_buf && payload_capacity)) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact(ch->sockfd, wire, sizeof(wire)) < 0)
        return -1;
    decode_chunk_header(wire, &hdr);
    if (!is_valid_chunk_header(&hdr)) {
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

int fct_channel_connect_until(fct_channel_t* ch, const char* server_ip, int port,
                              const struct timespec* deadline) {
    struct sockaddr_in addr = {0};
    int fd, flags, result, saved, so_err = 0, dummy = 0;
    socklen_t so_len = sizeof(so_err);

    if (!ch || !server_ip || port < 1 || port > 65535 || !is_valid_deadline(deadline)) {
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
    if (remaining_ms_until(deadline, &dummy) < 0)
        return -1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS && errno != EINTR) {
            saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        if (poll_fd_until(fd, POLLOUT, deadline) < 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) < 0) {
            saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        if (so_err != 0) {
            close(fd);
            errno = so_err;
            return -1;
        }
    }

    if (fcntl(fd, F_SETFL, flags) < 0 || channel_init(ch, fd, &addr) < 0) {
        saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return 0;
}

int fct_channel_accept_until(fct_channel_t* ch, int listen_fd, const struct timespec* deadline) {
    int old_flags, fd = -1, saved = 0, dummy = 0;

    if (!ch || listen_fd < 0 || !is_valid_deadline(deadline)) {
        errno = EINVAL;
        return -1;
    }
    memset(ch, 0, sizeof(*ch));
    ch->sockfd = -1;

    if (remaining_ms_until(deadline, &dummy) < 0)
        return -1;

    old_flags = fcntl(listen_fd, F_GETFL, 0);
    if (old_flags < 0)
        return -1;
    if (!(old_flags & O_NONBLOCK) && fcntl(listen_fd, F_SETFL, old_flags | O_NONBLOCK) < 0)
        return -1;

    for (;;) {
        if (poll_fd_until(listen_fd, POLLIN, deadline) < 0) {
            saved = errno;
            break;
        }
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        fd = accept(listen_fd, (struct sockaddr*)&peer, &len);
        if (fd >= 0) {
            if (channel_init(ch, fd, &peer) < 0) {
                saved = errno;
                close(fd);
                fd = -1;
            }
            break;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
            continue;
        saved = errno;
        break;
    }

    if (!(old_flags & O_NONBLOCK)) {
        int restore_rc = fcntl(listen_fd, F_SETFL, old_flags);
        if (fd >= 0 && restore_rc < 0) {
            saved = errno;
            fct_channel_close(ch);
            fd = -1;
        }
    }
    if (fd < 0) {
        errno = saved;
        return -1;
    }
    return 0;
}

int fct_channel_send_control_until(fct_channel_t* ch, const fct_control_frame_t* frame,
                                   const struct timespec* deadline) {
    unsigned char wire[CONTROL_SIZE];
    if (!ch || !frame || ch->sockfd < 0 || !is_valid_deadline(deadline) ||
        frame->magic != FCT_MAGIC || !is_valid_control_type(frame->type)) {
        errno = EINVAL;
        return -1;
    }
    encode_control(wire, frame);
    if (send_exact_until(ch->sockfd, wire, sizeof(wire), deadline) < 0)
        return -1;
    ch->bytes_sent += sizeof(wire);
    return 0;
}

int fct_channel_recv_control_until(fct_channel_t* ch, fct_control_frame_t* frame,
                                   const struct timespec* deadline) {
    unsigned char wire[CONTROL_SIZE];
    fct_control_frame_t parsed;
    if (!ch || !frame || ch->sockfd < 0 || !is_valid_deadline(deadline)) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact_until(ch->sockfd, wire, sizeof(wire), deadline) < 0)
        return -1;
    decode_control(wire, &parsed);
    if (parsed.magic != FCT_MAGIC || !is_valid_control_type(parsed.type)) {
        errno = EPROTO;
        return -1;
    }
    *frame = parsed;
    ch->bytes_recv += sizeof(wire);
    return 0;
}

int fct_channel_send_meta_until(fct_channel_t* ch, const fct_file_meta_t* meta,
                                const struct timespec* deadline) {
    unsigned char wire[META_SIZE];
    if (!ch || !meta || ch->sockfd < 0 || !is_valid_deadline(deadline) ||
        meta->magic != FCT_MAGIC) {
        errno = EINVAL;
        return -1;
    }
    encode_meta(wire, meta);
    if (send_exact_until(ch->sockfd, wire, sizeof(wire), deadline) < 0)
        return -1;
    ch->bytes_sent += sizeof(wire);
    return 0;
}

int fct_channel_recv_meta_until(fct_channel_t* ch, fct_file_meta_t* meta,
                                const struct timespec* deadline) {
    unsigned char wire[META_SIZE];
    fct_file_meta_t parsed;
    if (!ch || !meta || ch->sockfd < 0 || !is_valid_deadline(deadline)) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact_until(ch->sockfd, wire, sizeof(wire), deadline) < 0)
        return -1;
    decode_meta(wire, &parsed);
    if (parsed.magic != FCT_MAGIC) {
        errno = EPROTO;
        return -1;
    }
    *meta = parsed;
    ch->bytes_recv += sizeof(wire);
    return 0;
}

int fct_channel_send_bitmap_until(fct_channel_t* ch, const uint8_t* bits, size_t byte_len,
                                  const struct timespec* deadline) {
    unsigned char hdr[BITMAP_HEADER_SIZE];
    if (!ch || ch->sockfd < 0 || !is_valid_deadline(deadline) || byte_len > FCT_MAX_BITMAP_BYTES ||
        (byte_len > 0 && !bits)) {
        errno = EINVAL;
        return -1;
    }
    put32(hdr, FCT_MAGIC);
    put32(hdr + 4, (uint32_t)byte_len);
    if (send_exact_until(ch->sockfd, hdr, sizeof(hdr), deadline) < 0)
        return -1;
    if (byte_len > 0 && send_exact_until(ch->sockfd, bits, byte_len, deadline) < 0)
        return -1;
    ch->bytes_sent += sizeof(hdr) + (uint64_t)byte_len;
    return 0;
}

int fct_channel_recv_bitmap_until(fct_channel_t* ch, uint8_t* bits, size_t expected_byte_len,
                                  const struct timespec* deadline) {
    unsigned char hdr[BITMAP_HEADER_SIZE];
    if (!ch || ch->sockfd < 0 || !is_valid_deadline(deadline) ||
        expected_byte_len > FCT_MAX_BITMAP_BYTES || (expected_byte_len > 0 && !bits)) {
        errno = EINVAL;
        return -1;
    }
    if (recv_exact_until(ch->sockfd, hdr, sizeof(hdr), deadline) < 0)
        return -1;
    uint32_t magic = get32(hdr);
    uint32_t wire_len = get32(hdr + 4);
    if (magic != FCT_MAGIC || wire_len > FCT_MAX_BITMAP_BYTES || wire_len != expected_byte_len) {
        errno = EPROTO;
        return -1;
    }
    if (wire_len > 0 && recv_exact_until(ch->sockfd, bits, wire_len, deadline) < 0)
        return -1;
    ch->bytes_recv += sizeof(hdr) + (uint64_t)wire_len;
    return 0;
}

int fct_channel_send_chunk_idle(fct_channel_t* ch, const fct_chunk_header_t* hdr,
                                const void* payload, unsigned idle_timeout_ms) {
    unsigned char wire[CHUNK_SIZE];
    struct timespec now, deadline;
    if (!ch || !hdr || ch->sockfd < 0 || !is_valid_chunk_header(hdr) ||
        (hdr->payload_len && !payload)) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    deadline = add_timeout_ms(&now, idle_timeout_ms);
    encode_chunk_header(wire, hdr);
    if (send_exact_idle(ch->sockfd, wire, sizeof(wire), idle_timeout_ms, &deadline) < 0)
        return -1;
    if (hdr->payload_len > 0 &&
        send_exact_idle(ch->sockfd, payload, hdr->payload_len, idle_timeout_ms, &deadline) < 0)
        return -1;
    ch->bytes_sent += sizeof(wire) + (uint64_t)hdr->payload_len;
    return 0;
}

int fct_channel_recv_chunk_idle(fct_channel_t* ch, fct_chunk_header_t* hdr, void* payload,
                                size_t capacity, unsigned idle_timeout_ms) {
    unsigned char wire[CHUNK_SIZE];
    fct_chunk_header_t parsed;
    struct timespec now, deadline;
    int rc;
    if (!ch || !hdr || ch->sockfd < 0 || (!payload && capacity)) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    deadline = add_timeout_ms(&now, idle_timeout_ms);
    rc = recv_exact_idle(ch->sockfd, wire, sizeof(wire), idle_timeout_ms, &deadline, true);
    if (rc != 0)
        return rc;
    decode_chunk_header(wire, &parsed);
    if (!is_valid_chunk_header(&parsed)) {
        errno = EPROTO;
        return -1;
    }
    if (parsed.payload_len > capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    if (parsed.payload_len > 0 && recv_exact_idle(ch->sockfd, payload, parsed.payload_len,
                                                  idle_timeout_ms, &deadline, false) < 0)
        return -1;
    *hdr = parsed;
    ch->bytes_recv += sizeof(wire) + (uint64_t)parsed.payload_len;
    return 0;
}

void fct_channel_close(fct_channel_t* ch) {
    if (ch && ch->sockfd >= 0) {
        close(ch->sockfd);
        ch->sockfd = -1;
    }
}
