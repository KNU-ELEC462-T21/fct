# fct
File Compression Transfer

On Ubuntu 24.04, run `make init` to install GCC and clang-format using
`sudo apt-get`. Build with `make`; the executable is `build/fct`.

```sh
./build/fct -s [-p port] [-d output_dir]
./build/fct -c server_ip -f file_path [-p port]
```

The default port is 9000 and the default receive directory is the current
directory. The command-line interface and module headers are scaffolding:
file transfer and file I/O are not implemented yet. The network module uses
big-endian protocol fields and rejects chunk payloads larger than the supplied
buffer. Run `make test` to exercise the network module.
