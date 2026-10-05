#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ================= ctn_patch 开机加载 =================
# Magisk/KernelSU/APatch 在 late_start 阶段执行本文件。
# 此时 init 早就把 vendor 的 oplus_bsp_game_opt 加载好了，但保险起见还是等它出现。
# 整段放后台跑，不卡开机。加载失败会把 dmesg 一起写进 boot.log，便于定位。
#
# 本脚本只负责「加载 + 记录」；兼容性闸门在 customize.sh（刷入时），
# 功能验证在 verify.sh（也可点管理器「操作」按钮跑）。

MODDIR=${0%/*}
LOG="$MODDIR/boot.log"

(
	echo "==== $(date) 开始加载 ===="
	echo "内核: $(uname -r)"

	i=0
	while [ "$i" -lt 90 ]; do
		grep -q '^oplus_bsp_game_opt ' /proc/modules && break
		sleep 1
		i=$((i + 1))
	done
	if ! grep -q '^oplus_bsp_game_opt ' /proc/modules; then
		echo "等 90 秒还没见到 oplus_bsp_game_opt，放弃（游戏模块没起来？）"
		exit 1
	fi
	echo "oplus_bsp_game_opt 已加载，refcount=$(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)"

	if grep -q '^ctn_patch ' /proc/modules; then
		echo "ctn_patch 已在内核里（可能是手动 insmod 过），跳过"
	elif insmod "$MODDIR/ctn_patch.ko"; then
		echo "insmod 成功，refcount 现为 $(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)"
		if [ -e /proc/game_opt/task_boost/critical_task_name ]; then
			echo "节点已出现：$(cat /proc/game_opt/task_boost/critical_task_name)"
		else
			echo "警告：insmod 成功但节点没出现？查下面 dmesg"
		fi
	else
		echo "insmod 失败 rc=$?，dmesg 里 ctn_patch 相关行："
		dmesg | grep -i ctn_patch | tail -n 20
		echo "--- dmesg 尾部 ---"
		dmesg | tail -n 30
	fi
	echo "==== 完成 ===="
) >> "$LOG" 2>&1 &

exit 0
