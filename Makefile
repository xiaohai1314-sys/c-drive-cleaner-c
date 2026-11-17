CC_NATIVE ?= gcc
CC_WIN ?= x86_64-w64-mingw32-gcc
# 与 AGENTS.md 一致：apt 没有 gcc-mingw-w64-x86-64，用磁盘上的 llvm-mingw
MINGW_BIN ?= /home/hatch/workspace/.scratch/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64/bin

CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Werror -Isrc
SRCS := src/main.c src/mft_util.c src/bigfile.c src/mft_raw.c

.PHONY: all native win clean check time-build

all: native

native: build/cdc-c-pilot
build/cdc-c-pilot: $(SRCS) src/*.h
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -o $@ $(SRCS)

win: build/cdc-c-pilot.exe
build/cdc-c-pilot.exe: $(SRCS) src/*.h
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) -o $@ $(SRCS)

check: native
	./build/cdc-c-pilot selftest

# 干净编译计时：与 Rust 版 cargo check ~15 秒、CI release 构建 5 分 20 秒对照
time-build:
	@$(MAKE) -s clean >/dev/null
	@echo "--- C native clean build ---"
	@bash -c 'time $(MAKE) -s native'
	@$(MAKE) -s clean >/dev/null
	@echo "--- C Windows cross clean build ---"
	@bash -c 'time $(MAKE) -s win'

clean:
	rm -rf build
