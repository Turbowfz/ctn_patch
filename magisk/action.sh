#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Magisk/KernelSU 的「操作 / Action」按钮：
# 重载内核模块 + 重启 daemon + 跑完整验证，结果写到 action.log。
MODDIR=${0%/*}
LOG="$MODDIR/action.log"
DLOG="$MODDIR/daemon.log"
STOP="$MODDIR/.stop"

echo "执行中，日志：$LOG"

{
	echo "==== $(date) 重载 ===="

	# ---------- 停 daemon（先放哨兵，否则守护循环会把它拉回来）----------
	touch "$STOP" 2>/dev/null
	sleep 1
	pkill -f "$MODDIR/ctnd" 2>/dev/null
	pkill -x ctnd 2>/dev/null
	sleep 1
	echo "daemon 已停，残留进程: $(ps -A | grep -c '[c]tnd')"

	# ---------- 重载内核模块 ----------
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
		echo "insmod 成功，refcount=$(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)"
		echo "节点: $(cat /proc/game_opt/task_boost/critical_task_name 2>/dev/null)"
	else
		echo "insmod 失败，dmesg 尾部："
		dmesg | tail -n 20
	fi

	# ---------- 重启 daemon ----------
	rm -f "$STOP"
	if [ -x "$MODDIR/ctnd" ]; then
		setsid "$MODDIR/ctnd" >> "$DLOG" 2>&1 < /dev/null &
		sleep 2
		if ps -A | grep -q '[c]tnd'; then
			echo "daemon 已重启"
		else
			echo "daemon 启动失败，看 $DLOG"
		fi
	else
		echo "找不到 $MODDIR/ctnd"
	fi

	# ---------- 验证 ----------
	echo
	sh "$MODDIR/verify.sh" "$MODDIR/ctn_patch.ko" 2>&1 | tail -n 70
} > "$LOG" 2>&1
