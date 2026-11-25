#include "cli.h"

#include <assert.h>
#include <stdio.h>

static int parse(int argc, char **argv, fct_options_t *options)
{
    return fct_parse_options(argc, argv, options, stderr);
}

int main(void)
{
    fct_options_t options;

    char *normal[] = {
        "fct", "-c", "127.0.0.1", "-f", "test.bin",
        "-t", "4", "-z", NULL
    };

    assert(parse(8, normal, &options) == 0);
    assert(options.mode == FCT_MODE_CLIENT);
    assert(options.worker_count == 4);
    assert(options.compression_enabled == 1);

    char *invalid_workers[] = {
        "fct", "-c", "127.0.0.1", "-f", "test.bin",
        "-t", "9", NULL
    };
    assert(parse(7, invalid_workers, &options) == -1);

    char *invalid_zero[] = {
        "fct", "-c", "127.0.0.1", "-f", "test.bin",
        "-t", "0", NULL
    };
    assert(parse(7, invalid_zero, &options) == -1);

    char *server_compression[] = {
        "fct", "-s", "-z", NULL
    };
    assert(parse(3, server_compression, &options) == -1);

    char *defaults[] = {
        "fct", "-s", NULL
    };
    assert(parse(2, defaults, &options) == 0);
    assert(options.worker_count == 1);
    assert(options.compression_enabled == 0);

    printf("Extended CLI tests passed!\n");
    return 0;
}
