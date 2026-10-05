#!/system/bin/sh
# ctnd v1.1 端到端验证（真机）
#
# 用 --db 指向一份合成的云控库，逐个走一遍所有配置形态：
#   本地覆盖 / 明文 JSON / base64(带填充) / base64(无填充) / 加密(未知) /
#   有配置无 ctn / game_config 为 NULL / 库里没这个包
# 用 fakepkg 伪造「进程名 = 包名」的进程，再把它的 pid 写进 game_pid，
# 让 ctnd 走完整的判断链。
#
# 不动设备上真正的 COSA 库和模块目录（结束时还原）。

NODE=/proc/game_opt/task_boost/critical_task_name
GAMEPID=/proc/game_opt/game_pid
CTEN=/proc/game_opt/task_boost/ct_enable
DB=/data/local/tmp/synth.db
CONF=/data/adb/ctn_patch/ctn.conf
LOG=/data/local/tmp/ctnd.log
CTND=/data/local/tmp/ctnd
FAKE=/data/local/tmp/fakepkg
PIDS=""

ok=0; fail=0
check() { # check <说明> <期望> <实际>
	if [ "$2" = "$3" ]; then
		echo "  [PASS] $1"
		ok=$((ok+1))
	else
		echo "  [FAIL] $1"
		echo "         期望: [$2]"
		echo "         实际: [$3]"
		fail=$((fail+1))
	fi
}

cleanup() {
	for p in $PIDS; do kill $p 2>/dev/null; done
	echo -1 > $GAMEPID 2>/dev/null
	kill $CTND_PID 2>/dev/null
	rm -f "$CONF"
	rmmod ctn_patch 2>/dev/null
}
trap cleanup EXIT

echo "=== 0. 准备 ==="
touch /data/adb/modules/ctn_patch/.stop 2>/dev/null
pkill -f ctnd 2>/dev/null
sleep 1
rmmod ctn_patch 2>/dev/null
mkdir -p /data/adb/ctn_patch

cat > "$CONF" <<'EOF'
# 测试用本地覆盖
aaa.local.test = LocalMain LocalRender
aaa.one.test   = SoloMain
aaa.long.test  = AVeryLongThreadName123 SecondName
EOF

echo "--- 起模块"
insmod /data/local/tmp/ctn_patch.ko || { echo "!! insmod 失败"; exit 1; }
echo "--- 版本"
"$CTND" --version
echo "--- 起 daemon（--db 指向合成库，并代管 ct_enable 以验证 ctb 路径）"
rm -f "$LOG"
"$CTND" -e --db "$DB" > "$LOG" 2>&1 &
CTND_PID=$!
sleep 1
[ -f "$LOG" ] && sed -n '1,5p' "$LOG" | sed 's/^/    /'

# run <包名>  —— 伪造进程、写 game_pid、等一拍
# 两个坑（都踩过）：
#   1. fakepkg 必须重定向掉 stdout —— 它在 pause() 里挂着不退出，要是还占着
#      命令替换的管道，`p=$(...)` 会一直等下去。
#   2. game_pid 节点只认**裸 pid**（内核里是 sscanf("%d")，见 task_util.c:56），
#      写成 "game_pid=N child_num=0" 会被 -EINVAL 拒掉（读出来才是那个格式）。
run() {
	"$FAKE" "$1" >/dev/null 2>&1 &
	p=$!
	PIDS="$PIDS $p"
	echo "$p" > $GAMEPID
	sleep 2
}

echo
echo "=== 1. 本地覆盖（优先级最高）==="
run aaa.local.test
check "本地覆盖生效" "LocalMain:-1,LocalRender:-1" "$(cat $NODE)"

echo
echo "=== 2. 本地覆盖：只写一个名字 → 应同名写两遍 ==="
run aaa.one.test
check "单名写两遍" "SoloMain:-1,SoloMain:-1" "$(cat $NODE)"

echo
echo "=== 3. 本地覆盖：名字超 15 字符 → 照样写，但日志告警 ==="
run aaa.long.test
check "超长名字照写" "AVeryLongThreadName123:-1,SecondName:-1" "$(cat $NODE)"
grep -q "超过 15 字符" "$LOG" && echo "  [PASS] 日志里有超长告警" && ok=$((ok+1)) \
	|| { echo "  [FAIL] 日志里没有超长告警"; fail=$((fail+1)); }

echo
echo "=== 4. 云控库：明文 JSON ==="
run aaa.json.test
check "明文 JSON 取值" "JsonMain:-1,JsonRender:-1" "$(cat $NODE)"

echo
echo "=== 5. 云控库：base64 编码的 JSON（带填充）==="
run aaa.b64.test
check "base64 解码取值" "B64Main:-1,B64Render:-1" "$(cat $NODE)"

echo
echo "=== 6. 云控库：base64 编码的 JSON（无填充）==="
run aaa.nopad.test
check "无填充 base64 尾部不丢" "NoPadMain:-1,NoPadRender:-1" "$(cat $NODE)"

echo
echo "=== 7. 云控库：加密/未知形态 → 告警 + 回退默认值 ==="
run aaa.opaque.test
check "未知形态回退默认值" "UnityMain:-1,UnityGfxDevice:-1" "$(cat $NODE)"
grep -q "既不是 JSON 也不是 base64-JSON" "$LOG" && echo "  [PASS] 日志里有明确告警" && ok=$((ok+1)) \
	|| { echo "  [FAIL] 日志里没有告警"; fail=$((fail+1)); }
grep -q "aaa.opaque.test = 名字1 名字2" "$LOG" && echo "  [PASS] 日志给出了本地覆盖写法" && ok=$((ok+1)) \
	|| { echo "  [FAIL] 日志没给出兜底写法"; fail=$((fail+1)); }

echo
echo "=== 8. 云控库：有配置但没 ctn（Unity 游戏）→ 默认值 ==="
run aaa.empty.test
check "无 ctn 用默认值" "UnityMain:-1,UnityGfxDevice:-1" "$(cat $NODE)"

echo
echo "=== 9. 云控库：game_config 为 NULL → 默认值 ==="
run aaa.null.test
check "NULL 配置用默认值" "UnityMain:-1,UnityGfxDevice:-1" "$(cat $NODE)"

echo
echo "=== 10. 云控库里没有这个包 → 默认值 ==="
run aaa.missing.test
check "库外包用默认值" "UnityMain:-1,UnityGfxDevice:-1" "$(cat $NODE)"

echo
echo "=== 11. ct_enable 代管（-e，按 ctb）==="
run aaa.json.test      # ctb=1
check "ctb=1 → ct_enable=1" "1" "$(cat $CTEN)"
run aaa.empty.test     # ctb=0
check "ctb=0 → ct_enable=0" "0" "$(cat $CTEN)"

echo
echo "=== 12. 缓存是否真的在省事（第二次查同一个包不应再拷库）==="
run aaa.json.test
grep -c "内容有变" "$LOG" | sed 's/^/    库指纹变化次数: /'
grep -q "云控 ctn" "$LOG" && echo "  [PASS] 缓存路径也正常出结果" && ok=$((ok+1)) \
	|| { echo "  [FAIL] 缓存路径没出结果"; fail=$((fail+1)); }

echo
echo "=== 13. 单实例锁（再起一个应立刻退出，rc=3）==="
"$CTND" --db "$DB" >/dev/null 2>&1
check "第二个实例退出码 3" "3" "$?"

echo
echo "=== 14. 结束：还原 ==="
kill $CTND_PID 2>/dev/null
CTND_PID=""
sleep 1
echo -1 > $GAMEPID
echo "0" > $CTEN
rmmod ctn_patch && echo "  模块已卸载" || echo "  !! rmmod 失败"
rm -f "$CONF"
rm -f /data/adb/modules/ctn_patch/.stop

echo
echo "=================================="
echo "结果: 通过 $ok 项，失败 $fail 项"
echo "=================================="
[ $fail -eq 0 ]
