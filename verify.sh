#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Default: read-only static/identity checks. Only --exercise may load/write.

CTN_EXERCISE=0
CTN_DETAILS=0
ctn_status() { printf '[%s] %s\n' "$1" "$2"; }
ctn_summary_version() {
    local version=${1#v}
    case "$version" in ''|*[!0-9.]*|.*|*.) printf '未知'; return 0 ;; esac
    [ "${#version}" -le 16 ] || { printf '未知'; return 0; }
    printf 'v%s' "$version"
}
ctn_finish() {
    local rc=$1
    trap - EXIT
    if [ "$rc" = 0 ]; then
        if [ "$CTN_EXERCISE" = 1 ]; then
            ctn_status 正常 '自检结果：演练通过并完成收尾'
        else
            ctn_status 正常 '自检结果：只读检查完成；未进行写入演练'
        fi
    else
        ctn_status 失败 "自检结果：未通过或已中断（退出码 $rc）"
    fi
    exit "$rc"
}
ctn_abort() {
    local rc=${2:-1}
    [ "$rc" != 0 ] || rc=1
    if [ -n "$CTN_ERROR_REASON" ]; then
        ctn_status 失败 "$CTN_ERROR_REASON；${CTN_ERROR_HINT:-$1}"
    else
        ctn_status 失败 "$1"
    fi
    exit "$rc"
}
for CTN_ARG in "$@"; do
    case "$CTN_ARG" in
        --exercise) CTN_EXERCISE=1 ;;
        --details) CTN_DETAILS=1 ;;
        -h|--help)
            printf '%s\n' '用法：sh verify.sh [--details] [--exercise]' \
                '默认只读，不加载、不写入、不改变启用状态。' \
                '--details 显示完整技术诊断，可与 --exercise 按任意顺序组合。' \
                '--exercise 仅暂停本模块服务，演练后恢复原值；不接受任意内核文件路径。'
            exit 0 ;;
        *) ctn_status 失败 '参数不支持；仅接受 --details 和 --exercise'; exit 2 ;;
    esac
done
CTN_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P) || {
    ctn_status 失败 '无法定位模块目录'; exit 1;
}
[ -r "$CTN_DIR/lib/common.sh" ] || {
    ctn_status 失败 '模块文件不完整；请从已安装或完整解压的模块目录运行'; exit 1;
}
. "$CTN_DIR/lib/common.sh"
# Keep machine-valued helpers untouched. Logging must never change a gate's rc.
ctn_note() { if [ "$CTN_DETAILS" = 1 ]; then printf 'ctn_patch: %s\n' "$*"; fi; return 0; }
ctn_error() { if [ "$CTN_DETAILS" = 1 ]; then printf 'ctn_patch: %s\n' "$*" >&2; fi; return 0; }
[ "$CTN_DETAILS" = 1 ] || exec 2>/dev/null
trap 'ctn_finish "$?"' EXIT
CTN_ERROR_REASON= CTN_ERROR_HINT=
ctn_init "$CTN_DIR" || ctn_abort '模块初始化失败；请查看详细诊断' "$?"
CTN_DISPLAY_VERSION=$(ctn_summary_version "$CTN_PACKAGE_VERSION")
if [ "$CTN_EXERCISE" = 1 ]; then
    ctn_status 注意 "磁盘包 $CTN_DISPLAY_VERSION；安全演练，结束后恢复原值"
else
    ctn_status 注意 "磁盘包 $CTN_DISPLAY_VERSION；只读自检，不加载、不写入"
fi
ctn_note "verification mode=$([ "$CTN_EXERCISE" = 1 ] && printf exercise || printf read-only)"
CTN_ERROR_REASON= CTN_ERROR_HINT=
ctn_select || ctn_abort '文件或兼容性检查失败；请查看详细诊断，不尝试其它内核文件' "$?"
if [ "$CTN_RELEASE_VERIFIED" = 1 ]; then
    ctn_status 正常 "文件与兼容性：内核 $CTN_PROFILE，对应 KO 与厂商模块就绪"
else
    ctn_status 注意 "文件与兼容性：内核 $CTN_PROFILE，完整版本未在实测名单；KO 由内核 ABI 校验，厂商模块已就绪"
fi
ctn_report_running
if [ "$CTN_DETAILS" = 1 ] && command -v ctn_report_data >/dev/null 2>&1; then ctn_report_data; fi

CTN_IDENTITY_RC=0
CTN_ERROR_REASON= CTN_ERROR_HINT=
if ctn_loaded; then
    if ctn_running_identity; then
        ctn_status 正常 "当前运行：$(ctn_summary_version "$CTN_RUNNING_VERSION")；补丁身份与磁盘包一致"
    else
        ctn_status 失败 "当前运行：$(ctn_summary_version "${CTN_RUNNING_VERSION:-}")；${CTN_ERROR_HINT:-身份不同或未知，请正常重启后再自检，不会自动启用}"
        CTN_IDENTITY_RC=1
    fi
elif [ -e "$CTN_NODE" ]; then
    ctn_error 'unknown native node provenance; no daemon-only profile is approved'
    ctn_status 失败 '运行接口：来源不明；拒绝演练或自动启用，请正常重启后再检查'
    CTN_IDENTITY_RC=1
else
    ctn_status 跳过 '当前运行：补丁尚未加载；磁盘包版本不代表已经生效'
fi
CTN_ERROR_REASON= CTN_ERROR_HINT=
CTN_SAMPLE_DAEMON=$(ctn_daemon_pid) || CTN_SAMPLE_DAEMON=
if [ -n "$CTN_SAMPLE_DAEMON" ]; then
    if [ "$(ctn_sha256 "$CTN_PROC/$CTN_SAMPLE_DAEMON/exe")" = "$CTN_DAEMON_HASH" ]; then
        ctn_status 正常 '后台进程：文件身份匹配（只读快照，不代表游戏效果）'
    else
        ctn_status 注意 '后台进程：仍是其它版本或身份不可读；新文件不代表已生效'
    fi
else
    ctn_status 跳过 '后台进程：未确认当前版本；过期或无关进程不算生效'
fi
if command -v ctn_data_ready >/dev/null 2>&1; then
    if ctn_data_ready; then ctn_status 正常 '数据库：文件可读取；不代表游戏配置已成功下发'
    else ctn_status 注意 '数据库：尚未就绪或不可读取；原因见详细诊断'; fi
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
        ctn_status 正常 '进程样本：已匹配登记名称（只读快照，不代表调度提升）'
    else
        ctn_note 'SKIP real PID exercise: no stable HAL-registered sample; rt_info is never overwritten'
        ctn_status 跳过 '进程样本：没有稳定登记样本；不判失败、不覆盖登记信息'
    fi
}
if [ -r "$CTN_NODE" ]; then ctn_pid_sample
else ctn_status 跳过 '进程样本：接口不可读或尚未加载'; fi

if [ "$CTN_EXERCISE" = 0 ]; then
    ctn_note 'Static checks only: no loading, node writes, new threads, PID signals, log writes, or enable changes.'
    ctn_note 'No runtime exercise was performed; a matching on-disk file is not proof of a running new version.'
    ctn_status 跳过 '写入演练：只读模式，不暂停服务、不改变启用状态'
    exit "$CTN_IDENTITY_RC"
fi

CTN_ERROR_REASON= CTN_ERROR_HINT=
[ "$(id -u)" = 0 ] || { ctn_error '--exercise requires root'; ctn_abort '演练条件：需要管理员权限；未执行演练写入'; }
ctn_disabled && { ctn_error 'disabled/removing: --exercise refused; flags unchanged'; ctn_abort '演练条件：模块已禁用或待删除；保持原状态'; }
[ "$CTN_IDENTITY_RC" = 0 ] || { ctn_error 'cannot exercise an old/unknown loaded instance'; ctn_abort '演练条件：旧版或未知补丁仍在运行；请正常重启，不会自动启用'; }
ctn_dependency_ready || { ctn_error 'vendor dependency is not ready'; ctn_abort '演练条件：厂商依赖尚未就绪；未执行演练写入'; }
ctn_game_idle || { ctn_error 'game running or idle state unknown; no exercise writes'; ctn_abort '演练条件：游戏运行中或空闲状态不明；未执行演练写入'; }
CTN_ERROR_REASON= CTN_ERROR_HINT=
ctn_maintenance_lock || ctn_abort '演练条件：维护锁不可用；拒绝并发操作' "$?"
CTN_ERROR_REASON= CTN_ERROR_HINT=
ctn_exercise_guard || ctn_abort '演练条件：存在未完成恢复记录；保留记录，不恢复服务'
CTN_RESUME_SERVICE=0
CTN_PAUSED=0
CTN_SAVED_NAMES=
CTN_SAVED_ENABLE=
CTN_NAMES_TOUCHED=0
CTN_EXERCISE_RECORD_HASH=
CTN_KEEP_STOP=0
[ ! -e "$CTN_RUNTIME/.service.stop" ] || CTN_KEEP_STOP=1
if ctn_service_pid >/dev/null; then
    CTN_RESUME_SERVICE=1
elif ! ctn_service_idle; then
    ctn_error 'service lock has no matching process identity; refusing to interrupt it'
    ctn_abort '服务暂停：锁与进程身份不符；拒绝中断无关进程'
elif ctn_daemon_pid >/dev/null; then
    ctn_error 'standalone daemon is not managed by this service; refusing to lose its original arguments'
    ctn_abort '服务暂停：存在独立后台进程；拒绝丢失其启动参数'
fi
ctn_status 正常 '演练条件：权限、空闲状态、维护锁与恢复记录允许演练'

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
    if [ -n "$CTN_EXERCISE_RECORD_HASH" ] && [ "$restore_rc" = 0 ]; then
        if ctn_safe_file "$CTN_RUNTIME/exercise.state" &&
           [ "$(ctn_sha256 "$CTN_RUNTIME/exercise.state")" = "$CTN_EXERCISE_RECORD_HASH" ]; then
            rm -f "$CTN_RUNTIME/exercise.state" || restore_rc=1
        else
            ctn_error 'self-check recovery record changed; retained'
            restore_rc=1
        fi
    fi
    if [ "$CTN_LOADED_BY_US" = 1 ]; then
        if [ "$restore_rc" = 0 ] && [ ! -e "$CTN_RUNTIME/restore.state" ]; then
            rmmod ctn_patch || { restore_rc=$?; ctn_error "cannot unload the test-owned KO rc=$restore_rc"; }
        else ctn_error 'restoration failed; refusing to unload test KO'; restore_rc=1
        fi
    fi
    if [ "$CTN_NAMES_TOUCHED" = 1 ] || [ "$CTN_LOADED_BY_US" = 1 ]; then
        if [ "$restore_rc" != 0 ]; then
            ctn_status 失败 '原值恢复：未完成；保留恢复记录和停止标记，不恢复服务'
        elif [ "$CTN_NAMES_TOUCHED" = 1 ]; then
            ctn_status 正常 '原值恢复：名称与启用值已恢复并回读确认'
        else
            ctn_status 正常 '原值恢复：本次测试加载的补丁已卸载；尚未执行名称写入'
        fi
    else
        ctn_status 跳过 '原值恢复：尚未执行演练写入或加载测试补丁'
    fi
    if [ "$CTN_PAUSED" = 1 ] && [ "$CTN_KEEP_STOP" = 0 ] && [ "$restore_rc" = 0 ]; then
        rm -f "$CTN_RUNTIME/.service.stop" || restore_rc=1
    fi
    ctn_maintenance_unlock || restore_rc=1
    CTN_SERVICE_RESUMED=0
    CTN_RESUME_DISABLED=0
    if [ "$CTN_RESUME_SERVICE" = 1 ] && [ "$CTN_PAUSED" = 1 ] &&
       [ "$CTN_KEEP_STOP" = 0 ] && [ "$restore_rc" = 0 ]; then
        if ctn_disabled; then
            ctn_note 'disable/remove appeared during exercise; service deliberately not resumed'
            CTN_RESUME_DISABLED=1
        else
            "$CTN_SHELL" "$MODDIR/service.sh" || restore_rc=1
            while [ "$restore_rc" = 0 ]; do
                if ctn_service_pid >/dev/null && ctn_daemon_current >/dev/null; then
                    ctn_note 'original service and current daemon resumed'; CTN_SERVICE_RESUMED=1; break
                fi
                [ "$tries" -lt 15 ] || { ctn_error 'service did not resume; inspect runtime logs'; restore_rc=1; break; }
                sleep 1; tries=$((tries + 1))
            done
        fi
    fi
    if [ "$restore_rc" != 0 ]; then
        rc=$restore_rc
        ctn_status 失败 '服务收尾：恢复或解锁未完成；不启动新后台，请查看详细诊断'
    elif [ "$CTN_RESUME_DISABLED" = 1 ]; then
        ctn_status 注意 '服务收尾：期间出现禁用或删除标记；保持停止'
    elif [ "$CTN_SERVICE_RESUMED" = 1 ]; then
        ctn_status 正常 '服务收尾：原服务与当前后台已恢复'
    elif [ "$CTN_KEEP_STOP" = 1 ]; then
        ctn_status 注意 '服务收尾：保留原有停止请求；不重启服务'
    else
        ctn_status 跳过 '服务收尾：没有需要恢复的原服务'
    fi
    ctn_note "exercise finished rc=$rc profile=$CTN_PROFILE build_id=$CTN_BUILD_ID; dmesg was not cleared"
    ctn_finish "$rc"
}
trap ctn_exercise_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

CTN_ERROR_REASON= CTN_ERROR_HINT=
ctn_stop_service || ctn_abort '服务暂停：未能安全停止或原值恢复未完成；拒绝演练'
CTN_PAUSED=1
ctn_status 正常 '服务暂停：已安全停止本模块后台；无关进程未处理'
CTN_ERROR_REASON= CTN_ERROR_HINT=
ctn_disabled && { ctn_error 'disabled/removing while waiting'; ctn_abort '演练准备：等待期间被禁用或删除；不继续写入'; }
ctn_game_idle || { ctn_error 'game started while pausing service; no exercise writes'; ctn_abort '演练准备：暂停期间游戏启动；不继续写入'; }
ctn_ensure_module || ctn_abort '演练准备：补丁加载或运行身份检查失败；不尝试其它内核文件' "$?"
ctn_read_names || { ctn_error 'original names cannot be restored unambiguously; no writes performed'; ctn_abort '演练准备：原名称无法可靠恢复；未执行演练写入'; }
CTN_SAVED_NAMES=$CTN_NAMES
CTN_SAVED_ENABLE=$(ctn_scalar "$CTN_ENABLE") || ctn_abort '演练准备：无法读取原启用值；未执行演练写入'
case "$CTN_SAVED_ENABLE" in 0|1) ;; *) ctn_error 'unknown ct_enable original; no writes performed'; ctn_abort '演练准备：原启用值未知；未执行演练写入' ;; esac
[ -w "$CTN_NODE" ] || ctn_abort '演练准备：名称接口不可写；未执行演练写入'
ctn_game_idle || { ctn_error 'game started before node tests; no exercise writes'; ctn_abort '演练准备：写入前游戏启动；未执行演练写入'; }
ctn_exercise_record "$CTN_SAVED_NAMES" "$CTN_SAVED_ENABLE" || {
    ctn_error 'cannot save self-check recovery record; no writes performed'; ctn_abort '演练准备：无法保存恢复记录；未执行演练写入';
}
CTN_NAMES_TOUCHED=1
ctn_expect_names 'CTNTestMain CTNTestGfx' 'CTNTestMain CTNTestGfx' || {
    CTN_WRITE_RC=$?; ctn_error "dual-name write/readback failed rc=$CTN_WRITE_RC"; ctn_abort '名称演练：双名称写入或回读失败；正在恢复原值' "$CTN_WRITE_RC";
}
ctn_expect_names 'CTNTestSingle' 'CTNTestSingle CTNTestSingle' || {
    CTN_WRITE_RC=$?; ctn_error "single-name write/readback failed rc=$CTN_WRITE_RC"; ctn_abort '名称演练：单名称写入或回读失败；正在恢复原值' "$CTN_WRITE_RC";
}
if ctn_write_node 'A B C' 2>/dev/null; then ctn_error 'invalid three-name input was accepted'; ctn_abort '名称演练：错误接受了三个名称；正在恢复原值'; fi
if ctn_write_node ' ' 2>/dev/null; then ctn_error 'empty input was accepted'; ctn_abort '名称演练：错误接受了空名称；正在恢复原值'; fi
ctn_note 'Name write/read and rejection tests passed; restoring saved values, not defaults.'
ctn_status 正常 '名称演练：单双名称写入、回读和无效输入拒绝均通过'
ctn_note 'SKIP synthetic PID registration: rt_info has no proven reversible restore contract.'
ctn_status 跳过 '模拟进程登记：没有可靠可逆恢复约定；不执行'
exit 0
