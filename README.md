# fct
File Compression Transfer

On Ubuntu 24.04, run `make init` to install GCC and clang-format using
`sudo apt-get`. Build with `make`; the executable is `build/fct`.

```sh
./build/fct -s [-p port] [-d output_dir]
./build/fct -c server_ip -f file_path [-p port]
```

The default port is 9000 and the default receive directory is the current
directory. Use `-h` for help. Server and client modes are mutually exclusive;
the client requires an IPv4 address and a file path, while `-d` is only valid
for the server. Ports must be in the range 1–65535.

Start the server first. It receives one file over a single TCP connection and
exits. The output directory must already exist. The server saves the file under
its original name after receiving it in full and reports an error if that name
already exists. Check the server's exit status to confirm the file was saved.

Run `make test` to exercise the CLI and network module.
