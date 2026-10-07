#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Magisk/KernelSU 移除本模块时执行。
#
# 顺序要紧：
#   1. 先放哨兵 .stop —— service.sh 里那层守护循环只认这个才会自己退出。
#      少了它，pkill 只杀得掉 ctnd 本体，外层 shell 循环会在 5 秒后又把它
#      拉起来，模块都卸了 daemon 还在跑。
#   2. 再杀 ctnd。
#   3. 最后 rmmod —— 反过来的话 daemon 会在节点消失后一直写失败刷日志。
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
MODDIR=${0%/*}

touch "$MODDIR/.stop" 2>/dev/null
sleep 1

ctnd_kill

sleep 1

rmmod ctn_patch 2>/dev/null

rm -f "$MODDIR/.stop" 2>/dev/null
exit 0
