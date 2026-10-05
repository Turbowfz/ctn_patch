#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# 用最严格的警告级别静态检查 ctnd.c（不产出文件，只看警告）
set -uo pipefail
CC=/opt/ndkroot/android-ndk-r26b/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang
SRC=/mnt/c/Users/User/Desktop/风驰/6.1_一加ace3pro/05_补丁模块/ctn_patch/daemon/ctnd.c

echo "=== 严格警告检查 ==="
"$CC" -O2 -Wall -Wextra -Wpedantic -Wshadow -Wsign-compare \
	-Wformat=2 -Wstrict-prototypes -Wmissing-prototypes \
	-Wcast-qual -Wwrite-strings -Wundef -Wswitch-enum \
	-fsyntax-only "$SRC" 2>&1 | head -40
echo "--- 检查结束（上面无输出 = 干净）---"
