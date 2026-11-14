#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# One supervisor per runtime. No implicit enable, no cross-profile insmod retry.

CTN_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
[ -r "$CTN_DIR/lib/common.sh" ] || { printf '%s\n' 'ctn_patch: missing lib/common.sh' >&2; exit 1; }
. "$CTN_DIR/lib/common.sh"
ctn_init "$CTN_DIR" || exit 1

case "${1:-}" in
    '')
        ctn_disabled && { ctn_note 'disabled/removing; service not started'; exit 0; }
        ctn_runtime_prepare || exit 1
        # Re-exec gives /proc/PID/cmdline an exact, verifiable service identity.
        "$CTN_SHELL" "$MODDIR/service.sh" --run >> "$CTN_RUNTIME/boot.log" 2>&1 < /dev/null &
        exit 0
        ;;
    --run) ;;
    *) ctn_error 'usage: service.sh [--run]'; exit 2 ;;
esac

ctn_disabled && exit 0
ctn_runtime_prepare || exit 1
# Short setup lock prevents a start racing with uninstall/exercise.
exec 8>>"$CTN_RUNTIME/.maintenance.lock" || exit $?
ctn_flock -n 8
CTN_LOCK_RC=$?
[ "$CTN_LOCK_RC" = 0 ] || { ctn_error 'maintenance lock unavailable; not starting service'; exit "$CTN_LOCK_RC"; }
exec 9>>"$CTN_RUNTIME/.service.lock" || exit $?
ctn_flock -n 9
CTN_LOCK_RC=$?
case "$CTN_LOCK_RC" in
    0) ;;
    1) ctn_note 'another service already owns the lock'; exit 0 ;;
    *) ctn_error "service flock failed rc=$CTN_LOCK_RC"; exit "$CTN_LOCK_RC" ;;
esac
[ ! -e "$MODDIR/.uninstalling" ] || exit 1
ctn_disabled && exit 0
rm -f "$CTN_RUNTIME/.service.stop" || exit 1
CTN_START=$(ctn_process_start "$$") || { ctn_error 'cannot identify service process'; exit 1; }
printf '%s %s\n' "$$" "$CTN_START" > "$CTN_RUNTIME/service.pid" || exit 1
ctn_flock -u 8 || exit 1
exec 8>&-

CTN_SERVICE_CLEANUP=0
ctn_service_cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    if [ "$CTN_SERVICE_CLEANUP" = 1 ]; then
        # Stop only our precise daemon path; wait for its restore before unlocking.
        ctn_stop_daemon || rc=1
    fi
    if [ "$(ctn_scalar "$CTN_RUNTIME/service.pid")" = "$$ $CTN_START" ]; then
        rm -f "$CTN_RUNTIME/service.pid" || rc=1
    fi
    ctn_flock -u 9 || rc=1
    exec 9>&-
    exit "$rc"
}
trap ctn_service_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

ctn_service_stopping() { ctn_disabled || [ -e "$CTN_RUNTIME/.service.stop" ]; }
ctn_note 'service waiting for vendor game_opt (up to 90 seconds)'
i=0
while ! ctn_dependency_ready; do
    ctn_service_stopping && exit 0
    [ "$i" -lt 90 ] || { ctn_error 'vendor dependency timed out'; exit 1; }
    sleep 1; i=$((i + 1))
done
ctn_service_stopping && exit 0
ctn_select || exit 1
ctn_stop_legacy || exit 1
CTN_SERVICE_CLEANUP=1
ctn_service_stopping && exit 0
ctn_ensure_module || exit $?
ctn_start_daemon || exit $?
ctn_note 'service active; disable/remove are never changed'

while :; do
    ctn_service_stopping && exit 0
    [ -d "$MODDIR" ] || exit 0
    ctn_running_identity && [ -r "$CTN_NODE" ] || {
        ctn_error 'running patch/node lost; stopping daemon'; exit 1;
    }
    if ! ctn_daemon_current >/dev/null; then
        # New binary or a crashed process: recheck hashes before any restart.
        ctn_select || exit 1
        ctn_service_stopping && exit 0
        ctn_start_daemon || exit $?
    fi
    sleep 2
done
