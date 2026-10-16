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
#   adb push daemon/ctnd daemon/fakepkg daemon/device_e2e.sh daemon/synth.db \
#        daemon/synth2.db daemon/synth_removed.db ctn_patch.ko /data/local/tmp/
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
	# 5f 会把库位置换成目录；中途失败时得先清掉再还原
	[ -f "$DB.bak" ] && { rm -rf "$DB" 2>/dev/null; cp "$DB.bak" "$DB" 2>/dev/null; }
}
trap cleanup EXIT

echo "=== 0. 准备 ==="
for f in "$CTND" "$FAKE" "$DB" "$KO" "$DB.2" "$DB.r"; do
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
chmod 755 "$CTND" "$FAKE"   # adb push（Windows 侧）不带 exec 位   # adb push（Windows 侧）不带 exec 位
dmesg -c >/dev/null 2>&1
cp "$DB" "$DB.bak"	# 5d/5e 会覆盖库，备份以便复跑
: > "$LOG"

"$CTND" --db "$DB" -r 5 >> "$LOG" 2>&1 &
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
# pid 槽是读时现算的真实 pid（v2.2 起数据源可用），不再写死 -1 —— 只校验名字
OUT=$(cat $NODE)
FIRST=${OUT%%,*}; SECOND=${OUT##*,}
if [ "${FIRST%%:*}" = "SoloThread" ] && [ "${SECOND%%:*}" = "SoloThread" ]; then
	echo "  [PASS] 单名 ctn 补成两槽（pid 现算: $OUT）"; ok=$((ok+1))
else
	echo "  [FAIL] 单名 ctn 补成两槽"; echo "         期望: SoloThread:<pid>,SoloThread:<pid>"; echo "         实际: [$OUT]"; fail=$((fail+1))
fi

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
echo "=== 5d. 运行中云控 ctn 变化 → 自动重新应用（v2.1）==="
# 当前"游戏"（fakepkg，pid 没变）还在跑：把云控库换成有效 ctn 的版本，
# 一个复查周期（-r 5 → 5 秒）内应当被自动应用 —— 这就是
# 「db 里的 ctn 参数被修改也要应用」的主路径。
cp /data/local/tmp/synth2.db "$DB"
sleep 12
check "运行中 ctn 变化重新应用" "NewMain:-1,NewRender:-1" "$(cat $NODE)"

echo
echo "=== 5f. 运行中查库失败 → 保持现状，不许恢复原值（v2.1 修正）==="
# 把库换成一个**目录**：find_db 的 access(R_OK) 过得去，但 stage_db 拷不动
# （read 目录报 EISDIR）→ lookup_cfg 返回"查不动"。此时 daemon 必须保持现状：
# 早期版本会把"查不动"当成"云控删了 ctn"，于是把正在加速的节点恢复成默认名，
# 30 秒后下个周期又写回来 —— 来回跳。
mv "$DB" "$DB.save"
mkdir "$DB"
sleep 12
check "查库失败不恢复（节点保持已应用值）" "NewMain:-1,NewRender:-1" "$(cat $NODE)"
grep -q "复查: 查库失败，保持现状" "$LOG" \
	&& { echo "  [PASS] 日志明确说「查库失败，保持现状」"; ok=$((ok+1)); } \
	|| { echo "  [FAIL] 日志里没有「查库失败，保持现状」"; fail=$((fail+1)); }
rmdir "$DB"
mv "$DB.save" "$DB"

echo
echo "=== 5e. 云控里 ctn 被移除 → 恢复原值（v2.1）==="
# 云控把 ctn 整个下掉了：复查应当撤销本次应用、恢复启动时的原值
cp /data/local/tmp/synth_removed.db "$DB"
sleep 12
check "运行中 ctn 移除恢复原值" "UnityMain:-1,UnityGfxDevice:-1" "$(cat $NODE)"
cp "$DB.bak" "$DB"	# 恢复库，下一轮 e2e 才是干净起点

echo
echo "=== 6. 游戏退出 → 恢复启动时的原值 ==="
echo -1 > $GAMEPID
sleep 7
check "退出后恢复原值" "UnityMain:-1,UnityGfxDevice:-1" "$(cat $NODE)"

echo
echo "=== 7. dmesg：内核侧只该有真写过的那些 ==="
UPDATES=$(dmesg | grep -c "名单更新")
# 6 条 = 5 次写入（用例 1-4 + 5d）+ 1 次恢复（5e）；
# 用例 6 的退出恢复在 5e 已复位 g_patched 后不再重复写 —— 这是设计行为
check "写入次数（5 次写入 + 1 次恢复）" "6" "$UPDATES"

echo
echo "=== 8. 决策日志 ==="
grep -E "不写节点|已写入" "$LOG" | sed 's/^/    /'

echo
echo "=================================="
echo "结果: 通过 $ok 项，失败 $fail 项"
echo "=================================="
[ $fail -eq 0 ]
