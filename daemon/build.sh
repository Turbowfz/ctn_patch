#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# 用 NDK 编 ctnd（aarch64 Android 用户态程序）
#
# 为什么用 C 而不是 Rust：NDK 里现成有 aarch64 clang，编出来是几十 KB 的
# 单文件、零运行时依赖（只 dlopen 设备自带的 libsqlite.so），
# 塞进 Magisk 模块最省事。
#
# 用法：
#   bash daemon/build.sh                    # 默认用 NDK r26b
#   NDK=/path/to/ndk bash daemon/build.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NDK="${NDK:-/opt/ndkroot/android-ndk-r26b}"
TOOL="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
# API 21 就够：只用 open/read/write/usleep/dlopen 这些老接口，
# 低 API 编出来的二进制在新系统上照样跑。
API="${API:-21}"
CC="$TOOL/aarch64-linux-android${API}-clang"

[ -x "$CC" ] || { echo "!! 找不到 $CC（用 NDK=... 指定 NDK 根目录）"; exit 1; }

echo "=== 编译 ctnd ==="
echo "  编译器: $CC"
"$CC" --version | head -1 | sed 's/^/  /'

"$CC" -O2 -Wall -Wextra -Wno-unused-parameter \
	-fno-strict-aliasing \
	-Wl,--build-id=none \
	-o "$HERE/ctnd" "$HERE/ctnd.c" -ldl 2>&1 | sed 's/^/  /'

RC=${PIPESTATUS[0]}
if [ $RC -ne 0 ] || [ ! -f "$HERE/ctnd" ]; then
	echo "!! 编译失败 rc=$RC"
	exit 1
fi

echo
echo "=== 产物 ==="
ls -la "$HERE/ctnd" | sed 's/^/  /'
echo "  架构: $("$TOOL/llvm-readelf" -h "$HERE/ctnd" 2>/dev/null | sed -n 's/.*Machine: *//p')"
echo "  依赖:"
"$TOOL/llvm-readelf" -d "$HERE/ctnd" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/    \1/p'
echo
echo "=== 语法/警告检查（-Wall -Wextra 无输出即干净）==="
echo "  完成"
