# fct

[한국어](README.md) | **English**

**File Compression Transfer** — A C-based CLI file transfer tool supporting parallel multi-TCP connections, adaptive zlib chunk compression, CRC32/SHA-256 integrity verification, and bitmap-based resumable transfers.

## Build

Run the commands below on Ubuntu 24.04.

### Install Dependencies

Install `gcc`, `clang-format`, and `zlib1g-dev` via `sudo apt-get` (linked with standard POSIX `pthread` and `zlib` libraries).

```sh
make init
```

### Compile

Run `make` (or `make all`) in the project root to build the `build/fct` executable.

```sh
make
```

### Test, Benchmark, Format, and Clean

- **Run tests**: Execute unit tests and end-to-end integration tests (`network_test`, `cli_test`, `cli_extended_test`, `progress_test`, `test_file_writer`, `file_reader_test`, `compress_test`, `checksum_test`, `bitmap_test`, `worker_test`, `integration_test`)
  ```sh
  make test
  ```
- **Run benchmarks**: Build `build/fct` and run the end-to-end benchmark suite comparing `fct` (`-t 1`, `-t 4`, and `-z` on incompressible files) against `rsync` (`raw`, `-z` zstd, `-z` zlib-1) and 3-step staged compression-transfer-decompression (`gzip -1`, `zstd`)
  ```sh
  make bench
  ```
  - **Workloads**: `compressible.log`, `mixed.tar`, `archive.gz`, `random.bin`
  - **Network profiles** (via Linux network namespaces and `tc netem`): `wan_100m` (`100 Mbps` / `20 ms RTT`), `lan_1g` (`1 Gbps` / `2 ms RTT`), `unlimited`
  - **Prerequisites**: `python3`, `sudo` privileges for `ip netns`/`tc`, `rsync`, `gzip`, `zstd`
  - **Outputs & options**: Writes `raw_runs.csv`, `summary.json`, `report.md`, and SVG charts under `bench/results/`. Customize via `python3 bench/run_bench.py` flags (`--size-mb`, `--runs`, `--warmup`, `--profiles`, `--files`, `--output-dir`, etc.).
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
| `-t workers` | Number of parallel data workers | Client | `1–8` (default: `1`, negotiated count is `min(workers, missing_chunks)` — i.e., `min(workers, total_chunks)` on a fresh transfer, `0` for empty files or already-complete resumed sessions) |
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

### Transfer Flow, Integrity, Resume, and Progress

- **Handshake & parallel transfer**: The client computes the source file's SHA-256 digest and sends a `HELLO` control frame along with file metadata (including `file_sha256`). The server opens or resumes the session and responds with a `READY` frame (carrying the negotiated worker count and session token) followed by the completed-chunk bitmap. When `2` or more workers are negotiated, the client registers `DATA` TCP connections with the session token and transfers only missing chunks in parallel.
- **Chunk CRC32 verification & automatic retry**: Each chunk header carries the IEEE CRC32 of the raw (uncompressed) chunk data. Each round ends with an `EOR` marker on every data channel without closing the connections. If any chunk fails CRC32 verification, the server checkpoints completed chunks and sends a `RETRY` control frame with the updated bitmap for up to `3` retry rounds (`FCT_MAX_CHUNK_RETRIES = 3`), resending only the remaining missing chunks.
- **Resumable transfers**: The server stages each transfer in a deterministic, `flock`-locked hidden directory (`.fct-<sha256>`) inside the output directory, derived from file size, chunk size, file SHA-256, and filename. Partial data is stored in `data.part` and a CRC32-verified checkpoint bitmap in `data.bitmap` (written atomically via `mkstemp` + `fsync` + `rename`). If a transfer is interrupted after at least one chunk completes, the checkpoint is preserved so re-running the server and client with the same file automatically resumes and skips already-received chunks. Different versions of the same filename (different size or SHA-256) use separate session directories.
- **Full-file SHA-256 verification & safe atomic publishing**: Once all chunks are complete, the server verifies the assembled `data.part` against `file_sha256` from the metadata (returning a checksum error and discarding the session on mismatch). On match, it calls `fsync()`, atomically publishes the file via `linkat()` (returning an error without overwriting if a file with the same name already exists), syncs the output directory, and removes the session directory.
- **Terminal output & progress reporting**:
  - **Client**: Prints transfer summary (`Sending ...`), negotiated worker count (`Workers`), resume status (`Resuming`, when resuming), and compression mode with skip reason (`Compression: ON (auto)` / `ON (force)` / `OFF (compressed format)` / `OFF (low ratio)` / `OFF (empty)`). During transfer, it prints progress (`Progress`), throughput (`MiB/s`), compression savings (`Saved`), and estimated time remaining (`ETA`) to `stderr` at most once per second (displaying `—` when a denominator is `0`; on resumed transfers, `Progress` includes already-resumed bytes while throughput, `Saved`, and `ETA` reflect data sent during the current session), plus `Retry` notices and `Transfer complete (SHA-256 verified)` upon completion.
  - **Server**: Prints listening port and directory (`Listening on port ... (dir: ...)`), incoming file summary (`Receiving ...`), resume or invalid-checkpoint notices (`Resuming`, `Checkpoint invalid...`), CRC32 retry requests (`Retry`), received compression statistics (`Compression: ON/OFF/N/A ...`) and `Received ... (SHA-256 verified)` on completion, or `Checkpoint saved` if interrupted with resumable chunks.
- **Exit status**: The client exits with status `0` only after receiving a valid `RESULT_OK` control frame from the server; protocol, storage, transfer, and checksum errors exit with a non-zero status.

## Project Structure

```text
fct/
├── include/            # Shared protocol (common.h) and module headers
├── src/
│   ├── main.c          # Entry point, handshake, resumable session, retry & SHA-256 orchestration
│   ├── cli.c           # CLI option parsing and usage output
│   ├── network.c       # TCP socket connections, timed I/O, and control/meta/chunk/bitmap framing
│   ├── worker.c        # Multi-TCP parallel send/receive workers with CRC32 & selective transfer
│   ├── compress.c      # Adaptive and forced zlib chunk compression/decompression
│   ├── checksum.c      # IEEE CRC32 and FIPS 180-4 SHA-256 checksum computation
│   ├── bitmap.c        # Completed-chunk bitmap tracking and atomic checkpoint persistence
│   ├── file_reader.c   # Chunk-based file reading
│   ├── file_writer.c   # Out-of-order chunk writing, preallocation, and partial-file reopening
│   └── progress.c      # Progress, throughput, compression savings, ETA, and resume reporting
├── tests/              # Module unit tests and end-to-end integration tests
├── bench/              # fct vs rsync / staged-compression benchmark suite (generate_data.py, netns_env.py, run_bench.py, generate_report.py)
└── Makefile            # Dependency setup, build, test, benchmark, and formatting
```

## Team & Roles

| Name | GitHub | Role |
| --- | --- | --- |
| Huigyun Jeong | [@Segyun](https://github.com/Segyun) | TCP network transport, control/bitmap frame protocol, multi-TCP parallel & selective retry workers, and `fct` vs `rsync` benchmark suite |
| Junwoo Seo | [@junwoo00](https://github.com/junwoo00) | File writer (`file_writer`), extended CLI options (`-t`, `-z`), progress tracking (`progress`), and resumable session, integrity verification & safe publishing integration |
| Eonwoo Jeong | [@eonuo9](https://github.com/eonuo9) | File reader (`file_reader`), adaptive chunk compression (`compress`), CRC32/SHA-256 checksums (`checksum`), and persistent chunk bitmap (`bitmap`) |

## License

[MIT License](LICENSE)

---

This project was developed for the System Programming course at Kyungpook National University.
