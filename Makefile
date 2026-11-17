CC_NATIVE ?= gcc
CC_WIN ?= x86_64-w64-mingw32-gcc
# 与 AGENTS.md 一致：apt 没有 gcc-mingw-w64-x86-64，用磁盘上的 llvm-mingw
MINGW_BIN ?= /home/hatch/workspace/.scratch/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64/bin

CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -Isrc -Ithird_party/sqlite3 -Ithird_party/blake3
SRCS := src/main.c src/mft_util.c src/bigfile.c src/mft_raw.c src/util.c \
        src/fileindex.c src/indextask.c src/hashcache.c src/scan.c src/fastio.c
# mft.c / sqlite3.c / blake3 对象不在 SRCS 里，见下面单独的编译规则。
MFT_NATIVE_OBJ := build/mft-native.o
MFT_WIN_OBJ := build/mft-win.o
SQLITE3_NATIVE_OBJ := build/sqlite3-native.o
SQLITE3_WIN_OBJ := build/sqlite3-win.o
# BLAKE3 对象（与下面切片 2 的 CORE_BLAKE3_* 是同一组文件；这些变量
# 必须在 native/win 规则之前定义——make 在读 Makefile 时就展开依赖表，
# 写在后面的变量在前面的规则里是空的，链接会找不到 .o；recipe 里的
# 引用是执行时才展开，所以链接命令里会出现 .o 但从没编过）。
CORE_BUILD ?= build
CORE_BLAKE3_OBJS := $(CORE_BUILD)/blake3-core.o $(CORE_BUILD)/blake3_dispatch-core.o \
	$(CORE_BUILD)/blake3_portable-core.o $(CORE_BUILD)/blake3_sse2-core.o \
	$(CORE_BUILD)/blake3_sse41-core.o $(CORE_BUILD)/blake3_avx2-core.o \
	$(CORE_BUILD)/blake3_avx512-core.o
CORE_BLAKE3_WIN_OBJS := $(CORE_BUILD)/blake3-core-win.o $(CORE_BUILD)/blake3_dispatch-core-win.o \
	$(CORE_BUILD)/blake3_portable-core-win.o $(CORE_BUILD)/blake3_sse2-core-win.o \
	$(CORE_BUILD)/blake3_sse41-core-win.o $(CORE_BUILD)/blake3_avx2-core-win.o \
	$(CORE_BUILD)/blake3_avx512-core-win.o

.PHONY: all native win clean check time-build test_util check-slice3 win-slice3

all: native

native: build/cdc-c-pilot
build/cdc-c-pilot: $(SRCS) $(MFT_NATIVE_OBJ) $(SQLITE3_NATIVE_OBJ) $(CORE_BLAKE3_OBJS) src/*.h
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -pthread -o $@ $(SRCS) $(MFT_NATIVE_OBJ) $(SQLITE3_NATIVE_OBJ) $(CORE_BLAKE3_OBJS)

win: build/cdc-c-pilot.exe build/test_util.exe
build/cdc-c-pilot.exe: $(SRCS) $(MFT_WIN_OBJ) $(SQLITE3_WIN_OBJ) $(CORE_BLAKE3_WIN_OBJS) src/*.h
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) -o $@ $(SRCS) $(MFT_WIN_OBJ) $(SQLITE3_WIN_OBJ) $(CORE_BLAKE3_WIN_OBJS) -ladvapi32 -lshell32

# sqlite3.c 单独编：vendored 代码只开 -Wall -Wextra、不开 -Werror
# （与 CMakeLists.txt 的策略一致，上游源码自带 -Wunused-parameter
# 等警告）。注意：这里不加 -Isrc——历史原因：src/process.h 曾遮住
# CRT 的 <process.h>，导致 _beginthreadex/_endthreadex 未声明；
# 该文件已改名 cdc_process.h，地雷已拆，但本规则保持最小 flag 不变。
$(SQLITE3_NATIVE_OBJ): third_party/sqlite3/sqlite3.c
	@mkdir -p build
	$(CC_NATIVE) -O2 -std=c11 -Wall -Wextra -Ithird_party/sqlite3 -c $< -o $@
$(SQLITE3_WIN_OBJ): third_party/sqlite3/sqlite3.c
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Wall -Wextra -Ithird_party/sqlite3 -c $< -o $@

# mft.c 单独编：需 POSIX 声明（lstat），故加 -D_POSIX_C_SOURCE=200809L
# -D_DEFAULT_SOURCE=1。-Wno-error=unused-function 放宽已于 2026-11-17
# 去掉：mft.c 里只在 _WIN32 代码里被调用的 static 函数（快照内存缓存、
# u64 哈希表、互斥/线程、resolve_dir 等）已用 #ifdef _WIN32 隔离，
# Linux 下严格编译干净。Windows 下这两个问题都不存在。
$(MFT_NATIVE_OBJ): src/mft.c src/mft.h
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE=1 -c $< -o $@
$(MFT_WIN_OBJ): src/mft.c src/mft.h
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) -c $< -o $@

# util 自测：Linux 原生跑；Windows 交叉只编不过不跑（无 Windows 环境）
test_util: build/test_util
	./build/test_util
build/test_util: tests/test_util.c src/util.c src/util.h
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -pthread -o $@ tests/test_util.c src/util.c
build/test_util.exe: tests/test_util.c src/util.c src/util.h
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) -o $@ tests/test_util.c src/util.c

check: native test_util check-core
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

# 切片 4 备注：
#  - fileindex.c / indextask.c 已并入 SRCS（--index 编排在 main.c 里）。
#  - 切片 3 真管线已落地：hashcache.c / scan.c / fastio.c 在 SRCS 里，
#    临时桩 src/hashcache_scan_stub.c 已删除（main.c 的 CROSS-SLICE
#    INTERFACE 本地块已移入 hashcache.h / scan.h）；--index 现在跑真
#    哈希管线，不再是"只写元数据 + 游标"。
#  - /tmp 私有副本的端到端 --index 验证（桩 + 夹具树）见切片 4 汇报；
#    真管线落地后应在同一副本上重跑（见切片 3 收尾汇报）。

# ---- 切片 3（safety/rules/删除/隐私/系统/IM）自测（增补） ----
# 只加目标，不改现有目标。验证在 /tmp 私有副本里跑，不要和别的
# 切片抢 ~/workspace/cdc-c/build/：SLICE3_BUILD 可覆盖输出目录。
SLICE3_BUILD ?= build
SLICE3_SRCS := src/safety.c src/util.c src/mft_util.c src/rules.c \
	src/im.c src/system.c src/privacy.c src/shred.c src/recycle.c src/process.c
# logs.c / timeutil.c（均为本切片文件）单独编译：timeutil.c 在
# -std=c11 -Werror 下有 -Wformat-truncation 误报（年月日来自系统时间，
# 范围已定），仍不带 -Werror；logs.c 的 cdc_write_log 里 index>10000
# 分支曾有真 use-after-free（free(stamp) 后又在错误文案中读 stamp，
# 同一秒内 10001 个日志文件才触发），2026-11-17 已修（先拼文案再 free），
# 现与其它自家文件一样带 -Werror 编译。
# -D_GNU_SOURCE：-std=c11 下 strdup/setenv/readlink 等 POSIX 函数
# 需要它才有声明（logs.c / 测试文件都用到了）。
SLICE3_DEPS := src/logs.c src/timeutil.c
SLICE3_INC := -Isrc -Ithird_party -Ithird_party/sqlite3 -Ithird_party/cjson
SLICE3_DEFS := -D_GNU_SOURCE
SLICE3_LIBS_WIN := -ladvapi32 -lshell32 -lwinhttp -lbcrypt -lntdll

check-slice3: $(SLICE3_BUILD)/test_safety $(SLICE3_BUILD)/test_slice3
	./$(SLICE3_BUILD)/test_safety
	./$(SLICE3_BUILD)/test_slice3

$(SLICE3_BUILD)/test_safety: tests/test_safety.c src/safety.c src/util.c src/mft_util.c $(SLICE3_DEPS) third_party/cjson/cJSON.c src/*.h
	@mkdir -p $(SLICE3_BUILD)
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -c src/logs.c -o $(SLICE3_BUILD)/logs.o
	$(CC_NATIVE) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) -c src/timeutil.c -o $(SLICE3_BUILD)/timeutil.o
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/cjson -c third_party/cjson/cJSON.c -o $(SLICE3_BUILD)/cjson.o
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -pthread -o $@ tests/test_safety.c src/safety.c src/util.c src/mft_util.c $(SLICE3_BUILD)/logs.o $(SLICE3_BUILD)/timeutil.o $(SLICE3_BUILD)/cjson.o

# 第三方 amalgamation（sqlite3/cJSON）单独编译，不带 -Wall -Wextra -Werror
#（上游代码自带 -Wunused-parameter 等警告，不是本切片的问题）。
$(SLICE3_BUILD)/test_slice3: tests/test_slice3.c $(SLICE3_SRCS) $(SLICE3_DEPS) third_party/sqlite3/sqlite3.c third_party/cjson/cJSON.c src/*.h
	@mkdir -p $(SLICE3_BUILD)
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -c src/logs.c -o $(SLICE3_BUILD)/logs.o
	$(CC_NATIVE) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) -c src/timeutil.c -o $(SLICE3_BUILD)/timeutil.o
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/sqlite3 -Ithird_party/cjson -c third_party/sqlite3/sqlite3.c -o $(SLICE3_BUILD)/sqlite3.o
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/cjson -c third_party/cjson/cJSON.c -o $(SLICE3_BUILD)/cjson.o
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -pthread -o $@ tests/test_slice3.c $(SLICE3_SRCS) $(SLICE3_BUILD)/logs.o $(SLICE3_BUILD)/timeutil.o $(SLICE3_BUILD)/sqlite3.o $(SLICE3_BUILD)/cjson.o -ldl -lm

win-slice3: $(SLICE3_BUILD)/test_safety.exe $(SLICE3_BUILD)/test_slice3.exe

$(SLICE3_BUILD)/test_safety.exe: tests/test_safety.c src/safety.c src/util.c src/mft_util.c $(SLICE3_DEPS) third_party/cjson/cJSON.c src/*.h
	@mkdir -p $(SLICE3_BUILD)
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -c src/logs.c -o $(SLICE3_BUILD)/logs-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) -c src/timeutil.c -o $(SLICE3_BUILD)/timeutil-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/cjson -c third_party/cjson/cJSON.c -o $(SLICE3_BUILD)/cjson-win.o
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -o $@ tests/test_safety.c src/safety.c src/util.c src/mft_util.c $(SLICE3_BUILD)/logs-win.o $(SLICE3_BUILD)/timeutil-win.o $(SLICE3_BUILD)/cjson-win.o $(SLICE3_LIBS_WIN)

# BLAKE3：rules.c 的可选 pin 校验只在 Windows 用（WinHTTP update 内）；
# 配方照抄 CMakeLists.txt：3 个通用文件 + x86 SIMD 文件（各带 -m 开关）。
$(SLICE3_BUILD)/test_slice3.exe: tests/test_slice3.c $(SLICE3_SRCS) $(SLICE3_DEPS) third_party/sqlite3/sqlite3.c third_party/cjson/cJSON.c third_party/blake3/blake3.c src/*.h
	@mkdir -p $(SLICE3_BUILD)
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -c src/logs.c -o $(SLICE3_BUILD)/logs-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) -c src/timeutil.c -o $(SLICE3_BUILD)/timeutil-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/sqlite3 -Ithird_party/cjson -c third_party/sqlite3/sqlite3.c -o $(SLICE3_BUILD)/sqlite3-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/cjson -c third_party/cjson/cJSON.c -o $(SLICE3_BUILD)/cjson-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c third_party/blake3/blake3.c -o $(SLICE3_BUILD)/blake3-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c third_party/blake3/blake3_dispatch.c -o $(SLICE3_BUILD)/blake3_dispatch-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c third_party/blake3/blake3_portable.c -o $(SLICE3_BUILD)/blake3_portable-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -msse2 -Ithird_party/blake3 -c third_party/blake3/blake3_sse2.c -o $(SLICE3_BUILD)/blake3_sse2-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -msse4.1 -Ithird_party/blake3 -c third_party/blake3/blake3_sse41.c -o $(SLICE3_BUILD)/blake3_sse41-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -mavx2 -Ithird_party/blake3 -c third_party/blake3/blake3_avx2.c -o $(SLICE3_BUILD)/blake3_avx2-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -mavx512f -mavx512vl -Ithird_party/blake3 -c third_party/blake3/blake3_avx512.c -o $(SLICE3_BUILD)/blake3_avx512-win.o
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -o $@ tests/test_slice3.c $(SLICE3_SRCS) $(SLICE3_BUILD)/logs-win.o $(SLICE3_BUILD)/timeutil-win.o $(SLICE3_BUILD)/sqlite3-win.o $(SLICE3_BUILD)/cjson-win.o $(SLICE3_BUILD)/blake3-win.o $(SLICE3_BUILD)/blake3_dispatch-win.o $(SLICE3_BUILD)/blake3_portable-win.o $(SLICE3_BUILD)/blake3_sse2-win.o $(SLICE3_BUILD)/blake3_sse41-win.o $(SLICE3_BUILD)/blake3_avx2-win.o $(SLICE3_BUILD)/blake3_avx512-win.o $(SLICE3_LIBS_WIN)

# ---- 切片 2（mft/fastio/hashcache/scan）自测（增补） ----
# 只加目标，不改现有目标（check 除外：按任务要求并入 make check）。
# 验证在 /tmp 私有副本里跑，不要和别的切片抢 ~/workspace/cdc-c/build/：
# CORE_BUILD 可覆盖输出目录（默认 build）。
CORE_BUILD ?= build
CORE_SRCS := src/mft.c src/fastio.c src/hashcache.c src/scan.c src/util.c \
	src/bigfile.c src/mft_util.c
CORE_INC := -Isrc -Ithird_party/sqlite3 -Ithird_party/blake3 -Ithird_party/cjson
CORE_DEFS := -D_GNU_SOURCE
# cJSON 在树里时才开 v2 迁移（CDC_HAVE_CJSON）；不在时 hashcache 的
# 迁移为 no-op，测试的迁移段自动跳过（#ifdef CDC_HAVE_CJSON）。
ifneq ($(wildcard third_party/cjson/cJSON.c),)
CORE_DEFS += -DCDC_HAVE_CJSON
CORE_CJSON_SRC := third_party/cjson/cJSON.c
CORE_CJSON_OBJ := $(CORE_BUILD)/cjson-core.o
CORE_CJSON_WIN_OBJ := $(CORE_BUILD)/cjson-core-win.o
else
CORE_CJSON_SRC :=
CORE_CJSON_OBJ :=
CORE_CJSON_WIN_OBJ :=
endif
# BLAKE3：照抄 CMakeLists.txt / 切片 3 的配方：3 个通用文件 + x86 SIMD
# 文件（各带 -m 开关），不用 BLAKE3_NO_* portable-only 定义。
# 对象列表见文件顶部（CORE_BLAKE3_OBJS / CORE_BLAKE3_WIN_OBJS，
# 必须在 native/win 规则之前定义，见那里注释）。
CORE_BLAKE3_SRCS := third_party/blake3/blake3.c third_party/blake3/blake3_dispatch.c \
	third_party/blake3/blake3_portable.c third_party/blake3/blake3_sse2.c \
	third_party/blake3/blake3_sse41.c third_party/blake3/blake3_avx2.c \
	third_party/blake3/blake3_avx512.c
CORE_THIRDPARTY_OBJS := $(CORE_BUILD)/sqlite3-core.o $(CORE_CJSON_OBJ) $(CORE_BLAKE3_OBJS)
CORE_THIRDPARTY_WIN_OBJS := $(CORE_BUILD)/sqlite3-core-win.o $(CORE_CJSON_WIN_OBJ) $(CORE_BLAKE3_WIN_OBJS)

check-core: $(CORE_BUILD)/test_scan
	@test -f tests/fixtures/setup.exe || sh tests/gen_fixtures.sh
	./$(CORE_BUILD)/test_scan

$(CORE_BUILD)/test_scan: tests/test_scan.c $(CORE_SRCS) third_party/sqlite3/sqlite3.c $(CORE_CJSON_SRC) $(CORE_BLAKE3_SRCS) src/*.h $(CORE_THIRDPARTY_OBJS)
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) $(CFLAGS) $(CORE_DEFS) $(CORE_INC) -pthread -o $@ tests/test_scan.c $(CORE_SRCS) $(CORE_THIRDPARTY_OBJS) -ldl -lm

# Windows 交叉：只编译不链接（test_scan 是 Linux-only，用的 mkdtemp/link/
# pwrite 等 POSIX API 在 MinGW 下没有）。编 CORE_SRCS + 第三方对象
# 即证明切片 2 交叉编译干净。
core-win: $(CORE_THIRDPARTY_WIN_OBJS)
	@mkdir -p $(CORE_BUILD)
	@for f in $(CORE_SRCS); do \
		$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(CORE_DEFS) $(CORE_INC) -c $$f -o $(CORE_BUILD)/$$(basename $$f .c)-win.o || exit 1; \
	done
	@echo "core-win: all slice-2 sources cross-compile clean"

# 第三方 amalgamation 单独编，不带 -Werror（上游代码自带警告）。
$(CORE_BUILD)/sqlite3-core.o: third_party/sqlite3/sqlite3.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Wall -Wextra -Ithird_party/sqlite3 -c $< -o $@
$(CORE_BUILD)/sqlite3-core-win.o: third_party/sqlite3/sqlite3.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Wall -Wextra -Ithird_party/sqlite3 -c $< -o $@
$(CORE_BUILD)/cjson-core.o: third_party/cjson/cJSON.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/cjson -c $< -o $@
$(CORE_BUILD)/cjson-core-win.o: third_party/cjson/cJSON.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/cjson -c $< -o $@

# BLAKE3 对象（native）。
$(CORE_BUILD)/blake3-core.o: third_party/blake3/blake3.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_dispatch-core.o: third_party/blake3/blake3_dispatch.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_portable-core.o: third_party/blake3/blake3_portable.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_sse2-core.o: third_party/blake3/blake3_sse2.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -msse2 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_sse41-core.o: third_party/blake3/blake3_sse41.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -msse4.1 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_avx2-core.o: third_party/blake3/blake3_avx2.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -mavx2 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_avx512-core.o: third_party/blake3/blake3_avx512.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -mavx512f -mavx512vl -Ithird_party/blake3 -c $< -o $@

# BLAKE3 对象（Windows 交叉，-m 开关同 native）。
$(CORE_BUILD)/blake3-core-win.o: third_party/blake3/blake3.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_dispatch-core-win.o: third_party/blake3/blake3_dispatch.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_portable-core-win.o: third_party/blake3/blake3_portable.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_sse2-core-win.o: third_party/blake3/blake3_sse2.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -msse2 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_sse41-core-win.o: third_party/blake3/blake3_sse41.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -msse4.1 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_avx2-core-win.o: third_party/blake3/blake3_avx2.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -mavx2 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_avx512-core-win.o: third_party/blake3/blake3_avx512.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -mavx512f -mavx512vl -Ithird_party/blake3 -c $< -o $@
