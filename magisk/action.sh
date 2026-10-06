#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Magisk/KernelSU 的「操作 / Action」按钮
#
# 流程（顺序有讲究）：
#   1. 停 daemon —— 先放 .stop 哨兵，否则 service.sh 的守护循环会立刻把它拉回来
#   2. 卸载模块   —— 必须卸，这样 verify.sh 才能真测到「加载 → 卸载」这一链
#   3. 跑 verify.sh（它自己负责 insmod / 测试 / rmmod）
#   4. 恢复可用状态：重新加载模块 + 起 daemon
#
# 结果写 action.log；同时 echo 一份路径，管理器会把它显示出来。

MODDIR=${0%/*}
LOG="$MODDIR/action.log"
DLOG="$MODDIR/daemon.log"
STOP="$MODDIR/.stop"
NODE=/proc/game_opt/task_boost/critical_task_name

echo "执行中，日志：$LOG"

{
	echo "==== $(date) 重载并自检 ===="

	# --- 1. 停 daemon ---
	touch "$STOP" 2>/dev/null
	pkill -f "$MODDIR/ctnd" 2>/dev/null
	pkill -x ctnd 2>/dev/null
	sleep 1
	echo "daemon 已停（残留 $(ps -A | grep -c '[c]tnd') 个）"

	# --- 2. 卸载模块 ---
	if grep -q '^ctn_patch ' /proc/modules; then
		if rmmod ctn_patch 2>/dev/null; then
			echo "旧模块已卸载"
		else
			echo "rmmod 失败（有进程正拿着节点？下面自检会体现）"
		fi
	else
		echo "模块当前未加载"
	fi
	echo

	# --- 3. 自检（它自己 insmod / 测试 / rmmod / 再把模块和 daemon 拉起来）---
	sh "$MODDIR/verify.sh" "$MODDIR/ctn_patch.ko"
	RC=$?

	# --- 4. 兜底：保证设备不留在坏状态 ---
	# 先摘哨兵，再确认模块和 daemon 都在。daemon 没起来就重跑 service.sh ——
	# 它会把守护循环和 daemon 一起拉起来（光起 daemon 没有守护循环的话，
	# 它挂了就没人重启，等于少了一层保险）。
	rm -f "$STOP"
	grep -q '^ctn_patch ' /proc/modules || insmod "$MODDIR/ctn_patch.ko" 2>/dev/null

	if ! pgrep -x ctnd >/dev/null 2>&1; then
		echo
		echo "daemon 没在运行，重跑 service.sh 恢复（含守护循环）"
		sh "$MODDIR/service.sh"
		i=1
		while [ $i -le 10 ]; do
			pgrep -x ctnd >/dev/null 2>&1 && break
			sleep 1
			i=$((i+1))
		done
	fi

	echo
	if pgrep -x ctnd >/dev/null 2>&1; then
		echo "当前状态：模块 $(grep -c '^ctn_patch ' /proc/modules) 个 ｜ daemon $(pgrep -c -x ctnd) 个（pid $(pgrep -x ctnd | head -1)）｜ 节点 $(cat $NODE 2>/dev/null)"
	else
		echo "!! 当前状态：模块 $(grep -c '^ctn_patch ' /proc/modules) 个 ｜ daemon 仍然没起来 ｜ 节点 $(cat $NODE 2>/dev/null)"
		echo "   daemon.log 尾部："
		tail -n 5 "$DLOG" 2>/dev/null | sed 's/^/     /'
	fi
	[ "$RC" = "0" ] && echo "（自检全部通过）" || echo "（自检有失败项，看上面标 [!!] 的行）"
} > "$LOG" 2>&1
