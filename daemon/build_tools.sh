#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# 用 NDK 编 daemon 目录里的两个测试工具（fakepkg / fakethreads）
#
# 为什么单独一个脚本：这两个工具的编译参数以前只存在于历史会话里，
# 没有落盘 —— 本机重编和 GitHub Actions（debian:13 容器）都要用同一套参数，
# 产物才能对得上。ctnd 的编译在 daemon/build.sh，互不依赖。
#
# 用法：
#   bash daemon/build_tools.sh              # 默认 NDK r26b（/opt/ndkroot）
#   NDK=/path/to/ndk bash daemon/build_tools.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NDK="${NDK:-/opt/ndkroot/android-ndk-r26b}"
TOOL="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
API="${API:-21}"
CC="$TOOL/aarch64-linux-android${API}-clang"

[ -x "$CC" ] || { echo "!! 找不到 $CC（用 NDK=... 指定 NDK 根目录）"; exit 1; }

build_one() {	# build_one <名字> <额外参数...>
	local name="$1"; shift
	echo "=== 编译 $name ==="
	"$CC" -O2 -Wall -Wextra -Wno-unused-parameter \
		-fno-strict-aliasing -Wl,--build-id=none \
		-o "$HERE/$name" "$HERE/$name.c" "$@" 2>&1 | sed 's/^/  /'
	local rc=${PIPESTATUS[0]}
	[ $rc -ne 0 ] || [ ! -f "$HERE/$name" ] && { echo "!! $name 编译失败 rc=$rc"; return 1; }
	"$TOOL/llvm-strip" "$HERE/$name"
	ls -la "$HERE/$name" | sed 's/^/  /'
}

build_one fakepkg
# fakethreads 开线程用 pthread，必须 -pthread
build_one fakethreads -pthread
