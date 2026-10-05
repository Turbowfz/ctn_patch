#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Magisk「操作 / Action」按钮（KernelSU 同样支持）：
# 重载一次 ctn_patch 并跑完整验证，结果写到 action.log。
MODDIR=${0%/*}
LOG="$MODDIR/action.log"

echo "执行中，日志：$LOG"

{
	echo "==== $(date) 重载 ===="
	if grep -q '^ctn_patch ' /proc/modules; then
		if rmmod ctn_patch; then
			echo "rmmod 成功"
		else
			echo "rmmod 失败（有人正拿着节点？改用重启）"
		fi
	else
		echo "当前未加载，直接 insmod"
	fi

	if insmod "$MODDIR/ctn_patch.ko"; then
		echo "insmod 成功"
	else
		echo "insmod 失败，dmesg 尾部："
		dmesg | tail -n 20
	fi

	sh "$MODDIR/verify.sh" "$MODDIR/ctn_patch.ko" | tail -n 60
} > "$LOG" 2>&1
