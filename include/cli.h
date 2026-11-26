#ifndef FCT_CLI_H
#define FCT_CLI_H

#include <stdbool.h>
#include <stdio.h>

typedef enum { FCT_MODE_SERVER, FCT_MODE_CLIENT } fct_mode_t;

typedef struct {
    fct_mode_t mode;
    const char* server_ip;
    const char* file_path;
    const char* output_dir;
    int port;
    int worker_count;
    bool force_compression;
} fct_options_t;

/* 0: success, 1: help, -1: invalid options */
int fct_parse_options(int argc, char** argv, fct_options_t* options, FILE* err);

void fct_print_usage(FILE* stream, const char* program);

#endif
