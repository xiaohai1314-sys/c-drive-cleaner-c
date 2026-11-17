# c-drive-cleaner-c

C11 rewrite of [c-drive-cleaner](https://github.com/xiaohai1314-sys/c-drive-cleaner):
C backend (MFT/USN/index/safety) + Vue 3 frontend via WebView2.

Rust 版的问题只有一个：编译太慢。搜索速度的瓶颈在磁盘 I/O，换语言省不了；
省的是编译时间（C 干净编译秒级）和二进制体积（试点 exe 77KB）。

## 当前状态：种子

- `src/`：已移植的纯逻辑（试点验证通过）——`mft_util.c`（$MFT 解析函数）
  与 `bigfile.c`（10 档大文件阈值，与 Rust 版逐条一致），见 `docs/pilot.md`
- `src/mft_raw.c`：Windows 卷探测骨架（`CreateFileW(\\.\C:)` + `FSCTL_GET_NTFS_VOLUME_DATA`）
- `web/`：Vue 3 + Vite + TypeScript 前端（取自 Rust 版 main 3cf8b26，`npm run build` 通过）
- `docs/ARCHITECTURE.md`：架构契约——模块划分、API 约定、移植顺序

还没做的事：`parse_record_in_place`（$MFT 完整解析）、USN/快照/索引、
安全层、删除逻辑、WebView2 宿主与前后端桥接。按 `docs/ARCHITECTURE.md`
分片做，每片带自测 + Windows 交叉编译过关。

## 构建

```sh
# C 后端自测 + 编译（Linux 原生 / Windows 交叉）
make check
make time-build

# 前端
cd web && npm ci && npm run build
```

Windows 交叉编译用 llvm-mingw（`x86_64-w64-mingw32-gcc`）。

## 参考实现

Rust + Tauri 版：`xiaohai1314-sys/c-drive-cleaner`（main 3cf8b26，
含常驻索引 phases 1–4 的本地合入分支 `index/merge-main-3cf8b26`）。
