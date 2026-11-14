#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ctnd-host.XXXXXX")"
trap 'rm -rf -- "$ROOT"' EXIT
NDK="${NDK:-/opt/ndkroot/android-ndk-r26b}"
COMPILERS=()
if [[ -n ${CC:-} ]]; then
	command -v "$CC" >/dev/null || { printf 'requested compiler unavailable: %s\n' "$CC" >&2; exit 1; }
	COMPILERS=("$CC")
else
	if command -v gcc >/dev/null; then COMPILERS+=(gcc); else printf 'SKIP gcc: unavailable\n'; fi
	if command -v clang >/dev/null; then
		COMPILERS+=(clang)
	elif [[ -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
		COMPILERS+=("$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang")
	else
		printf 'SKIP clang: unavailable\n'
	fi
fi
((${#COMPILERS[@]})) || { printf 'no native C compiler\n' >&2; exit 1; }
FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2
	-Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wwrite-strings -Wundef)
PARSE=("$HERE/ctnd_parse.c")
STATE=("$HERE/ctnd_state.c" "${PARSE[@]}")
RUNTIME=("$HERE/ctnd_runtime.c" "$HERE/ctnd_fs.c" "${PARSE[@]}")
DB=("$HERE/ctnd_db.c" "$HERE/ctnd_fs.c" "${PARSE[@]}")
ALL=("$HERE/ctnd.c" "$HERE/ctnd_parse.c" "$HERE/ctnd_state.c" "$HERE/ctnd_runtime.c" "$HERE/ctnd_fs.c" "$HERE/ctnd_db.c")
SAN_LINK=()
run_suite() {
	local cc="$1" out="$2"; shift 2
	local -a extra=("$@")
	mkdir -p "$out"
	"$cc" "${FLAGS[@]}" "${extra[@]}" -shared -fPIC "$HERE/tests/incomplete_sqlite.c" "${SAN_LINK[@]}" -o "$out/incomplete.so"
	"$cc" "${FLAGS[@]}" "${extra[@]}" "$HERE/tests/test_parse.c" "${PARSE[@]}" "${SAN_LINK[@]}" -o "$out/test_parse"
	"$cc" "${FLAGS[@]}" "${extra[@]}" "$HERE/tests/test_state.c" "${STATE[@]}" "${SAN_LINK[@]}" -o "$out/test_state"
	"$cc" "${FLAGS[@]}" "${extra[@]}" "$HERE/tests/test_runtime.c" "${RUNTIME[@]}" "${SAN_LINK[@]}" -o "$out/test_runtime"
	"$cc" "${FLAGS[@]}" "${extra[@]}" -DCTND_TEST "$HERE/tests/test_db.c" "${DB[@]}" -ldl "${SAN_LINK[@]}" -o "$out/test_db"
	"$out/test_parse"
	"$out/test_state"
	"$out/test_runtime"
	"$out/test_db" "$out/incomplete.so"
}
index=0
for cc in "${COMPILERS[@]}"; do
	SAN_LINK=()
	index=$((index + 1))
	out="$ROOT/compiler-$index"
	printf '\n=== Native tests: %s ===\n' "$cc"
	run_suite "$cc" "$out/plain"
	"$cc" "${FLAGS[@]}" "${ALL[@]}" -ldl -o "$out/ctnd"
	[[ "$("$out/ctnd" -V)" == 'ctnd 3.0' ]]
	"$out/ctnd" -h >/dev/null
	for arg in --unknown --node /arbitrary; do
		if "$out/ctnd" "$arg" >"$out/cli.log" 2>&1; then printf 'FAIL: unknown CLI accepted\n' >&2; exit 1; fi
	done
	for value in -1 abc 9999999999999999999999999999999 1x; do
		if "$out/ctnd" -r "$value" >"$out/cli.log" 2>&1; then printf 'FAIL: invalid -r accepted\n' >&2; exit 1; fi
	done
	if "$out/ctnd" -d >"$out/cli.log" 2>&1; then printf 'FAIL: missing -d accepted\n' >&2; exit 1; fi
	printf 'PASS: version and CLI rejection tests\n'
	if command -v python3 >/dev/null; then
		mkdir -p "$out/process"
		"$cc" "${FLAGS[@]}" -DCTND_RUNTIME_DIR="\"$out/process/runtime\"" \
			-DCTND_NAMES_PATH="\"$out/process/names\"" -DCTND_ENABLE_PATH="\"$out/process/enable\"" \
			-DCTND_GAMEPID_PATH="\"$out/process/game_pid\"" -DCTND_KMSG_PATH='"/dev/null"' \
			"${ALL[@]}" -ldl -o "$out/process/ctnd"
		"$cc" -std=c11 -O1 -Wall -Wextra -Werror "$HERE/fakepkg.c" -o "$out/process/fakepkg"
		python3 -I "$HERE/tests/test_process.py" "$out/process/ctnd" "$out/process/fakepkg" "$out/process"
	else
		printf 'SKIP process integration: python3 unavailable\n'
	fi
	CC="$cc" bash "$HERE/lint.sh"
	if [[ ${CTND_SANITIZERS:-auto} == 0 ]]; then
		printf 'SKIP sanitizers: explicitly disabled\n'
		continue
	fi
	printf '#include <stdio.h>\n#include <stdlib.h>\nint main(void) { char *p = malloc(64); if (!p) return 1; p[0] = 0; puts(p); free(p); return 0; }\n' >"$out/san_probe.c"
	for san in address undefined; do
		SAN_LINK=()
		SAN_FLAGS=(-fsanitize="$san" -fno-omit-frame-pointer)
		# NDK's bundled Linux sanitizer is older than Debian's glibc. Keep clang
		# instrumentation but use the installed native runtime for these host tests.
		if [[ "$cc" == "$NDK/"* ]]; then
			SAN_FLAGS+=(-fno-sanitize-link-runtime)
			if [[ "$san" == address ]]; then SAN_LINK=(-lasan); else SAN_LINK=(-lubsan); fi
			printf 'Using Debian runtime for NDK clang %s instrumentation\n' "$san"
		fi
		if "$cc" "${SAN_FLAGS[@]}" "$out/san_probe.c" "${SAN_LINK[@]}" -o "$out/san_probe" >"$out/san_probe.log" 2>&1 &&
		   "$out/san_probe" >>"$out/san_probe.log" 2>&1; then
			printf '\n=== Sanitizer %s: %s ===\n' "$san" "$cc"
			ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
				run_suite "$cc" "$out/$san" "${SAN_FLAGS[@]}"
		else
			printf 'SKIP %s for %s: compiler/runtime probe failed\n' "$san" "$cc"
			while IFS= read -r line; do printf '%s\n' "$line"; done <"$out/san_probe.log"
			[[ ${CTND_SANITIZERS:-auto} != required ]] || exit 1
		fi
	done
done
printf '\nPASS: all available host suites completed\n'
