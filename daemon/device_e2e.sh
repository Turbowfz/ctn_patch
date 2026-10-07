#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ctnd 端到端验证（真机，需要 root）
#
# 用 --db 指向一份**合成**的云控库，逐个走一遍所有配置形态；用 fakepkg 伪造
# 「进程名 = 包名」的进程、把它的 pid 写进 game_pid，让 ctnd 走完整判断链。
# 全程用临时目录和临时库，**不碰**设备上真正的 COSA 库与模块目录。
#
# v2.0 起的行为约定（本脚本校验的就是它）：
#   有 ctn                    → 写节点（唯一会写的情况，单名也接受）
#   无 ctn / 库外包 / 加密形态 → **一个字节都不写**，节点保持原值
#   游戏退出                  → 恢复成启动时读到的原值
#
# 用法：
#   adb push daemon/ctnd daemon/fakepkg daemon/device_e2e.sh daemon/synth.db /data/local/tmp/
#   adb push ctn_patch.ko /data/local/tmp/
#   adb shell su -c 'sh /data/local/tmp/device_e2e.sh'

NODE=/proc/game_opt/task_boost/critical_task_name
GAMEPID=/proc/game_opt/game_pid
CTND=/data/local/tmp/ctnd
FAKE=/data/local/tmp/fakepkg
DB=/data/local/tmp/synth.db
KO=/data/local/tmp/ctn_patch.ko
LOG=/data/local/tmp/ctnd_e2e.log
PIDS=""
ok=0; fail=0

check() {
	if [ "$2" = "$3" ]; then
		echo "  [PASS] $1"; ok=$((ok+1))
	else
		echo "  [FAIL] $1"; echo "         期望: [$2]"; echo "         实际: [$3]"; fail=$((fail+1))
	fi
}

cleanup() {
	for p in $PIDS; do kill $p 2>/dev/null; done
	echo -1 > $GAMEPID 2>/dev/null
	[ -n "$CTND_PID" ] && kill $CTND_PID 2>/dev/null
	rmmod ctn_patch 2>/dev/null
}
trap cleanup EXIT

echo "=== 0. 准备 ==="
for f in "$CTND" "$FAKE" "$DB" "$KO"; do
	[ -f "$f" ] || { echo "  缺 $f，见脚本头部用法"; exit 1; }
done
for d in /proc/[0-9]*; do			# 停掉现役 daemon，避免互相干扰
	[ -r "$d/comm" ] || continue
	read -r n < "$d/comm" 2>/dev/null
	[ "$n" = "ctnd" ] && kill "${d#/proc/}" 2>/dev/null
done
sleep 2; rmmod ctn_patch 2>/dev/null
insmod "$KO" || { echo "  insmod 失败"; exit 1; }
rm -rf /data/local/tmp/.ctnd
dmesg -c >/dev/null 2>&1
: > "$LOG"

"$CTND" --db "$DB" >> "$LOG" 2>&1 &
CTND_PID=$!
sleep 2
echo "  起始节点: $(cat $NODE)"

run() {		# run <包名>：伪造进程 + 写 game_pid + 等一拍
	"$FAKE" "$1" >/dev/null 2>&1 &
	p=$!
	PIDS="$PIDS $p"
	echo "$p" > $GAMEPID
	sleep 2
}

echo
echo "=== 1. 有 ctn（单名）→ 内核补成两槽 ==="
run aaa.single.test
check "单名 ctn 补成两槽" "SoloThread:-1,SoloThread:-1" "$(cat $NODE)"

echo
echo "=== 2. 有 ctn（两名，明文 JSON）==="
run aaa.json.test
check "明文 JSON 取值" "JsonMain:-1,JsonRender:-1" "$(cat $NODE)"

echo
echo "=== 3. base64 编码的 JSON（带填充）==="
run aaa.b64.test
check "base64 解码取值" "B64Main:-1,B64Render:-1" "$(cat $NODE)"

echo
echo "=== 4. base64 编码的 JSON（无填充）==="
run aaa.nopad.test
check "无填充 base64 尾部不丢" "NoPadMain:-1,NoPadRender:-1" "$(cat $NODE)"

echo
echo "=== 5. 三种「不该写」的情况：节点必须保持前一个值 ==="
BEFORE=$(cat $NODE)
echo "  --- 5a. 有配置但没 ctn（Unity 游戏）"
run aaa.empty.test
check "无 ctn 不写节点" "$BEFORE" "$(cat $NODE)"
echo "  --- 5b. 云控库里没有这个游戏"
run aaa.missing.test
check "库外包不写节点" "$BEFORE" "$(cat $NODE)"
echo "  --- 5c. 加密/未知形态"
run aaa.opaque.test
check "未知形态不写节点" "$BEFORE" "$(cat $NODE)"

echo
echo "=== 6. 游戏退出 → 恢复启动时的原值 ==="
echo -1 > $GAMEPID
sleep 7
check "退出后恢复原值" "UnityMain:-1,UnityGfxDevice:-1" "$(cat $NODE)"

echo
echo "=== 7. dmesg：内核侧只该有真写过的那些 ==="
UPDATES=$(dmesg | grep -c "名单更新")
check "写入次数（4 次写入 + 1 次恢复）" "5" "$UPDATES"

echo
echo "=== 8. 决策日志 ==="
grep -E "不写节点|已写入" "$LOG" | sed 's/^/    /'

echo
echo "=================================="
echo "结果: 通过 $ok 项，失败 $fail 项"
echo "=================================="
[ $fail -eq 0 ]
