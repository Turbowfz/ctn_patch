#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ctn_patch 兼容性检查（真机只读，不改任何东西）
# 内容与 magisk/customize.sh 的闸门一致，供 adb 侧一次性跑完

FAIL=0; WARN=0; INFO=0
p() { echo "  [OK]   $*"; }
w() { WARN=$((WARN+1)); echo "  [警告] $*"; }
i() { INFO=$((INFO+1)); echo "  [备注] $*"; }
b() { FAIL=$((FAIL+1)); echo "  [不满足] $*"; }

cfg() { [ "$HAVE_CONFIG" = 1 ] && echo "$CONFIG_TXT" | sed -n "s/^$1=//p" | head -n1; }
cfg_on() { v=$(cfg "$1"); [ "$v" = "y" ] || [ "$v" = "m" ]; }

CONFIG_TXT=""; HAVE_CONFIG=0
if [ -r /proc/config.gz ]; then
	CONFIG_TXT="$(zcat /proc/config.gz 2>/dev/null)" || CONFIG_TXT="$(gzip -dc /proc/config.gz 2>/dev/null)"
fi
[ -n "$CONFIG_TXT" ] && HAVE_CONFIG=1

echo "===== 1. 内核版本 ====="
case "$(uname -r)" in
	6.1.141-android14-11*) p "内核 $(uname -r)" ;;
	*) b "内核 $(uname -r) 不是 6.1.141-android14-11 系列" ;;
esac

echo "===== 2. 目标模块 ====="
if grep -q '^oplus_bsp_game_opt ' /proc/modules; then
	p "oplus_bsp_game_opt 已加载（模块）refcount=$(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)"
else
	b "oplus_bsp_game_opt 不在 /proc/modules"
fi
[ -d /proc/game_opt/task_boost ] && p "/proc/game_opt/task_boost 存在" || b "缺 /proc/game_opt/task_boost"
echo "  task_boost 现有节点: $(ls /proc/game_opt/task_boost 2>/dev/null | tr '\n' ' ')"

echo "===== 3. 是否已有 critical_task_name ====="
if [ -e /proc/game_opt/task_boost/critical_task_name ]; then
	b "已存在！$(cat /proc/game_opt/task_boost/critical_task_name 2>/dev/null)"
else
	p "不存在，安装后可创建"
fi

echo "===== 4. 内核配置 ====="
if [ "$HAVE_CONFIG" != 1 ]; then
	w "读不到 /proc/config.gz"
else
	p "读到内核配置（$(echo "$CONFIG_TXT" | wc -l) 行）"
	cfg_on CONFIG_KPROBES      && p "KPROBES=y"          || b "KPROBES 未开（无法解析符号）"
	cfg_on CONFIG_MODULE_UNLOAD && p "MODULE_UNLOAD=y"   || w "MODULE_UNLOAD 未开（装上无法卸载）"
	[ "$(cfg CONFIG_MODULE_SIG_FORCE)" = y ] && b "MODULE_SIG_FORCE=y（未签名模块不让加载）" || p "未强制模块签名"
	cfg_on CONFIG_KALLSYMS_ALL && p "KALLSYMS_ALL=y"     || w "KALLSYMS_ALL 未开"
	cfg_on CONFIG_MODVERSIONS  && p "MODVERSIONS=y"      || w "MODVERSIONS 未开"
	cfg_on CONFIG_CFI_CLANG    && p "CFI_CLANG=y"        || w "CFI_CLANG 未开（kCFI 无需对齐）"
	cfg_on CONFIG_STRICT_MODULE_RWX && p "STRICT_MODULE_RWX=y" || i "STRICT_MODULE_RWX 未开"
fi

echo "===== 5. 运行时符号 ====="
for s in kallsyms_lookup_name find_module register_kprobe unregister_kprobe; do
	if awk -v n="$s" '$3==n{found=1;exit} END{exit !found}' /proc/kallsyms 2>/dev/null; then
		p "有 $s"
	else
		[ "$s" = "kallsyms_lookup_name" ] && b "缺 $s（无法解析模块符号）" || w "缺 $s"
	fi
done
awk '$3=="copy_from_kernel_nofault"{f=1;exit} END{exit !f}' /proc/kallsyms 2>/dev/null \
	&& p "有 copy_from_kernel_nofault" \
	|| w "缺 copy_from_kernel_nofault（会退回 memcpy，无影响）"

echo "===== 6. 其它 ====="
i "页大小 $(getconf PAGE_SIZE 2>/dev/null)"
i "SELinux $(getenforce 2>/dev/null)"
i "kptr_restrict $(cat /proc/sys/kernel/kptr_restrict 2>/dev/null)"

echo
echo "===== 结论：FAIL=$FAIL WARN=$WARN INFO=$INFO ====="
[ "$FAIL" -eq 0 ] && echo "兼容性检查通过" || echo "存在不满足项，不该安装"
