#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Never unload under a live daemon or discard restoration debt.
CTN_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
[ -r "$CTN_DIR/lib/common.sh" ] || { printf '%s\n' 'ctn_patch: missing lib/common.sh; refusing blind teardown' >&2; exit 1; }
. "$CTN_DIR/lib/common.sh"
ctn_init "$CTN_DIR" || exit 1
ctn_maintenance_lock || exit $?
ctn_safe_file "$MODDIR/.uninstalling" || exit 1
: > "$MODDIR/.uninstalling" || exit 1
# .uninstalling remains until the manager removes the package, blocking races.
ctn_stop_service || { ctn_error 'uninstall stopped: restore/stop failed; keeping module and runtime'; exit 1; }
ctn_exercise_guard || { ctn_error 'uninstall stopped: unresolved self-check restoration'; exit 1; }
if ctn_loaded; then
    rmmod ctn_patch
    CTN_RC=$?
    [ "$CTN_RC" = 0 ] || { ctn_error "rmmod failed rc=$CTN_RC; runtime retained"; exit "$CTN_RC"; }
    ctn_loaded && { ctn_error 'ctn_patch still loaded; runtime retained'; exit 1; }
fi
# Delete only known runtime files; no recursive deletion or following symlinks.
# restore.state is never removed by shell. daemon must have removed it itself.
[ ! -e "$CTN_RUNTIME/restore.state" ] || { ctn_error 'outstanding restore debt'; exit 1; }
# After unloading, the saved self-check node values cannot remain in this kernel.
for CTN_FILE in daemon.pid service.pid .service.stop exercise.state boot.log daemon.log action.log action-detail.log .daemon.lock .service.lock; do
    ctn_safe_file "$CTN_RUNTIME/$CTN_FILE" || exit 1
    rm -f "$CTN_RUNTIME/$CTN_FILE" || exit 1
done
ctn_maintenance_unlock || exit 1
rm -f "$CTN_RUNTIME/.maintenance.lock" || exit 1
rmdir "$CTN_RUNTIME" || { ctn_error 'runtime contains unexpected files; retained for diagnosis'; exit 1; }
ctn_note 'daemon restored, module unloaded, known runtime removed; manager flags unchanged'
