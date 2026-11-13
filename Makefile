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

format:
	find src include -type f \( -name '*.c' -o -name '*.h' \) -exec $(CLANG_FORMAT) -i {} +

clean:
	rm -rf build

.PHONY: all clean format init
