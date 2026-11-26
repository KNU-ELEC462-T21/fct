#include "cli.h"
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

void fct_print_usage(FILE* stream, const char* program) {
    fprintf(stream,
            "Usage: %s -s [-p port] [-d dir]\n"
            "       %s -c ip -f file [-p port] [-t workers] [-z]\n"
            "       %s -h\n"
            "\n"
            "Options:\n"
            "  -s          Server mode\n"
            "  -c ip       Server IPv4 address\n"
            "  -f file     Input file\n"
            "  -p port     TCP port (default: 9000)\n"
            "  -d dir      Output directory (default: .)\n"
            "  -t workers  Data workers, client only (1-8, default: 1)\n"
            "  -z          Force compression, client only (bypass auto-skip)\n"
            "  -h          Show help\n",
            program, program, program);
}

static int parse_number(const char* text, long min, long max, int* out) {
    char* end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno || end == text || *end != '\0' || value < min || value > max)
        return -1;

    *out = (int)value;
    return 0;
}

int fct_parse_options(int argc, char** argv, fct_options_t* options, FILE* err) {
    if (!options || !err)
        return -1;

    int opt;
    int server = 0;
    int port = FCT_DEFAULT_PORT;
    int workers = 1;
    int workers_set = 0;
    bool force_compression = false;

    const char* address = NULL;
    const char* file = NULL;
    const char* directory = NULL;

    struct in_addr ip;

    opterr = 0;
    optind = 1;

    while ((opt = getopt(argc, argv, "sc:f:p:d:t:zh")) != -1) {
        switch (opt) {
        case 's':
            server = 1;
            break;

        case 'c':
            address = optarg;
            break;

        case 'f':
            file = optarg;
            break;

        case 'd':
            directory = optarg;
            break;

        case 'p':
            if (parse_number(optarg, 1, 65535, &port) < 0) {
                fprintf(err, "Invalid port: %s\n", optarg);
                return -1;
            }
            break;

        case 't':
            if (parse_number(optarg, 1, FCT_MAX_DATA_WORKERS, &workers) < 0) {
                fprintf(err, "Invalid worker count: %s\n", optarg);
                return -1;
            }
            workers_set = 1;
            break;

        case 'z':
            force_compression = true;
            break;

        case 'h':
            return 1;

        default:
            fprintf(err, "Unknown or missing option.\n");
            return -1;
        }
    }

    if (optind != argc || (server && address) || (!server && !address) || (server && file) ||
        (address && (!file || !*file || directory)) || (directory && !*directory) ||
        (address && !*address) || (server && workers_set) || (server && force_compression)) {
        fprintf(err, "Invalid combination of options.\n");
        return -1;
    }

    if (address && inet_pton(AF_INET, address, &ip) != 1) {
        fprintf(err, "Invalid IPv4 address: %s\n", address);
        return -1;
    }

    options->mode = server ? FCT_MODE_SERVER : FCT_MODE_CLIENT;
    options->server_ip = address;
    options->file_path = file;
    options->output_dir = directory ? directory : ".";
    options->port = port;
    options->worker_count = workers;
    options->force_compression = force_compression;

    return 0;
}
