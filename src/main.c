/* main.c — cdc-c-pilot：C 版小试点 CLI
 *
 *   cdc-c-pilot selftest          跑全部移植单元测试（退出码 0=全过）
 *   cdc-c-pilot big <dir>         目录树大文件扫描（对标 Rust scan_big 的过滤逻辑）
 *   cdc-c-pilot probe C           [仅 Windows] 打开卷、读 NTFS 卷信息
 *
 * big 的扫描逻辑与 Rust scan_big 对齐：
 *   枚举时先按 2MB 预过滤（CDC_SCAN_MIN_BYTES），再按扩展名分档阈值过滤，
 *   跳过 pagefile.sys 等系统文件，按大小降序输出。
 * 区别：这里是目录遍历（Linux 上可跑、可验证），Rust 版走 $MFT 直读；
 * MFT 直读本身的速度优势不在这个 pilot 的验证范围里，见 README。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bigfile.h"
#include "mft_util.h"

#ifdef _WIN32
int cdc_mft_probe_volume(const wchar_t *drive_letter);
#endif

/* ---------------- selftest ---------------- */

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_filetime(void) {
    /* 1970-01-01T00:00:00Z 的 FILETIME = 11644473600 * 10^7 */
    CHECK(mft_filetime_to_unix(116444736000000000ULL) == 0, "filetime epoch");
    /* 2020-01-01T00:00:00Z: unix 1577836800 */
    CHECK(mft_filetime_to_unix(132223104000000000ULL) == 1577836800ULL, "filetime 2020");
    CHECK(mft_filetime_to_unix(0) == 0, "filetime 0 saturates");
    CHECK(mft_filetime_to_unix(50000000ULL) == 0, "filetime pre-epoch saturates");
}

static void test_runlist(void) {
    /* run1: header 0x11 len=2 off=+5 -> (vcn 0, lcn 5, len 2)
     * run2: header 0x21 len=3 off=+16 -> (vcn 2, lcn 21, len 3) */
    const uint8_t buf[] = {0x11, 0x02, 0x05, 0x21, 0x03, 0x10, 0x00, 0x00};
    size_t n = 0;
    MftRun *runs = mft_parse_runlist(buf, sizeof buf, &n);
    CHECK(runs && n == 2, "runlist count");
    if (runs && n == 2) {
        CHECK(runs[0].vcn == 0 && runs[0].lcn == 5 && runs[0].len == 2, "runlist run1");
        CHECK(runs[1].vcn == 2 && runs[1].lcn == 21 && runs[1].len == 3, "runlist run2");
    }
    free(runs);
    /* 稀疏 run：header 0x01（len_size=1, off_size=0）-> lcn -1 */
    const uint8_t sparse[] = {0x01, 0x04, 0x00};
    runs = mft_parse_runlist(sparse, sizeof sparse, &n);
    CHECK(runs && n == 1 && runs[0].lcn == -1 && runs[0].len == 4, "runlist sparse");
    free(runs);
    /* 非法 header（len_size=0）直接停，之前已解析的保留 */
    const uint8_t bad[] = {0x11, 0x02, 0x05, 0x00};
    runs = mft_parse_runlist(bad, sizeof bad, &n);
    CHECK(runs && n == 1, "runlist stops on bad header");
    free(runs);
}

static void test_first_extent(void) {
    uint64_t lcn = 0, clusters = 0;
    const uint8_t ok[] = {0x21, 0x10, 0x34, 0x12}; /* len 16, lcn 0x1234 */
    mft_parse_first_extent(ok, sizeof ok, &lcn, &clusters);
    CHECK(lcn == 0x1234 && clusters == 16, "first_extent ok");
    const uint8_t neg[] = {0x21, 0x01, 0xFF, 0xFF}; /* lcn -1 -> 无效 */
    mft_parse_first_extent(neg, sizeof neg, &lcn, &clusters);
    CHECK(lcn == 0 && clusters == 0, "first_extent negative lcn");
    mft_parse_first_extent(NULL, 0, &lcn, &clusters);
    CHECK(lcn == 0 && clusters == 0, "first_extent empty");
}

static void test_fixups(void) {
    uint8_t rec[1024];
    memset(rec, 0, sizeof rec);
    /* usa_off=8, usa_count=3；usa[1]=AA BB, usa[2]=CC DD */
    rec[4] = 8; rec[5] = 0; rec[6] = 3; rec[7] = 0;
    rec[8] = 0x55; rec[9] = 0x55;
    rec[10] = 0xAA; rec[11] = 0xBB;
    rec[12] = 0xCC; rec[13] = 0xDD;
    rec[510] = 0x11; rec[511] = 0x22;
    rec[1022] = 0x33; rec[1023] = 0x44;
    mft_apply_fixups(rec, sizeof rec, 512);
    CHECK(rec[510] == 0xAA && rec[511] == 0xBB, "fixup sector 1");
    CHECK(rec[1022] == 0xCC && rec[1023] == 0xDD, "fixup sector 2");
    CHECK(rec[8] == 0x55, "fixup leaves usa array alone");
}

static void test_utf16(void) {
    /* "A" + U+4E2D(中) + 代理对 U+1F600(😀) */
    const uint8_t bytes[] = {0x41, 0x00, 0x2D, 0x4E, 0x3D, 0xD8, 0x00, 0xDE};
    char *s = mft_utf16le_to_utf8(bytes, sizeof bytes);
    CHECK(s && strcmp(s, "A\xe4\xb8\xad\xf0\x9f\x98\x80") == 0, "utf16 incl. surrogate pair");
    free(s);
}

static void test_thresholds(void) {
    /* 与 Rust main.rs big_file_thresholds_follow_kbigfile_tiers 逐条一致 */
    const uint64_t MB = CDC_MB;
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\setup.exe") == 100 * MB, "thr exe");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\setup.msi") == 100 * MB, "thr msi");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\movie.mp4") == 20 * MB, "thr mp4");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\movie.MP4") == 20 * MB, "thr MP4 case");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\clip.tmp") == 20 * MB, "thr tmp");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\song.mp3") == 5 * MB, "thr mp3");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\pack.zip") == 5 * MB, "thr zip");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\app.apk") == 5 * MB, "thr apk");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\photo.jpg") == 2 * MB, "thr jpg");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\doc.pdf") == 2 * MB, "thr pdf");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\app.log") == 2 * MB, "thr log");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\data.bin") == 50 * MB, "thr bin fallback");
    CHECK(cdc_big_file_threshold_bytes("C:\\a\\README") == 50 * MB, "thr no-ext fallback");
    CHECK(cdc_is_system_file_name("C:\\pagefile.sys"), "sys pagefile");
    CHECK(!cdc_is_system_file_name("C:\\a\\movie.mp4"), "sys negative");
}

static int run_selftest(void) {
    test_filetime();
    test_runlist();
    test_first_extent();
    test_fixups();
    test_utf16();
    test_thresholds();
    printf("selftest: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

/* ---------------- big scan (POSIX 目录遍历；Windows 下用 _WIN32 分支时
 * 同样逻辑换 FindFirstFileW，见 README) ---------------- */

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>

typedef struct { char *path; uint64_t size; } BigRow;
static BigRow *rows = NULL;
static size_t nrows = 0, caprows = 0;

static void add_row(const char *path, uint64_t size) {
    if (nrows == caprows) {
        caprows = caprows ? caprows * 2 : 256;
        rows = realloc(rows, caprows * sizeof(BigRow));
        if (!rows) { perror("realloc"); exit(1); }
    }
    rows[nrows].path = strdup(path);
    rows[nrows].size = size;
    nrows++;
}

static void walk(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *full = malloc(strlen(dir) + strlen(e->d_name) + 2);
        sprintf(full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) { free(full); continue; }
        if (S_ISDIR(st.st_mode)) {
            walk(full);
        } else if (S_ISREG(st.st_mode)) {
            uint64_t size = (uint64_t)st.st_size;
            /* 与 Rust scan_big 相同：2MB 预过滤 + 分档阈值 + 跳过系统文件 */
            if (size >= CDC_SCAN_MIN_BYTES &&
                size >= cdc_big_file_threshold_bytes(full) &&
                !cdc_is_system_file_name(full))
                add_row(full, size);
        }
        free(full);
    }
    closedir(d);
}

static int cmp_desc(const void *a, const void *b) {
    uint64_t x = ((const BigRow *)a)->size, y = ((const BigRow *)b)->size;
    return (x < y) - (x > y);
}

static int run_big(const char *dir) {
    walk(dir);
    qsort(rows, nrows, sizeof(BigRow), cmp_desc);
    for (size_t i = 0; i < nrows; i++)
        printf("%llu\t%s\n", (unsigned long long)rows[i].size, rows[i].path);
    fprintf(stderr, "%zu big files\n", nrows);
    return 0;
}
#endif

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) return run_selftest();
    if (argc >= 3 && strcmp(argv[1], "big") == 0) {
#ifdef _WIN32
        fprintf(stderr, "big: POSIX walk build only in this pilot; Windows build uses MFT path (see README)\n");
        return 2;
#else
        return run_big(argv[2]);
#endif
    }
#ifdef _WIN32
    if (argc >= 3 && strcmp(argv[1], "probe") == 0) {
        wchar_t drive[4] = {0};
        drive[0] = (wchar_t)argv[2][0];
        return cdc_mft_probe_volume(drive);
    }
#endif
    fprintf(stderr,
        "usage:\n"
        "  %s selftest        移植单元测试\n"
        "  %s big <dir>       大文件扫描（分档阈值过滤，按大小降序）\n"
#ifdef _WIN32
        "  %s probe <C>       打开卷并读 NTFS 卷信息\n"
#endif
        , argv[0], argv[0]
#ifdef _WIN32
        , argv[0]
#endif
        );
    return 2;
}
