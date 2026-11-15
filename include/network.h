#ifndef NETWORK_H
#define NETWORK_H
#include <stdint.h>

#include "common.h"

typedef struct {
    int sockfd;
    char peer_ip[64];
    int peer_port;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
} fct_channel_t;

int fct_channel_connect(fct_channel_t *ch, const char *server_ip, int port);
int fct_server_listen(int port);
int fct_channel_accept(fct_channel_t *ch, int listen_fd);
int fct_channel_send_meta(fct_channel_t *ch, const fct_file_meta_t *meta);
int fct_channel_recv_meta(fct_channel_t *ch, fct_file_meta_t *out_meta);
int fct_channel_send_chunk(fct_channel_t *ch,
                           const fct_chunk_header_t *hdr,
                           const void *payload);
/* Packet functions return 0 on success, -1 on error with errno set.
 * Counters include complete successful packets only. After an I/O or protocol
 * error the stream may be out of sync; close the channel before reuse.
 * A zero-length payload may use NULL and capacity zero. */
int fct_channel_recv_chunk(fct_channel_t *ch,
                           fct_chunk_header_t *out_hdr,
                           void *payload_buf,
                           size_t payload_capacity);
/* Close only a channel initialized by connect/accept or with sockfd set to -1.
 */
void fct_channel_close(fct_channel_t *ch);
#endif
