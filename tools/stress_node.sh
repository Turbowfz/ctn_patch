#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# 节点压测：专门压 ctn_patch 的双缓冲 + synchronize_rcu 换指针那条路
#
# 为什么单独有这个脚本：模块里最容易出事的就是「换指针」—— 6.1 的名单是
# .rodata 里的指针数组，写它要走 vmap 可执行别名 + 双缓冲，读者是 sched_switch
# tracepoint。写坏了不会立刻崩，而是表现为「读回上一轮甚至好几轮之前的值」。
# verify.sh 只写 8 次，量太小；这个脚本按需压几千次。
#
# 用法（root）：
#   sh stress_node.sh            # 默认 300 轮
#   sh stress_node.sh 2000
#
# 退出码 = 不一致次数（0 = 全对）。

N=/proc/game_opt/task_boost/critical_task_name
ROUNDS=${1:-300}

[ "$(id -u)" = "0" ] || { echo "需要 root"; exit 1; }
[ -e "$N" ] || { echo "$N 不存在 —— 模块没加载？"; exit 1; }

bad=0

echo "=== A. 连续写读 $ROUNDS 次（每轮一个独特名字）==="
i=1
while [ $i -le "$ROUNDS" ]; do
	echo "W${i}Main W${i}Gfx" > $N
	got=$(cat $N)
	[ "$got" = "W${i}Main:-1,W${i}Gfx:-1" ] || { echo "  第 $i 次: [$got]"; bad=$((bad+1)); }
	i=$((i+1))
done
echo "  不一致 $bad / $ROUNDS"

echo "=== B. 长名/短名交替（最容易暴露缓冲槽复用）==="
b2=0
i=1
while [ $i -le 50 ]; do
	echo "ABCDEFGHIJKLMNO0 ABCDEFGHIJKLMNO1" > $N
	got=$(cat $N)
	[ "$got" = "ABCDEFGHIJKLMNO0:-1,ABCDEFGHIJKLMNO1:-1" ] || { echo "  长名第 $i 次: [$got]"; b2=$((b2+1)); }
	echo "S${i} S${i}x" > $N
	got=$(cat $N)
	[ "$got" = "S${i}:-1,S${i}x:-1" ] || { echo "  短名第 $i 次: [$got]"; b2=$((b2+1)); }
	i=$((i+1))
done
echo "  不一致 $b2 / 100"

echo "=== C. 非法写入不应改动现有值 ==="
echo "UnityMain UnityGfxDevice" > $N
before=$(cat $N)
echo "onlyone" > $N 2>/dev/null
echo "A B C"   > $N 2>/dev/null
after=$(cat $N)
[ "$before" = "$after" ] && echo "  未改动（正确）" || { echo "  !! [$before] -> [$after]"; bad=$((bad+1)); }

echo "=== D. 恢复默认值 ==="
echo "UnityMain UnityGfxDevice" > $N
got=$(cat $N)
[ "$got" = "UnityMain:-1,UnityGfxDevice:-1" ] && echo "  OK" || { echo "  !! [$got]"; bad=$((bad+1)); }

echo
echo "总计不一致: $bad"
exit $bad
