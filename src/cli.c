#include "cli.h"
#include "common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

void fct_print_usage(FILE* stream, const char* program) {
    fprintf(stream,
            "Usage: %s -s [-p port] [-d output_dir]\n"
            "       %s -c server_ip -f file_path [-p port]\n"
            "       %s -h\n"
            "  -s          Receive files (server mode)\n"
            "  -c address  Send a file to an IPv4 address\n"
            "  -f path     File to send (client mode)\n"
            "  -p port     TCP port, 1-65535 (default: 9000)\n"
            "  -d dir      Receive directory (default: .)\n"
            "  -h          Show this help\n",
            program, program, program);
}

int fct_parse_options(int argc, char** argv, fct_options_t* options, FILE* err) {
    int opt, server = 0;
    const char *address = NULL, *file = NULL, *directory = NULL;
    int port = FCT_DEFAULT_PORT;
    struct in_addr ip;
    opterr = 0;
    optind = 1;
    while ((opt = getopt(argc, argv, "sc:f:p:d:h")) != -1) {
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
        case 'p': {
            char* end;
            long value;
            errno = 0;
            value = strtol(optarg, &end, 10);
            if (errno || end == optarg || *end || value < 1 || value > 65535) {
                fprintf(err, "Invalid port: %s\n", optarg);
                return -1;
            }
            port = (int)value;
            break;
        }
        case 'h':
            return 1;
        default:
            fprintf(err, "Unknown option or missing option argument.\n");
            return -1;
        }
    }
    if (optind != argc || (server && address) || (!server && !address) || (server && file) ||
        (address && (!file || !*file || directory)) || (directory && !*directory)) {
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
    return 0;
}
