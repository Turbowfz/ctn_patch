#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Magisk/KernelSU 移除本模块时执行。
#
# 顺序要紧：先停 daemon，再 rmmod。
# 反过来的话 daemon 会在模块卸载后继续往已经不存在的节点里写
# （写不进去，但会一直刷日志、白占 CPU）。
MODDIR=${0%/*}

pkill -f "$MODDIR/ctnd" 2>/dev/null
pkill -f ctnd 2>/dev/null
sleep 1

rmmod ctn_patch 2>/dev/null
exit 0
