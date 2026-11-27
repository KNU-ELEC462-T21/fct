#define _POSIX_C_SOURCE 200809L

#include "checksum.h"
#include "common.h"
#include "compress.h"
#include "network.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static int g_next_port = 19200;

static const char* get_fct_bin(void) {
    const char* env = getenv("FCT_BIN");
    return (env && *env) ? env : "./build/fct";
}

static struct timespec deadline_after(unsigned ms) {
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    ts.tv_sec += (time_t)(ms / 1000U);
    ts.tv_nsec += (long)((ms % 1000U) * 1000000UL);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static void make_temp_dir(char* buf, size_t size) {
    snprintf(buf, size, "/tmp/fct_int_XXXXXX");
    CHECK(mkdtemp(buf) != NULL);
}

static void remove_tree(const char* dir) {
    DIR* d = opendir(dir);
    if (!d)
        return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(path);
        else
            unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static bool dir_has_temp_entries(const char* dir) {
    DIR* d = opendir(dir);
    CHECK(d != NULL);
    struct dirent* ent;
    bool found = false;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, ".fct-", 5) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void write_file_bytes(const char* path, const unsigned char* data, size_t len) {
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    if (len > 0)
        CHECK(fwrite(data, 1, len, f) == len);
    CHECK(fclose(f) == 0);
}

static bool files_equal(const char* a, const char* b) {
    FILE* fa = fopen(a, "rb");
    FILE* fb = fopen(b, "rb");
    if (!fa || !fb) {
        if (fa)
            fclose(fa);
        if (fb)
            fclose(fb);
        return false;
    }
    unsigned char ba[4096], bb[4096];
    bool eq = true;
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || (na > 0 && memcmp(ba, bb, na) != 0)) {
            eq = false;
            break;
        }
        if (na == 0)
            break;
    }
    fclose(fa);
    fclose(fb);
    return eq;
}

static unsigned char* make_text_bytes(size_t len) {
    static const char words[] = "system programming parallel tcp compression transfer ";
    unsigned char* buf = malloc(len ? len : 1);
    CHECK(buf != NULL);
    for (size_t i = 0; i < len; ++i)
        buf[i] = (unsigned char)words[i % (sizeof(words) - 1)];
    return buf;
}

static unsigned char* make_random_bytes(size_t len, uint32_t seed) {
    unsigned char* buf = malloc(len ? len : 1);
    CHECK(buf != NULL);
    uint32_t x = seed ? seed : 1;
    for (size_t i = 0; i < len; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (unsigned char)(x & 0xff);
    }
    return buf;
}

static unsigned char* make_zip_bytes(size_t len, uint32_t seed) {
    unsigned char* buf = make_random_bytes(len, seed);
    if (len >= 4)
        memcpy(buf, "PK\x03\x04", 4);
    return buf;
}

static unsigned char* make_mp4_bytes(size_t len, uint32_t seed) {
    unsigned char* buf = make_random_bytes(len, seed);
    if (len >= 12)
        memcpy(buf,
               "\x00\x00\x00\x18"
               "ftypisom",
               12);
    return buf;
}

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int wait_pid_timeout(pid_t pid, int timeout_ms, int* exit_code) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            if (WIFEXITED(status)) {
                *exit_code = WEXITSTATUS(status);
                return 0;
            }
            if (WIFSIGNALED(status)) {
                *exit_code = 128 + WTERMSIG(status);
                return 0;
            }
            return -1;
        }
        CHECK(rc == 0);
        sleep_ms(10);
        elapsed += 10;
    }
    return -1;
}

static pid_t spawn_server_ready(const char* out_dir, int* out_port) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        int port = g_next_port++;
        if (g_next_port > 60000)
            g_next_port = 19200;

        int err_pipe[2];
        CHECK(pipe(err_pipe) == 0);

        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            close(err_pipe[0]);
            dup2(err_pipe[1], STDERR_FILENO);
            close(err_pipe[1]);
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%d", port);
            execl(get_fct_bin(), "fct", "-s", "-p", port_str, "-d", out_dir, (char*)NULL);
            _exit(127);
        }

        close(err_pipe[1]);
        char buf[512];
        size_t pos = 0;
        bool ready = false;
        int elapsed = 0;
        while (elapsed < 5000) {
            struct pollfd pfd = {.fd = err_pipe[0], .events = POLLIN};
            int prc = poll(&pfd, 1, 50);
            if (prc > 0) {
                ssize_t n = read(err_pipe[0], buf + pos, sizeof(buf) - 1 - pos);
                if (n <= 0)
                    break;
                pos += (size_t)n;
                buf[pos] = '\0';
                if (strstr(buf, "Listening on port ") != NULL) {
                    ready = true;
                    break;
                }
                if (pos == sizeof(buf) - 1)
                    break;
            } else {
                elapsed += 50;
            }
        }
        close(err_pipe[0]);
        if (ready) {
            *out_port = port;
            return pid;
        }
        int code = -1;
        if (wait_pid_timeout(pid, 1000, &code) < 0) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }
    }
    CHECK(false && "Failed to start server on an ephemeral port");
    return -1;
}

static int run_client_capture(int port, const char* file_path, const char* workers_str,
                              bool force_z, char* stderr_buf, size_t stderr_cap) {
    int err_pipe[2];
    CHECK(pipe(err_pipe) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(err_pipe[0]);
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        if (force_z) {
            execl(get_fct_bin(), "fct", "-c", "127.0.0.1", "-f", file_path, "-p", port_str, "-t",
                  workers_str, "-z", (char*)NULL);
        } else {
            execl(get_fct_bin(), "fct", "-c", "127.0.0.1", "-f", file_path, "-p", port_str, "-t",
                  workers_str, (char*)NULL);
        }
        _exit(127);
    }

    close(err_pipe[1]);
    size_t total = 0;
    if (stderr_buf && stderr_cap > 0) {
        while (total < stderr_cap - 1) {
            ssize_t n = read(err_pipe[0], stderr_buf + total, stderr_cap - 1 - total);
            if (n <= 0)
                break;
            total += (size_t)n;
        }
        stderr_buf[total] = '\0';
    }
    close(err_pipe[0]);

    int code = -1;
    CHECK(wait_pid_timeout(pid, 20000, &code) == 0);
    return code;
}

static void test_e2e_case(const char* name, const unsigned char* data, size_t len,
                          const char* workers_str, bool force_z, bool expect_empty_progress) {
    char src_dir[64], dst_dir[64], src_path[256], dst_path[256];
    make_temp_dir(src_dir, sizeof(src_dir));
    make_temp_dir(dst_dir, sizeof(dst_dir));
    snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, name);
    snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, name);
    write_file_bytes(src_path, data, len);

    int port = 0;
    pid_t srv = spawn_server_ready(dst_dir, &port);

    char client_err[2048] = {0};
    int cli_rc =
        run_client_capture(port, src_path, workers_str, force_z, client_err, sizeof(client_err));
    int srv_rc = -1;
    CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);

    CHECK(cli_rc == 0);
    CHECK(srv_rc == 0);
    CHECK(files_equal(src_path, dst_path));
    CHECK(!dir_has_temp_entries(dst_dir));
    if (expect_empty_progress) {
        CHECK(strstr(client_err, "100.00%") != NULL);
        CHECK(strstr(client_err, "Saved: —") != NULL);
    }

    remove_tree(src_dir);
    remove_tree(dst_dir);
}

static void test_all_e2e(void) {
    const size_t three_mb = 3 * 1024 * 1024;
    const size_t five_mb = 5 * 1024 * 1024;
    const size_t tail_len = 2 * (size_t)DEFAULT_CHUNK_SZ + 12345;

    unsigned char* rand3 = make_random_bytes(three_mb, 11);
    unsigned char* rand5 = make_random_bytes(five_mb + 777, 22);
    unsigned char* text5 = make_text_bytes(five_mb);
    unsigned char* zip3 = make_zip_bytes(three_mb, 33);
    unsigned char* mp43 = make_mp4_bytes(three_mb, 44);
    unsigned char* exact1 = make_text_bytes(DEFAULT_CHUNK_SZ);
    unsigned char* tail = make_text_bytes(tail_len);

    test_e2e_case("raw_single.bin", rand3, three_mb, "1", false, false);
    test_e2e_case("raw_multi.bin", rand5, five_mb + 777, "4", false, false);
    test_e2e_case("auto_text.txt", text5, five_mb, "4", false, false);
    test_e2e_case("skip.zip", zip3, three_mb, "4", false, false);
    test_e2e_case("skip.mp4", mp43, three_mb, "4", false, false);
    test_e2e_case("force.zip", zip3, three_mb, "4", true, false);
    test_e2e_case("empty.dat", NULL, 0, "4", false, true);
    test_e2e_case("exact_chunk.txt", exact1, DEFAULT_CHUNK_SZ, "4", false, false);
    test_e2e_case("short_tail.txt", tail, tail_len, "4", false, false);

    free(rand3);
    free(rand5);
    free(text5);
    free(zip3);
    free(mp43);
    free(exact1);
    free(tail);
}

static uint8_t receive_small_bitmap(fct_channel_t* control, uint32_t chunks,
                                    const struct timespec* deadline) {
    uint8_t bits = 0;
    CHECK(chunks <= 8);
    CHECK(fct_channel_recv_bitmap_until(control, &bits, chunks != 0, deadline) == 0);
    return bits;
}

typedef struct {
    int listener;
    uint16_t expected_workers;
    bool expect_compressed;
    bool expect_raw;
    bool drop_result;
    bool retry_last;
    size_t saw_compressed;
    size_t saw_raw;
    uint32_t received_chunks;
    bool ok;
} mock_server_cfg_t;

static void* mock_server_thread(void* arg) {
    mock_server_cfg_t* cfg = arg;
    struct timespec dl = deadline_after(10000);
    fct_channel_t ctrl = {.sockfd = -1};
    fct_channel_t data[FCT_MAX_DATA_WORKERS];
    for (size_t i = 0; i < FCT_MAX_DATA_WORKERS; ++i)
        data[i].sockfd = -1;

    CHECK(fct_channel_accept_until(&ctrl, cfg->listener, &dl) == 0);
    fct_control_frame_t hello;
    fct_file_meta_t meta;
    CHECK(fct_channel_recv_control_until(&ctrl, &hello, &dl) == 0);
    CHECK(hello.type == FCT_CONTROL_HELLO);
    CHECK(hello.arg == cfg->expected_workers);
    CHECK(fct_channel_recv_meta_until(&ctrl, &meta, &dl) == 0);

    uint8_t token[FCT_TRANSFER_TOKEN_SIZE] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                              0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01};
    fct_control_frame_t ready = {.magic = FCT_MAGIC, .type = FCT_CONTROL_READY, .arg = hello.arg};
    memcpy(ready.token, token, FCT_TRANSFER_TOKEN_SIZE);
    CHECK(fct_channel_send_control_until(&ctrl, &ready, &dl) == 0);
    uint8_t completed = 0;
    CHECK(fct_channel_send_bitmap_until(&ctrl, &completed, 1, &dl) == 0);

    bool seen_idx[FCT_MAX_DATA_WORKERS] = {false};
    for (uint16_t i = 0; i < hello.arg; ++i) {
        fct_channel_t ch = {.sockfd = -1};
        CHECK(fct_channel_accept_until(&ch, cfg->listener, &dl) == 0);
        fct_control_frame_t df;
        CHECK(fct_channel_recv_control_until(&ch, &df, &dl) == 0);
        CHECK(df.type == FCT_CONTROL_DATA);
        CHECK(memcmp(df.token, token, FCT_TRANSFER_TOKEN_SIZE) == 0);
        CHECK(df.arg < hello.arg && !seen_idx[df.arg]);
        seen_idx[df.arg] = true;
        data[df.arg] = ch;
    }

    size_t cap = fct_compress_bound(meta.chunk_size);
    if (cap < meta.chunk_size)
        cap = meta.chunk_size;
    unsigned char* buf = malloc(cap);
    CHECK(buf != NULL);

    for (uint16_t i = 0; i < hello.arg; ++i) {
        for (;;) {
            fct_chunk_header_t hdr;
            int rc = fct_channel_recv_chunk_idle(&data[i], &hdr, buf, cap, 5000);
            CHECK(rc == 0);
            if (hdr.flags == FCT_CHUNK_FLAG_EOR)
                break;
            uint64_t off = (uint64_t)hdr.chunk_id * meta.chunk_size;
            uint64_t rem = meta.file_size - off;
            size_t raw_len = rem < meta.chunk_size ? (size_t)rem : meta.chunk_size;
            if (hdr.flags == FCT_CHUNK_FLAG_COMPRESSED) {
                cfg->saw_compressed++;
                if (cfg->expect_compressed)
                    CHECK(hdr.payload_len < raw_len);
            } else {
                CHECK(hdr.flags == 0);
                CHECK(hdr.payload_len == raw_len);
                cfg->saw_raw++;
            }
            cfg->received_chunks++;
        }
    }
    if (cfg->retry_last) {
        fct_control_frame_t retry = ready;
        retry.type = FCT_CONTROL_RETRY;
        retry.arg = 1;
        uint32_t last = meta.total_chunks - 1;
        completed = (uint8_t)((1U << last) - 1);
        CHECK(fct_channel_send_control_until(&ctrl, &retry, &dl) == 0);
        CHECK(fct_channel_send_bitmap_until(&ctrl, &completed, 1, &dl) == 0);
        unsigned retried = 0;
        for (uint16_t i = 0; i < hello.arg; ++i) {
            for (;;) {
                fct_chunk_header_t hdr;
                CHECK(fct_channel_recv_chunk_idle(&data[i], &hdr, buf, cap, 5000) == 0);
                if (hdr.flags == FCT_CHUNK_FLAG_EOR)
                    break;
                CHECK(hdr.chunk_id == last);
                ++retried;
            }
        }
        CHECK(retried == 1);
    }
    for (uint16_t i = 0; i < hello.arg; ++i)
        fct_channel_close(&data[i]);
    free(buf);
    CHECK(cfg->received_chunks == meta.total_chunks);
    if (cfg->expect_compressed)
        CHECK(cfg->saw_compressed == meta.total_chunks && cfg->saw_raw == 0);
    if (cfg->expect_raw)
        CHECK(cfg->saw_raw == meta.total_chunks && cfg->saw_compressed == 0);

    if (!cfg->drop_result) {
        fct_control_frame_t res = {
            .magic = FCT_MAGIC, .type = FCT_CONTROL_RESULT, .arg = FCT_RESULT_OK};
        memcpy(res.token, token, FCT_TRANSFER_TOKEN_SIZE);
        CHECK(fct_channel_send_control_until(&ctrl, &res, &dl) == 0);
    }
    fct_channel_close(&ctrl);
    cfg->ok = true;
    return NULL;
}

static void run_wire_client_check(const char* filename, const unsigned char* data, size_t len,
                                  const char* workers_str, uint16_t expected_workers, bool force_z,
                                  bool expect_compressed, bool expect_raw, bool drop_result,
                                  bool retry_last, int expect_cli_rc) {
    char dir[64], path[256];
    make_temp_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    write_file_bytes(path, data, len);

    int port = g_next_port++;
    int listener = fct_server_listen(port);
    CHECK(listener >= 0);

    mock_server_cfg_t cfg = {
        .listener = listener,
        .expected_workers = expected_workers,
        .expect_compressed = expect_compressed,
        .expect_raw = expect_raw,
        .drop_result = drop_result,
        .retry_last = retry_last,
    };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, mock_server_thread, &cfg) == 0);

    char err_buf[512] = {0};
    int cli_rc = run_client_capture(port, path, workers_str, force_z, err_buf, sizeof(err_buf));
    CHECK(pthread_join(th, NULL) == 0);
    close(listener);
    CHECK(cfg.ok);
    CHECK(cli_rc == expect_cli_rc);

    remove_tree(dir);
}

static void test_wire_client_behaviors(void) {
    const size_t five_mb = 5 * 1024 * 1024;
    const size_t five_mb_plus = 5 * 1024 * 1024 + 100;
    unsigned char* text = make_text_bytes(five_mb);
    unsigned char* zip = make_zip_bytes(five_mb_plus, 77);
    unsigned char* rand_buf = make_random_bytes(five_mb_plus, 88);

    /* Multi-TCP auto compression uses 3 chunks -> 3 workers with -t 4, all compressed. */
    run_wire_client_check("wire.txt", text, five_mb, "4", 3, false, true, false, false, false, 0);
    /* Auto-skip on ZIP and incompressible random data. */
    run_wire_client_check("wire.zip", zip, five_mb_plus, "4", 3, false, false, true, false, false, 0);
    run_wire_client_check("wire.bin", rand_buf, five_mb_plus, "4", 3, false, false, true, false,
                          true, 0);
    /* -z forces compression even on random data. */
    run_wire_client_check("force.bin", rand_buf, five_mb_plus, "4", 3, true, false, false, false,
                          false, 0);
    /* Lost RESULT after transfer must cause non-zero client exit. */
    run_wire_client_check("lost_res.txt", text, five_mb, "4", 3, false, true, false, true, false, 1);

    free(text);
    free(zip);
    free(rand_buf);
}

static void send_eor(fct_channel_t* ch) {
    fct_chunk_header_t eor = {.magic = FCT_MAGIC, .flags = FCT_CHUNK_FLAG_EOR};
    CHECK(fct_channel_send_chunk_idle(ch, &eor, NULL, 5000) == 0);
}

static void test_server_out_of_order_multi(void) {
    char dst_dir[64];
    make_temp_dir(dst_dir, sizeof(dst_dir));
    int port = 0;
    pid_t srv = spawn_server_ready(dst_dir, &port);

    const size_t len = 2 * (size_t)DEFAULT_CHUNK_SZ + 1000;
    unsigned char* data = make_text_bytes(len);

    struct timespec dl = deadline_after(10000);
    fct_channel_t ctrl = {.sockfd = -1}, d0 = {.sockfd = -1}, d1 = {.sockfd = -1};
    CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);

    fct_control_frame_t hello = {.magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO, .arg = 2};
    fct_file_meta_t meta = {
        .magic = FCT_MAGIC,
        .file_size = len,
        .chunk_size = DEFAULT_CHUNK_SZ,
        .total_chunks = 3,
        .filename = "ooo.txt",
    };
    CHECK(fct_channel_send_control_until(&ctrl, &hello, &dl) == 0);
    fct_sha256_bytes(data, len, meta.file_sha256);
    CHECK(fct_channel_send_meta_until(&ctrl, &meta, &dl) == 0);

    fct_control_frame_t ready;
    CHECK(fct_channel_recv_control_until(&ctrl, &ready, &dl) == 0);
    CHECK(ready.type == FCT_CONTROL_READY && ready.arg == 2);
    CHECK(receive_small_bitmap(&ctrl, meta.total_chunks, &dl) == 0);

    CHECK(fct_channel_connect_until(&d0, "127.0.0.1", port, &dl) == 0);
    CHECK(fct_channel_connect_until(&d1, "127.0.0.1", port, &dl) == 0);
    fct_control_frame_t f0 = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 0};
    fct_control_frame_t f1 = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 1};
    memcpy(f0.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
    memcpy(f1.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
    CHECK(fct_channel_send_control_until(&d0, &f0, &dl) == 0);
    CHECK(fct_channel_send_control_until(&d1, &f1, &dl) == 0);

    /* Send chunks out of order: 2 -> 0 -> 1. */
    uint32_t order[3] = {2, 0, 1};
    fct_channel_t* chs[3] = {&d1, &d0, &d1};
    for (int i = 0; i < 3; ++i) {
        uint32_t id = order[i];
        uint64_t off = (uint64_t)id * DEFAULT_CHUNK_SZ;
        size_t chunk_len = (id == 2) ? 1000 : DEFAULT_CHUNK_SZ;
        fct_chunk_header_t hdr = {
            .magic = FCT_MAGIC,
            .chunk_id = id,
            .offset = off,
            .payload_len = (uint32_t)chunk_len,
            .flags = 0,
            .crc32 = fct_crc32(data + off, chunk_len),
        };
        CHECK(fct_channel_send_chunk_idle(chs[i], &hdr, data + off, 5000) == 0);
    }
    send_eor(&d0);
    send_eor(&d1);

    fct_control_frame_t res;
    CHECK(fct_channel_recv_control_until(&ctrl, &res, &dl) == 0);
    CHECK(res.type == FCT_CONTROL_RESULT && res.arg == FCT_RESULT_OK);

    fct_channel_close(&d0);
    fct_channel_close(&d1);
    fct_channel_close(&ctrl);

    int srv_rc = -1;
    CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
    CHECK(srv_rc == 0);

    char ref_path[256], dst_path[256];
    snprintf(ref_path, sizeof(ref_path), "%s/ref.txt", dst_dir);
    snprintf(dst_path, sizeof(dst_path), "%s/ooo.txt", dst_dir);
    write_file_bytes(ref_path, data, len);
    CHECK(files_equal(ref_path, dst_path));
    CHECK(!dir_has_temp_entries(dst_dir));

    free(data);
    remove_tree(dst_dir);
}

static void test_server_single_conn_failure_result(void) {
    char dst_dir[64];
    make_temp_dir(dst_dir, sizeof(dst_dir));
    int port = 0;
    pid_t srv = spawn_server_ready(dst_dir, &port);

    struct timespec dl = deadline_after(10000);
    fct_channel_t ctrl = {.sockfd = -1};
    CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);

    fct_control_frame_t hello = {.magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO, .arg = 1};
    fct_file_meta_t meta = {
        .magic = FCT_MAGIC,
        .file_size = 16,
        .chunk_size = DEFAULT_CHUNK_SZ,
        .total_chunks = 1,
        .filename = "badflag.bin",
    };
    CHECK(fct_channel_send_control_until(&ctrl, &hello, &dl) == 0);
    CHECK(fct_channel_send_meta_until(&ctrl, &meta, &dl) == 0);

    fct_control_frame_t ready;
    CHECK(fct_channel_recv_control_until(&ctrl, &ready, &dl) == 0);
    CHECK(ready.type == FCT_CONTROL_READY && ready.arg == 1);
    CHECK(receive_small_bitmap(&ctrl, meta.total_chunks, &dl) == 0);

    /* Send raw wire chunk header with invalid flags = 4 directly on socket. */
    unsigned char raw[28 + 16] = {0};
    raw[0] = 0x46;
    raw[1] = 0x43;
    raw[2] = 0x54;
    raw[3] = 0x31;
    raw[19] = 16; /* payload_len = 16 */
    raw[23] = 4;  /* flags = 4 (invalid) */
    CHECK(send_exact(ctrl.sockfd, raw, sizeof(raw)) == 0);

    fct_control_frame_t res;
    CHECK(fct_channel_recv_control_until(&ctrl, &res, &dl) == 0);
    CHECK(res.type == FCT_CONTROL_RESULT && res.arg == FCT_RESULT_PROTOCOL_ERROR);
    CHECK(memcmp(res.token, ready.token, FCT_TRANSFER_TOKEN_SIZE) == 0);
    fct_channel_close(&ctrl);

    int srv_rc = -1;
    CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
    CHECK(srv_rc != 0);

    char dst_path[256];
    snprintf(dst_path, sizeof(dst_path), "%s/badflag.bin", dst_dir);
    struct stat st;
    CHECK(stat(dst_path, &st) < 0 && errno == ENOENT);
    CHECK(!dir_has_temp_entries(dst_dir));
    remove_tree(dst_dir);
}

static void test_server_lost_result_after_publish(void) {
    char dst_dir[64];
    make_temp_dir(dst_dir, sizeof(dst_dir));
    int port = 0;
    pid_t srv = spawn_server_ready(dst_dir, &port);

    const size_t len = (size_t)DEFAULT_CHUNK_SZ + 512;
    unsigned char* data = make_random_bytes(len, 99);

    struct timespec dl = deadline_after(10000);
    fct_channel_t ctrl = {.sockfd = -1}, d0 = {.sockfd = -1}, d1 = {.sockfd = -1};
    CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);

    fct_control_frame_t hello = {.magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO, .arg = 2};
    fct_file_meta_t meta = {
        .magic = FCT_MAGIC,
        .file_size = len,
        .chunk_size = DEFAULT_CHUNK_SZ,
        .total_chunks = 2,
        .filename = "published.bin",
    };
    CHECK(fct_channel_send_control_until(&ctrl, &hello, &dl) == 0);
    fct_sha256_bytes(data, len, meta.file_sha256);
    CHECK(fct_channel_send_meta_until(&ctrl, &meta, &dl) == 0);

    fct_control_frame_t ready;
    CHECK(fct_channel_recv_control_until(&ctrl, &ready, &dl) == 0);
    CHECK(receive_small_bitmap(&ctrl, meta.total_chunks, &dl) == 0);

    CHECK(fct_channel_connect_until(&d0, "127.0.0.1", port, &dl) == 0);
    CHECK(fct_channel_connect_until(&d1, "127.0.0.1", port, &dl) == 0);
    fct_control_frame_t f0 = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 0};
    fct_control_frame_t f1 = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 1};
    memcpy(f0.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
    memcpy(f1.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
    CHECK(fct_channel_send_control_until(&d0, &f0, &dl) == 0);
    CHECK(fct_channel_send_control_until(&d1, &f1, &dl) == 0);

    fct_chunk_header_t h0 = {.magic = FCT_MAGIC,
                             .chunk_id = 0,
                             .offset = 0,
                             .payload_len = DEFAULT_CHUNK_SZ,
                             .flags = 0,
                             .crc32 = fct_crc32(data, DEFAULT_CHUNK_SZ)};
    fct_chunk_header_t h1 = {.magic = FCT_MAGIC,
                             .chunk_id = 1,
                             .offset = DEFAULT_CHUNK_SZ,
                             .payload_len = 512,
                             .flags = 0,
                             .crc32 = fct_crc32(data + DEFAULT_CHUNK_SZ, 512)};
    CHECK(fct_channel_send_chunk_idle(&d0, &h0, data, 5000) == 0);
    CHECK(fct_channel_send_chunk_idle(&d1, &h1, data + DEFAULT_CHUNK_SZ, 5000) == 0);
    send_eor(&d0);
    send_eor(&d1);

    /* Drop control channel immediately so server RESULT send fails after publish. */
    fct_channel_close(&ctrl);
    fct_channel_close(&d0);
    fct_channel_close(&d1);

    int srv_rc = -1;
    CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
    CHECK(srv_rc == 0);

    char ref_path[256], dst_path[256];
    snprintf(ref_path, sizeof(ref_path), "%s/ref.bin", dst_dir);
    snprintf(dst_path, sizeof(dst_path), "%s/published.bin", dst_dir);
    write_file_bytes(ref_path, data, len);
    CHECK(files_equal(ref_path, dst_path));
    CHECK(!dir_has_temp_entries(dst_dir));

    free(data);
    remove_tree(dst_dir);
}

static void test_server_protocol_errors(void) {
    /* 1. Truncated HELLO frame */
    {
        char dst_dir[64];
        make_temp_dir(dst_dir, sizeof(dst_dir));
        int port = 0;
        pid_t srv = spawn_server_ready(dst_dir, &port);
        struct timespec dl = deadline_after(5000);
        fct_channel_t ctrl = {.sockfd = -1};
        CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);
        unsigned char partial[10] = {0x46, 0x43, 0x54, 0x31, 0, 1, 0, 1, 0, 0};
        CHECK(send_exact(ctrl.sockfd, partial, sizeof(partial)) == 0);
        fct_channel_close(&ctrl);
        int srv_rc = -1;
        CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
        CHECK(srv_rc != 0);
        CHECK(!dir_has_temp_entries(dst_dir));
        remove_tree(dst_dir);
    }

    /* 2. Invalid DATA connection index (arg >= worker_count) */
    {
        char dst_dir[64];
        make_temp_dir(dst_dir, sizeof(dst_dir));
        int port = 0;
        pid_t srv = spawn_server_ready(dst_dir, &port);
        struct timespec dl = deadline_after(5000);
        fct_channel_t ctrl = {.sockfd = -1}, d0 = {.sockfd = -1};
        CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);
        fct_control_frame_t hello = {.magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO, .arg = 2};
        fct_file_meta_t meta = {.magic = FCT_MAGIC,
                                .file_size = (uint64_t)DEFAULT_CHUNK_SZ + 10,
                                .chunk_size = DEFAULT_CHUNK_SZ,
                                .total_chunks = 2,
                                .filename = "badidx.bin"};
        CHECK(fct_channel_send_control_until(&ctrl, &hello, &dl) == 0);
        CHECK(fct_channel_send_meta_until(&ctrl, &meta, &dl) == 0);
        fct_control_frame_t ready;
        CHECK(fct_channel_recv_control_until(&ctrl, &ready, &dl) == 0);
        CHECK(receive_small_bitmap(&ctrl, meta.total_chunks, &dl) == 0);

        CHECK(fct_channel_connect_until(&d0, "127.0.0.1", port, &dl) == 0);
        fct_control_frame_t bad = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 2};
        memcpy(bad.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
        CHECK(fct_channel_send_control_until(&d0, &bad, &dl) == 0);

        fct_control_frame_t res;
        CHECK(fct_channel_recv_control_until(&ctrl, &res, &dl) == 0);
        CHECK(res.type == FCT_CONTROL_RESULT && res.arg == FCT_RESULT_PROTOCOL_ERROR);
        fct_channel_close(&d0);
        fct_channel_close(&ctrl);

        int srv_rc = -1;
        CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
        CHECK(srv_rc != 0);
        CHECK(!dir_has_temp_entries(dst_dir));
        remove_tree(dst_dir);
    }

    /* 3. Duplicate chunk across workers */
    {
        char dst_dir[64];
        make_temp_dir(dst_dir, sizeof(dst_dir));
        int port = 0;
        pid_t srv = spawn_server_ready(dst_dir, &port);
        struct timespec dl = deadline_after(10000);
        fct_channel_t ctrl = {.sockfd = -1}, d0 = {.sockfd = -1}, d1 = {.sockfd = -1};
        CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);
        fct_control_frame_t hello = {.magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO, .arg = 2};
        fct_file_meta_t meta = {.magic = FCT_MAGIC,
                                .file_size = (uint64_t)DEFAULT_CHUNK_SZ + 10,
                                .chunk_size = DEFAULT_CHUNK_SZ,
                                .total_chunks = 2,
                                .filename = "dup.bin"};
        CHECK(fct_channel_send_control_until(&ctrl, &hello, &dl) == 0);
        CHECK(fct_channel_send_meta_until(&ctrl, &meta, &dl) == 0);
        fct_control_frame_t ready;
        CHECK(fct_channel_recv_control_until(&ctrl, &ready, &dl) == 0);
        CHECK(receive_small_bitmap(&ctrl, meta.total_chunks, &dl) == 0);

        CHECK(fct_channel_connect_until(&d0, "127.0.0.1", port, &dl) == 0);
        CHECK(fct_channel_connect_until(&d1, "127.0.0.1", port, &dl) == 0);
        fct_control_frame_t f0 = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 0};
        fct_control_frame_t f1 = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 1};
        memcpy(f0.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
        memcpy(f1.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
        CHECK(fct_channel_send_control_until(&d0, &f0, &dl) == 0);
        CHECK(fct_channel_send_control_until(&d1, &f1, &dl) == 0);

        unsigned char* chunk0 = calloc(1, DEFAULT_CHUNK_SZ);
        CHECK(chunk0 != NULL);
        fct_chunk_header_t h0 = {.magic = FCT_MAGIC,
                                 .chunk_id = 0,
                                 .offset = 0,
                                 .payload_len = DEFAULT_CHUNK_SZ,
                                 .flags = 0,
                                 .crc32 = fct_crc32(chunk0, DEFAULT_CHUNK_SZ)};
        CHECK(fct_channel_send_chunk_idle(&d0, &h0, chunk0, 5000) == 0);
        /* Send chunk 0 again on d1 (may get EPIPE if server already aborted). */
        (void)fct_channel_send_chunk_idle(&d1, &h0, chunk0, 5000);
        free(chunk0);

        fct_control_frame_t res;
        CHECK(fct_channel_recv_control_until(&ctrl, &res, &dl) == 0);
        CHECK(res.type == FCT_CONTROL_RESULT && res.arg == FCT_RESULT_PROTOCOL_ERROR);
        fct_channel_close(&d0);
        fct_channel_close(&d1);
        fct_channel_close(&ctrl);

        int srv_rc = -1;
        CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
        CHECK(srv_rc != 0);
        CHECK(dir_has_temp_entries(dst_dir));
        remove_tree(dst_dir);
    }

    /* 4. A round ending without chunk 1 preserves the validated chunk. */
    {
        char dst_dir[64];
        make_temp_dir(dst_dir, sizeof(dst_dir));
        int port = 0;
        pid_t srv = spawn_server_ready(dst_dir, &port);
        struct timespec dl = deadline_after(10000);
        fct_channel_t ctrl = {.sockfd = -1}, d0 = {.sockfd = -1}, d1 = {.sockfd = -1};
        CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);
        fct_control_frame_t hello = {.magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO, .arg = 2};
        fct_file_meta_t meta = {.magic = FCT_MAGIC,
                                .file_size = (uint64_t)DEFAULT_CHUNK_SZ + 10,
                                .chunk_size = DEFAULT_CHUNK_SZ,
                                .total_chunks = 2,
                                .filename = "missing.bin"};
        CHECK(fct_channel_send_control_until(&ctrl, &hello, &dl) == 0);
        CHECK(fct_channel_send_meta_until(&ctrl, &meta, &dl) == 0);
        fct_control_frame_t ready;
        CHECK(fct_channel_recv_control_until(&ctrl, &ready, &dl) == 0);
        CHECK(receive_small_bitmap(&ctrl, meta.total_chunks, &dl) == 0);

        CHECK(fct_channel_connect_until(&d0, "127.0.0.1", port, &dl) == 0);
        CHECK(fct_channel_connect_until(&d1, "127.0.0.1", port, &dl) == 0);
        fct_control_frame_t f0 = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 0};
        fct_control_frame_t f1 = {.magic = FCT_MAGIC, .type = FCT_CONTROL_DATA, .arg = 1};
        memcpy(f0.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
        memcpy(f1.token, ready.token, FCT_TRANSFER_TOKEN_SIZE);
        CHECK(fct_channel_send_control_until(&d0, &f0, &dl) == 0);
        CHECK(fct_channel_send_control_until(&d1, &f1, &dl) == 0);

        unsigned char* chunk0 = calloc(1, DEFAULT_CHUNK_SZ);
        CHECK(chunk0 != NULL);
        fct_chunk_header_t h0 = {.magic = FCT_MAGIC,
                                 .chunk_id = 0,
                                 .offset = 0,
                                 .payload_len = DEFAULT_CHUNK_SZ,
                                 .flags = 0,
                                 .crc32 = fct_crc32(chunk0, DEFAULT_CHUNK_SZ)};
        CHECK(fct_channel_send_chunk_idle(&d0, &h0, chunk0, 5000) == 0);
        free(chunk0);
        send_eor(&d0);
        send_eor(&d1);

        fct_control_frame_t res;
        CHECK(fct_channel_recv_control_until(&ctrl, &res, &dl) == 0);
        CHECK(res.type == FCT_CONTROL_RESULT && res.arg == FCT_RESULT_PROTOCOL_ERROR);
        fct_channel_close(&d0);
        fct_channel_close(&d1);
        fct_channel_close(&ctrl);

        int srv_rc = -1;
        CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
        CHECK(srv_rc != 0);
        CHECK(dir_has_temp_entries(dst_dir));
        remove_tree(dst_dir);
    }

    /* 5. Aborted mid-payload */
    {
        char dst_dir[64];
        make_temp_dir(dst_dir, sizeof(dst_dir));
        int port = 0;
        pid_t srv = spawn_server_ready(dst_dir, &port);
        struct timespec dl = deadline_after(10000);
        fct_channel_t ctrl = {.sockfd = -1};
        CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);
        fct_control_frame_t hello = {.magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO, .arg = 1};
        fct_file_meta_t meta = {.magic = FCT_MAGIC,
                                .file_size = 1024,
                                .chunk_size = DEFAULT_CHUNK_SZ,
                                .total_chunks = 1,
                                .filename = "aborted.bin"};
        CHECK(fct_channel_send_control_until(&ctrl, &hello, &dl) == 0);
        CHECK(fct_channel_send_meta_until(&ctrl, &meta, &dl) == 0);
        fct_control_frame_t ready;
        CHECK(fct_channel_recv_control_until(&ctrl, &ready, &dl) == 0);
        CHECK(receive_small_bitmap(&ctrl, meta.total_chunks, &dl) == 0);

        /* Send valid 28-byte chunk header claiming 1024 bytes, but only 100 bytes of payload. */
        unsigned char raw[28 + 100] = {0};
        raw[0] = 0x46;
        raw[1] = 0x43;
        raw[2] = 0x54;
        raw[3] = 0x31;
        raw[18] = 0x04;
        raw[19] = 0x00; /* payload_len = 1024 */
        CHECK(send_exact(ctrl.sockfd, raw, sizeof(raw)) == 0);
        fct_channel_close(&ctrl);

        int srv_rc = -1;
        CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
        CHECK(srv_rc != 0);
        CHECK(!dir_has_temp_entries(dst_dir));
        remove_tree(dst_dir);
    }
}

static fct_control_frame_t start_single_round(fct_channel_t* control, int port,
                                               const fct_file_meta_t* meta,
                                               const struct timespec* deadline,
                                               uint16_t requested_workers,
                                               uint8_t expected_bitmap) {
    fct_control_frame_t hello = {
        .magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO, .arg = requested_workers};
    fct_control_frame_t ready;
    CHECK(fct_channel_connect_until(control, "127.0.0.1", port, deadline) == 0);
    CHECK(fct_channel_send_control_until(control, &hello, deadline) == 0);
    CHECK(fct_channel_send_meta_until(control, meta, deadline) == 0);
    CHECK(fct_channel_recv_control_until(control, &ready, deadline) == 0);
    CHECK(ready.type == FCT_CONTROL_READY && ready.arg == 1);
    CHECK(receive_small_bitmap(control, meta->total_chunks, deadline) == expected_bitmap);
    return ready;
}

static void send_raw_chunk(fct_channel_t* control, uint32_t id, const unsigned char* data,
                           size_t len, bool bad_crc) {
    fct_chunk_header_t header = {
        .magic = FCT_MAGIC,
        .chunk_id = id,
        .offset = (uint64_t)id * DEFAULT_CHUNK_SZ,
        .payload_len = (uint32_t)len,
        .crc32 = fct_crc32(data, len) ^ (uint32_t)bad_crc,
    };
    CHECK(fct_channel_send_chunk_idle(control, &header, data, 5000) == 0);
}

static void test_resume_after_disconnect(bool all_chunks_written) {
    char src_dir[64], dst_dir[64], src_path[256], dst_path[256];
    make_temp_dir(src_dir, sizeof(src_dir));
    make_temp_dir(dst_dir, sizeof(dst_dir));
    snprintf(src_path, sizeof(src_path), "%s/resume.zip", src_dir);
    snprintf(dst_path, sizeof(dst_path), "%s/resume.zip", dst_dir);
    const size_t tail = 123;
    const size_t len = (size_t)DEFAULT_CHUNK_SZ + tail;
    unsigned char* data = make_random_bytes(len, 103);
    write_file_bytes(src_path, data, len);

    fct_file_meta_t meta = {.magic = FCT_MAGIC,
                            .file_size = len,
                            .chunk_size = DEFAULT_CHUNK_SZ,
                            .total_chunks = 2,
                            .filename = "resume.zip"};
    fct_sha256_bytes(data, len, meta.file_sha256);

    int port = 0, server_rc = -1;
    pid_t server = spawn_server_ready(dst_dir, &port);
    struct timespec deadline = deadline_after(10000);
    fct_channel_t control = {.sockfd = -1};
    (void)start_single_round(&control, port, &meta, &deadline, 1, 0);
    /* Preserve the short tail first, with or without the other chunk. */
    send_raw_chunk(&control, 1, data + DEFAULT_CHUNK_SZ, tail, false);
    if (all_chunks_written)
        send_raw_chunk(&control, 0, data, DEFAULT_CHUNK_SZ, false);
    fct_channel_close(&control);
    CHECK(wait_pid_timeout(server, 10000, &server_rc) == 0 && server_rc != 0);
    CHECK(access(dst_path, F_OK) < 0 && errno == ENOENT);
    CHECK(dir_has_temp_entries(dst_dir));

    if (!all_chunks_written) {
        server = spawn_server_ready(dst_dir, &port);
        deadline = deadline_after(10000);
        /* READY must advertise only the short tail and reduce two workers to one. */
        (void)start_single_round(&control, port, &meta, &deadline, 2, 1U << 1);
        fct_channel_close(&control);
        CHECK(wait_pid_timeout(server, 10000, &server_rc) == 0 && server_rc != 0);
    }

    server = spawn_server_ready(dst_dir, &port);
    char progress[2048] = {0};
    CHECK(run_client_capture(port, src_path, "4", false, progress, sizeof(progress)) == 0);
    CHECK(wait_pid_timeout(server, 10000, &server_rc) == 0 && server_rc == 0);
    CHECK(strstr(progress, "100.00%") != NULL);
    CHECK(strstr(progress, all_chunks_written ? "Saved: —" : "Saved: 0.0%") != NULL);
    CHECK(files_equal(src_path, dst_path));
    CHECK(!dir_has_temp_entries(dst_dir));

    free(data);
    remove_tree(src_dir);
    remove_tree(dst_dir);
}

static void test_server_checksum_result(bool wrong_sha, bool exhaust_retries) {
    char dst_dir[64], dst_path[256], reference[256];
    make_temp_dir(dst_dir, sizeof(dst_dir));
    snprintf(dst_path, sizeof(dst_path), "%s/checked.bin", dst_dir);
    snprintf(reference, sizeof(reference), "%s/reference", dst_dir);

    const size_t len = (size_t)DEFAULT_CHUNK_SZ + 19;
    unsigned char* data = make_text_bytes(len);
    fct_file_meta_t meta = {.magic = FCT_MAGIC,
                            .file_size = len,
                            .chunk_size = DEFAULT_CHUNK_SZ,
                            .total_chunks = 2,
                            .filename = "checked.bin"};
    fct_sha256_bytes(data, len, meta.file_sha256);
    if (wrong_sha)
        meta.file_sha256[0] ^= 1;

    int port = 0, server_rc = -1;
    pid_t server = spawn_server_ready(dst_dir, &port);
    struct timespec deadline = deadline_after(10000);
    fct_channel_t control = {.sockfd = -1};
    fct_control_frame_t ready = start_single_round(&control, port, &meta, &deadline, 1, 0);
    send_raw_chunk(&control, 0, data, DEFAULT_CHUNK_SZ, false);
    send_raw_chunk(&control, 1, data + DEFAULT_CHUNK_SZ, 19, !wrong_sha);
    send_eor(&control);

    fct_control_frame_t response;
    if (!wrong_sha) {
        unsigned rounds = exhaust_retries ? FCT_MAX_CHUNK_RETRIES : 1;
        for (unsigned round = 1; round <= rounds; ++round) {
            CHECK(fct_channel_recv_control_until(&control, &response, &deadline) == 0);
            CHECK(response.type == FCT_CONTROL_RETRY && response.arg == round);
            CHECK(memcmp(response.token, ready.token, sizeof(ready.token)) == 0);
            CHECK(receive_small_bitmap(&control, meta.total_chunks, &deadline) == 1);
            send_raw_chunk(&control, 1, data + DEFAULT_CHUNK_SZ, 19, exhaust_retries);
            send_eor(&control);
        }
    }

    bool success = !wrong_sha && !exhaust_retries;
    CHECK(fct_channel_recv_control_until(&control, &response, &deadline) == 0);
    CHECK(response.type == FCT_CONTROL_RESULT);
    CHECK(response.arg == (success ? FCT_RESULT_OK : FCT_RESULT_CHECKSUM_ERROR));
    CHECK(memcmp(response.token, ready.token, sizeof(ready.token)) == 0);
    fct_channel_close(&control);
    CHECK(wait_pid_timeout(server, 10000, &server_rc) == 0);
    CHECK((server_rc == 0) == success);
    if (success) {
        write_file_bytes(reference, data, len);
        CHECK(files_equal(reference, dst_path));
    } else {
        CHECK(access(dst_path, F_OK) < 0 && errno == ENOENT);
    }
    CHECK(dir_has_temp_entries(dst_dir) == exhaust_retries);

    free(data);
    remove_tree(dst_dir);
}

static void test_server_rejects_invalid_filenames(void) {
    const char* names[] = {"", ".", "..", "../escape", "/absolute", "subdir/file"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char dst_dir[64];
        make_temp_dir(dst_dir, sizeof(dst_dir));
        int port = 0;
        pid_t srv = spawn_server_ready(dst_dir, &port);
        struct timespec dl = deadline_after(5000);
        fct_channel_t ctrl = {.sockfd = -1};
        fct_control_frame_t hello = {.magic = FCT_MAGIC, .type = FCT_CONTROL_HELLO};
        fct_file_meta_t meta = {.magic = FCT_MAGIC, .chunk_size = DEFAULT_CHUNK_SZ};
        strcpy(meta.filename, names[i]);

        CHECK(fct_channel_connect_until(&ctrl, "127.0.0.1", port, &dl) == 0);
        CHECK(fct_channel_send_control_until(&ctrl, &hello, &dl) == 0);
        CHECK(fct_channel_send_meta_until(&ctrl, &meta, &dl) == 0);
        fct_control_frame_t result;
        CHECK(fct_channel_recv_control_until(&ctrl, &result, &dl) == 0);
        CHECK(result.type == FCT_CONTROL_RESULT && result.arg == FCT_RESULT_PROTOCOL_ERROR);
        fct_channel_close(&ctrl);

        int srv_rc = -1;
        CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);
        CHECK(srv_rc != 0);
        CHECK(!dir_has_temp_entries(dst_dir));
        CHECK(rmdir(dst_dir) == 0);
    }
}

static void test_existing_destination_preserved(void) {
    static const unsigned char sentinel[] = "DO_NOT_OVERWRITE_SENTINEL";
    for (int scenario = 0; scenario < 3; ++scenario) {
        bool empty = scenario == 1;
        bool is_symlink = scenario == 2;
        char src_dir[64], dst_dir[64], src_path[256], dst_path[256], ref_path[256], target[256];
        make_temp_dir(src_dir, sizeof(src_dir));
        make_temp_dir(dst_dir, sizeof(dst_dir));
        snprintf(src_path, sizeof(src_path), "%s/existing.dat", src_dir);
        snprintf(dst_path, sizeof(dst_path), "%s/existing.dat", dst_dir);
        snprintf(ref_path, sizeof(ref_path), "%s/sentinel.ref", src_dir);

        if (empty) {
            write_file_bytes(src_path, NULL, 0);
        } else {
            unsigned char* new_data = make_random_bytes(1024, 55);
            write_file_bytes(src_path, new_data, 1024);
            free(new_data);
        }
        if (is_symlink) {
            snprintf(target, sizeof(target), "%s/target.dat", src_dir);
            write_file_bytes(target, sentinel, sizeof(sentinel) - 1);
            CHECK(symlink(target, dst_path) == 0);
        } else {
            write_file_bytes(dst_path, sentinel, sizeof(sentinel) - 1);
        }
        write_file_bytes(ref_path, sentinel, sizeof(sentinel) - 1);

        int port = 0;
        pid_t srv = spawn_server_ready(dst_dir, &port);
        char err_buf[512] = {0};
        int cli_rc = run_client_capture(port, src_path, "2", false, err_buf, sizeof(err_buf));
        int srv_rc = -1;
        CHECK(wait_pid_timeout(srv, 10000, &srv_rc) == 0);

        CHECK(cli_rc != 0);
        CHECK(srv_rc != 0);
        CHECK(files_equal(ref_path, dst_path));
        if (is_symlink) {
            char actual[256];
            ssize_t len = readlink(dst_path, actual, sizeof(actual));
            CHECK(len == (ssize_t)strlen(target));
            CHECK(memcmp(actual, target, (size_t)len) == 0);
        }
        CHECK(!dir_has_temp_entries(dst_dir));

        remove_tree(src_dir);
        remove_tree(dst_dir);
    }
}

int main(void) {
    test_all_e2e();
    test_wire_client_behaviors();
    test_server_out_of_order_multi();
    test_server_single_conn_failure_result();
    test_server_lost_result_after_publish();
    test_server_protocol_errors();
    test_resume_after_disconnect(false);
    test_resume_after_disconnect(true);
    test_server_checksum_result(false, false);
    test_server_checksum_result(false, true);
    test_server_checksum_result(true, false);
    test_server_rejects_invalid_filenames();
    test_existing_destination_preserved();
    printf("integration_test: all tests passed\n");
    return 0;
}
