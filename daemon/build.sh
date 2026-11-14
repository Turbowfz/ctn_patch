#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
OUT="${CTN_OUT_DIR:-$HERE/../out/user}"
while (($#)); do
	case "$1" in
		--out) [[ $# -ge 2 && -n "$2" ]] || { printf 'missing --out directory\n' >&2; exit 2; }; OUT="$2"; shift 2 ;;
		*) printf 'usage: build.sh [--out DIRECTORY]\n' >&2; exit 2 ;;
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
[[ "$OUT" != "$HERE" ]] || { printf 'refusing to overwrite shipped daemon binaries\n' >&2; exit 2; }
TMP="$(mktemp "$OUT/.ctnd.XXXXXX")"
trap 'rm -f -- "$TMP"' EXIT
SOURCES=(ctnd.c ctnd_parse.c ctnd_state.c ctnd_fs.c ctnd_db.c ctnd_runtime.c)
for i in "${!SOURCES[@]}"; do SOURCES[$i]="$HERE/${SOURCES[$i]}"; done
printf 'Building ctnd with %s\n' "$CC"
"$CC" -std=c11 -Os -Wall -Wextra -Werror -fPIE -pie \
	-ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--build-id=none \
	"${SOURCES[@]}" -ldl -o "$TMP"
"$STRIP" "$TMP"
chmod 0755 "$TMP"
mv -f -- "$TMP" "$OUT/ctnd"
printf 'Built %s/ctnd\n' "$OUT"
