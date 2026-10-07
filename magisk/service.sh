#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ================= ctn_patch 开机加载 =================
# Magisk/KernelSU/APatch 在 late_start 阶段执行本文件。
#
# 两件事，必须按顺序：
#   1. 等 oplus_bsp_game_opt 起来 → insmod ctn_patch.ko → 节点出现
#   2. 再拉起 ctnd —— 它要往那个节点里写，节点没出来它只能报错
#
# 整段放一个后台子 shell 里串行做，不卡开机。
# （早先版本把 insmod 放后台、ctnd 紧接着启动，实测撞上竞态：
#   ctnd 先跑起来，节点还不存在，第一局游戏的名字没写进去。）

MODDIR=${0%/*}
LOG="$MODDIR/boot.log"
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

# 重新加载前清掉上次卸载可能留下的哨兵
rm -f "$STOP"

# 日志轮转：daemon.log 会一直长，超过 512KB 就只留最后 1000 行
if [ -f "$DLOG" ]; then
	SZ=$(stat -c%s "$DLOG" 2>/dev/null || echo 0)
	if [ "$SZ" -gt 524288 ]; then
		tail -n 1000 "$DLOG" > "$DLOG.tmp" 2>/dev/null && mv -f "$DLOG.tmp" "$DLOG"
		echo "==== $(date) daemon.log 超 512KB，已截断 ====" >> "$DLOG"
	fi
fi

(
	# ---------- 阶段一：等 game_opt → insmod ----------
	{
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
			echo "模块没起来，不启动 ctnd"
			exit 1
		fi
		echo "==== 加载流程结束 ===="
	} >> "$LOG" 2>&1

	# ---------- 阶段二：守护 ctnd（此时节点已经在了）----------
	[ -x "$MODDIR/ctnd" ] || { echo "警告：找不到 $MODDIR/ctnd，节点不会被自动写入" >> "$LOG"; exit 0; }


	while true; do
		# 两个退出条件缺一不可：哨兵出现、或模块目录没了。
		# 少了它们，卸载时 pkill 只杀得掉 ctnd 本体，杀不掉这个循环，
		# 一会儿它又会把 ctnd 拉起来。
		[ -e "$STOP" ] && { echo "==== $(date) 收到 .stop，守护退出 ====" >> "$DLOG"; exit 0; }
		[ -d "$MODDIR" ] || exit 0

		# 哨兵在「拉起前」再查一遍：action.sh 是 touch .stop 后才 pkill，
		# 而本循环可能刚睡到一半，醒来时正好卡在停与卸载之间 —— 这时
		# 再拉一个 ctnd 就会去撞锁，白刷一行「已有另一个实例」。
		[ -e "$STOP" ] && { echo "==== $(date) 收到 .stop，守护退出 ====" >> "$DLOG"; exit 0; }

		# ctnd 正在跑就不拉新的（不然 pkill 没杀干净时，这里会叠一个）
		if ! ctnd_alive; then
			echo "==== $(date) 启动 ctnd ====" >> "$DLOG"
			"$MODDIR/ctnd" >> "$DLOG" 2>&1
			RC=$?
		else
			RC=0
			sleep 2
			continue
		fi

		[ -e "$STOP" ] && { echo "==== $(date) 收到 .stop，守护退出 ====" >> "$DLOG"; exit 0; }

		# 退出码 3 = 已有别的 ctnd 实例在跑（说明有另一个守护循环）。
		# 这种情况要退出，不然会每几秒起一次、每次都被锁挡回来，白刷日志。
		if [ "$RC" = "3" ]; then
			echo "==== $(date) 已有其它 ctnd 实例，本守护退出 ====" >> "$DLOG"
			exit 0
		fi

		echo "==== $(date) ctnd 退出 rc=$RC，2 秒后重启 ====" >> "$DLOG"
		sleep 2
	done
) &

exit 0
