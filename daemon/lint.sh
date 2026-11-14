#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
NDK="${NDK:-/opt/ndkroot/android-ndk-r26b}"
CC="${CC:-$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${API:-21}-clang}"
command -v "$CC" >/dev/null || { printf 'compiler unavailable: %s\n' "$CC" >&2; exit 1; }
FLAGS=(-std=c11 -O2 -Wall -Wextra -Werror -Wpedantic -Wshadow -Wsign-compare
	-Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wwrite-strings
	-Wundef -Wswitch-enum -fsyntax-only)
printf 'Strict daemon checks with %s\n' "$CC"
for src in ctnd.c ctnd_parse.c ctnd_state.c ctnd_fs.c ctnd_db.c ctnd_runtime.c; do
	"$CC" "${FLAGS[@]}" "$HERE/$src"
done
printf 'PASS: all production translation units\n'
