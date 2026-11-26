#ifndef FCT_PROGRESS_H
#define FCT_PROGRESS_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    uint64_t total_bytes;
    uint64_t completed_bytes;
    uint64_t payload_bytes;
    struct timespec started_at;
    struct timespec last_print;
    FILE* output;
    bool has_printed;
} fct_progress_state_t;

int fct_progress_init(fct_progress_state_t* state, uint64_t total_bytes, FILE* output);

void fct_progress_on_chunk(void* context, size_t raw_len, size_t payload_len);

void fct_progress_finish(fct_progress_state_t* state);

void fct_progress_abort(fct_progress_state_t* state);

void fct_progress_destroy(fct_progress_state_t* state);

#endif
