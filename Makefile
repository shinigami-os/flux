CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11

.PHONY: clean

OBJS = build/main.o build/util.o build/cmd_install.o build/cmd_search.o build/cmd_build.o build/cmd_remove.o build/cmd_update.o build/cmd_info.o build/cmd_cache.o build/cmd_compat.o build/parser.o

all: build/flux

build:
	mkdir -p build

build/flux: $(OBJS) | build
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o build/flux

build/%.o : src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build/