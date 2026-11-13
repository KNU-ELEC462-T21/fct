#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s -s [-p port] [-d output_dir]\n"
            "       %s -c server_ip -f file_path [-p port]\n",
            program,
            program);
}

int main(int argc, char **argv) {
    int opt, server = 0, port = FCT_DEFAULT_PORT;
    const char *address = NULL, *file = NULL, *directory = NULL;

    opterr = 0;
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
            char *end;
            long value;
            errno = 0;
            value = strtol(optarg, &end, 10);
            if (errno || end == optarg || *end != '\0' || value < 1 ||
                value > 65535) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            port = (int)value;
            break;
        }
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind != argc || (server && address) || (!server && !address) ||
        (server && file) ||
        (address && (!*address || !file || !*file || directory)) ||
        (directory && !*directory)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    (void)port;
    fprintf(stderr, "File transfer is not implemented yet.\n");

    return EXIT_FAILURE;
}
