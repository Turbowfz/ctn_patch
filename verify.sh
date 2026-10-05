#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ctn_patch 自检：内核模块 + daemon + 环境，一趟跑完
#
# 输出约定（为了一眼能看出问题）：
#   [OK]  一行一项，通过了
#   [!!]  失败 —— 失败时才在下面缩进打印细节
#   [--]  跳过（没条件测）
#   最后一行是汇总。脚本退出码 = 失败项数（0 表示全过）。
#
# 用法：
#   sh verify.sh                  # ko 取脚本同目录的 ctn_patch.ko
#   sh verify.sh /path/to/ko      # 指定 ko
#
# 模块生命周期由本脚本负责：自己 insmod、测完自己 rmmod。
# 所以调它之前请确保模块没被加载（action.sh 会先 rmmod）。

DIR=/proc/game_opt/task_boost
NODE=$DIR/critical_task_name
VICTIM=oplus_bsp_game_opt
MODDIR=${0%/*}
KO="${1:-$MODDIR/ctn_patch.ko}"

PASS=0; FAIL=0; SKIP=0

# 标签手工补齐到显示宽 12（中文算 2 列，printf 的 %-12s 按字符数算，用不了）
row() { printf '[%s]  %s  %s\n' "$1" "$2" "$3"; }
ok()  { PASS=$((PASS+1)); row OK "$1" "$2"; }
ng()  { FAIL=$((FAIL+1)); row '!!' "$1" "$2"; }
sk()  { SKIP=$((SKIP+1)); row '--' "$1" "$2"; }
det() { printf '        %s\n' "$*"; }

# 写一个值、读回核对。不一致时**立刻重读**再判 —— 实测遇到过一次
# 「读回好几轮之前的旧值」（模块刚重载 + 连续写的场景；之后 300 轮压测复现不出来）。
# 重读能区分两种情况，而且两种都如实报出来：
#   瞬时旧值 → 重读就对了，记进 STALE，最后以备注行提示（不算失败）
#   真写坏了 → 怎么读都不对，返回 1，由调用方报失败
STALE=""
STALEN=0
wread() { # wread <要写的> <期望读回> [轮次说明]
	echo "$1" > "$NODE" 2>/dev/null
	got=$(cat "$NODE")
	[ "$got" = "$2" ] && return 0
	sleep 0.2; g2=$(cat "$NODE")
	sleep 0.2; g3=$(cat "$NODE")
	if [ "$g2" = "$2" ] || [ "$g3" = "$2" ]; then
		STALEN=$((STALEN+1))
		STALE="$STALE${STALE:+；}${3:-写 $1} 首次读到 [$got]"
		return 0
	fi
	return 1
}

# 打一个内核日志标记：第 11 项只扫标记之后的日志，免得被无关模块的
# WARNING 误伤（实测踩过：抓到 10 分钟前一条无关的 warn_alloc）。
KLOG_MARK="CTN_VERIFY_$$_$(date +%s)"
echo "$KLOG_MARK START" > /dev/kmsg 2>/dev/null

# ---------------------------------------------------------------- 环境
VER=$(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null)
echo "ctn_patch 自检${VER:+  $VER}"
echo "  机型 $(getprop ro.product.model 2>/dev/null) / $(getprop ro.board.platform 2>/dev/null)" \
     "｜ 内核 $(uname -r) ｜ 页 $(getconf PAGE_SIZE 2>/dev/null || echo ?) ｜ SELinux $(getenforce 2>/dev/null)"
echo

if [ "$(id -u)" != "0" ]; then echo "必须以 root 运行"; exit 1; fi
if ! grep -q "^$VICTIM " /proc/modules; then
	echo "$VICTIM 未加载，本模块无意义"
	exit 1
fi

# ---------------------------------------------------------------- 1 模块文件
# ctnd 的可执行位必须显式检查：解包器可能丢掉它（实测踩过，装完 daemon 起不来）
F_MISS=""; F_BAD=""
[ -f "$KO" ] || F_MISS="$F_MISS ctn_patch.ko"
[ -s "$MODDIR/ctnd" ] || F_MISS="$F_MISS ctnd"
[ -f "$MODDIR/ctnd" ] && [ ! -x "$MODDIR/ctnd" ] && F_BAD="$F_BAD ctnd(无可执行位)"
[ -f "$MODDIR/expected_vendor.txt" ] || F_MISS="$F_MISS expected_vendor.txt"
if [ -n "$F_MISS" ]; then
	ng "模块文件    " "缺:$F_MISS"
elif [ -n "$F_BAD" ]; then
	ng "模块文件    " "权限不对:$F_BAD（chmod 755 或重装）"
else
	ok "模块文件    " "ko $(stat -c%s "$KO")B ｜ ctnd $(stat -c%s "$MODDIR/ctnd")B 0755 $(grep -qa ELF "$MODDIR/ctnd" && echo ELF)"
fi

# ---------------------------------------------------------------- 2 厂商模块
# struct module 布局与厂商 ko 绑定，换了就必须重编（曾因此崩机）
VKO=""
for p in /vendor/lib/modules/$VICTIM.ko /system/lib/modules/$VICTIM.ko \
         /vendor_dlkm/lib/modules/$VICTIM.ko; do
	[ -f "$p" ] && { VKO="$p"; break; }
done
WANT=$(sed -n 's/^vendor_ko_sha256=//p' "$MODDIR/expected_vendor.txt" 2>/dev/null | head -n1)
if [ -z "$VKO" ]; then
	sk "厂商模块    " "找不到设备上的 $VICTIM.ko，跳过比对"
elif [ -z "$WANT" ]; then
	sk "厂商模块    " "zip 里没有 expected_vendor.txt，跳过比对"
else
	GOT=$(sha256sum "$VKO" 2>/dev/null | cut -d' ' -f1)
	if [ "$WANT" = "$GOT" ]; then
		ok "厂商模块    " "sha256 与构建时一致"
	else
		ng "厂商模块    " "与构建时不是同一份 → 结构体布局可能不匹配，会崩机"
		det "构建时 $WANT"
		det "设备上 $GOT"
	fi
fi

# ---------------------------------------------------------------- 3 vermagic
KO_VM=$(grep -aom1 'vermagic=[ -~]*' "$KO" 2>/dev/null | cut -d= -f2)
K_REL=$(uname -r)
if [ -z "$KO_VM" ]; then
	ng "vermagic    " "读不到 ko 的 vermagic"
else
	KO4=$(echo "$KO_VM" | cut -d' ' -f1 | awk -F- '{print $1"-"$2"-"$3"-"$4}')
	K4=$(echo "$K_REL"  | awk -F- '{print $1"-"$2"-"$3"-"$4}')
	if [ "$KO4" = "$K4" ]; then
		ok "vermagic    " "$KO4"
	else
		ng "vermagic    " "ko [$KO4] 与内核 [$K4] 不一致，insmod 大概率失败"
	fi
fi

# ---------------------------------------------------------------- 4 insmod
say_loaded=0
if grep -q '^ctn_patch ' /proc/modules; then
	# 已经加载着：没法测加载/卸载，但要能测节点行为
	sk "insmod      " "ctn_patch 已在运行，本次跳过加载/卸载测试"
	say_loaded=1
else
	R0=$(sed -n "s/^$VICTIM [0-9]* \([0-9]*\).*/\1/p" /proc/modules)
	if insmod "$KO" 2>/dev/null; then
		R1=$(sed -n "s/^$VICTIM [0-9]* \([0-9]*\).*/\1/p" /proc/modules)
		if [ "${R1:-0}" -gt "${R0:-0}" ] 2>/dev/null; then
			ok "insmod      " "成功，$VICTIM refcount $R0→$R1（已钉住）"
		else
			ng "insmod      " "成功但 refcount 没涨（$R0→$R1），钉住可能没生效"
		fi
	else
		ng "insmod      " "失败"
		dmesg | tail -n 12 | while read -r l; do det "$l"; done
	fi
fi

# ---------------------------------------------------------------- 5 节点
if [ -e "$NODE" ]; then
	ok "节点        " "$(ls -l "$NODE" | awk '{print $1, $3, $4}')"
else
	ng "节点        " "不存在（模块没加载成功？）"
fi

# ---------------------------------------------------------------- 6 读格式
V=$(cat "$NODE" 2>/dev/null)
case "$V" in
	*:*","*:*) ok "读格式      " "[$V]" ;;
	"")        ng "读格式      " "读不到内容" ;;
	*)         ng "读格式      " "不是 名字:pid,名字:pid 格式：[$V]" ;;
esac

# ---------------------------------------------------------------- 7 写入读回
if wread "CTNTestMain CTNTestGfx" "CTNTestMain:-1,CTNTestGfx:-1" "写入读回"; then
	ok "写入读回    " "正确"
else
	ng "写入读回    " "写进去和读回来不一致（重读也对不上）"
	det "读回 [$(cat "$NODE")]"
fi

# ---------------------------------------------------------------- 8 反复写
RCU_BAD=""
i=1
while [ $i -le 8 ]; do
	wread "A${i}Main A${i}Gfx" "A${i}Main:-1,A${i}Gfx:-1" "反复写第 $i 次" || RCU_BAD="$RCU_BAD $i"
	i=$((i+1))
done
if [ -z "$RCU_BAD" ]; then ok "反复写      " "8/8 正确（双缓冲/RCU 路径）"; else ng "反复写      " "第$RCU_BAD 次不一致"; fi

# ---------------------------------------------------------------- 9 边界输入
E=""
echo "onlyone" > "$NODE" 2>/dev/null && E="$E 单名被接受(应拒)"
echo "A B C"   > "$NODE" 2>/dev/null && E="$E 三名被接受(应拒)"
echo "ABCDEFGHIJKLMNO0 ABCDEFGHIJKLMNO1" > "$NODE" 2>/dev/null || E="$E 16字符被拒(应接受)"
LONG=$(printf 'x%.0s' $(seq 1 120) 2>/dev/null)
echo "$LONG test" > "$NODE" 2>/dev/null && E="$E 120字符被接受(应拒)"
if [ -z "$E" ]; then
	ok "边界输入    " "单名/三名/120字符 被拒，16字符 接受"
else
	ng "边界输入    " "不符合预期:$E"
fi

# ---------------------------------------------------------------- 10 恢复
if wread "UnityMain UnityGfxDevice" "UnityMain:-1,UnityGfxDevice:-1" "恢复默认值"; then
	ok "恢复默认值  " "UnityMain/UnityGfxDevice"
else
	ng "恢复默认值  " "恢复失败（重读也对不上）：[$(cat "$NODE")]"
fi

# ---------------------------------------------------------------- 11 dmesg
# 只取本次测试期间的日志。标记必须出现，否则说明 /dev/kmsg 写不进去 ——
# 那种情况绝不能报 OK（等于什么都没查却说没问题），宁可报失败让人看见。
LOG=$(dmesg | sed -n "/$KLOG_MARK START/,\$p")
if [ -z "$LOG" ]; then
	ng "dmesg 异常  " "取不到本次日志段（标记不在 dmesg 里），未扫描"
	det "确认：dmesg | grep CTN_VERIFY_"
else
	BAD=$(echo "$LOG" | grep -iE "BUG:|WARNING:|Unable to handle|Internal error|Call trace|Oops|CFI failure" \
	      | grep -vE "CTN_VERIFY_" | tail -n 5)
	if [ -z "$BAD" ]; then
		ok "dmesg 异常  " "本次无 BUG/WARNING/oops/CFI failure"
	else
		ng "dmesg 异常  " "本次测试期间有异常"
		echo "$BAD" | while read -r l; do det "$l"; done
	fi
fi
echo "$KLOG_MARK END" > /dev/kmsg 2>/dev/null

# ---------------------------------------------------------------- 12 卸载
if [ "$say_loaded" = "1" ]; then
	sk "卸载        " "本次没由脚本加载，跳过"
elif rmmod ctn_patch 2>/dev/null; then
	if [ -e "$NODE" ]; then
		ng "卸载        " "rmmod 成功但节点还在"
	else
		ok "卸载        " "rmmod 后节点已消失，refcount 还原"
	fi
else
	ng "卸载        " "rmmod 失败（有进程正拿着节点？）"
fi

# ------------------------------------------------- 恢复运行状态（后面要查 daemon）
# 自检不能把设备留在「模块没加载 / daemon 没跑」的状态，而且 daemon 那几项
# 必须它真的在跑才有意义 —— 所以这里先把它拉起来。这几行不算检查项。
echo "  --- 恢复运行状态 ---"
if ! grep -q '^ctn_patch ' /proc/modules; then
	insmod "$KO" 2>/dev/null && echo "    模块已重新加载" || echo "    !! 模块重新加载失败，建议重启"
fi
if ! pgrep -x ctnd >/dev/null 2>&1; then
	if [ -x "$MODDIR/ctnd" ]; then
		setsid "$MODDIR/ctnd" >> "$MODDIR/daemon.log" 2>&1 < /dev/null &
		sleep 2
		echo "    daemon 已拉起"
	else
		echo "    !! 找不到 $MODDIR/ctnd"
	fi
fi

# ---------------------------------------------------------------- 13 daemon
NP=$(pgrep -x ctnd 2>/dev/null | wc -l)
if [ "$NP" = "1" ]; then
	DV=$("$MODDIR/ctnd" --version 2>/dev/null | awk '{print $2}')
	ok "daemon      " "ctnd ${DV:-?} 运行中（pid $(pgrep -x ctnd | head -1)）"
elif [ "$NP" = "0" ]; then
	ng "daemon      " "ctnd 没在运行 —— 节点不会被自动写入"
else
	ng "daemon      " "有 $NP 个 ctnd 实例，应只允许 1 个（互抢写节点）"
fi

# ---------------------------------------------------------------- 14 daemon 依赖
D=""
for p in /system/lib64/libsqlite.so /system/lib64/libsqlite3.so \
         /apex/com.android.runtime/lib64/libsqlite3.so; do
	[ -r "$p" ] && { D="$D sqlite=ok"; break; }
done
case "$D" in *sqlite*) ;; *) D="$D sqlite=缺" ;; esac
[ -r /data/user/0/com.oplus.cosa/databases/db_game_database ] && D="$D cosa库=ok" || D="$D cosa库=缺"
[ -r /data/adb/ctn_patch/ctn.conf ] && D="$D ctn.conf=有" || D="$D ctn.conf=无(可自动铺)"
case "$D" in
	*"sqlite=缺"*|*"cosa库=缺"*) ng "daemon 依赖 " "$D" ;;
	*) ok "daemon 依赖 " "$D" ;;
esac

# ---------------------------------------------------------------- 15 daemon 日志
DLOG="$MODDIR/daemon.log"
if [ ! -f "$DLOG" ]; then
	sk "daemon 日志 " "还没有 $DLOG"
else
	# 只找明确的失败标记，不做模糊匹配（避免误报）
	BADL=$(tail -n 200 "$DLOG" 2>/dev/null \
	       | grep -E "加载失败|写入失败|缺少符号|找不到可读的云控库|已有另一个" | tail -n 3)
	if [ -z "$BADL" ]; then
		ok "daemon 日志 " "近 200 行无失败记录"
	else
		ng "daemon 日志 " "有失败记录"
		echo "$BADL" | while read -r l; do det "$l"; done
	fi
fi

# ---------------------------------------------------------------- 汇总
if [ "$STALEN" != "0" ]; then
	sk "瞬时旧值    " "有 $STALEN 次首次读回旧值（重读即正常，不算失败）"
	det "$STALE"
fi
echo "------------------------------------------------"
if [ "$FAIL" = "0" ]; then
	echo "结果: $PASS 项全过${SKIP:+（$SKIP 项跳过/备注）}"
else
	echo "结果: $PASS 过 / $FAIL 失败${SKIP:+ / $SKIP 跳过}   ← 看上面标 [!!] 的行"
fi
[ "$FAIL" = "0" ]
