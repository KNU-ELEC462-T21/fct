#include "progress.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    fct_progress_state_t state;

    assert(fct_progress_init(&state, 1000, stdout) == 0);

    fct_progress_on_chunk(&state, 200, 100);
    fct_progress_on_chunk(&state, 300, 150);

    assert(state.completed_bytes == 500);
    assert(state.payload_bytes == 250);

    fct_progress_on_chunk(&state, 500, 250);

    assert(state.completed_bytes == 1000);
    assert(state.payload_bytes == 500);

    fct_progress_finish(&state);
    fct_progress_destroy(&state);

    printf("Progress tests passed!\n");
    return 0;
}
