#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ================= ctn_patch 安装脚本（Magisk / KernelSU / APatch 通用）=================
#
# Magisk 默认（SKIPUNZIP=0）会把 zip 里所有文件解到 $MODPATH，所以到这一步
# ctn_patch.ko / service.sh / verify.sh 都已经在模块目录里了。
#
# 本脚本的核心职责是「兼容性闸门」：把 .ko 真正依赖的内核条件逐条验一遍，
# 不满足就拒绝安装，而不是等重启后 insmod 失败才发现。
# 这些条件全部来自设备实测（见 04_文档/6.1补全ctn可写节点模块.md 第八节）。
#
# 判定分三级：
#   FAIL  必定装不上/会冲突 → abort，不安装
#   WARN  能装但功能打折   → 继续，刷完提示
#   INFO  只作记录         → 继续

ui_print "=============================="
ui_print " ctn_patch 安装中"
ui_print " 把 6.6 的 critical_task_name"
ui_print " 可写节点搬回 6.1 内核"
ui_print " （作者 Turbo）"
ui_print "=============================="

FAIL=0
WARN=0
INFO=0

# Magisk 在 customize.sh 里给的是 MODPATH；个别环境只有 MODDIR，兜一下
[ -n "$MODPATH" ] || MODPATH="$MODDIR"
[ -n "$MODPATH" ] || abort "拿不到模块目录（MODPATH/MODDIR 都为空）"

# ---------- 小工具 ----------
say()  { ui_print "$*"; }
pass() { ui_print "  [OK]   $*"; }
warn() { WARN=$((WARN+1)); ui_print "  [警告] $*"; }
info() { INFO=$((INFO+1)); ui_print "  [备注] $*"; }
bad()  { FAIL=$((FAIL+1)); ui_print "  [不满足] $*"; }

# 解 gz：优先 zcat，退回 gzip -dc
gunzip_stdout() {
	if command -v zcat >/dev/null 2>&1; then
		zcat "$1" 2>/dev/null && return 0
	fi
	if command -v gzip >/dev/null 2>&1; then
		gzip -dc "$1" 2>/dev/null && return 0
	fi
	return 1
}

# 读一个内核配置项的值（未设置返回空）
cfg() {
	if [ "$HAVE_CONFIG" = "1" ]; then
		echo "$CONFIG_TXT" | sed -n "s/^$1=//p" | head -n1
	fi
}
cfg_is() { [ "$(cfg "$1")" = "$2" ]; }
cfg_on() { [ "$(cfg "$1")" = "y" ] || [ "$(cfg "$1")" = "m" ]; }

# ---------- 0. 读内核配置 ----------
CONFIG_TXT=""
HAVE_CONFIG=0
if [ -r /proc/config.gz ]; then
	CONFIG_TXT="$(gunzip_stdout /proc/config.gz)"
elif [ -r /proc/config ]; then
	CONFIG_TXT="$(cat /proc/config 2>/dev/null)"
fi
[ -n "$CONFIG_TXT" ] && HAVE_CONFIG=1

say "--- 环境"
say "  机型: $(getprop ro.product.model 2>/dev/null) / 平台: $(getprop ro.board.platform 2>/dev/null)"
say "  内核: $(uname -r)"
say "  SELinux: $(getenforce 2>/dev/null)"
if [ "$HAVE_CONFIG" = "1" ]; then
	pass "读到内核配置（/proc/config.gz，$(echo "$CONFIG_TXT" | wc -l) 行）"
else
	warn "读不到 /proc/config.gz，跳过配置项检查（只做运行时检查）"
fi

# ================= 1. 内核版本 =================
say "--- 1. 内核版本"
EXPECT="6.1.141-android14-11"
UREL="$(uname -r)"
case "$UREL" in
	"$EXPECT"*)
		pass "内核 $UREL 属于 $EXPECT 系列"
		;;
	*)
		bad "内核是 $UREL，不是 $EXPECT 系列 —— .ko 不是为它编的"
		;;
esac

# ================= 2. .ko 文件与 vermagic =================
say "--- 2. 模块文件"
if [ ! -f "$MODPATH/ctn_patch.ko" ]; then
	bad "zip 里没有 ctn_patch.ko（先按 README 第五节编译，再 build_zip.py 打包）"
else
	pass "ctn_patch.ko 存在（$(stat -c%s "$MODPATH/ctn_patch.ko" 2>/dev/null) 字节）"
	# toybox 没有 modinfo，直接从 .ko 里抽 vermagic
	KO_VM="$(grep -aom1 'vermagic=[ -~]*' "$MODPATH/ctn_patch.ko" 2>/dev/null | cut -d= -f2)"
	if [ -z "$KO_VM" ]; then
		bad "读不到 .ko 的 vermagic（文件可能损坏或不是内核模块）"
	else
		KO_REL4="$(echo "$KO_VM" | cut -d' ' -f1 | awk -F- '{print $1"-"$2"-"$3"-"$4}')"
		K_REL4="$(echo "$UREL" | awk -F- '{print $1"-"$2"-"$3"-"$4}')"
		if [ "$KO_REL4" = "$K_REL4" ]; then
			pass "vermagic 版本前缀一致（$KO_REL4）"
		else
			bad "vermagic 不匹配：ko=[$KO_REL4] 内核=[$K_REL4]"
		fi
		case "$KO_VM" in
			*" SMP preempt mod_unload modversions aarch64") ;;
			*) warn "vermagic 尾部标志与设备标准串不同：$KO_VM" ;;
		esac
	fi
fi

# ================= 3. 目标模块必须在且是「模块」 =================
say "--- 3. oplus_bsp_game_opt"
if grep -q '^oplus_bsp_game_opt ' /proc/modules 2>/dev/null; then
	pass "oplus_bsp_game_opt 已加载（是模块，可解析符号）"
	REF="$(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)"
	info "当前 refcount=$REF（装上后会 +1，这是有意钉住防野指针）"
else
	if grep -q '^oplus_bsp_game_opt$' /proc/modules 2>/dev/null; then
		pass "oplus_bsp_game_opt 已加载"
	else
		bad "oplus_bsp_game_opt 不在 /proc/modules —— 若是内建进内核则本模块不支持"
	fi
fi

# 目标 proc 目录必须在（我们要往里挂节点）
if [ -d /proc/game_opt/task_boost ]; then
	pass "/proc/game_opt/task_boost 目录存在"
else
	bad "/proc/game_opt/task_boost 不存在（game_opt 没起来或结构不同）"
fi

# ================= 4. 厂商模块版本（对应上次崩机的根因）=================
say "--- 4. 厂商模块版本比对"
VKO=""
for p in /vendor/lib/modules/oplus_bsp_game_opt.ko \
         /system/lib/modules/oplus_bsp_game_opt.ko \
         /vendor_dlkm/lib/modules/oplus_bsp_game_opt.ko; do
	[ -f "$p" ] && { VKO="$p"; break; }
done
if [ -n "$VKO" ] && [ -f "$MODPATH/expected_vendor.txt" ]; then
	WANT=$(sed -n 's/^vendor_ko_sha256=//p' "$MODPATH/expected_vendor.txt" | head -n1)
	GOT=$(sha256sum "$VKO" 2>/dev/null | cut -d' ' -f1)
	if [ -n "$WANT" ] && [ "$WANT" = "$GOT" ]; then
		pass "oplus_bsp_game_opt.ko 与构建时对照的一致"
	else
		bad "厂商模块与构建时对照的不是同一份！"
		say "        构建时: ${WANT:-未知}"
		say "        设备上: ${GOT:-读不到}"
		say "        这会导致 struct module 布局不匹配 → insmod 时崩机。请按本机重编。"
	fi
elif [ -z "$VKO" ]; then
	warn "找不到设备上的 oplus_bsp_game_opt.ko 文件，跳过版本比对"
else
	warn "zip 里没有 expected_vendor.txt，跳过版本比对"
fi

# ================= 5. 是否已经有了这个节点（6.6 内核 / 重复安装）=================
say "--- 5. 是否已存在 critical_task_name"
if [ -e /proc/game_opt/task_boost/critical_task_name ]; then
	bad "该节点已存在 —— 内核自带（6.6 那种）或已装过同类补丁，重复安装无意义"
	info "当前值：$(cat /proc/game_opt/task_boost/critical_task_name 2>/dev/null)"
else
	pass "节点不存在，安装后由本模块提供"
fi

# ================= 5.5 ctnd 与它依赖的 SQLite =================
say "--- 5.5 ctnd（自动写入节点必需）"
if [ -f "$MODPATH/ctnd" ]; then
	pass "ctnd 存在（$(stat -c%s "$MODPATH/ctnd" 2>/dev/null) 字节）"
	# 架构对不上会在启动时才炸，这里先看一眼
	if grep -qa "ELF" "$MODPATH/ctnd" 2>/dev/null; then
		pass "ctnd 是 ELF 可执行文件"
	else
		bad "ctnd 不是有效的 ELF（zip 坏了？）"
	fi
else
	bad "zip 里没有 ctnd —— 没有它节点永远停在默认值，装了也白装"
fi

# ctnd 靠 dlopen 读 COSA 的 SQLite 库拿云控配置，库不在就没法自动填名字
SQLITE=""
for p in /system/lib64/libsqlite.so /system/lib64/libsqlite3.so          /apex/com.android.runtime/lib64/libsqlite3.so          /apex/com.android.art/lib64/libsqlite3.so; do
	[ -f "$p" ] && { SQLITE="$p"; break; }
done
if [ -n "$SQLITE" ]; then
	pass "找到 SQLite 库：$SQLITE"
else
	warn "找不到 SQLite 库，ctnd 读不了云控配置（节点会停在默认值）"
fi

# 云控库（COSA 的 SQLite）——没有它 ctnd 拿不到 ctn
COSADB=""
for p in /data/user/0/com.oplus.cosa/databases/db_game_database          /data/data/com.oplus.cosa/databases/db_game_database; do
	[ -f "$p" ] && { COSADB="$p"; break; }
done
if [ -n "$COSADB" ]; then
	pass "找到云控库：$COSADB"
else
	warn "找不到 COSA 的 db_game_database（游戏没启动过？ctnd 会重试）"
fi

# ================= 6. 配置项检查 =================
say "--- 6. 内核配置"
if [ "$HAVE_CONFIG" != "1" ]; then
	warn "跳过（读不到配置）"
else
	# 硬条件
	if cfg_on CONFIG_KPROBES; then
		pass "CONFIG_KPROBES=y（取 kallsyms_lookup_name 靠它）"
	else
		bad "CONFIG_KPROBES 未开 —— 无法解析符号，模块装不上"
	fi
	if cfg_on CONFIG_MODULE_UNLOAD; then
		pass "CONFIG_MODULE_UNLOAD=y（可以 rmmod 卸载）"
	else
		warn "CONFIG_MODULE_UNLOAD 未开 —— 装上后无法卸载，只能重启"
	fi
	if cfg_is CONFIG_MODULE_SIG_FORCE y; then
		bad "CONFIG_MODULE_SIG_FORCE=y —— 未签名模块不允许加载"
	else
		pass "未强制模块签名（未签名 .ko 可以加载）"
	fi
	# 软条件
	if cfg_on CONFIG_KALLSYMS_ALL; then
		pass "CONFIG_KALLSYMS_ALL=y"
	else
		warn "CONFIG_KALLSYMS_ALL 未开 —— 可能取不到 copy_from_kernel_nofault（会退回 memcpy，功能正常）"
	fi
	if cfg_on CONFIG_MODVERSIONS; then
		pass "CONFIG_MODVERSIONS=y（.ko 里的符号 CRC 会被校验）"
	else
		warn "CONFIG_MODVERSIONS 未开 —— 会跳过 CRC 校验（本机应该开着）"
	fi
	if cfg_on CONFIG_CFI_CLANG; then
		pass "CONFIG_CFI_CLANG=y（kCFI 类型号必须与内核一致，本 .ko 已对齐 clang 17.0.2）"
	fi
	if cfg_on CONFIG_STRICT_MODULE_RWX; then
		pass "CONFIG_STRICT_MODULE_RWX=y（.rodata 只读，本模块用 vmap 别名写）"
	fi
fi

# ================= 7. 运行时符号可用性 =================
say "--- 7. 运行时符号"
if [ -r /proc/kallsyms ]; then
	for s in kallsyms_lookup_name find_module register_kprobe; do
		if awk -v n="$s" '$3==n {found=1; exit} END{exit !found}' /proc/kallsyms 2>/dev/null; then
			pass "kallsyms 里有 $s"
		else
			if [ "$s" = "kallsyms_lookup_name" ]; then
				bad "kallsyms 里没有 kallsyms_lookup_name —— 无法解析模块符号"
			else
				warn "kallsyms 里没有 $s（可能被裁掉，加载时会报出来）"
			fi
		fi
	done
	if awk '$3=="copy_from_kernel_nofault" {found=1; exit} END{exit !found}' /proc/kallsyms 2>/dev/null; then
		pass "有 copy_from_kernel_nofault"
	else
		warn "没有 copy_from_kernel_nofault（被 TRIM 裁掉）→ 退回 memcpy 读串，功能不受影响"
	fi
else
	warn "读不到 /proc/kallsyms，跳过符号检查"
fi

# ================= 8. 其它环境 =================
say "--- 8. 其它"
PS="$(getconf PAGE_SIZE 2>/dev/null)"
[ -n "$PS" ] || PS="$(getconf PAGESIZE 2>/dev/null)"
if [ -n "$PS" ]; then
	if [ "$PS" = "4096" ]; then
		pass "页大小 4096（与构建目标一致）"
	else
		info "页大小 $PS（非 4096 也能用，只要指针数组不跨页；加载时会自行判断）"
	fi
fi
ENF="$(getenforce 2>/dev/null)"
case "$ENF" in
	Enforcing) info "SELinux=Enforcing（magisk 域通常有 sys_module 权限；若 insmod 被拦看 boot.log）" ;;
	*) ;;
esac
if [ -e /proc/sys/kernel/kptr_restrict ]; then
	info "kptr_restrict=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null)（只影响地址显示，不影响符号名）"
fi

# ================= 结论 =================
say ""
say "=============================="
if [ "$FAIL" -gt 0 ]; then
	ui_print " 兼容性检查未通过：$FAIL 项不满足"
	ui_print " （警告 $WARN 项，备注 $INFO 项）"
	ui_print " 详见 04_文档/6.1补全ctn可写节点模块.md 第八节"
	ui_print "=============================="
	abort "环境不兼容，已取消安装"
fi
if [ "$WARN" -gt 0 ]; then
	ui_print " 兼容性检查通过（$WARN 项警告，$INFO 项备注）"
else
	ui_print " 兼容性检查全部通过"
fi
ui_print "=============================="

# ---------- 权限 ----------
# ctnd 必须显式 chmod：解包器不一定认 zip 里存的 Unix 权限位
# （实测 KernelSU 用 Info-ZIP unzip 解包，遇到 create_system=0 的条目会
#   按 DOS 属性处理，可执行位直接丢掉，解出来是 0644 —— daemon 起不来）。
# 带 #! 的脚本会被解包器/管理器补上 0755，但没有 shebang 的二进制不会。
set_perm "$MODPATH/ctn_patch.ko"     0 0 0644
set_perm "$MODPATH/ctnd"             0 0 0755
set_perm "$MODPATH/service.sh"       0 0 0755
set_perm "$MODPATH/uninstall.sh"     0 0 0755
set_perm "$MODPATH/action.sh"        0 0 0755
set_perm "$MODPATH/verify.sh"        0 0 0755
set_perm "$MODPATH/ctn.conf.example" 0 0 0644

say "- 安装完成，重启后自动加载"
say "- 开机日志：/data/adb/modules/ctn_patch/boot.log"
say "- 用法：echo \"名字1 名字2\" > /proc/game_opt/task_boost/critical_task_name"
say "- 注意：名字填「游戏进程内」的线程才真正生效（见文档第六节 6.3）"
