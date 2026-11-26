#ifndef NETWORK_H
#define NETWORK_H
#include <stdint.h>
#include <time.h>

#include "common.h"

typedef struct {
    int sockfd;
    char peer_ip[64];
    int peer_port;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
} fct_channel_t;

int fct_channel_connect(fct_channel_t* ch, const char* server_ip, int port);
int fct_server_listen(int port);
int fct_channel_accept(fct_channel_t* ch, int listen_fd);
int fct_channel_send_meta(fct_channel_t* ch, const fct_file_meta_t* meta);
int fct_channel_recv_meta(fct_channel_t* ch, fct_file_meta_t* out_meta);
int fct_channel_send_chunk(fct_channel_t* ch, const fct_chunk_header_t* hdr, const void* payload);
/* Packet functions return 0 on success, -1 on error with errno set.
 * Counters include complete successful packets only. After an I/O or protocol
 * error the stream may be out of sync; close the channel before reuse.
 * A zero-length payload may use NULL and capacity zero. */
int fct_channel_recv_chunk(fct_channel_t* ch, fct_chunk_header_t* out_hdr, void* payload_buf,
                           size_t payload_capacity);

/* These calls take an absolute CLOCK_MONOTONIC deadline. They return 0 on
 * success and -1 with errno set on failure, including ETIMEDOUT at the deadline. */
int fct_channel_connect_until(fct_channel_t* ch, const char* server_ip, int port,
                              const struct timespec* deadline);
int fct_channel_accept_until(fct_channel_t* ch, int listen_fd, const struct timespec* deadline);
int fct_channel_send_control_until(fct_channel_t* ch, const fct_control_frame_t* frame,
                                   const struct timespec* deadline);
int fct_channel_recv_control_until(fct_channel_t* ch, fct_control_frame_t* frame,
                                   const struct timespec* deadline);
int fct_channel_send_meta_until(fct_channel_t* ch, const fct_file_meta_t* meta,
                                const struct timespec* deadline);
int fct_channel_recv_meta_until(fct_channel_t* ch, fct_file_meta_t* meta,
                                const struct timespec* deadline);

/* Wire format: magic(u32) | byte_len(u32) | bits[byte_len] (big-endian).
 * byte_len must not exceed FCT_MAX_BITMAP_BYTES; receive verifies byte_len
 * matches expected_byte_len and returns -1/EPROTO otherwise. */
int fct_channel_send_bitmap_until(fct_channel_t* ch, const uint8_t* bits, size_t byte_len,
                                  const struct timespec* deadline);
int fct_channel_recv_bitmap_until(fct_channel_t* ch, uint8_t* bits, size_t expected_byte_len,
                                  const struct timespec* deadline);

/* The idle timeout covers the header and payload. Only bytes transferred
 * reset it. Return 0 on success or -1 with errno set. The receive call also
 * returns 1 for EOF before the next header and -1/EPROTO for a truncated frame. */
int fct_channel_send_chunk_idle(fct_channel_t* ch, const fct_chunk_header_t* hdr,
                                const void* payload, unsigned idle_timeout_ms);
int fct_channel_recv_chunk_idle(fct_channel_t* ch, fct_chunk_header_t* hdr, void* payload,
                                size_t capacity, unsigned idle_timeout_ms);

/* Close only a channel initialized by connect/accept or with sockfd set to -1.
 */
void fct_channel_close(fct_channel_t* ch);
#endif
