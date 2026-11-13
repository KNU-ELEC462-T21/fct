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
file transfer, file I/O, and network operations are not implemented yet.
The protocol's integer byte order must be agreed on before implementing the
transport. Chunk reception also needs a capacity-aware interface before it can
safely accept untrusted payload lengths.
