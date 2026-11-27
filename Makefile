CC = gcc
CFLAGS ?= -O2
CLANG_FORMAT ?= clang-format
LDLIBS += -lz -lpthread

SRC = $(shell find src -name '*.c')
OBJ = $(SRC:src/%.c=build/%.o)

TARGET = build/fct

all: $(TARGET)

$(TARGET): $(OBJ) Makefile
	mkdir -p $(@D)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

build/%.o: src/%.c
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

init:
	sudo apt-get update
	sudo apt-get install -y gcc clang-format zlib1g-dev

test: build/network_test build/cli_test build/cli_extended_test build/progress_test build/test_file_writer build/file_reader_test build/compress_test build/checksum_test build/bitmap_test build/worker_test build/integration_test
	./build/network_test
	./build/cli_test
	./build/cli_extended_test
	./build/progress_test
	./build/test_file_writer
	./build/file_reader_test
	./build/compress_test
	./build/checksum_test
	./build/bitmap_test
	./build/worker_test
	./build/integration_test

build/file_reader_test: tests/file_reader_test.c src/file_reader.c include/file_reader.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/file_reader_test.c src/file_reader.c -o $@

build/compress_test: tests/compress_test.c src/compress.c include/compress.h include/common.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/compress_test.c src/compress.c -o $@ -lz

build/checksum_test: tests/checksum_test.c src/checksum.c include/checksum.h include/common.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/checksum_test.c src/checksum.c -o $@ -lz

build/bitmap_test: tests/bitmap_test.c src/bitmap.c src/checksum.c include/bitmap.h include/checksum.h include/common.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/bitmap_test.c src/bitmap.c src/checksum.c -o $@ -lz

build/test_file_writer: tests/test_file_writer.c src/file_writer.c include/file_writer.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/test_file_writer.c src/file_writer.c -o $@

build/cli_test: tests/cli_test.c src/cli.c include/cli.h include/common.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/cli_test.c src/cli.c -o $@

build/cli_extended_test: tests/cli_extended_test.c src/cli.c include/cli.h include/common.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/cli_extended_test.c src/cli.c -o $@

build/progress_test: tests/progress_test.c src/progress.c include/progress.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/progress_test.c src/progress.c -o $@ -lpthread

build/network_test: tests/network_test.c src/network.c include/network.h include/common.h
	mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude tests/network_test.c src/network.c -o $@

build/worker_test: tests/worker_test.c tests/compress_stub.c tests/compress_stub.h tests/checksum_bitmap_stub.c src/worker.c src/network.c src/file_reader.c src/file_writer.c include/worker.h include/bitmap.h include/checksum.h include/network.h include/compress.h include/file_reader.h include/file_writer.h include/common.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/worker_test.c tests/compress_stub.c tests/checksum_bitmap_stub.c src/worker.c src/network.c src/file_reader.c src/file_writer.c -o $@ -lz -lpthread

build/integration_test: $(TARGET) tests/integration_test.c src/network.c src/compress.c include/network.h include/compress.h include/common.h
	mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude tests/integration_test.c src/network.c src/compress.c -o $@ -lz -lpthread

format:
	find src include -type f \( -name '*.c' -o -name '*.h' \) -exec $(CLANG_FORMAT) -i {} +

clean:
	rm -rf build

bench: $(TARGET)
	python3 bench/run_bench.py

.PHONY: all clean format init test bench
