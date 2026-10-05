#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Magisk/KernelSU 移除本模块时执行。
# 尽力当场 rmmod；失败也没关系，重启后模块目录没了自然不会再加载。
rmmod ctn_patch 2>/dev/null
exit 0
