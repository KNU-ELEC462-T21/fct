# fct
File Compression Transfer

On Ubuntu 24.04, run `make init` to install GCC, `clang-format`, and `zlib1g-dev`
using `sudo apt-get`. Build with `make`; the executable is `build/fct`.

```sh
./build/fct -s [-p port] [-d output_dir]
./build/fct -c server_ip -f file_path [-p port] [-t workers] [-z]
```

## Options

- `-s`: Server mode (receive one file and exit).
- `-c server_ip`: Client mode (send a file to an IPv4 address).
- `-f file_path`: Input file to send (client only).
- `-p port`: TCP port in `1–65535` (default: `9000`).
- `-d output_dir`: Existing receive directory (server only, default: `.`).
- `-t workers`: Data worker count in `1–8` (client only, default: `1`).
  The negotiated worker count is `min(workers, total_chunks)` (`0` for empty files).
- `-z`: Force zlib chunk compression (client only). Without `-z`, `fct` skips
  known compressed formats (`.zip`, `.gz`, `.tgz`, `.mp4` or matching magic bytes)
  and trials the first chunk, enabling per-chunk compression only when it shrinks
  to at most 95% of raw size.
- `-h`: Show usage help.

## Behavior & Verification

Start the server first. After the `HELLO` / `READY` control handshake (and
`DATA` connection registration when using 2+ workers), chunks are transferred over
TCP, assembled out of order on the server, `fsync()`ed, and published atomically
without overwriting an existing destination file. The client prints transfer
progress, throughput, compression savings, and ETA to `stderr` (at most once per
second, showing `—` when a denominator is zero) and exits with status `0` only
after receiving a valid `RESULT_OK` frame.

Run `make test` to execute all unit and end-to-end integration tests.
