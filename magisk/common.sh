#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Shared, fail-closed runtime contract. metadata.prop is DATA: never source/eval it.

ctn_error() { printf 'ctn_patch: %s\n' "$*" >&2; }
ctn_note() { printf 'ctn_patch: %s\n' "$*"; }

ctn_init() {
    MODDIR=$(CDPATH= cd -- "$1" 2>/dev/null && pwd -P) || return 1
    # Fixed production roots. Host tests replace this constant in a temporary copy.
    CTN_ROOT=
    CTN_RUNTIME="$CTN_ROOT/data/adb/ctn_patch"
    CTN_PROC="$CTN_ROOT/proc"
    CTN_SYS="$CTN_ROOT/sys"
    CTN_NODE="$CTN_PROC/game_opt/task_boost/critical_task_name"
    CTN_ENABLE="$CTN_PROC/game_opt/task_boost/ct_enable"
    CTN_SHELL=$(command -v sh) || return 1
    CTN_SHELL_EXE=$(readlink -f "$CTN_SHELL") || return 1
    CTN_PACKAGE_VERSION=$(ctn_property "$MODDIR/module.prop" version) || return 1
    [ "$(ctn_property "$MODDIR/module.prop" id)" = ctn_patch ] || {
        ctn_error 'module.prop identity is not ctn_patch'; return 1;
    }
    CTN_EXPECTED_VERSION=${CTN_PACKAGE_VERSION#v}
    CTN_LOADED_BY_US=0
    # No pathname expansion of values read from files (lists are parsed explicitly).
    set -f
}

ctn_property() {
    local line key value found=0
    [ -f "$1" ] && [ ! -L "$1" ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        key=${line%%=*}
        [ "$key" = "$2" ] || continue
        [ "$line" != "$key" ] && [ "$found" = 0 ] || return 1
        value=${line#*=}; found=1
    done < "$1"
    [ "$found" = 1 ] || return 1
    printf '%s\n' "$value"
}

ctn_scalar() {
    local value
    value=$(cat "$1" 2>/dev/null) || return 1
    case "$value" in ''|*'
'*|*'\r'*) return 1 ;; esac
    printf '%s\n' "$value"
}

ctn_hash_valid() {
    [ "${#1}" = 64 ] || return 1
    case "$1" in *[!0-9a-f]*) return 1 ;; esac
}

ctn_sha256() {
    local output value
    output=$(sha256sum "$1" 2>/dev/null) || return 1
    value=${output%% *}
    ctn_hash_valid "$value" || return 1
    printf '%s\n' "$value"
}

ctn_list_contains() {
    local list=$1 wanted=$2 item
    while [ -n "$list" ]; do
        item=${list%%\|*}
        [ "$item" = "$wanted" ] && return 0
        [ "$list" != "$item" ] || break
        list=${list#*\|}
    done
    return 1
}

ctn_validate_list() {
    local list=$1 kind=$2 item
    case "$list" in ''|'|'*|*'|'|*'||'*) return 1 ;; esac
    while [ -n "$list" ]; do
        item=${list%%\|*}
        case "$kind" in
            hash) ctn_hash_valid "$item" || return 1 ;;
            path) case "$item" in
                /vendor/lib/modules/oplus_bsp_game_opt.ko|/vendor_dlkm/lib/modules/oplus_bsp_game_opt.ko) ;;
                *) return 1 ;; esac ;;
            release)
                case "$item" in *[!A-Za-z0-9_.+-]*) return 1 ;; esac
                case "$item" in "$CTN_PROFILE"|"$CTN_PROFILE"-*) ;; *) return 1 ;; esac ;;
            *) return 1 ;;
        esac
        [ "$item" != "$list" ] || break
        list=${list#*\|}
    done
}

ctn_read_metadata() {
    local line key value seen='|' exact='' releases=''
    CTN_SCHEMA= CTN_PROFILE= CTN_KERNEL_VERSION= CTN_ARCH= CTN_PAGE_SIZE=
    CTN_VENDOR_PATHS= CTN_VENDOR_HASHES= CTN_VENDOR_VERMAGIC= CTN_MODULE_HASH=
    CTN_PROFILE_HASH= CTN_BUILD_ID= CTN_MODULE_VERSION= CTN_DAEMON_HASH=
    CTN_NATIVE= CTN_PACKAGE_MODE= CTN_STATIC= CTN_RUNTIME_VERIFICATION= CTN_MANIFEST_HASH=
    [ -f "$1" ] && [ ! -L "$1" ] || { ctn_error "missing metadata: $1"; return 1; }
    while IFS= read -r line || [ -n "$line" ]; do
        key=${line%%=*}; value=${line#*=}
        [ "$key" != "$line" ] || { ctn_error 'metadata line lacks ='; return 1; }
        case "$seen" in *"|$key|"*) ctn_error "duplicate metadata key: $key"; return 1 ;; esac
        seen="$seen$key|"
        case "$key" in
            schema) CTN_SCHEMA=$value ;;
            profile_id) CTN_PROFILE=$value ;;
            kernel_version) CTN_KERNEL_VERSION=$value ;;
            arch) CTN_ARCH=$value ;;
            page_size) CTN_PAGE_SIZE=$value ;;
            runtime_kernel_release_exact) exact=$value ;;
            runtime_kernel_release_list) releases=$value ;;
            vendor_paths) CTN_VENDOR_PATHS=$value ;;
            vendor_sha256) CTN_VENDOR_HASHES=$value ;;
            vendor_vermagic) CTN_VENDOR_VERMAGIC=$value ;;
            module_sha256) CTN_MODULE_HASH=$value ;;
            profile_sha256) CTN_PROFILE_HASH=$value ;;
            build_id) CTN_BUILD_ID=$value ;;
            module_version) CTN_MODULE_VERSION=$value ;;
            daemon_sha256) CTN_DAEMON_HASH=$value ;;
            native_node) CTN_NATIVE=$value ;;
            package_mode) CTN_PACKAGE_MODE=$value ;;
            static_verification) CTN_STATIC=$value ;;
            runtime_verification) CTN_RUNTIME_VERIFICATION=$value ;;
            manifest_sha256) CTN_MANIFEST_HASH=$value ;;
            *) ctn_error "unknown metadata key: $key"; return 1 ;;
        esac
    done < "$1"
    [ "$CTN_SCHEMA" = 1 ] && [ "$CTN_PROFILE" = "$CTN_SELECTED_PROFILE" ] &&
        [ "$CTN_KERNEL_VERSION" = "$CTN_PROFILE" ] && [ "$CTN_ARCH" = aarch64 ] &&
        [ "$CTN_PAGE_SIZE" = 4096 ] && [ "$CTN_NATIVE" = 0 ] &&
        [ "$CTN_MODULE_VERSION" = "$CTN_EXPECTED_VERSION" ] && [ "$CTN_STATIC" = passed ] || {
        ctn_error 'incomplete or unsupported profile metadata'; return 1;
    }
    [ -z "$exact" ] || [ -z "$releases" ] || { ctn_error 'ambiguous release list'; return 1; }
    case "$exact" in *'|'*) ctn_error 'exact release contains a list'; return 1 ;; esac
    CTN_RELEASES=${exact:-$releases}
    ctn_validate_list "$CTN_RELEASES" release && ctn_validate_list "$CTN_VENDOR_PATHS" path &&
        ctn_validate_list "$CTN_VENDOR_HASHES" hash || { ctn_error 'invalid allowlist'; return 1; }
    for value in "$CTN_MODULE_HASH" "$CTN_PROFILE_HASH" "$CTN_BUILD_ID" "$CTN_DAEMON_HASH" "$CTN_MANIFEST_HASH"; do
        ctn_hash_valid "$value" || { ctn_error 'invalid/missing metadata SHA256'; return 1; }
    done
    case "$CTN_VENDOR_VERMAGIC" in
        "$CTN_PROFILE"-*' SMP preempt mod_unload modversions aarch64') ;;
        *) ctn_error 'missing complete vendor vermagic'; return 1 ;;
    esac
    case "$CTN_PACKAGE_MODE:$CTN_RUNTIME_VERIFICATION" in
        release:passed|development:passed|development:not_tested) ;;
        *) ctn_error 'invalid package verification status'; return 1 ;;
    esac
}

# This is an identity gate, not an ELF heuristic. The allowlisted whole vendor
# image binds full vermagic, CRCs, KCFI and layouts to the verified profile.
ctn_select() {
    local paths path file hash found=0
    CTN_RELEASE=$(uname -r) || return 1
    CTN_SELECTED_PROFILE=${CTN_RELEASE%%-*}
    case "$CTN_SELECTED_PROFILE" in 6.1.141|6.1.157) ;; *)
        ctn_error "unsupported kernel: $CTN_RELEASE (no insmod probing)"; return 1 ;; esac
    [ "$(uname -m)" = aarch64 ] || { ctn_error 'requires aarch64'; return 1; }
    CTN_ACTUAL_PAGE_SIZE=$(getconf PAGE_SIZE 2>/dev/null) || CTN_ACTUAL_PAGE_SIZE=$(getconf PAGESIZE 2>/dev/null)
    [ "$CTN_ACTUAL_PAGE_SIZE" = 4096 ] || { ctn_error 'requires verified 4096-byte pages'; return 1; }
    CTN_KERNEL_DIR="$MODDIR/kernels/$CTN_SELECTED_PROFILE"
    CTN_KO="$CTN_KERNEL_DIR/ctn_patch.ko"
    ctn_read_metadata "$CTN_KERNEL_DIR/metadata.prop" || return 1
    ctn_list_contains "$CTN_RELEASES" "$CTN_RELEASE" || {
        ctn_error "kernel release is not allowlisted: $CTN_RELEASE"; return 1;
    }
    [ ! -L "$CTN_KERNEL_DIR/manifest.json" ] &&
        [ "$(ctn_sha256 "$CTN_KERNEL_DIR/manifest.json")" = "$CTN_MANIFEST_HASH" ] || {
        ctn_error 'manifest hash mismatch'; return 1;
    }
    [ -f "$CTN_KO" ] && [ ! -L "$CTN_KO" ] &&
        [ "$(ctn_sha256 "$CTN_KO")" = "$CTN_MODULE_HASH" ] || {
        ctn_error 'target KO hash mismatch'; return 1;
    }
    [ -f "$MODDIR/bin/ctnd" ] && [ ! -L "$MODDIR/bin/ctnd" ] &&
        [ "$(ctn_sha256 "$MODDIR/bin/ctnd")" = "$CTN_DAEMON_HASH" ] || {
        ctn_error 'daemon file hash mismatch'; return 1;
    }
    CTN_VENDOR_FILE=
    paths=$CTN_VENDOR_PATHS
    while [ -n "$paths" ]; do
        path=${paths%%\|*}; file="$CTN_ROOT$path"
        if [ -e "$file" ] || [ -L "$file" ]; then
            [ -f "$file" ] && [ -r "$file" ] || { ctn_error "unreadable vendor KO: $path"; return 1; }
            hash=$(ctn_sha256 "$file") || { ctn_error "cannot hash vendor KO: $path"; return 1; }
            ctn_list_contains "$CTN_VENDOR_HASHES" "$hash" || {
                ctn_error "unknown vendor identity: $path SHA256=$hash"; return 1;
            }
            [ -n "$CTN_VENDOR_FILE" ] || CTN_VENDOR_FILE=$path
            found=1
        fi
        [ "$paths" != "$path" ] || break
        paths=${paths#*\|}
    done
    [ "$found" = 1 ] || { ctn_error 'no real allowlisted vendor KO is readable'; return 1; }
    ctn_note "profile=$CTN_PROFILE kernel=$CTN_RELEASE vendor=$CTN_VENDOR_FILE package=$CTN_PACKAGE_MODE runtime_test=$CTN_RUNTIME_VERIFICATION"
}

ctn_disabled() { [ -e "$MODDIR/disable" ] || [ -e "$MODDIR/remove" ] || [ -e "$MODDIR/.uninstalling" ]; }
ctn_game_idle() {
    local value pid
    value=$(ctn_scalar "$CTN_PROC/game_opt/game_pid") || return 1
    pid=${value%% *}
    [ "$pid" = game_pid=-1 ] || [ "$pid" = game_pid=0 ]
}
ctn_loaded() { [ -d "$CTN_SYS/module/ctn_patch" ]; }
ctn_dependency_ready() {
    grep -q '^oplus_bsp_game_opt ' "$CTN_PROC/modules" 2>/dev/null &&
        [ -d "$CTN_PROC/game_opt/task_boost" ]
}

ctn_running_identity() {
    ctn_loaded || { ctn_error 'ctn_patch is not loaded'; return 1; }
    CTN_RUNNING_VERSION=$(ctn_scalar "$CTN_SYS/module/ctn_patch/version")
    CTN_RUNNING_PROFILE=$(ctn_scalar "$CTN_SYS/module/ctn_patch/parameters/profile")
    CTN_RUNNING_BUILD=$(ctn_scalar "$CTN_SYS/module/ctn_patch/parameters/build_id")
    CTN_PATCH_READY=$(ctn_scalar "$CTN_SYS/module/ctn_patch/parameters/patch_ready")
    [ "$CTN_RUNNING_VERSION" = "$CTN_MODULE_VERSION" ] &&
        [ "$CTN_RUNNING_PROFILE" = "$CTN_PROFILE" ] && [ "$CTN_RUNNING_BUILD" = "$CTN_BUILD_ID" ] || {
        ctn_error "loaded identity differs/unknown: version=${CTN_RUNNING_VERSION:-unknown} profile=${CTN_RUNNING_PROFILE:-unknown} build_id=${CTN_RUNNING_BUILD:-unknown}; file is NOT running proof; reboot required"; return 1;
    }
    case "$CTN_PATCH_READY" in Y|1) ;; *) ctn_error 'loaded patch_ready is not true'; return 1 ;; esac
}

ctn_ensure_module() {
    local rc
    ctn_disabled && { ctn_error 'disabled/removing: refusing to load or enable'; return 1; }
    ctn_dependency_ready || { ctn_error 'vendor dependency is not ready'; return 1; }
    if ! ctn_loaded; then
        [ ! -e "$CTN_NODE" ] || { ctn_error 'unknown native node provenance; refusing'; return 1; }
        insmod "$CTN_KO"
        rc=$?
        [ "$rc" = 0 ] || { ctn_error "insmod failed rc=$rc; no retry with another KO"; return "$rc"; }
        CTN_LOADED_BY_US=1
    fi
    ctn_running_identity || return 1
    [ -r "$CTN_NODE" ] && [ -w "$CTN_NODE" ] || { ctn_error 'verified patch has no usable node; no daemon'; return 1; }
}

ctn_safe_file() {
    [ ! -L "$1" ] && { [ ! -e "$1" ] || [ -f "$1" ]; } || {
        ctn_error "unsafe state/log path: $1"; return 1;
    }
}

ctn_runtime_prepare() {
    [ "$CTN_RUNTIME" = "$CTN_ROOT/data/adb/ctn_patch" ] && [ ! -L "$CTN_RUNTIME" ] &&
        [ ! -L "$CTN_ROOT/data/adb" ] || { ctn_error 'unsafe runtime directory'; return 1; }
    umask 077
    [ -d "$CTN_RUNTIME" ] || mkdir -m 700 "$CTN_RUNTIME" || return 1
    chmod 700 "$CTN_RUNTIME" || return 1
    local name
    for name in .service.lock .maintenance.lock .daemon.lock daemon.pid service.pid .service.stop restore.state boot.log daemon.log action.log; do
        ctn_safe_file "$CTN_RUNTIME/$name" || return 1
    done
}

ctn_flock() {
    local option=$1 fd=$2
    [ "$#" = 2 ] || return 2
    case "$option" in -n|-u) ;; *) return 2 ;; esac
    # mksh marks descriptors opened by exec close-on-exec. Re-export the one
    # descriptor at the external command itself, without reopening its inode.
    case "$fd" in
        6) if command -v flock >/dev/null 2>&1; then flock "$@" 6>&6
           elif command -v busybox >/dev/null 2>&1; then busybox flock "$@" 6>&6
           elif [ -x "$CTN_ROOT/data/adb/magisk/busybox" ]; then "$CTN_ROOT/data/adb/magisk/busybox" flock "$@" 6>&6
           else ctn_error 'flock is required; refusing unlocked operation'; return 2; fi ;;
        7) if command -v flock >/dev/null 2>&1; then flock "$@" 7>&7
           elif command -v busybox >/dev/null 2>&1; then busybox flock "$@" 7>&7
           elif [ -x "$CTN_ROOT/data/adb/magisk/busybox" ]; then "$CTN_ROOT/data/adb/magisk/busybox" flock "$@" 7>&7
           else ctn_error 'flock is required; refusing unlocked operation'; return 2; fi ;;
        8) if command -v flock >/dev/null 2>&1; then flock "$@" 8>&8
           elif command -v busybox >/dev/null 2>&1; then busybox flock "$@" 8>&8
           elif [ -x "$CTN_ROOT/data/adb/magisk/busybox" ]; then "$CTN_ROOT/data/adb/magisk/busybox" flock "$@" 8>&8
           else ctn_error 'flock is required; refusing unlocked operation'; return 2; fi ;;
        9) if command -v flock >/dev/null 2>&1; then flock "$@" 9>&9
           elif command -v busybox >/dev/null 2>&1; then busybox flock "$@" 9>&9
           elif [ -x "$CTN_ROOT/data/adb/magisk/busybox" ]; then "$CTN_ROOT/data/adb/magisk/busybox" flock "$@" 9>&9
           else ctn_error 'flock is required; refusing unlocked operation'; return 2; fi ;;
        *) return 2 ;;
    esac
}

ctn_pid_valid() {
    case "$1" in ''|*[!0-9]*|0|1) return 1 ;; esac
    [ "${#1}" -le 10 ]
}

ctn_process_start() {
    local line
    line=$(cat "$CTN_PROC/$1/stat" 2>/dev/null) || return 1
    line=${line##*) }
    set -- $line
    [ "$#" -ge 20 ] || return 1
    shift 19
    case "$1" in ''|*[!0-9]*) return 1 ;; esac
    printf '%s\n' "$1"
}

ctn_process_matches() {
    local exe
    ctn_pid_valid "$1" && kill -0 "$1" 2>/dev/null || return 1
    exe=$(readlink "$CTN_PROC/$1/exe" 2>/dev/null) || return 1
    # A replaced executable retains this exact path with the kernel's suffix.
    [ "$exe" = "$2" ] || [ "$exe" = "$2 (deleted)" ] || return 1
    [ -z "${3:-}" ] || [ "$(ctn_process_start "$1")" = "$3" ]
}

ctn_daemon_pid() {
    local pid
    pid=$(ctn_scalar "$CTN_RUNTIME/daemon.pid") || return 1
    ctn_process_matches "$pid" "$MODDIR/bin/ctnd" || return 1
    printf '%s\n' "$pid"
}

ctn_daemon_current() {
    local pid rc
    pid=$(ctn_daemon_pid) || return 1
    [ "$(ctn_sha256 "$CTN_PROC/$pid/exe")" = "$CTN_DAEMON_HASH" ] || return 1
    # PID/path/hash alone do not prove the singleton daemon contract. A current
    # daemon publishes its PID only after acquiring the fixed lock inode.
    [ -f "$CTN_RUNTIME/.daemon.lock" ] && [ ! -L "$CTN_RUNTIME/.daemon.lock" ] || return 1
    ctn_daemon_idle
    rc=$?
    [ "$rc" = 1 ] || return 1
    ctn_process_matches "$pid" "$MODDIR/bin/ctnd" || return 1
    printf '%s\n' "$pid"
}

ctn_service_pid() {
    local pid start extra commandline
    [ -r "$CTN_RUNTIME/service.pid" ] || return 1
    IFS=' ' read -r pid start extra < "$CTN_RUNTIME/service.pid" || return 1
    [ -n "$start" ] && [ -z "$extra" ] || return 1
    ctn_process_matches "$pid" "$CTN_SHELL_EXE" "$start" || return 1
    commandline=$(tr '\000' '\n' < "$CTN_PROC/$pid/cmdline") || return 1
    [ "$commandline" = "$CTN_SHELL
$MODDIR/service.sh
--run" ] || return 1
    printf '%s\n' "$pid"
}

ctn_stop_pid() {
    local pid=$1 exe=$2 start tries=0
    ctn_process_matches "$pid" "$exe" || return 0
    start=$(ctn_process_start "$pid") || { ctn_error "cannot validate PID $pid start time"; return 1; }
    ctn_process_matches "$pid" "$exe" "$start" || return 0
    kill -TERM "$pid" || return 1
    while ctn_process_matches "$pid" "$exe" "$start"; do
        [ "$tries" -lt 20 ] || { ctn_error "SIGTERM timed out for own PID $pid; not using SIGKILL"; return 1; }
        sleep 1; tries=$((tries + 1))
    done
}

ctn_daemon_idle() (
    ctn_safe_file "$CTN_RUNTIME/.daemon.lock" || exit 1
    exec 7>>"$CTN_RUNTIME/.daemon.lock" || exit $?
    ctn_flock -n 7
)

ctn_stop_daemon() {
    local pid tries=0
    pid=$(ctn_daemon_pid) && ctn_stop_pid "$pid" "$MODDIR/bin/ctnd" || {
        [ -z "$pid" ] || return 1
    }
    while ! ctn_daemon_idle; do
        [ "$tries" -lt 20 ] || { ctn_error 'daemon lock remains held; no unrelated process will be killed'; return 1; }
        sleep 1; tries=$((tries + 1))
    done
    # State contents belong exclusively to ctnd. Presence is outstanding restore debt.
    [ ! -e "$CTN_RUNTIME/restore.state" ] || { ctn_error 'daemon restoration is incomplete; preserving restore.state'; return 1; }
}

ctn_stop_legacy() {
    local pid
    # Only this module's old path and PID hint; never comm, pgrep or /proc/*/fd.
    if [ -e "$MODDIR/ctnd" ] || [ -e "$MODDIR/ctnd.pid" ]; then
        ctn_safe_file "$MODDIR/.stop" || return 1
        : > "$MODDIR/.stop" || return 1
        pid=$(ctn_scalar "$MODDIR/ctnd.pid") || pid=
        if [ -n "$pid" ] && ctn_process_matches "$pid" "$MODDIR/ctnd"; then
            ctn_stop_pid "$pid" "$MODDIR/ctnd" || return 1
        fi
    fi
}

ctn_maintenance_lock() {
    local rc
    ctn_runtime_prepare || return $?
    exec 8>>"$CTN_RUNTIME/.maintenance.lock" || return $?
    ctn_flock -n 8
    rc=$?
    [ "$rc" = 0 ] || { ctn_error "maintenance lock unavailable rc=$rc"; return "$rc"; }
}
ctn_maintenance_unlock() {
    local rc
    ctn_flock -u 8; rc=$?
    exec 8>&-
    return "$rc"
}

ctn_service_idle() (
    ctn_safe_file "$CTN_RUNTIME/.service.lock" || exit 1
    exec 6>>"$CTN_RUNTIME/.service.lock" || exit $?
    ctn_flock -n 6
)

ctn_stop_service() {
    local tries=0
    ctn_safe_file "$CTN_RUNTIME/.service.stop" || return 1
    : > "$CTN_RUNTIME/.service.stop" || return 1
    while ! ctn_service_idle; do
        [ "$tries" -lt 30 ] || { ctn_error 'service did not release its lock; refusing teardown'; return 1; }
        sleep 1; tries=$((tries + 1))
    done
    ctn_stop_legacy && ctn_stop_daemon
}

ctn_start_daemon() {
    local pid child tries=0 rc
    pid=$(ctn_daemon_current) && { ctn_note "existing current daemon pid=$pid"; return 0; }
    pid=$(ctn_daemon_pid) && ctn_stop_daemon || { [ -z "$pid" ] || return 1; }
    ctn_disabled && return 1
    [ ! -e "$CTN_RUNTIME/.service.stop" ] && [ -r "$CTN_NODE" ] && [ -x "$MODDIR/bin/ctnd" ] || return 1
    ctn_safe_file "$CTN_RUNTIME/daemon.log" || return 1
    # The daemon owns its own flock; it MUST NOT inherit the service/maintenance locks.
    "$MODDIR/bin/ctnd" 9>&- 8>&- >> "$CTN_RUNTIME/daemon.log" 2>&1 &
    child=$!
    CTN_CHILD=$child
    while [ "$tries" -lt 10 ]; do
        pid=$(ctn_daemon_current) && { ctn_note "daemon started pid=$pid"; return 0; }
        if ! kill -0 "$child" 2>/dev/null; then
            wait "$child"; rc=$?
            [ "$rc" != 0 ] || rc=1
            ctn_error "daemon failed to become ready rc=$rc"; return "$rc"
        fi
        sleep 1; tries=$((tries + 1))
    done
    ctn_stop_pid "$child" "$MODDIR/bin/ctnd" || return 1
    ctn_error 'daemon did not publish a verified PID'; return 1
}

ctn_report_running() {
    local pid hash version profile build ready
    ctn_note "FILE module=$CTN_PACKAGE_VERSION profile=$CTN_PROFILE build_id=$CTN_BUILD_ID KO_SHA256=$CTN_MODULE_HASH"
    ctn_note "FILE daemon_SHA256=$CTN_DAEMON_HASH (not a running-version claim)"
    if ctn_loaded; then
        version=$(ctn_scalar "$CTN_SYS/module/ctn_patch/version")
        profile=$(ctn_scalar "$CTN_SYS/module/ctn_patch/parameters/profile")
        build=$(ctn_scalar "$CTN_SYS/module/ctn_patch/parameters/build_id")
        ready=$(ctn_scalar "$CTN_SYS/module/ctn_patch/parameters/patch_ready")
        ctn_note "RUNNING module_version=${version:-unknown} profile=${profile:-unknown} build_id=${build:-unknown} patch_ready=${ready:-unknown}; running_KO_SHA256=not_exposed"
    else ctn_note 'RUNNING module=not_loaded'; fi
    pid=$(ctn_daemon_pid) || pid=
    if [ -n "$pid" ]; then
        hash=$(ctn_sha256 "$CTN_PROC/$pid/exe")
        ctn_note "RUNNING daemon_pid=$pid exe_SHA256=${hash:-unreadable}"
        [ "$hash" = "$CTN_DAEMON_HASH" ] || ctn_note 'RUNNING daemon differs from FILE; no version is inferred from the new binary'
    else
        ctn_note 'RUNNING current daemon=not_verified (stale/unrelated PIDs are ignored)'
        pid=$(ctn_scalar "$MODDIR/ctnd.pid") || pid=
        if [ -n "$pid" ] && ctn_process_matches "$pid" "$MODDIR/ctnd"; then
            ctn_note "RUNNING legacy daemon_pid=$pid exe_SHA256=$(ctn_sha256 "$CTN_PROC/$pid/exe")"
        fi
    fi
    if ctn_disabled; then ctn_note 'MODULE disabled/removing; its flags are unchanged'; fi
}
