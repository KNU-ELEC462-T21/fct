#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli.h"
#include "file_reader.h"
#include "file_writer.h"
#include "network.h"
#include "progress.h"

static int send_file(const fct_options_t* options) {
    const char* name = strrchr(options->file_path, '/');
    name = name ? name + 1 : options->file_path;
    fct_file_meta_t meta = {.magic = FCT_MAGIC};
    if (strlen(name) >= sizeof(meta.filename)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(meta.filename, name);

    file_reader_t reader;
    if (fr_init(&reader, options->file_path, DEFAULT_CHUNK_SZ) < 0)
        return -1;

    meta.file_size = reader.file_size;
    meta.chunk_size = reader.chunk_size;
    meta.total_chunks = reader.total_chunks;

    fct_channel_t channel = {.sockfd = -1};
    void* payload = NULL;
    int result = -1, saved;
    if (meta.total_chunks != 0) {
        payload = malloc(meta.chunk_size);
        if (!payload)
            goto cleanup;
    }
    if (fct_channel_connect(&channel, options->server_ip, options->port) < 0 ||
        fct_channel_send_meta(&channel, &meta) < 0)
        goto cleanup;

    fct_progress_state_t progress;
    if (fct_progress_init(&progress, meta.file_size, stderr) < 0)
        goto cleanup;

    for (uint32_t id = 0; id < meta.total_chunks; ++id) {
        ssize_t length = fr_read_chunk(&reader, id, payload);
        if (length < 0)
            goto cleanup;
        fct_chunk_header_t header = {
            .magic = FCT_MAGIC,
            .chunk_id = id,
            .offset = (uint64_t)id * meta.chunk_size,
            .payload_len = (uint32_t)length,
        };
        if (fct_channel_send_chunk(&channel, &header, payload) < 0) {
            fct_progress_destroy(&progress);
            goto cleanup;
        }

        fct_progress_on_chunk(&progress, (size_t)length, (size_t)length);
    }
    fct_progress_finish(&progress);
    fct_progress_destroy(&progress);
    result = 0;

cleanup:
    saved = errno;
    fct_channel_close(&channel);
    fr_close(&reader);
    free(payload);
    errno = saved;
    return result;
}

static int receive_chunks(fct_channel_t* channel, file_writer_t* writer) {
    if (writer->total_chunks == 0)
        return 0;
    void* payload = malloc(writer->chunk_size);
    if (!payload)
        return -1;

    int result = -1, saved;
    for (uint32_t id = 0; id < writer->total_chunks; ++id) {
        uint64_t offset = (uint64_t)id * writer->chunk_size;
        uint64_t remaining = writer->file_size - offset;
        size_t length = remaining < writer->chunk_size ? (size_t)remaining : writer->chunk_size;
        fct_chunk_header_t header;
        if (fct_channel_recv_chunk(channel, &header, payload, length) < 0)
            goto cleanup;
        if (header.chunk_id != id || header.offset != offset || header.payload_len != length) {
            errno = EPROTO;
            goto cleanup;
        }
        if (fw_write_chunk(writer, id, payload, length) < 0)
            goto cleanup;
    }
    result = 0;

cleanup:
    saved = errno;
    free(payload);
    errno = saved;
    return result;
}

static char* join_path(const char* directory, const char* name) {
    size_t directory_length = strlen(directory);
    size_t name_size = strlen(name) + 1;
    char* path = malloc(directory_length + 1 + name_size);
    if (!path)
        return NULL;
    memcpy(path, directory, directory_length);
    path[directory_length] = '/';
    memcpy(path + directory_length + 1, name, name_size);
    return path;
}

static int receive_file(fct_channel_t* channel, const char* directory) {
    fct_file_meta_t meta;
    if (fct_channel_recv_meta(channel, &meta) < 0)
        return -1;

    if (!memchr(meta.filename, '\0', sizeof(meta.filename)) || !meta.filename[0] ||
        strchr(meta.filename, '/') || strcmp(meta.filename, ".") == 0 ||
        strcmp(meta.filename, "..") == 0 || meta.chunk_size == 0 ||
        meta.chunk_size > DEFAULT_CHUNK_SZ ||
        meta.total_chunks !=
            meta.file_size / meta.chunk_size + (meta.file_size % meta.chunk_size != 0)) {
        errno = EPROTO;
        return -1;
    }

    file_writer_t writer = {.fd = -1};
    char* destination = join_path(directory, meta.filename);
    if (!destination)
        return -1;
    char* temporary_dir = join_path(directory, ".fct-XXXXXX");
    char* temporary_file = NULL;
    char* created_dir = temporary_dir ? mkdtemp(temporary_dir) : NULL;
    int result = -1;
    int saved;
    if (!created_dir)
        goto cleanup;

    temporary_file = join_path(temporary_dir, "data");
    if (temporary_file &&
        fw_init(&writer, temporary_file, meta.file_size, meta.chunk_size) == 0 &&
        receive_chunks(channel, &writer) == 0 && fsync(writer.fd) == 0) {
        /* Publish only a complete file and preserve any existing destination. */
        result = link(temporary_file, destination);
    }
cleanup:
    saved = errno;
    fw_close(&writer);
    if (temporary_file)
        unlink(temporary_file);
    if (created_dir)
        rmdir(created_dir);
    free(temporary_file);
    free(temporary_dir);
    free(destination);
    errno = saved;
    return result;
}

static int run_server(const fct_options_t* options) {
    struct stat st;
    if (stat(options->output_dir, &st) < 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    int listener = fct_server_listen(options->port);
    if (listener < 0)
        return -1;
    fprintf(stderr, "Listening on port %d\n", options->port);
    fct_channel_t channel;
    int result = fct_channel_accept(&channel, listener);
    int saved = errno;
    close(listener);
    if (result < 0) {
        errno = saved;
        return -1;
    }

    result = receive_file(&channel, options->output_dir);
    saved = errno;
    fct_channel_close(&channel);
    errno = saved;
    return result;
}

int main(int argc, char** argv) {
    fct_options_t options;
    int result = fct_parse_options(argc, argv, &options, stderr);

    if (result != 0) {
        fct_print_usage(result > 0 ? stdout : stderr, argv[0]);
        return result > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (options.mode == FCT_MODE_SERVER) {
        if (run_server(&options) == 0)
            return EXIT_SUCCESS;
        fprintf(stderr, "Receive into '%s': %s\n", options.output_dir, strerror(errno));
    } else {
        if (send_file(&options) == 0)
            return EXIT_SUCCESS;
        fprintf(stderr, "Send '%s' to %s:%d: %s\n", options.file_path, options.server_ip,
                options.port, strerror(errno));
    }
    return EXIT_FAILURE;
}
