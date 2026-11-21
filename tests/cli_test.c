#include "cli.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    fct_options_t opts;
    FILE *err = tmpfile();
    char *server[] = {"fct", "-s", NULL};
    char *client[] = {"fct", "-c", "127.0.0.1", "-f", "test.bin", "-p", "65535", NULL};
    char *custom[] = {"fct", "-s", "-d", "/tmp", "-p", "1", NULL};
    char *both[] = {"fct", "-s", "-c", "127.0.0.1", "-f", "x", NULL};
    char *missing[] = {"fct", "-c", "127.0.0.1", NULL};
    char *badport[] = {"fct", "-s", "-p", "65536", NULL};
    char *badip[] = {"fct", "-c", "bad", "-f", "x", NULL};
    char *wrongdir[] = {"fct", "-c", "127.0.0.1", "-f", "x", "-d", ".", NULL};
    char *extra[] = {"fct", "-s", "extra", NULL};
    char *help[] = {"fct", "-h", NULL};
    assert(err);
    assert(fct_parse_options(2, server, &opts, err) == 0);
    assert(opts.mode == FCT_MODE_SERVER && opts.port == 9000 && strcmp(opts.output_dir, ".") == 0);
    assert(fct_parse_options(7, client, &opts, err) == 0);
    assert(opts.mode == FCT_MODE_CLIENT && opts.port == 65535 && strcmp(opts.file_path, "test.bin") == 0);
    assert(fct_parse_options(6, custom, &opts, err) == 0);
    assert(opts.port == 1 && strcmp(opts.output_dir, "/tmp") == 0);
    assert(fct_parse_options(6, both, &opts, err) == -1);
    assert(fct_parse_options(3, missing, &opts, err) == -1);
    assert(fct_parse_options(4, badport, &opts, err) == -1);
    assert(fct_parse_options(5, badip, &opts, err) == -1);
    assert(fct_parse_options(7, wrongdir, &opts, err) == -1);
    assert(fct_parse_options(3, extra, &opts, err) == -1);
    assert(fct_parse_options(2, help, &opts, err) == 1);
    fclose(err);
    puts("CLI tests passed");
    return 0;
}
