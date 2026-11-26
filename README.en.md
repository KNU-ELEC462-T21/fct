# fct

[한국어](README.md) | **English**

**File Compression Transfer** — A C-based CLI file transfer tool supporting parallel multi-TCP connections and adaptive zlib chunk compression.

## Build

Run the commands below on Ubuntu 24.04.

### Install Dependencies

Install `gcc`, `clang-format`, and `zlib1g-dev` via `sudo apt-get` (linked with the standard POSIX `pthread` library).

```sh
make init
```

### Compile

Run `make` (or `make all`) in the project root to build the `build/fct` executable.

```sh
make
```

### Test, Format, and Clean

- **Run tests**: Execute unit tests and end-to-end integration tests (`network_test`, `cli_test`, `cli_extended_test`, `progress_test`, `test_file_writer`, `file_reader_test`, `compress_test`, `worker_test`, `integration_test`)
  ```sh
  make test
  ```
- **Format code**: Format `src/` and `include/` sources with `clang-format`
  ```sh
  make format
  ```
- **Remove build artifacts**: Delete the `build/` directory
  ```sh
  make clean
  ```

## Running

```sh
./build/fct -s [-p port] [-d output_dir]
./build/fct -c server_ip -f file_path [-p port] [-t workers] [-z]
./build/fct -h
```

Always start the server first before sending a file from the client.

1. **Default run** (port `9000`, receive into the current directory)

   ```sh
   # Receiver (server)
   ./build/fct -s

   # Sender (client)
   ./build/fct -c 127.0.0.1 -f ./example.txt
   ```

2. **Custom port, receive directory, and parallel workers (`-t 4`)**

   ```sh
   # Receiver (server): /tmp/recv must already exist
   ./build/fct -s -p 9100 -d /tmp/recv

   # Sender (client): transfer in parallel using 4 data workers
   ./build/fct -c 192.168.0.10 -f ./archive.tar -p 9100 -t 4
   ```

3. **Forced compression (`-z`)**

   ```sh
   ./build/fct -c 192.168.0.10 -f ./data.bin -p 9100 -t 4 -z
   ```

## Usage

### CLI Options

| Option | Description | Mode | Default / Constraints |
| --- | --- | --- | --- |
| `-s` | Server mode (receive one file and exit) | Server | Mutually exclusive with `-c` |
| `-c server_ip` | Client mode (send a file to the given IPv4 address) | Client | Mutually exclusive with `-s`, valid IPv4 address required |
| `-f file_path` | Path to the input file to send | Client | Required in client mode |
| `-p port` | TCP port number | Both | `1–65535` (default: `9000`) |
| `-d output_dir` | Directory to store the received file | Server | Must already exist (default: `.`) |
| `-t workers` | Number of parallel data workers | Client | `1–8` (default: `1`, negotiated count is `min(workers, total_chunks)`, `0` for empty files) |
| `-z` | Force zlib chunk compression (bypass auto-skip) | Client | Disabled by default (auto mode) |
| `-h` | Show usage help | Both | — |

### Option Constraints

- `-s` and `-c` are mutually exclusive, and exactly one of them must be specified.
- Client mode (`-c`) requires `-f` and cannot be combined with `-d`.
- Server mode (`-s`) cannot be combined with `-f`, `-t`, or `-z`.

### Compression Behavior

- The default chunk size is `2 MiB`, and zlib `Z_BEST_SPEED` (level 1) is used to prioritize throughput over network bandwidth.
- **Auto mode (without `-z`)**: Files with already-compressed extensions (`.zip`, `.gz`, `.tgz`, `.mp4`) or magic bytes (ZIP, GZIP, MP4), as well as empty files, skip compression (`RAW`). For other files, `fct` trial-compresses the first chunk and enables `AUTO` mode only if it shrinks to at most `95%` of the raw size; even in `AUTO` mode, compressed data is sent only when an individual chunk actually shrinks below its raw size.
- **Forced compression (with `-z`)**: Bypasses auto-skip and compresses every chunk (`FORCE`).

### Transfer Flow, Storage, and Progress

- **Handshake & parallel transfer**: The client sends a `HELLO` control frame and file metadata, and the server responds with a `READY` frame carrying a session token. When using `2` or more workers, the client registers `DATA` TCP connections with the session token and transfers chunks in parallel.
- **Out-of-order assembly & safe publishing**: The server assembles chunks arriving out of order across connections inside a temporary directory (`.fct-XXXXXX`) within the output directory. Once complete, it calls `fsync()` and atomically publishes the file via `link()`, returning an error without overwriting if a file with the same name already exists.
- **Progress reporting**: The client prints progress (`Progress`), throughput (`MiB/s`), compression savings (`Saved`), and estimated time remaining (`ETA`) to `stderr` at most once per second (displaying `—` when a denominator is `0`).
- **Exit status**: The client exits with status `0` only after receiving a valid `RESULT_OK` control frame from the server.

## Project Structure

```text
fct/
├── include/            # Shared protocol (common.h) and module headers
├── src/
│   ├── main.c          # Entry point, control handshake, and transfer session orchestration
│   ├── cli.c           # CLI option parsing and usage output
│   ├── network.c       # TCP socket connections, timed I/O, and frame serialization
│   ├── worker.c        # Multi-TCP parallel send/receive worker threads
│   ├── compress.c      # Adaptive and forced zlib chunk compression/decompression
│   ├── file_reader.c   # Chunk-based file reading
│   ├── file_writer.c   # Out-of-order chunk file writing and assembly
│   └── progress.c      # Progress, throughput, compression savings, and ETA reporting
├── tests/              # Module unit tests and end-to-end integration tests
└── Makefile            # Dependency setup, build, test, and formatting
```

## Team & Roles

| Name | GitHub | Role |
| --- | --- | --- |
| Huigyun Jeong | [@Segyun](https://github.com/Segyun) | TCP network transport, control frames, and multi-TCP parallel workers |
| Junwoo Seo | [@junwoo00](https://github.com/junwoo00) | File writer (`file_writer`), extended CLI options (`-t`, `-z`), and transfer progress tracking (`progress`) |
| Eonwoo Jeong | [@eonuo9](https://github.com/eonuo9) | File reader (`file_reader`) and adaptive chunk compression (`compress`) |

## License

[MIT License](LICENSE)

---

This project was developed for the System Programming course at Kyungpook National University.
