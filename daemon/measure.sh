#!/system/bin/sh
# 量 ctnd 的占用：内存 / 线程 / 空闲 CPU / 处理一次游戏启动的 CPU
P=$(pgrep -x ctnd)
[ -n "$P" ] || { echo "ctnd 没在跑"; exit 1; }

echo "=== 进程 $P ==="
grep -E "VmRSS|VmSize|Threads" /proc/$P/status | sed 's/^/  /'

echo
echo "=== 文件 ==="
for f in /data/adb/modules/ctn_patch/ctnd /data/adb/modules_update/ctn_patch/ctnd; do
	[ -f "$f" ] && echo "  $f: $(stat -c%s "$f") 字节"
done
echo "  临时目录 /data/local/tmp/.ctnd: $(du -sk /data/local/tmp/.ctnd 2>/dev/null | cut -f1) KB"
ls -l /data/local/tmp/.ctnd/ 2>/dev/null | sed 's/^/    /'

ticks() { awk '{print $14+$15}' /proc/$P/stat; }

echo
echo "=== 空闲 60 秒 ==="
t1=$(ticks); sleep 60; t2=$(ticks)
d=$((t2 - t1))
echo "  CPU 时间增加 $d tick（100 tick = 1 秒）"
echo "  折算：$(awk -v d=$d 'BEGIN{printf "%.3f", d/100/60*100}')% 单核占用"

echo
echo "=== 处理一次游戏启动的 CPU（起一个假包名进程 + 写 game_pid）==="
t1=$(ticks)
/data/local/tmp/fakepkg com.tencent.tmgp.pubgmhd >/dev/null 2>&1 &
FP=$!
echo $FP > /proc/game_opt/game_pid
sleep 4
t2=$(ticks)
echo "  这一次注入花了 $((t2 - t1)) tick = $(awk -v d=$((t2-t1)) 'BEGIN{printf "%.3f", d/100}') 秒 CPU"
echo "  节点现在是: $(cat /proc/game_opt/task_boost/critical_task_name)"
kill $FP 2>/dev/null
echo -1 > /proc/game_opt/game_pid

echo
echo "=== 内存峰值 ==="
grep -E "VmHWM|VmRSS" /proc/$P/status | sed 's/^/  /'
