CC = gcc

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

clean:
	rm -rf build

.PHONY: all clean
