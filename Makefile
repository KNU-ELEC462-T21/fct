CC = gcc
CLANG_FORMAT ?= clang-format

SRC = $(shell find src -name '*.c')
OBJ = $(SRC:src/%.c=build/%.o)

TARGET = build/fct

all: $(TARGET)

$(TARGET): $(OBJ)
	mkdir -p $(@D)
	$(CC) $^ -o $@

build/%.o: src/%.c
	mkdir -p $(@D)
	$(CC) -Iinclude -c $< -o $@

init:
	sudo apt-get update
	sudo apt-get install -y gcc clang-format

test: build/network_test build/cli_test build/test_file_writer
	./build/network_test
	./build/cli_test
	./build/test_file_writer

build/test_file_writer: tests/test_file_writer.c src/file_writer.c include/file_writer.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/test_file_writer.c src/file_writer.c -o $@

build/cli_test: tests/cli_test.c src/cli.c include/cli.h include/common.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/cli_test.c src/cli.c -o $@

build/network_test: tests/network_test.c src/network.c include/network.h include/common.h
	mkdir -p $(@D)
	$(CC) -Wall -Wextra -Werror -Iinclude tests/network_test.c src/network.c -o $@

format:
	find src include -type f \( -name '*.c' -o -name '*.h' \) -exec $(CLANG_FORMAT) -i {} +

clean:
	rm -rf build

.PHONY: all clean format init test
