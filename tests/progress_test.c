#include "progress.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void read_stream(FILE* stream, char* buf, size_t size) {
    rewind(stream);
    size_t n = fread(buf, 1, size - 1, stream);
    buf[n] = '\0';
}

static void test_invalid_args(void) {
    fct_progress_state_t state;
    FILE* out = tmpfile();
    CHECK(out != NULL);

    errno = 0;
    CHECK(fct_progress_init(NULL, 100, out) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(fct_progress_init(&state, 100, NULL) == -1 && errno == EINVAL);

    fclose(out);
}

static void test_empty_file_and_zero_denominators(void) {
    fct_progress_state_t state;
    FILE* out = tmpfile();
    char buf[512];
    CHECK(out != NULL);

    CHECK(fct_progress_init(&state, 0, out) == 0);
    fct_progress_finish(&state);
    fct_progress_destroy(&state);

    read_stream(out, buf, sizeof(buf));
    CHECK(strstr(buf, "100.00%") != NULL);
    CHECK(strstr(buf, "Saved: —") != NULL);
    CHECK(strstr(buf, "ETA: —") != NULL);
    fclose(out);

    out = tmpfile();
    CHECK(out != NULL);
    CHECK(fct_progress_init(&state, 1000, out) == 0);
    fct_progress_finish(&state);
    fct_progress_destroy(&state);

    read_stream(out, buf, sizeof(buf));
    CHECK(strstr(buf, "  0.00%") != NULL);
    CHECK(strstr(buf, "Saved: —") != NULL);
    CHECK(strstr(buf, "ETA: —") != NULL);
    fclose(out);
}

static void test_clamping_and_abort(void) {
    fct_progress_state_t state;
    FILE* out = tmpfile();
    char buf[512];
    CHECK(out != NULL);

    CHECK(fct_progress_init(&state, 1000, out) == 0);
    fct_progress_abort(&state);
    read_stream(out, buf, sizeof(buf));
    CHECK(buf[0] == '\0');

    state.last_print.tv_sec -= 2;
    fct_progress_on_chunk(&state, 600, 300);
    fct_progress_on_chunk(&state, 600, 300); /* raw_len clamped to remaining 400 */
    CHECK(state.completed_bytes == 1000);
    CHECK(state.payload_bytes == 600);
    CHECK(state.has_printed);

    fct_progress_abort(&state);
    fct_progress_destroy(&state);

    read_stream(out, buf, sizeof(buf));
    size_t len = strlen(buf);
    CHECK(len > 0 && buf[len - 1] == '\n');
    fclose(out);
}

typedef struct {
    fct_progress_state_t* state;
    int chunks;
    size_t raw_len;
    size_t payload_len;
} worker_arg_t;

static void* worker_main(void* arg) {
    worker_arg_t* w = arg;
    for (int i = 0; i < w->chunks; i++)
        fct_progress_on_chunk(w->state, w->raw_len, w->payload_len);
    return NULL;
}

static void test_multithreaded_aggregation(void) {
    enum { THREADS = 4, CHUNKS_PER_THREAD = 100 };
    const size_t raw_len = 256;
    const size_t payload_len = 128;
    const uint64_t total = (uint64_t)THREADS * CHUNKS_PER_THREAD * raw_len;

    fct_progress_state_t state;
    FILE* out = tmpfile();
    CHECK(out != NULL);
    CHECK(fct_progress_init(&state, total, out) == 0);

    pthread_t threads[THREADS];
    worker_arg_t arg = {
        .state = &state,
        .chunks = CHUNKS_PER_THREAD,
        .raw_len = raw_len,
        .payload_len = payload_len,
    };

    for (int i = 0; i < THREADS; i++)
        CHECK(pthread_create(&threads[i], NULL, worker_main, &arg) == 0);
    for (int i = 0; i < THREADS; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);

    CHECK(state.completed_bytes == total);
    CHECK(state.payload_bytes == (uint64_t)THREADS * CHUNKS_PER_THREAD * payload_len);

    fct_progress_finish(&state);
    fct_progress_destroy(&state);

    char buf[512];
    read_stream(out, buf, sizeof(buf));
    CHECK(strstr(buf, "100.00%") != NULL);
    CHECK(strstr(buf, "Saved: 50.0%") != NULL);
    fclose(out);
}

int main(void) {
    test_invalid_args();
    test_empty_file_and_zero_denominators();
    test_clamping_and_abort();
    test_multithreaded_aggregation();
    printf("Progress tests passed!\n");
    return 0;
}
