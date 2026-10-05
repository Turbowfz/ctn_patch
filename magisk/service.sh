#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ================= ctn_patch 开机加载 =================
# Magisk/KernelSU/APatch 在 late_start 阶段执行本文件。
#
# 做两件事：
#   1. insmod 内核模块，把 /proc/game_opt/task_boost/critical_task_name 补出来
#   2. 拉起 ctnd —— 没有它，那个节点永远是默认的 UnityMain/UnityGfxDevice，
#      对非 Unity 游戏（虚幻引擎那类）完全没用
#
# 顺序不能颠倒：ctnd 要往节点里写，节点得先存在。
# 整段放后台跑，不卡开机。兼容性闸门在 customize.sh（刷入时）。

MODDIR=${0%/*}
LOG="$MODDIR/boot.log"
DLOG="$MODDIR/daemon.log"

# ---------- 1+2：等 game_opt → insmod（后台跑，不卡开机）----------
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
	echo "==== 加载流程结束 ===="
) >> "$LOG" 2>&1 &

# ---------- 3. 守护 ctnd ----------
# 套一层循环：ctnd 万一崩了能自己起来。日志单独一份。
# 这里不加 -e —— ct_enable 归 HAL 管（它按 game_config.ctb + 游戏场景判定
# 开关），两边同时写会互相打架。想强制代管就自己给 ctnd 加 -e。
if [ -x "$MODDIR/ctnd" ]; then
	(
		while true; do
			echo "==== $(date) 启动 ctnd ====" >> "$DLOG"
			"$MODDIR/ctnd" -v >> "$DLOG" 2>&1
			echo "==== $(date) ctnd 退出，5 秒后重启 ====" >> "$DLOG"
			sleep 5
		done
	) &
	echo "ctnd 守护已启动（日志 $DLOG）" >> "$LOG"
else
	echo "警告：找不到 $MODDIR/ctnd，节点不会被自动写入" >> "$LOG"
fi

exit 0
