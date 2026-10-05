#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# 在 PC 上用原生 gcc 跑 ctnd 的解析逻辑单元测试（不需要设备）
set -uo pipefail
D=/mnt/c/Users/User/Desktop/风驰/6.1_一加ace3pro/05_补丁模块/ctn_patch/daemon
rm -rf /tmp/ct && mkdir -p /tmp/ct
cp "$D/ctnd.c" "$D/ctnd_test.c" /tmp/ct/ || { echo "!! 拷源码失败"; exit 1; }
cd /tmp/ct || exit 1
echo "=== 编译 ==="
# 注意：不要用 `gcc ... | head` —— head 提前关管道会给 gcc 发 SIGPIPE 把它弄死，
# 看起来像编译失败。日志重定向到文件再挑着看。
gcc -O1 -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
	-o t ctnd_test.c 2>cc.log
RC=$?
echo "gcc rc=$RC"
if [ -s cc.log ]; then
	echo "--- 编译器输出 ---"
	cat cc.log
fi
[ -x ./t ] || { echo "!! 编译失败"; exit 1; }
echo
./t
