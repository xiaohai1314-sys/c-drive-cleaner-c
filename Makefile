CC_NATIVE ?= gcc
CC_WIN ?= x86_64-w64-mingw32-gcc
# 与 AGENTS.md 一致：apt 没有 gcc-mingw-w64-x86-64，用磁盘上的 llvm-mingw
MINGW_BIN ?= /home/hatch/workspace/.scratch/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64/bin

CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -Isrc
SRCS := src/main.c src/mft_util.c src/bigfile.c src/mft_raw.c src/util.c

.PHONY: all native win clean check time-build test_util

all: native

native: build/cdc-c-pilot
build/cdc-c-pilot: $(SRCS) src/*.h
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -pthread -o $@ $(SRCS)

win: build/cdc-c-pilot.exe build/test_util.exe
build/cdc-c-pilot.exe: $(SRCS) src/*.h
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) -o $@ $(SRCS)

# util 自测：Linux 原生跑；Windows 交叉只编不过不跑（无 Windows 环境）
test_util: build/test_util
	./build/test_util
build/test_util: tests/test_util.c src/util.c src/util.h
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -pthread -o $@ tests/test_util.c src/util.c
build/test_util.exe: tests/test_util.c src/util.c src/util.h
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) -o $@ tests/test_util.c src/util.c

check: native test_util
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

# 切片 4（fileindex.c / indextask.c）暂未并入 SRCS：
# 它们依赖 util.h（切片 1）与 third_party/sqlite3.c（切片 1），
# 且 main.c 的 --index 编排依赖切片 2 的 mft.c 与切片 3 的
# hashcache.c / scan.c；等依赖落地后把这三个文件加进 SRCS 即可
# （链接 Windows 版时再加 -ladvapi32 -lshell32）。
# 本片验证在 /tmp 私有副本完成（桩实现 + sqlite amalgamation，
# 见切片 4 汇报），不经过本 Makefile。
