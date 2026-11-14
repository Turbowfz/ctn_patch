#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read-only installation checks; the root manager owns the installation log.

[ -n "${MODPATH:-}" ] || MODPATH=${MODDIR:-}
[ -n "$MODPATH" ] || abort 'CTN Patch：找不到安装目录，请通过模块管理器重新安装。'
[ -r "$MODPATH/lib/common.sh" ] || abort 'CTN Patch：安装包缺少公共脚本，请重新下载完整 ZIP。'
. "$MODPATH/lib/common.sh"
ctn_note() { return 0; }
ctn_install_abort() {
    ui_print "[失败] ${CTN_ERROR_REASON:-$1}"
    ui_print "处理：${CTN_ERROR_HINT:-请保存管理器的完整安装日志，并重新下载完整 ZIP。}"
    abort "CTN Patch：$1"
}
ctn_init "$MODPATH" || ctn_install_abort '模块初始化失败'

ui_print "- CTN Patch $CTN_PACKAGE_VERSION 适配检查（不加载、不写内核）"
ctn_select || ctn_install_abort '内核或模块文件适配检查失败'
if [ -e "$CTN_NODE" ] && ! ctn_loaded; then
    CTN_ERROR_REASON='发现来源不明的关键线程节点'
    CTN_ERROR_HINT='请提供管理器安装日志，当前不能在未知节点上叠加模块。'
    ctn_install_abort '节点来源无法确认'
fi
ui_print "[正常] 当前内核：$CTN_SELECTED_PROFILE，已选择对应 KO"
ui_print '[正常] 模块文件与厂商 game_opt 校验通过'
if ctn_loaded; then
    CTN_RUNNING_VERSION=$(ctn_scalar "$CTN_SYS/module/ctn_patch/version") || CTN_RUNNING_VERSION=unknown
    ui_print "[注意] 当前运行版本：$CTN_RUNNING_VERSION；安装后正常重启才启用新文件。"
fi
[ "$CTN_PACKAGE_MODE" != development ] ||
    ui_print '[注意] 开发验证包；各内核真机验证范围以说明为准。'

# Preserve existing manager decisions; installation never stops a daemon.
CTN_OLD="$CTN_ROOT/data/adb/modules/ctn_patch"
for CTN_FLAG in disable remove; do
    if [ -e "$CTN_OLD/$CTN_FLAG" ] && [ "$CTN_OLD" != "$MODDIR" ]; then
        ctn_safe_file "$MODDIR/$CTN_FLAG" || ctn_install_abort '无法安全保留管理器状态'
        [ -e "$MODDIR/$CTN_FLAG" ] || : > "$MODDIR/$CTN_FLAG" || ctn_install_abort '无法保留管理器状态'
        ui_print "[注意] 原 $CTN_FLAG 状态已保留，不会自动启用模块。"
    fi
done

for CTN_SCRIPT in customize.sh service.sh action.sh uninstall.sh verify.sh lib/common.sh; do
    set_perm "$MODDIR/$CTN_SCRIPT" 0 0 0755
done
set_perm "$MODDIR/bin/ctnd" 0 0 0755
for CTN_FILE in module.prop README.md changelog.txt oplus.png; do
    [ ! -f "$MODDIR/$CTN_FILE" ] || set_perm "$MODDIR/$CTN_FILE" 0 0 0644
done
CTN_KERNELS=
for CTN_VERSION in 6.1.141 6.1.157; do
    [ -d "$MODDIR/kernels/$CTN_VERSION" ] || continue
    CTN_KERNELS="$CTN_KERNELS $CTN_VERSION"
    for CTN_FILE in ctn_patch.ko manifest.json metadata.prop; do
        set_perm "$MODDIR/kernels/$CTN_VERSION/$CTN_FILE" 0 0 0644
    done
done

CTN_CHANGELOG="$MODDIR/changelog.txt"
CTN_CODE=$(ctn_property "$MODDIR/module.prop" versionCode) || CTN_CODE=
CTN_CURRENT_HEADER="CTN Patch $CTN_PACKAGE_VERSION（versionCode $CTN_CODE）更新日志"
if ctn_safe_file "$CTN_CHANGELOG" && [ -r "$CTN_CHANGELOG" ] &&
   grep -qxF -- "$CTN_CURRENT_HEADER" "$CTN_CHANGELOG"; then
    ui_print '- 当前版本更新日志：'
    CTN_PRINT_CURRENT=0
    CTN_PRINT_COUNT=0
    while IFS= read -r CTN_LINE || [ -n "$CTN_LINE" ]; do
        case "$CTN_LINE" in
            "$CTN_CURRENT_HEADER") CTN_PRINT_CURRENT=1; ui_print "  $CTN_LINE"; continue ;;
            'CTN Patch '*'更新日志') [ "$CTN_PRINT_CURRENT" = 0 ] || break ;;
        esac
        [ "$CTN_PRINT_CURRENT" = 1 ] && [ -n "$CTN_LINE" ] || continue
        case "$CTN_LINE" in '- '*) ;; *) continue ;; esac
        [ "$CTN_PRINT_COUNT" -lt 4 ] || break
        ui_print "  $CTN_LINE"
        CTN_PRINT_COUNT=$((CTN_PRINT_COUNT + 1))
    done < "$CTN_CHANGELOG"
else
    ui_print '[注意] 更新日志缺失或版本不符，已跳过显示。'
fi
ui_print "- 本包支持内核:$CTN_KERNELS；开机自动重新选择并校验，不用手动选 KO。"
ui_print '- CTN 只读 COSA 数据库，不读取或创建 ctn.conf。'
ui_print '- 安装日志由管理器保存；Action 可检查适配和运行状态。'
