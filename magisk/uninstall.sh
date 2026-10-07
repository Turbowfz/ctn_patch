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
# ---- ctnd 进程探测/击杀：不依赖 pgrep/pkill ----
# KernelSU 的 action 环境会把 PATH 指到 busybox，busybox 的 pgrep/pkill -x
# 匹配的是**整条命令行**（cmdline 是完整路径），永远匹配不上 —— 表现就是
# 「daemon 明明活着却报 0 个」「pkill 杀不掉导致重试全撞锁」（实测踩过）。
# 所以自己扫 /proc/*/comm，与 pgrep 的实现无关。
ctnd_pids() {
	for d in /proc/[0-9]*; do
		[ -r "$d/comm" ] || continue
		read -r n < "$d/comm" 2>/dev/null
		[ "$n" = "ctnd" ] && echo "${d#/proc/}"
	done
}
ctnd_count() { ctnd_pids | wc -l; }
# 握着 .ctnd.lock 的进程：按 /proc/*/fd 扫，与进程名、pgrep 实现**都无关**。
# 有些环境里进程的 comm 会跟预期对不上（模块升级换了文件、pgrep 实现差异），
# 这时候只有「谁握着锁」这个事实是可靠的 —— daemon 活着的定义就是它持着锁。
lock_holders() {
	for d in /proc/[0-9]*; do
		p=${d#/proc/}
		[ -r "$d/fd" ] || continue
		for fd in $d/fd/*; do
			case "$(readlink "$fd" 2>/dev/null)" in
				*ctnd.lock) echo "$p"; break ;;
			esac
		done
	done
}
ctnd_alive() { [ -n "$(ctnd_pids)$(lock_holders)" ]; }
ctnd_one() { local p; p=$(ctnd_pids | head -1); [ -n "$p" ] && { echo "$p"; return; }; lock_holders | head -1; }
ctnd_kill() { for p in $(ctnd_pids) $(lock_holders); do kill "$p" 2>/dev/null; done; }
# ---- MODDIR=${0%/*}

touch "$MODDIR/.stop" 2>/dev/null
sleep 1

ctnd_kill

sleep 1

rmmod ctn_patch 2>/dev/null

rm -f "$MODDIR/.stop" 2>/dev/null
exit 0
