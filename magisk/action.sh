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
# ---- ctnd 进程探测/击杀：读 pidfile ----
# 判断 daemon 在不在跑 = 读它自己写的 ctnd.pid（模块目录下）+ 确认 /proc/<pid>
# 还在、comm 是 ctnd。O(1)，只读一个文件、看一眼目录。
#
# 为什么不用别的（都试过，都不行）：
#   - pgrep/pkill -x：语义因实现而异（busybox 比的是整条命令行），永远匹配不上；
#   - 扫 /proc/*/fd 找「谁握着锁」：结果对，但**极慢** —— 有的进程 800+ 个 fd，
#     全系统扫一遍是几万次 readlink。守护循环每 2 秒跑一次，等于持续在后台扒全
#     系统的 fd（实测执行轨迹 6.7 万行没跑完）—— 这本身就是实打实的后台开销。
# pidfile 陈旧（被 kill -9）也没关系：下面会再确认 /proc/<pid> 和 comm。
# ★ 注意：插这段时曾经把它的结束标记和下一行粘在一起，把 STOP=... 注释掉了，
#   结果守护循环永远收不到 .stop、不停重启 daemon（撞锁刷屏 + 后台白跑）。
#   改这段时务必确认后面那行还在。★
CTND_PIDFILE="$MODDIR/ctnd.pid"

ctnd_pid() {
	_p=$(cat "$CTND_PIDFILE" 2>/dev/null)
	_p=${_p%%[!0-9]*}
	if [ -n "$_p" ] && [ -r "/proc/$_p/comm" ]; then
		read -r _n < "/proc/$_p/comm" 2>/dev/null
		[ "$_n" = "ctnd" ] && { echo "$_p"; return; }
	fi
	for _d in /proc/[0-9]*; do	# 兜底：pidfile 没有/失效时扫名字（便宜）
		[ -r "$_d/comm" ] || continue
		read -r _n < "$_d/comm" 2>/dev/null
		[ "$_n" = "ctnd" ] && { echo "${_d#/proc/}"; return; }
	done
}

ctnd_alive() { [ -n "$(ctnd_pid)" ]; }
ctnd_kill()  { _p=$(ctnd_pid); [ -n "$_p" ] && kill "$_p" 2>/dev/null; rm -f "$CTND_PIDFILE"; }
STOP="$MODDIR/.stop"
NODE=/proc/game_opt/task_boost/critical_task_name

echo "执行中，日志：$LOG"

{
	echo "==== $(date) 重载并自检 ===="

	# --- 1. 停 daemon ---
	touch "$STOP" 2>/dev/null
	ctnd_kill
	ctnd_kill
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

	if ! ctnd_alive; then
		echo
		echo "daemon 没在运行，重跑 service.sh 恢复（含守护循环）"
		sh "$MODDIR/service.sh"
		i=1
		while [ $i -le 10 ]; do
			ctnd_alive && break
			sleep 1
			i=$((i+1))
		done
	fi

	echo
	if ctnd_alive; then
		echo "当前状态：模块 $(grep -c '^ctn_patch ' /proc/modules) 个 ｜ daemon $(ctnd_alive && echo 1 || echo 0) 个（pid $(ctnd_pid)）｜ 节点 $(cat $NODE 2>/dev/null)"
	else
		echo "!! 当前状态：模块 $(grep -c '^ctn_patch ' /proc/modules) 个 ｜ daemon 仍然没起来 ｜ 节点 $(cat $NODE 2>/dev/null)"
		echo "   daemon.log 尾部："
		tail -n 5 "$DLOG" 2>/dev/null | sed 's/^/     /'
	fi
	[ "$RC" = "0" ] && echo "（自检全部通过）" || echo "（自检有失败项，看上面标 [!!] 的行）"
} > "$LOG" 2>&1
