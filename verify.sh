#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Default: read-only static/identity checks. Only --exercise may load/write.

CTN_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
case "${1:-}" in
    '') CTN_EXERCISE=0 ;;
    --exercise) CTN_EXERCISE=1 ;;
    -h|--help)
        printf '%s\n' 'Usage: sh verify.sh [--exercise]' \
            'Default is read-only. --exercise pauses only our service, tests, restores, then resumes it.' \
            'Run from an installed/extracted package; arbitrary KO paths are not accepted.'
        exit 0 ;;
    *) printf '%s\n' 'ctn_patch: only --exercise is supported; no arbitrary KO path' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
[ -r "$CTN_DIR/lib/common.sh" ] || { printf '%s\n' 'ctn_patch: run verify from the packaged module (missing lib/common.sh)' >&2; exit 1; }
. "$CTN_DIR/lib/common.sh"
ctn_init "$CTN_DIR" || exit 1
ctn_note "verification mode=$([ "$CTN_EXERCISE" = 1 ] && printf exercise || printf read-only)"
ctn_select || exit $?
ctn_report_running

CTN_IDENTITY_RC=0
if ctn_loaded; then
    ctn_running_identity || CTN_IDENTITY_RC=1
elif [ -e "$CTN_NODE" ]; then
    ctn_error 'unknown native node provenance; no daemon-only profile is approved'
    CTN_IDENTITY_RC=1
fi

# Parse only names whose read representation can be round-tripped unambiguously.
# Refuse to test rather than overwrite an original value we cannot restore.
ctn_read_names() {
    local raw left right
    raw=$(ctn_scalar "$CTN_NODE") || return 1
    left=${raw%%,*}; right=${raw#*,}
    [ "$left" != "$raw" ] || return 1
    case "$right" in *,*) return 1 ;; esac
    CTN_NAME_A=${left%:*}; CTN_PID_A=${left##*:}
    CTN_NAME_B=${right%:*}; CTN_PID_B=${right##*:}
    [ "$CTN_NAME_A" != "$left" ] && [ "$CTN_NAME_B" != "$right" ] || return 1
    case "$CTN_NAME_A$CTN_NAME_B" in *[!A-Za-z0-9_.+-]*) return 1 ;; esac
    [ -n "$CTN_NAME_A" ] && [ -n "$CTN_NAME_B" ] &&
        [ "${#CTN_NAME_A}" -le 99 ] && [ "${#CTN_NAME_B}" -le 99 ] || return 1
    case "$CTN_PID_A" in -1) ;; *) ctn_pid_valid "$CTN_PID_A" || return 1 ;; esac
    case "$CTN_PID_B" in -1) ;; *) ctn_pid_valid "$CTN_PID_B" || return 1 ;; esac
    CTN_NAMES="$CTN_NAME_A $CTN_NAME_B"
}

ctn_pid_sample() {
    local count=0 comm
    if ctn_read_names; then
        ctn_note "READ node=$CTN_NAME_A:$CTN_PID_A,$CTN_NAME_B:$CTN_PID_B"
        if [ "$CTN_PID_A" != -1 ]; then
            comm=$(ctn_scalar "$CTN_PROC/$CTN_PID_A/comm")
            [ "$comm" != "$(printf '%.15s' "$CTN_NAME_A")" ] || count=$((count + 1))
        fi
        if [ "$CTN_PID_B" != -1 ]; then
            comm=$(ctn_scalar "$CTN_PROC/$CTN_PID_B/comm")
            [ "$comm" != "$(printf '%.15s' "$CTN_NAME_B")" ] || count=$((count + 1))
        fi
    fi
    if [ "$count" -gt 0 ]; then
        ctn_note "READ HAL-registered PID/comm samples matched=$count (a snapshot, not a write/boost test)"
    else
        ctn_note 'SKIP real PID exercise: no stable HAL-registered sample; rt_info is never overwritten'
    fi
}
[ ! -r "$CTN_NODE" ] || ctn_pid_sample

if [ "$CTN_EXERCISE" = 0 ]; then
    ctn_note 'Static checks only: no loading, node writes, new threads, PID signals, log writes, or enable changes.'
    ctn_note 'No runtime exercise was performed; a matching on-disk file is not proof of a running new version.'
    exit "$CTN_IDENTITY_RC"
fi

[ "$(id -u)" = 0 ] || { ctn_error '--exercise requires root'; exit 1; }
ctn_disabled && { ctn_error 'disabled/removing: --exercise refused; flags unchanged'; exit 1; }
[ "$CTN_IDENTITY_RC" = 0 ] || { ctn_error 'cannot exercise an old/unknown loaded instance'; exit 1; }
ctn_dependency_ready || { ctn_error 'vendor dependency is not ready'; exit 1; }
ctn_maintenance_lock || exit $?
CTN_RESUME_SERVICE=0
CTN_PAUSED=0
CTN_SAVED_NAMES=
CTN_SAVED_ENABLE=
CTN_NAMES_TOUCHED=0
CTN_KEEP_STOP=0
[ ! -e "$CTN_RUNTIME/.service.stop" ] || CTN_KEEP_STOP=1
if ctn_service_pid >/dev/null; then
    CTN_RESUME_SERVICE=1
elif ! ctn_service_idle; then
    ctn_error 'service lock has no matching process identity; refusing to interrupt it'
    exit 1
elif ctn_daemon_pid >/dev/null; then
    ctn_error 'standalone daemon is not managed by this service; refusing to lose its original arguments'
    exit 1
fi

ctn_write_node() { printf '%s\n' "$1" > "$CTN_NODE"; }
ctn_write_enable() { printf '%s\n' "$1" > "$CTN_ENABLE"; }
ctn_expect_names() {
    ctn_write_node "$1" || return $?
    ctn_read_names && [ "$CTN_NAMES" = "$2" ]
}

ctn_exercise_cleanup() {
    local rc=$? restore_rc=0 tries=0
    trap - EXIT INT TERM HUP
    if [ "$CTN_NAMES_TOUCHED" = 1 ]; then
        ctn_write_node "$CTN_SAVED_NAMES" && ctn_read_names &&
            [ "$CTN_NAMES" = "$CTN_SAVED_NAMES" ] || {
            restore_rc=$?; ctn_error "cannot restore original names [$CTN_SAVED_NAMES]";
        }
        # We do not enable ct_enable for the test. If it drifted, restore the exact
        # saved value instead of an assumed default.
        if [ "$(ctn_scalar "$CTN_ENABLE")" != "$CTN_SAVED_ENABLE" ]; then
            ctn_write_enable "$CTN_SAVED_ENABLE" &&
                [ "$(ctn_scalar "$CTN_ENABLE")" = "$CTN_SAVED_ENABLE" ] || {
                restore_rc=$?; ctn_error "cannot restore ct_enable=$CTN_SAVED_ENABLE";
            }
        fi
    fi
    if [ "$CTN_LOADED_BY_US" = 1 ]; then
        if [ "$restore_rc" = 0 ] && [ ! -e "$CTN_RUNTIME/restore.state" ]; then
            rmmod ctn_patch || { restore_rc=$?; ctn_error "cannot unload the test-owned KO rc=$restore_rc"; }
        else ctn_error 'restoration failed; refusing to unload test KO'; restore_rc=1
        fi
    fi
    if [ "$CTN_PAUSED" = 1 ] && [ "$CTN_KEEP_STOP" = 0 ] && [ "$restore_rc" = 0 ]; then
        rm -f "$CTN_RUNTIME/.service.stop" || restore_rc=1
    fi
    ctn_maintenance_unlock || restore_rc=1
    if [ "$CTN_RESUME_SERVICE" = 1 ] && [ "$CTN_PAUSED" = 1 ] && [ "$restore_rc" = 0 ]; then
        if ctn_disabled; then
            ctn_note 'disable/remove appeared during exercise; service deliberately not resumed'
        else
            "$CTN_SHELL" "$MODDIR/service.sh" || restore_rc=1
            while [ "$restore_rc" = 0 ]; do
                if ctn_service_pid >/dev/null && ctn_daemon_current >/dev/null; then
                    ctn_note 'original service and current daemon resumed'; break
                fi
                [ "$tries" -lt 15 ] || { ctn_error 'service did not resume; inspect runtime logs'; restore_rc=1; break; }
                sleep 1; tries=$((tries + 1))
            done
        fi
    fi
    if [ "$restore_rc" != 0 ]; then rc=$restore_rc; fi
    ctn_note "exercise finished rc=$rc profile=$CTN_PROFILE build_id=$CTN_BUILD_ID; dmesg was not cleared"
    exit "$rc"
}
trap ctn_exercise_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

ctn_stop_service || exit 1
CTN_PAUSED=1
ctn_disabled && { ctn_error 'disabled/removing while waiting'; exit 1; }
ctn_ensure_module || exit $?
ctn_read_names || { ctn_error 'original names cannot be restored unambiguously; no writes performed'; exit 1; }
CTN_SAVED_NAMES=$CTN_NAMES
CTN_SAVED_ENABLE=$(ctn_scalar "$CTN_ENABLE") || exit 1
case "$CTN_SAVED_ENABLE" in 0|1) ;; *) ctn_error 'unknown ct_enable original; no writes performed'; exit 1 ;; esac
[ -w "$CTN_NODE" ] || exit 1
CTN_NAMES_TOUCHED=1
ctn_expect_names 'CTNTestMain CTNTestGfx' 'CTNTestMain CTNTestGfx' || {
    CTN_WRITE_RC=$?; ctn_error "dual-name write/readback failed rc=$CTN_WRITE_RC"; exit "$CTN_WRITE_RC";
}
ctn_expect_names 'CTNTestSingle' 'CTNTestSingle CTNTestSingle' || {
    CTN_WRITE_RC=$?; ctn_error "single-name write/readback failed rc=$CTN_WRITE_RC"; exit "$CTN_WRITE_RC";
}
if ctn_write_node 'A B C' 2>/dev/null; then ctn_error 'invalid three-name input was accepted'; exit 1; fi
if ctn_write_node ' ' 2>/dev/null; then ctn_error 'empty input was accepted'; exit 1; fi
ctn_note 'Name write/read and rejection tests passed; restoring saved values, not defaults.'
ctn_note 'SKIP synthetic PID registration: rt_info has no proven reversible restore contract.'
exit 0
