#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
OUT="${CTN_OUT_DIR:-$HERE/../out/user}"
while (($#)); do
	case "$1" in
		--out) [[ $# -ge 2 && -n "$2" ]] || { printf 'missing --out directory\n' >&2; exit 2; }; OUT="$2"; shift 2 ;;
		*) printf 'usage: build_tools.sh [--out DIRECTORY]\n' >&2; exit 2 ;;
	esac
done
NDK="${NDK:-/opt/ndkroot/android-ndk-r26b}"
TOOL="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
if [[ -z ${CC:-} ]]; then
	CC="$TOOL/aarch64-linux-android${API:-21}-clang"
	STRIP="${STRIP:-$TOOL/llvm-strip}"
else
	STRIP="${STRIP:-strip}"
fi
command -v "$CC" >/dev/null || { printf 'compiler unavailable: %s\n' "$CC" >&2; exit 1; }
command -v "$STRIP" >/dev/null || { printf 'strip unavailable: %s\n' "$STRIP" >&2; exit 1; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
[[ "$OUT" != "$HERE" ]] || { printf 'refusing to overwrite shipped test binaries\n' >&2; exit 2; }
TMP="$(mktemp -d "$OUT/.tools.XXXXXX")"
trap 'rm -rf -- "$TMP"' EXIT
for name in fakepkg fakethreads; do
	EXTRA=()
	[[ "$name" != fakethreads ]] || EXTRA=(-pthread)
	"$CC" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -fPIE -pie \
		-Wl,--build-id=none "$HERE/$name.c" "${EXTRA[@]}" -o "$TMP/$name"
	"$STRIP" "$TMP/$name"
	chmod 0755 "$TMP/$name"
done
for name in fakepkg fakethreads; do mv -f -- "$TMP/$name" "$OUT/$name"; done
printf 'Built test tools in %s\n' "$OUT"
