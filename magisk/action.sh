#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Action runs verification directly; keep its exit status and a persistent log.
CTN_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
[ -r "$CTN_DIR/verify.sh" ] && [ -r "$CTN_DIR/lib/common.sh" ] || {
    printf '%s\n' 'ctn_patch: missing verification files' >&2; exit 1;
}
. "$CTN_DIR/lib/common.sh"
ctn_init "$CTN_DIR" || exit 1
case "${1:-}" in
    '') CTN_ACTION_MODE=exercise
        if ctn_disabled; then
            CTN_ACTION_MODE=read-only
            printf '%s\n' '模块已禁用或待删除：只做文件/兼容性自检，不加载或启用。'
        elif ! ctn_game_idle; then
            CTN_ACTION_MODE=read-only
            printf '%s\n' '当前有游戏运行或无法确认空闲：只做文件/兼容性自检。'
        fi ;;
    --read-only) CTN_ACTION_MODE=read-only ;;
    --exercise) CTN_ACTION_MODE=exercise ;;
    -h|--help)
        printf '%s\n' '用法：sh action.sh [--read-only|--exercise]' \
            '默认直接自检；禁用或游戏运行时改为只读。' \
            '日志：/data/adb/ctn_patch/action.log'
        exit 0 ;;
    *) printf '%s\n' 'ctn_patch: unsupported Action argument' >&2; exit 2 ;;
esac
[ "$#" -le 1 ] || exit 2
ctn_runtime_prepare || exit 1
CTN_ACTION_LOG="$CTN_RUNTIME/action.log"
ctn_safe_file "$CTN_ACTION_LOG" || exit 1
CTN_ACTION_TEMP=$(mktemp "$CTN_RUNTIME/.action.XXXXXX") || exit 1
CTN_CHECK_PID=
CTN_ACTION_SIGNAL=0
ctn_action_signal() {
    CTN_ACTION_SIGNAL=$1
    [ -z "$CTN_CHECK_PID" ] || kill -TERM "$CTN_CHECK_PID" 2>/dev/null
}
trap '[ -z "$CTN_ACTION_TEMP" ] || rm -f "$CTN_ACTION_TEMP"' EXIT
trap 'ctn_action_signal 130' INT
trap 'ctn_action_signal 143' TERM HUP
printf '开始自检：%s\n日志位置：%s\n' "$CTN_ACTION_MODE" "$CTN_ACTION_LOG"
{
    printf 'CTN Patch %s self-check\nmode=%s\n' "$CTN_PACKAGE_VERSION" "$CTN_ACTION_MODE"
    date '+time=%Y-%m-%d %H:%M:%S'
} > "$CTN_ACTION_TEMP"
if [ "$CTN_ACTION_MODE" = exercise ]; then
    "$CTN_SHELL" "$CTN_DIR/verify.sh" --exercise >> "$CTN_ACTION_TEMP" 2>&1 &
else
    "$CTN_SHELL" "$CTN_DIR/verify.sh" >> "$CTN_ACTION_TEMP" 2>&1 &
fi
CTN_CHECK_PID=$!
[ "$CTN_ACTION_SIGNAL" = 0 ] || kill -TERM "$CTN_CHECK_PID" 2>/dev/null
wait "$CTN_CHECK_PID"
CTN_ACTION_RC=$?
if [ "$CTN_ACTION_SIGNAL" != 0 ]; then
    wait "$CTN_CHECK_PID" 2>/dev/null
    [ "$CTN_ACTION_RC" != 0 ] || CTN_ACTION_RC=$CTN_ACTION_SIGNAL
fi
CTN_CHECK_PID=
printf '\nSELF_CHECK_EXIT=%s\n日志位置：%s\n' "$CTN_ACTION_RC" "$CTN_ACTION_LOG" >> "$CTN_ACTION_TEMP"
# Atomic replacement does not truncate a previous log inode or hardlink target.
if ctn_safe_file "$CTN_ACTION_LOG" && mv -f "$CTN_ACTION_TEMP" "$CTN_ACTION_LOG"; then
    CTN_ACTION_TEMP=
    cat "$CTN_ACTION_LOG"
    printf '\n自检结束，退出码：%s\n日志位置：%s\n' "$CTN_ACTION_RC" "$CTN_ACTION_LOG"
else
    cat "$CTN_ACTION_TEMP"
    printf '无法保存自检日志：%s\n' "$CTN_ACTION_LOG" >&2
    [ "$CTN_ACTION_RC" != 0 ] || CTN_ACTION_RC=1
fi
exit "$CTN_ACTION_RC"
