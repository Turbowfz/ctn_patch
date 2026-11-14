#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Only verification owns kernel writes. Action publishes separate private logs.
CTN_ACTION_LOG=/data/adb/ctn_patch/action.log
CTN_DETAIL_LOG=/data/adb/ctn_patch/action-detail.log
CTN_ACTION_TEMP= CTN_DETAIL_TEMP= CTN_DETAIL_PUBLISH= CTN_CHECK_PID=
CTN_ACTION_SIGNAL=0 CTN_ACTION_INTERRUPTS=0 CTN_ACTION_LOG_RC=0 CTN_ACTION_RC=0
CTN_ACTION_EARLY_ERROR=
ctn_action_paths() { printf '摘要日志：%s\n详细日志：%s\n' "$CTN_ACTION_LOG" "$CTN_DETAIL_LOG"; }
ctn_action_setup_failure() {
    printf '[失败] %s\n[失败] 自检结果：未完成（退出码 1）\n' "$1"
    ctn_action_paths
    exit 1
}
CTN_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) || ctn_action_setup_failure '无法定位模块目录'
[ -r "$CTN_DIR/verify.sh" ] && [ -r "$CTN_DIR/lib/common.sh" ] || ctn_action_setup_failure '模块缺少自检文件'
. "$CTN_DIR/lib/common.sh"
# Do not hide machine values from property/hash/PID helpers or alter their rc.
ctn_error() {
    if [ -n "$CTN_DETAIL_TEMP" ]; then printf 'ctn_patch: %s\n' "$*" >> "$CTN_DETAIL_TEMP" 2>/dev/null
    else CTN_ACTION_EARLY_ERROR="${CTN_ACTION_EARLY_ERROR}${CTN_ACTION_EARLY_ERROR:+
}ctn_patch: $*"; fi
    return 0
}
ctn_note() { ctn_error "$@"; return 0; }
CTN_ERROR_REASON= CTN_ERROR_HINT=
ctn_init "$CTN_DIR" || ctn_action_setup_failure "${CTN_ERROR_HINT:-模块初始化失败；请检查完整安装包}"
CTN_ACTION_LOG="$CTN_RUNTIME/action.log"
CTN_DETAIL_LOG="$CTN_RUNTIME/action-detail.log"
CTN_ACTION_NOTICE=
case "${1:-}" in
    '') CTN_ACTION_MODE=exercise
        if ctn_disabled; then
            CTN_ACTION_MODE=read-only
            CTN_ACTION_NOTICE='（模块已禁用或待删除，不加载或启用）'
        elif ! ctn_game_idle; then
            CTN_ACTION_MODE=read-only
            CTN_ACTION_NOTICE='（游戏运行中或空闲状态不明，不执行写入）'
        fi ;;
    --read-only) CTN_ACTION_MODE=read-only ;;
    --exercise) CTN_ACTION_MODE=exercise ;;
    -h|--help)
        printf '%s\n' '用法：sh action.sh [--read-only|--exercise]' \
            '默认安全演练；禁用或游戏运行时只读。摘要与完整诊断分别保存。'
        ctn_action_paths
        exit 0 ;;
    *) printf '[失败] 参数不支持；仅接受 --read-only 或 --exercise\n'; exit 2 ;;
esac
[ "$#" -le 1 ] || { printf '[失败] 一次只能指定一种自检模式\n'; exit 2; }
trap '[ -z "$CTN_ACTION_TEMP" ] || rm -f "$CTN_ACTION_TEMP"; [ -z "$CTN_DETAIL_TEMP" ] || rm -f "$CTN_DETAIL_TEMP"; [ -z "$CTN_DETAIL_PUBLISH" ] || rm -f "$CTN_DETAIL_PUBLISH"' EXIT
ctn_action_signal() {
    CTN_ACTION_INTERRUPTS=$((CTN_ACTION_INTERRUPTS + 1))
    # Forward once: another signal must not kill verify during its restoration.
    [ "$CTN_ACTION_SIGNAL" = 0 ] || return 0
    CTN_ACTION_SIGNAL=$1
    [ -z "$CTN_CHECK_PID" ] || kill -TERM "$CTN_CHECK_PID" 2>/dev/null
    return 0
}
trap 'ctn_action_signal 130' INT
trap 'ctn_action_signal 143' TERM HUP
CTN_ERROR_REASON= CTN_ERROR_HINT=
ctn_runtime_prepare 2>/dev/null || ctn_action_setup_failure "${CTN_ERROR_HINT:-日志目录不可用；未开始自检}"
ctn_safe_file "$CTN_ACTION_LOG" && ctn_safe_file "$CTN_DETAIL_LOG" || ctn_action_setup_failure '日志路径不安全；未开始自检'
CTN_DETAIL_TEMP=$(mktemp "$CTN_RUNTIME/.action-detail.XXXXXX" 2>/dev/null) || ctn_action_setup_failure '无法创建详细日志；未开始自检'
CTN_ACTION_TEMP=$(mktemp "$CTN_RUNTIME/.action.XXXXXX" 2>> "$CTN_DETAIL_TEMP") || ctn_action_setup_failure '无法创建摘要日志；未开始自检'
exec 2>> "$CTN_DETAIL_TEMP"
chmod 600 "$CTN_ACTION_TEMP" "$CTN_DETAIL_TEMP" 2>> "$CTN_DETAIL_TEMP" || ctn_action_setup_failure '无法设置日志私有权限；未开始自检'
CTN_ACTION_TIME=$(date '+%Y-%m-%d %H:%M:%S %z' 2>> "$CTN_DETAIL_TEMP") || CTN_ACTION_TIME=时间不可用
CTN_DISPLAY_VERSION=${CTN_PACKAGE_VERSION#v}
case "$CTN_DISPLAY_VERSION" in ''|*[!0-9.]*|.*|*.) CTN_DISPLAY_VERSION=未知 ;; esac
[ "${#CTN_DISPLAY_VERSION}" -le 16 ] || CTN_DISPLAY_VERSION=未知
[ "$CTN_DISPLAY_VERSION" = 未知 ] || CTN_DISPLAY_VERSION="v$CTN_DISPLAY_VERSION"
{
    printf 'CTN Patch %s self-check\nmode=%s\ntime=%s\n' "$CTN_PACKAGE_VERSION" "$CTN_ACTION_MODE" "$CTN_ACTION_TIME"
    [ -z "$CTN_ACTION_EARLY_ERROR" ] || printf '%s\n' "$CTN_ACTION_EARLY_ERROR"
} >> "$CTN_DETAIL_TEMP" || ctn_action_setup_failure '无法写入详细日志；未开始自检'
if [ "$CTN_ACTION_MODE" = exercise ]; then
    "$CTN_SHELL" "$CTN_DIR/verify.sh" --details --exercise >> "$CTN_DETAIL_TEMP" 2>&1 &
else
    "$CTN_SHELL" "$CTN_DIR/verify.sh" --details >> "$CTN_DETAIL_TEMP" 2>&1 &
fi
CTN_CHECK_PID=$!
[ "$CTN_ACTION_SIGNAL" = 0 ] || kill -TERM "$CTN_CHECK_PID" 2>/dev/null
CTN_VERIFY_RC=
while :; do
    CTN_WAIT_INTERRUPTS=$CTN_ACTION_INTERRUPTS
    wait "$CTN_CHECK_PID"
    CTN_WAIT_RC=$?
    # wait can itself be interrupted. Always wait for the child's cleanup,
    # retaining its real failure code rather than an interrupted wait's status.
    if [ "$CTN_WAIT_INTERRUPTS" = "$CTN_ACTION_INTERRUPTS" ]; then
        [ "$CTN_WAIT_RC" = 127 ] && [ -n "$CTN_VERIFY_RC" ] || CTN_VERIFY_RC=$CTN_WAIT_RC
        break
    fi
    [ -n "$CTN_VERIFY_RC" ] || CTN_VERIFY_RC=$CTN_WAIT_RC
done
CTN_CHECK_PID=
CTN_ACTION_RC=$CTN_VERIFY_RC
ctn_action_result_rc() {
    [ "$CTN_ACTION_SIGNAL" = 0 ] || [ "$CTN_ACTION_RC" != 0 ] || CTN_ACTION_RC=$CTN_ACTION_SIGNAL
    [ "$CTN_ACTION_LOG_RC" = 0 ] || [ "$CTN_ACTION_RC" != 0 ] || CTN_ACTION_RC=1
    return 0
}
ctn_action_result_rc
ctn_action_summary() {
    local line count=0
    ctn_action_result_rc
    printf '当前版本：%s（磁盘包）\n时间：%s\n' "$CTN_DISPLAY_VERSION" "$CTN_ACTION_TIME"
    if [ "$CTN_ACTION_MODE" = exercise ]; then printf '模式：安全演练%s\n' "$CTN_ACTION_NOTICE"
    else printf '模式：只读自检%s\n' "$CTN_ACTION_NOTICE"; fi
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            '[正常] 自检结果：'*|'[失败] 自检结果：'*|'[注意] 磁盘包 '*) continue ;;
            '[正常] '*|'[注意] '*|'[失败] '*|'[跳过] '*)
                [ "$count" -lt 11 ] || continue
                printf '%s\n' "$line"; count=$((count + 1)) ;;
        esac
    done < "$CTN_DETAIL_TEMP"
    if [ "$CTN_ACTION_LOG_RC" != 0 ]; then
        printf '[失败] 自检结果：日志保存失败（退出码 %s）\n' "$CTN_ACTION_RC"
    elif [ "$CTN_ACTION_SIGNAL" != 0 ]; then
        printf '[失败] 自检结果：已中断（退出码 %s）\n' "$CTN_ACTION_RC"
    elif [ "$CTN_ACTION_RC" != 0 ]; then
        printf '[失败] 自检结果：未通过（退出码 %s）\n' "$CTN_ACTION_RC"
    elif [ "$CTN_ACTION_MODE" = exercise ]; then
        printf '[正常] 自检结果：演练通过，原值恢复完成（退出码 0）\n'
    else
        printf '[正常] 自检结果：只读检查完成；未做写入演练（退出码 0）\n'
    fi
}
ctn_action_save_details() {
    ctn_safe_file "$CTN_DETAIL_LOG" || return 1
    CTN_DETAIL_PUBLISH=$(mktemp "$CTN_RUNTIME/.action-detail.XXXXXX") || return 1
    # Keep the input temp until both logs are saved, to record any publication error.
    if cp "$CTN_DETAIL_TEMP" "$CTN_DETAIL_PUBLISH" && chmod 600 "$CTN_DETAIL_PUBLISH" &&
       ctn_safe_file "$CTN_DETAIL_LOG" && mv -f "$CTN_DETAIL_PUBLISH" "$CTN_DETAIL_LOG"; then
        CTN_DETAIL_PUBLISH=
        return 0
    fi
    rm -f "$CTN_DETAIL_PUBLISH"
    CTN_DETAIL_PUBLISH=
    return 1
}
ctn_action_result_rc
CTN_LOGGED_SIGNAL=$CTN_ACTION_SIGNAL
printf '\nVERIFY_EXIT=%s\nACTION_SIGNAL=%s\nSELF_CHECK_EXIT=%s\nACTION_EXIT=%s\n' \
    "$CTN_VERIFY_RC" "$CTN_ACTION_SIGNAL" "$CTN_ACTION_RC" "$CTN_ACTION_RC" >> "$CTN_DETAIL_TEMP" || CTN_ACTION_LOG_RC=1
if ! ctn_action_save_details 2>> "$CTN_DETAIL_TEMP"; then
    ctn_error 'cannot publish action-detail.log'
    CTN_ACTION_LOG_RC=1
fi
ctn_action_result_rc
if ctn_action_summary > "$CTN_ACTION_TEMP" && chmod 600 "$CTN_ACTION_TEMP" 2>> "$CTN_DETAIL_TEMP" &&
   ctn_safe_file "$CTN_ACTION_LOG" && mv -f "$CTN_ACTION_TEMP" "$CTN_ACTION_LOG" 2>> "$CTN_DETAIL_TEMP"; then
    CTN_ACTION_TEMP=
else
    ctn_error 'cannot publish action.log'
    CTN_ACTION_LOG_RC=1
    ctn_action_result_rc
    printf 'SELF_CHECK_EXIT=%s\nACTION_EXIT=%s\nLOG_WRITE_EXIT=1\n' "$CTN_ACTION_RC" "$CTN_ACTION_RC" >> "$CTN_DETAIL_TEMP"
    ctn_action_save_details 2>> "$CTN_DETAIL_TEMP" || :
fi
ctn_action_result_rc
# A signal may arrive during atomic publication, after verify already exited.
# Re-render that result instead of leaving a successful on-disk summary.
if [ "$CTN_LOGGED_SIGNAL" != "$CTN_ACTION_SIGNAL" ]; then
    printf 'ACTION_SIGNAL=%s\nSELF_CHECK_EXIT=%s\nACTION_EXIT=%s\n' \
        "$CTN_ACTION_SIGNAL" "$CTN_ACTION_RC" "$CTN_ACTION_RC" >> "$CTN_DETAIL_TEMP"
    ctn_action_save_details || CTN_ACTION_LOG_RC=1
    ctn_action_result_rc
    [ -n "$CTN_ACTION_TEMP" ] || CTN_ACTION_TEMP=$(mktemp "$CTN_RUNTIME/.action.XXXXXX")
    if [ -n "$CTN_ACTION_TEMP" ] && ctn_action_summary > "$CTN_ACTION_TEMP" &&
       chmod 600 "$CTN_ACTION_TEMP" && ctn_safe_file "$CTN_ACTION_LOG" && mv -f "$CTN_ACTION_TEMP" "$CTN_ACTION_LOG"; then
        CTN_ACTION_TEMP=
    else CTN_ACTION_LOG_RC=1; fi
fi
ctn_action_result_rc
ctn_action_summary
ctn_action_paths
exit "$CTN_ACTION_RC"
