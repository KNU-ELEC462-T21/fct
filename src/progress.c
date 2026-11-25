#define _POSIX_C_SOURCE 200809L

#include "progress.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

static void print_progress(fct_progress_state_t *state,
                           const struct timespec *now)
{
    double elapsed = elapsed_seconds(&state->started_at, now);

    double percent = state->total_bytes == 0
                         ? 100.0
                         : 100.0 * (double)state->completed_bytes /
                               (double)state->total_bytes;

    if (percent > 100.0)
        percent = 100.0;

    double speed = elapsed > 0.0
                       ? (double)state->completed_bytes / elapsed
                       : 0.0;

    double saved = state->completed_bytes == 0
                       ? 0.0
                       : 100.0 * (1.0 -
                           (double)state->payload_bytes /
                           (double)state->completed_bytes);

    double remaining = speed > 0.0
                           ? (double)(state->total_bytes -
                                      state->completed_bytes) / speed
                           : 0.0;

    fprintf(state->output,
            "\rProgress: %6.2f%% | %.2f MiB/s | "
            "Saved: %.1f%% | ETA: %.1fs   ",
            percent,
            speed / (1024.0 * 1024.0),
            saved,
            remaining);

    fflush(state->output);
}

int fct_progress_init(fct_progress_state_t *state,
                      uint64_t total_bytes, FILE *output)
{
    if (!state || !output) {
        errno = EINVAL;
        return -1;
    }

    memset(state, 0, sizeof(*state));

    int err = pthread_mutex_init(&state->mutex, NULL);
    if (err != 0) {
        errno = err;
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &state->started_at) != 0) {
        int saved = errno;
        pthread_mutex_destroy(&state->mutex);
        errno = saved;
        return -1;
    }

    state->last_print = state->started_at;
    state->total_bytes = total_bytes;
    state->output = output;

    return 0;
}

void fct_progress_on_chunk(void *context,
                           size_t raw_len, size_t payload_len)
{
    if (!context)
        return;

    fct_progress_state_t *state = context;
    pthread_mutex_lock(&state->mutex);

    uint64_t remaining = state->total_bytes -
                         state->completed_bytes;

    if (raw_len > remaining)
        raw_len = (size_t)remaining;

    state->completed_bytes += raw_len;

    if (UINT64_MAX - state->payload_bytes < payload_len)
        state->payload_bytes = UINT64_MAX;
    else
        state->payload_bytes += payload_len;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        if (elapsed_seconds(&state->last_print, &now) >= 0.2 ||
            state->completed_bytes == state->total_bytes) {
            print_progress(state, &now);
            state->last_print = now;
        }
    }

    pthread_mutex_unlock(&state->mutex);
}

void fct_progress_finish(fct_progress_state_t *state)
{
    if (!state)
        return;

    pthread_mutex_lock(&state->mutex);

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        print_progress(state, &now);

    fprintf(state->output, "\n");
    fflush(state->output);

    pthread_mutex_unlock(&state->mutex);
}

void fct_progress_destroy(fct_progress_state_t *state)
{
    if (state)
        pthread_mutex_destroy(&state->mutex);
}
