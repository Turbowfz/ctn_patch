#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Install-time gate is read-only with respect to the running kernel and daemon.
# Magisk/KernelSU/APatch have already extracted the allowlisted package.

[ -n "${MODPATH:-}" ] || MODPATH=${MODDIR:-}
[ -n "$MODPATH" ] || abort 'ctn_patch: missing module install directory'
[ -r "$MODPATH/lib/common.sh" ] || abort 'ctn_patch: missing lib/common.sh'
. "$MODPATH/lib/common.sh"
ctn_init "$MODPATH" || abort 'ctn_patch: invalid module identity'

ui_print '- ctn_patch: static installation check (no insmod, no node writes)'
ctn_select || abort 'ctn_patch: profile/vendor/KO identity check failed; nothing was loaded'
if [ -e "$CTN_NODE" ] && ! ctn_loaded; then
    abort 'ctn_patch: node exists with unknown provenance; no native profile is approved'
fi
if ctn_loaded; then
    ui_print '- A module is already loaded. It is NOT evidence that this new KO is compatible or running.'
    ctn_report_running
fi
[ "$CTN_PACKAGE_MODE" != development ] || ui_print '- DEVELOPMENT package: not a fully runtime-verified release'

# Preserve existing manager decisions on upgrade; never remove disable/remove.
CTN_OLD="$CTN_ROOT/data/adb/modules/ctn_patch"
for CTN_FLAG in disable remove; do
    if [ -e "$CTN_OLD/$CTN_FLAG" ] && [ "$CTN_OLD" != "$MODDIR" ]; then
        ctn_safe_file "$MODDIR/$CTN_FLAG" || abort 'ctn_patch: unsafe flag destination'
        [ -e "$MODDIR/$CTN_FLAG" ] || : > "$MODDIR/$CTN_FLAG" || abort 'ctn_patch: cannot preserve module flag'
        ui_print "- Existing $CTN_FLAG state preserved; this installer will not enable the module"
    fi
done

for CTN_SCRIPT in customize.sh service.sh action.sh uninstall.sh verify.sh lib/common.sh; do
    set_perm "$MODDIR/$CTN_SCRIPT" 0 0 0755
done
set_perm "$MODDIR/bin/ctnd" 0 0 0755
set_perm "$MODDIR/module.prop" 0 0 0644
set_perm "$MODDIR/README.md" 0 0 0644
for CTN_VERSION in 6.1.141 6.1.157; do
    [ -d "$MODDIR/kernels/$CTN_VERSION" ] || continue
    for CTN_FILE in ctn_patch.ko manifest.json metadata.prop; do
        set_perm "$MODDIR/kernels/$CTN_VERSION/$CTN_FILE" 0 0 0644
    done
done
ui_print '- All packaged KOs are retained; the next enabled boot reselects for the exact kernel/vendor.'
ui_print '- Default verify is read-only; Action exercises only when enabled and idle (otherwise read-only).'
ui_print '- Action log: /data/adb/ctn_patch/action.log'
