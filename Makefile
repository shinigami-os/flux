CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11

.PHONY: clean build install static

OBJS = build/main.o build/util.o build/cmd_install.o build/cmd_search.o build/cmd_build.o build/cmd_remove.o build/cmd_update.o build/cmd_info.o build/cmd_list.o build/cmd_cache.o build/cmd_autoremove.o build/cmd_version.o build/cmd_selfupdate.o build/cmd_baseupdate.o build/cmd_kernelupdate.o build/parser.o build/alpine_index.o build/alpine_apk.o build/alpine_verify.o build/alpine_resolve.o build/alpine_triggers.o

all: build/flux

build:
	mkdir -p build

build/flux: $(OBJS) | build
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o build/flux

HEADERS = include/alpine.h include/flux.h include/parser.h include/util.h

build/%.o : src/%.c $(HEADERS) | build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build/

install: build/flux
	install -m 755 build/flux /usr/bin/flux
	mkdir -p /etc/flux/alpine-keys
	install -m 644 keys/alpine/*.rsa.pub /etc/flux/alpine-keys/

static: clean
	$(MAKE) LDFLAGS="-static"