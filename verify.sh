#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ctn_patch 加载 + 验证脚本（在手机上以 root 运行）
#
# 用法：
#   adb push ctn_patch.ko /data/local/tmp/
#   adb push verify.sh /data/local/tmp/
#   adb shell su -c 'sh /data/local/tmp/verify.sh'
# 也可带参数指定 .ko 路径（Magisk 模块里的 action.sh 就这么调用）：
#   sh verify.sh /data/adb/modules/ctn_patch/ctn_patch.ko
#
# 只读检查 + 临时写测试（结束前恢复原值），每一步都会打印 PASS / FAIL。
# 读取格式与官方 6.6 一致：名字:pid,名字:pid（6.1 无 pid 数据，固定 -1）。
# 注意：Android 的 toybox 没有 modinfo，vermagic 用 grep 直接从 .ko 里抽。

set -u

KO="${1:-/data/local/tmp/ctn_patch.ko}"
NODE=/proc/game_opt/task_boost/critical_task_name
DIR=/proc/game_opt/task_boost
PASS=0
FAIL=0

say() { echo "$@"; }
ok()  { PASS=$((PASS+1)); echo "  [PASS] $1"; }
ng()  { FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

# 打一个内核日志标记：后面只截取标记之后的日志来判异常。
# 不这么做的话，会把 msm_vidc / binder_debug 这类跟本模块无关的
# WARNING 也算成失败（实测踩过，白报一个 FAIL）。
KLOG_MARK="CTN_VERIFY_$$_$(date +%s)"
echo "$KLOG_MARK START" > /dev/kmsg 2>/dev/null

say "=== 0. 前置检查 ==="
if [ "$(id -u)" != "0" ]; then
	echo "  必须以 root 运行"; exit 1
fi
ok "root"

if ! grep -q '^oplus_bsp_game_opt ' /proc/modules; then
	echo "  oplus_bsp_game_opt 未加载，本模块无意义"; exit 1
fi
ok "oplus_bsp_game_opt 已加载"

say ""
say "=== 1. 加载前状态 ==="
if [ -e "$NODE" ]; then
	say "  注意：节点已存在，说明这个内核本来就有 ctn 节点，或已加载过别的补丁"
	say "  当前值: $(cat $NODE)"
	ALREADY=1
else
	ok "加载前 /proc/game_opt/task_boost 无 critical_task_name（符合预期）"
	ALREADY=0
fi
say "  加载前 task_boost 节点:"
ls "$DIR" 2>/dev/null | sed 's/^/    /'

if [ ! -f "$KO" ]; then
	echo ""; echo "  找不到 $KO，请先 push ctn_patch.ko"; exit 1
fi

say ""
say "=== 2. vermagic 比对 ==="
KO_VM=$(grep -aom1 'vermagic=[ -~]*' "$KO" 2>/dev/null | cut -d= -f2)
K_VER=$(uname -r)
say "  模块 vermagic: ${KO_VM:-（读不到）}"
say "  内核版本    : $K_VER"
if [ -n "$KO_VM" ]; then
	KO_REL=$(echo "$KO_VM" | cut -d' ' -f1)
	KO_REL4=$(echo "$KO_REL" | awk -F- '{print $1"-"$2"-"$3"-"$4}')
	K_REL4=$(echo "$K_VER"  | awk -F- '{print $1"-"$2"-"$3"-"$4}')
	if [ "$KO_REL4" = "$K_REL4" ]; then
		ok "版本前缀一致（git hash 尾差 GKI 容忍）"
	else
		ng "ko 版本 [$KO_REL4] 与内核 [$K_REL4] 不一致，insmod 大概率失败"
	fi
else
	ng "读不到模块 vermagic"
fi

say ""
say "=== 3. insmod ==="
if [ "$ALREADY" = "1" ]; then
	say "  跳过 insmod（节点已存在）"
else
	OLD_REF=$(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)
	insmod "$KO"
	if [ $? -ne 0 ]; then
		ng "insmod 失败"
		dmesg | tail -n 20 | sed 's/^/    /'
		echo ""; echo "结果: PASS=$PASS FAIL=$FAIL"; exit 1
	fi
	ok "insmod 成功"
	NEW_REF=$(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)
	say "  oplus_bsp_game_opt refcount: $OLD_REF -> $NEW_REF (期望 +1，说明已被钉住)"
	if [ "$NEW_REF" -gt "$OLD_REF" ] 2>/dev/null; then
		ok "目标模块 refcount 增加"
	else
		ng "refcount 没增加，模块钉住可能没生效"
	fi
fi

say ""
say "=== 4. 节点存在性 ==="
if [ -e "$NODE" ]; then
	ok "节点已创建"
else
	ng "节点不存在"
	echo ""; echo "结果: PASS=$PASS FAIL=$FAIL"; exit 1
fi
say "  权限: $(ls -l $NODE | awk '{print $1, $3, $4}')"
say "  task_boost 节点列表:"
ls "$DIR" | sed 's/^/    /'

say ""
say "=== 5. 默认值（6.6 格式）==="
V=$(cat "$NODE")
say "  读到: [$V]"
case "$V" in
	"UnityMain:-1,UnityGfxDevice:-1") ok "默认值与格式和官方 6.6 初始状态一致" ;;
	*) say "  （不是默认值，可能之前被改过；只要格式是 名字:pid,名字:pid 就正常）" ;;
esac

say ""
say "=== 6. 任意填两个名字 / 读回 ==="
ORIG="$V"
if echo "CTNTestMain CTNTestGfx" > "$NODE" 2>/dev/null; then
	V2=$(cat "$NODE")
	if [ "$V2" = "CTNTestMain:-1,CTNTestGfx:-1" ]; then
		ok "写入任意两个名字后能正确读回（6.6 格式）"
	else
		ng "读回不一致: [$V2]"
	fi
else
	ng "写入失败"
fi

say ""
say "=== 7. 反复写（压双缓冲/RCU）==="
RCU_OK=1
i=1
while [ $i -le 8 ]; do
	echo "A${i}Main A${i}Gfx" > "$NODE" 2>/dev/null
	R=$(cat "$NODE")
	[ "$R" = "A${i}Main:-1,A${i}Gfx:-1" ] || { RCU_OK=0; say "  第 $i 次不一致: [$R]"; }
	i=$((i+1))
done
if [ "$RCU_OK" = "1" ]; then ok "连续 8 次写入/读回全部正确"; else ng "有写入读回不一致"; fi

say ""
say "=== 8. 边界与非法输入 ==="
# 8.1 只给一个名字应被拒（官方 sscanf 要求恰好 2 个）
if echo "onlyone" > "$NODE" 2>/dev/null; then
	ng "只给一个名字竟然被接受"
else
	ok "只给一个名字被拒绝（正确）"
fi
# 8.2 给三个名字应被拒（多了第三段）
if echo "A B C" > "$NODE" 2>/dev/null; then
	ng "三个名字竟然被接受"
else
	ok "三个名字被拒绝（正确）"
fi
# 8.3 接口层允许到 99 字符（与官方 %99s 一致），16 字符应当被接受
if echo "ABCDEFGHIJKLMNO0 ABCDEFGHIJKLMNO1" > "$NODE" 2>/dev/null; then
	ok "16 字符名字被接受（与 6.6 接口一致）"
else
	ng "16 字符名字被拒绝（接口层应允许到 99 字符）"
fi
# 8.4 超过 99 字符应被拒
LONG=$(printf 'x%.0s' $(seq 1 120))
if echo "$LONG test" > "$NODE" 2>/dev/null; then
	ng "120 字符名字竟然被接受"
else
	ok "120 字符名字被拒绝（正确）"
fi

say ""
say "=== 9. 恢复原值 ==="
echo "UnityMain UnityGfxDevice" > "$NODE" 2>/dev/null
V3=$(cat "$NODE")
if [ "$V3" = "UnityMain:-1,UnityGfxDevice:-1" ]; then
	ok "已恢复为 UnityMain/UnityGfxDevice"
else
	ng "恢复失败: [$V3]"
fi

say ""
say "=== 10. 内核日志 ==="
# 只取本次测试期间（标记之后）的日志，避免被无关模块的 WARNING 误伤
LOG=$(dmesg | sed -n "/$KLOG_MARK START/,\$p")
say "  --- ctn_patch 相关 ---"
echo "$LOG" | grep -i ctn_patch | tail -n 10 | sed 's/^/    /'
say "  --- 异常扫描（期望为空）---"
# 标记必须出现在 dmesg 里，否则说明 /dev/kmsg 写不进去（或日志被清了）。
# 这种情况下**绝不能报 PASS** —— 那等于「什么都没查却说没问题」，
# 比不查更糟：用户会以为自检通过了。宁可报一条 FAIL 让人看见。
if [ -z "$LOG" ]; then
	ng "取不到本次测试的日志段（标记 $KLOG_MARK 不在 dmesg 里），异常扫描未执行"
	echo "     /dev/kmsg 写不进去？手动确认：dmesg | grep CTN_VERIFY_" | sed 's/^/  /'
else
	BAD=$(echo "$LOG" | grep -iE "BUG:|WARNING:|Unable to handle|Internal error|Call trace|Oops|CFI failure" 	| grep -vE "CTN_VERIFY_" | tail -n 10)
	if [ -z "$BAD" ]; then
		ok "本次测试期间 dmesg 无 BUG/WARNING/oops/CFI failure"
	else
		ng "dmesg 有异常"
		echo "$BAD" | sed 's/^/    /'
	fi
fi
echo "$KLOG_MARK END" > /dev/kmsg 2>/dev/null

say ""
say "=== 11. 卸载 ==="
if [ "$ALREADY" = "1" ]; then
	say "  跳过 rmmod（本来就没由本脚本加载）"
else
	rmmod ctn_patch
	if [ $? -eq 0 ]; then
		ok "rmmod 成功"
		if [ -e "$NODE" ]; then ng "卸载后节点还在"; else ok "卸载后节点已消失"; fi
		V4=$(cat /proc/game_opt/task_boost/ct_enable 2>/dev/null)
		say "  剩余节点检查 ct_enable=[${V4}]（应还能读，说明原模块完好）"
	else
		ng "rmmod 失败"
		dmesg | tail -n 10 | sed 's/^/    /'
	fi
fi

say ""
say "=================================="
say "结果: PASS=$PASS  FAIL=$FAIL"
say "=================================="
[ "$FAIL" = "0" ] && exit 0 || exit 1
