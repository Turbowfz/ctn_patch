#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ctn_patch 真机加载/功能/卸载 验证
# 用 /dev/kmsg 打标记，之后只截取标记之后的 dmesg，避免污染判断
KO=/data/local/tmp/ctn_patch.ko
NODE=/proc/game_opt/task_boost/critical_task_name
MARK="CTN_VERIFY_$(date +%s)_$$"

echo "$MARK START" > /dev/kmsg
echo "===== 0. 加载前 ====="
echo "  game_opt refcount = $(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)"
echo "  节点存在? $([ -e $NODE ] && echo YES || echo NO)"
echo "  task_boost: $(ls /proc/game_opt/task_boost | tr '\n' ' ')"

echo "===== 1. insmod ====="
insmod "$KO"
RC=$?
echo "  insmod rc=$RC"
if [ $RC -ne 0 ]; then
	echo "  !! 加载失败，dmesg："
	dmesg | sed -n "/$MARK START/,\$p" | tail -30
	exit 1
fi
echo "  game_opt refcount = $(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)  (期望 +1)"
echo "  ctn_patch 在 /proc/modules: $(grep -c '^ctn_patch ' /proc/modules)"

echo "===== 2. 节点与默认值 ====="
if [ -e "$NODE" ]; then
	echo "  节点已创建：$(ls -l $NODE | awk '{print $1, $3, $4}')"
	echo "  默认值: [$(cat $NODE)]"
else
	echo "  !! 节点没出现"
fi
echo "  task_boost 现在: $(ls /proc/game_opt/task_boost | tr '\n' ' ')"

echo "===== 3. 写入自定义名字 ====="
echo "  echo 'MyGameMain MyGameGfx' > 节点"
echo "MyGameMain MyGameGfx" > "$NODE" && echo "  写入 rc=0" || echo "  写入失败 rc=$?"
echo "  读回: [$(cat $NODE)]"

echo "===== 4. 边界用例 ====="
echo "onlyone" > "$NODE" 2>/dev/null && echo "  单名: 竟然被接受 (BUG)" || echo "  单名: 被拒绝 (正确)"
echo "A B C"   > "$NODE" 2>/dev/null && echo "  三名: 竟然被接受 (BUG)" || echo "  三名: 被拒绝 (正确)"
echo "A    B"  > "$NODE" 2>/dev/null && echo "  多空格: 接受 -> [$(cat $NODE)]" || echo "  多空格: 被拒绝 (异常)"
printf "A\tB"  > "$NODE" 2>/dev/null && echo "  制表符: 接受 -> [$(cat $NODE)]" || echo "  制表符: 被拒绝 (异常)"

echo "===== 5. 反复写（压双缓冲/RCU）====="
i=1; OK=1
while [ $i -le 8 ]; do
	echo "T${i}Main T${i}Gfx" > "$NODE"
	R="$(cat $NODE)"
	[ "$R" = "T${i}Main:-1,T${i}Gfx:-1" ] || { OK=0; echo "  第 $i 次不一致: [$R]"; }
	i=$((i+1))
done
[ $OK -eq 1 ] && echo "  连续 8 次写读全部正确"

echo "===== 6. 恢复默认名 ====="
echo "UnityMain UnityGfxDevice" > "$NODE"
echo "  读回: [$(cat $NODE)]"

echo "===== 7. 内核日志（标记之后）====="
dmesg | sed -n "/$MARK START/,\$p" | grep -vE "^\[ *[0-9]+\.[0-9]+\] $MARK" | tail -25

echo "===== 8. 异常扫描 ====="
BAD=$(dmesg | sed -n "/$MARK START/,\$p" | grep -iE "BUG:|WARNING:|Unable to handle|Internal error|Call trace|Oops|panic|CFI failure" | head -10)
if [ -z "$BAD" ]; then echo "  无 BUG/WARNING/oops/CFI failure"; else echo "  !! 发现异常："; echo "$BAD"; fi

echo "===== 9. rmmod 还原 ====="
rmmod ctn_patch && echo "  rmmod rc=0" || echo "  rmmod 失败 rc=$?"
echo "  game_opt refcount = $(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)  (期望回到 1)"
echo "  节点还在? $([ -e $NODE ] && echo 'YES (BUG!)' || echo 'NO (正确)')"
echo "  task_boost: $(ls /proc/game_opt/task_boost | tr '\n' ' ')"
echo "  原节点可读? ct_enable=[$(cat /proc/game_opt/task_boost/ct_enable 2>/dev/null)]"
echo "$MARK END" > /dev/kmsg
dmesg | sed -n "/$MARK END/,\$p" | head -5
