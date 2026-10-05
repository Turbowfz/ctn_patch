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
MODDIR=${0%/*}

touch "$MODDIR/.stop" 2>/dev/null
sleep 1

pkill -f "$MODDIR/ctnd" 2>/dev/null
pkill -x ctnd 2>/dev/null
sleep 1

rmmod ctn_patch 2>/dev/null

rm -f "$MODDIR/.stop" 2>/dev/null
exit 0
