#include "cli.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static int parse(int argc, char** argv, fct_options_t* options, FILE* err) {
    return fct_parse_options(argc, argv, options, err);
}

int main(void) {
    fct_options_t options;
    FILE* err = tmpfile();
    CHECK(err != NULL);

    char* normal[] = {"fct", "-c", "127.0.0.1", "-f", "test.bin", "-t", "4", "-z", NULL};
    CHECK(parse(8, normal, &options, err) == 0);
    CHECK(options.mode == FCT_MODE_CLIENT);
    CHECK(options.worker_count == 4);
    CHECK(options.force_compression);

    char* client_defaults[] = {"fct", "-c", "127.0.0.1", "-f", "test.bin", NULL};
    CHECK(parse(5, client_defaults, &options, err) == 0);
    CHECK(options.mode == FCT_MODE_CLIENT);
    CHECK(options.worker_count == 1);
    CHECK(!options.force_compression);

    char* invalid_workers[] = {"fct", "-c", "127.0.0.1", "-f", "test.bin", "-t", "9", NULL};
    CHECK(parse(7, invalid_workers, &options, err) == -1);

    char* invalid_zero[] = {"fct", "-c", "127.0.0.1", "-f", "test.bin", "-t", "0", NULL};
    CHECK(parse(7, invalid_zero, &options, err) == -1);

    char* invalid_alpha[] = {"fct", "-c", "127.0.0.1", "-f", "test.bin", "-t", "abc", NULL};
    CHECK(parse(7, invalid_alpha, &options, err) == -1);

    char* invalid_neg[] = {"fct", "-c", "127.0.0.1", "-f", "test.bin", "-t", "-1", NULL};
    CHECK(parse(7, invalid_neg, &options, err) == -1);

    char* server_compression[] = {"fct", "-s", "-z", NULL};
    CHECK(parse(3, server_compression, &options, err) == -1);

    char* server_workers_one[] = {"fct", "-s", "-t", "1", NULL};
    CHECK(parse(4, server_workers_one, &options, err) == -1);

    char* server_workers_four[] = {"fct", "-s", "-t", "4", NULL};
    CHECK(parse(4, server_workers_four, &options, err) == -1);

    char* defaults[] = {"fct", "-s", NULL};
    CHECK(parse(2, defaults, &options, err) == 0);
    CHECK(options.worker_count == 1);
    CHECK(!options.force_compression);

    fclose(err);
    printf("Extended CLI tests passed!\n");
    return 0;
}
