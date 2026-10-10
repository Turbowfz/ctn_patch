#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# ================= ctn_patch 安装脚本（Magisk / KernelSU / APatch 通用）=================
#
# Magisk 默认（SKIPUNZIP=0）会把 zip 里所有文件解到 $MODPATH，所以到这一步
# ctn_patch.ko / service.sh / verify.sh 都已经在模块目录里了。
#
# 本脚本的核心职责是「兼容性闸门」：把 .ko 真正依赖的内核条件逐条验一遍，
# 不满足就拒绝安装，而不是等重启后 insmod 失败才发现。
#
# 输出分两档（用户要求：平时只要进度，被拦时才显示完整那一项）：
#   进度档   每组检查只打一行「[n/9] 名称 ✓」，一路刷下去像进度条
#   拦截档   某组里有 [不满足] 时，把**这一组完整**的检查过程全部打印出来
#            （包括组内本来是 [OK] 的行 —— 要拦人就得让人看清都查了什么）
#   警告不拦截安装，平时不展开，结尾统一给一行提示；被拦时随该组一起展开。
#
# 判定分三级（逻辑与旧版完全一致）：
#   FAIL  必定装不上/会冲突 → abort，不安装
#   WARN  能装但功能打折   → 继续
#   INFO  只作记录         → 继续

ui_print " ctn_patch 安装中（作者 Turbo）"
ui_print " 把 6.6 的 critical_task_name 可写节点搬回 6.1 内核"

FAIL=0
WARN=0
INFO=0

# Magisk 在 customize.sh 里给的是 MODPATH；个别环境只有 MODDIR，兜一下
[ -n "$MODPATH" ] || MODPATH="$MODDIR"
[ -n "$MODPATH" ] || abort "拿不到模块目录（MODPATH/MODDIR 都为空）"

# ---------- 小工具 ----------
say()  { ui_print "$*"; }
GRP_NAME=""
GRP_OUT=""
GRP_FAIL=0
GRP_WARN=0
PG=0
PG_TOTAL=9
WARNBUF=""

grp_start() { GRP_NAME="$1"; GRP_OUT=""; GRP_FAIL=0; GRP_WARN=0; }
ok()   { GRP_OUT="$GRP_OUT  [OK]     $*
"; }
warn() { WARN=$((WARN+1)); GRP_WARN=$((GRP_WARN+1)); GRP_OUT="$GRP_OUT  [警告]   $*
"; }
info() { INFO=$((INFO+1)); GRP_OUT="$GRP_OUT  [备注]   $*
"; }
bad()  { FAIL=$((FAIL+1)); GRP_FAIL=$((GRP_FAIL+1)); GRP_OUT="$GRP_OUT  [不满足] $*
"; }

# 每组结束：被拦 → 展开整组；有警告 → 一行带过 + 存档；正常 → 一行 ✓
grp_end() {
	PG=$((PG+1))
	if [ "$GRP_FAIL" -gt 0 ]; then
		ui_print "[$PG/$PG_TOTAL] $GRP_NAME —— 被拦截，明细如下"
		printf '%s' "$GRP_OUT" | while IFS= read -r l; do ui_print "$l"; done
	elif [ "$GRP_WARN" -gt 0 ]; then
		ui_print "[$PG/$PG_TOTAL] $GRP_NAME ⚠（$GRP_WARN 项警告，不拦截）"
		WARNBUF="$WARNBUF── $GRP_NAME ──
$GRP_OUT"
	else
		ui_print "[$PG/$PG_TOTAL] $GRP_NAME ✓"
	fi
}

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

# ================= 1. 内核版本 =================
grp_start "内核版本"
EXPECT="6.1.141-android14-11"
UREL="$(uname -r)"
case "$UREL" in
	"$EXPECT"*)
		ok "内核 $UREL 属于 $EXPECT 系列"
		;;
	*)
		bad "内核是 $UREL，不是 $EXPECT 系列 —— .ko 不是为它编的"
		info "本包的 .ko 按 $EXPECT 编译，内核不同（包括大版本相同但配置差异大的）必须重编"
		;;
esac
grp_end

# ================= 2. .ko 文件与 vermagic =================
grp_start "模块文件"
if [ ! -f "$MODPATH/ctn_patch.ko" ]; then
	bad "zip 里没有 ctn_patch.ko（先按 README 第五节编译，再 build_zip.py 打包）"
else
	ok "ctn_patch.ko 存在（$(stat -c%s "$MODPATH/ctn_patch.ko" 2>/dev/null) 字节）"
	# toybox 没有 modinfo，直接从 .ko 里抽 vermagic
	KO_VM="$(grep -aom1 'vermagic=[ -~]*' "$MODPATH/ctn_patch.ko" 2>/dev/null | cut -d= -f2)"
	if [ -z "$KO_VM" ]; then
		bad "读不到 .ko 的 vermagic（文件可能损坏或不是内核模块）"
	else
		KO_REL4="$(echo "$KO_VM" | cut -d' ' -f1 | awk -F- '{print $1"-"$2"-"$3"-"$4}')"
		K_REL4="$(echo "$UREL" | awk -F- '{print $1"-"$2"-"$3"-"$4}')"
		if [ "$KO_REL4" = "$K_REL4" ]; then
			ok "vermagic 版本前缀一致（$KO_REL4）"
		else
			bad "vermagic 不匹配：ko=[$KO_REL4] 内核=[$K_REL4]"
		fi
		case "$KO_VM" in
			*" SMP preempt mod_unload modversions aarch64") ;;
			*) warn "vermagic 尾部标志与设备标准串不同：$KO_VM" ;;
		esac
	fi
fi
grp_end

# ================= 3. 目标模块必须在且是「模块」 =================
grp_start "目标模块 oplus_bsp_game_opt"
if grep -q '^oplus_bsp_game_opt ' /proc/modules 2>/dev/null; then
	ok "oplus_bsp_game_opt 已加载（是模块，可解析符号）"
	REF="$(sed -n 's/^oplus_bsp_game_opt [0-9]* \([0-9]*\).*/\1/p' /proc/modules)"
	if grep -q '^ctn_patch ' /proc/modules 2>/dev/null; then
		info "当前 refcount=$REF（其中 1 是本模块旧版在引用，装完重启后由新版接管）"
	else
		info "当前 refcount=$REF（装上后会 +1，这是有意钉住防野指针）"
	fi
else
	if grep -q '^oplus_bsp_game_opt$' /proc/modules 2>/dev/null; then
		ok "oplus_bsp_game_opt 已加载"
	else
		bad "oplus_bsp_game_opt 不在 /proc/modules —— 若是内建进内核则本模块不支持"
		info "本模块靠 kallsyms 的「模块名:符号名」查符号，目标必须是可加载模块"
	fi
fi

# 目标 proc 目录必须在（我们要往里挂节点）
if [ -d /proc/game_opt/task_boost ]; then
	ok "/proc/game_opt/task_boost 目录存在"
else
	bad "/proc/game_opt/task_boost 不存在（game_opt 没起来或结构不同）"
fi
grp_end

# ================= 4. 试加载（用 insmod 自己判定，失败就抓 dmesg）=================
# 不猜、不预检：**直接让内核加载它**。符号 CRC、struct module 布局、kCFI、
# vermagic —— 这些全都由内核在 insmod 时自己校验，比任何用户态启发式都硬。
# 加载失败也不会崩机（CRC 不匹配是干净拒绝），内核会把原因写进 dmesg，
# 这里把那些行抓出来给用户看就完事了 —— 查 bug 看 dmesg 就够了。
#
# 为什么不再比厂商模块的 sha256 / struct module 布局（v1.8 之前有）：
#   一加12、Ace3Pro、Ace5、GT6 这些 8Gen3 机型在同一内核版本下 game_opt 源码
#   同源，跨机型通用；但不同机型/批次编出来的 ko 哈希天然不同，拿哈希当闸门
#   会把兼容机型全拒掉。而布局不一致时内核本来就会干净拒绝加载（CRC 对不上），
#   所以那套预检是多余的，删掉。
grp_start "试加载（insmod 判定）"
if grep -q '^ctn_patch ' /proc/modules 2>/dev/null; then
	# 升级场景：旧版正跑着。旧版能加载 = 本机内核接受我们的构建 → 新版同理，
	# 而且内核不允许同名模块加载两次，这里也没法试。
	ok "ctn_patch 旧版正在运行 —— 说明本机内核能加载我们的 .ko，跳过试加载"
	info "新版与旧版同一套构建，重启后由新版接管"
else
	TRY_ERR=$(insmod "$MODPATH/ctn_patch.ko" 2>&1)
	if [ $? -eq 0 ]; then
		ok "insmod 成功（CRC / 布局 / kCFI / vermagic 均由内核校验通过）"
		if rmmod ctn_patch 2>/dev/null; then
			info "已卸载试加载的实例，重启后由 service.sh 正式加载"
		else
			warn "试加载成功但 rmmod 失败 —— 重启后会重新加载，一般无碍"
		fi
	else
		bad "insmod 失败：${TRY_ERR:-无输出}"
		KT=$(dmesg 2>/dev/null | grep -i ctn_patch | tail -5)
		if [ -n "$KT" ]; then
			info "内核日志里 ctn_patch 相关的行："
			# 必须走 info（进本组缓冲）—— 直接 ui_print 会绕过分组缓冲，
			# 那几行会跑到「[n/9]」标题**前面**去，日志顺序就乱了（实测踩过）。
			# 用 here-doc 而不是管道：管道会让 while 跑在子 shell 里。
			while IFS= read -r l; do info "  $l"; done <<EOF
$KT
EOF
		else
			info "内核日志里没有 ctn_patch 的行 —— 说明它连 init 都没进去（多半是内核拒绝了这个 .ko）"
			info "手动确认：dmesg | tail -30"
		fi
		info "常见原因：内核版本/配置不同（vermagic 前缀已单独检查过）→ 按本机重编（README 6.1）"
	fi
fi
grp_end

# ================= 5. 是否已经有了这个节点（6.6 内核 / 重复安装）=================
# 节点存在有两种**完全不同**的情况，必须分开处理（早先没分，导致升级被误拒）：
#   a) ctn_patch 正加载着 → 节点是**我们自己**建的 → 这是「升级」，允许装。
#      KernelSU 装新版是装到 modules_update，旧版的 ko 还在内存里跑着、
#      节点还在 —— 这时候要是拒绝，就等于每升一次版卡一次。
#      装完重启，新版的 ko 接管，节点内容由新版重建。
#   b) ctn_patch 没加载却有节点 → 内核自带（6.6 那种）或别的补丁建的 → 拒绝。
# 为什么 (a) 能断定节点是我们的：如果内核本来就有这个节点，我们 insmod 时
# proc_create_data 会返回 NULL，init 直接 -EEXIST 退出，ctn_patch 就不会
# 出现在 /proc/modules 里。所以「ctn_patch 在跑」和「节点是我们建的」等价。
grp_start "节点占用情况"
if [ -e /proc/game_opt/task_boost/critical_task_name ]; then
	if grep -q '^ctn_patch ' /proc/modules 2>/dev/null; then
		ok "节点存在，但 ctn_patch 正在运行 —— 这是升级，重启后由新版接管"
		info "当前值：$(cat /proc/game_opt/task_boost/critical_task_name 2>/dev/null)"
	else
		bad "该节点已存在，而 ctn_patch 没在运行 —— 内核自带（6.6 那种）或别的补丁建的"
		info "当前值：$(cat /proc/game_opt/task_boost/critical_task_name 2>/dev/null)"
		info "重复安装无意义；若确要装，先卸掉占住节点的那个"
	fi
else
	ok "节点不存在，安装后由本模块提供"
fi
grp_end

# ================= 5.5 ctnd 与它依赖的 SQLite =================
grp_start "ctnd 与依赖"
if [ -f "$MODPATH/ctnd" ]; then
	ok "ctnd 存在（$(stat -c%s "$MODPATH/ctnd" 2>/dev/null) 字节）"
	# 架构对不上会在启动时才炸，这里先看一眼
	if grep -qa "ELF" "$MODPATH/ctnd" 2>/dev/null; then
		ok "ctnd 是 ELF 可执行文件"
	else
		bad "ctnd 不是有效的 ELF（zip 坏了？）"
	fi
else
	bad "zip 里没有 ctnd —— 没有它节点永远停在默认值，装了也白装"
fi

# ctnd 靠 dlopen 读 COSA 的 SQLite 库拿云控配置，库不在就没法自动填名字
SQLITE=""
for p in /system/lib64/libsqlite.so /system/lib64/libsqlite3.so \
         /apex/com.android.runtime/lib64/libsqlite3.so \
         /apex/com.android.art/lib64/libsqlite3.so; do
	[ -f "$p" ] && { SQLITE="$p"; break; }
done
if [ -n "$SQLITE" ]; then
	ok "找到 SQLite 库：$SQLITE"
else
	warn "找不到 SQLite 库，ctnd 读不了云控配置（节点会停在默认值）"
fi

# 云控库（COSA 的 SQLite）——没有它 ctnd 拿不到 ctn
COSADB=""
for p in /data/user/0/com.oplus.cosa/databases/db_game_database \
         /data/data/com.oplus.cosa/databases/db_game_database; do
	[ -f "$p" ] && { COSADB="$p"; break; }
done
if [ -n "$COSADB" ]; then
	ok "找到云控库：$COSADB"
else
	warn "找不到 COSA 的 db_game_database（游戏没启动过？ctnd 会重试）"
fi
grp_end

# ================= 6. 配置项检查 =================
grp_start "内核配置"
if [ "$HAVE_CONFIG" != "1" ]; then
	warn "读不到 /proc/config.gz，跳过配置项检查（只做运行时检查）"
else
	# 硬条件
	if cfg_on CONFIG_KPROBES; then
		ok "CONFIG_KPROBES=y（取 kallsyms_lookup_name 靠它）"
	else
		bad "CONFIG_KPROBES 未开 —— 无法解析符号，模块装不上"
	fi
	if cfg_on CONFIG_MODULE_UNLOAD; then
		ok "CONFIG_MODULE_UNLOAD=y（可以 rmmod 卸载）"
	else
		warn "CONFIG_MODULE_UNLOAD 未开 —— 装上后无法卸载，只能重启"
	fi
	if cfg_is CONFIG_MODULE_SIG_FORCE y; then
		bad "CONFIG_MODULE_SIG_FORCE=y —— 未签名模块不允许加载"
	else
		ok "未强制模块签名（未签名 .ko 可以加载）"
	fi
	# 软条件
	if cfg_on CONFIG_KALLSYMS_ALL; then
		ok "CONFIG_KALLSYMS_ALL=y"
	else
		warn "CONFIG_KALLSYMS_ALL 未开 —— 可能取不到 copy_from_kernel_nofault（会退回 memcpy，功能正常）"
	fi
	if cfg_on CONFIG_MODVERSIONS; then
		ok "CONFIG_MODVERSIONS=y（.ko 里的符号 CRC 会被校验）"
	else
		warn "CONFIG_MODVERSIONS 未开 —— 会跳过 CRC 校验（本机应该开着）"
	fi
	if cfg_on CONFIG_CFI_CLANG; then
		ok "CONFIG_CFI_CLANG=y（kCFI 类型号必须与内核一致，本 .ko 已对齐 clang 17.0.2）"
	fi
	if cfg_on CONFIG_STRICT_MODULE_RWX; then
		ok "CONFIG_STRICT_MODULE_RWX=y（.rodata 只读，本模块用 vmap 别名写）"
	fi
fi
grp_end

# ================= 7. 运行时符号可用性 =================
grp_start "运行时符号"
if [ -r /proc/kallsyms ]; then
	for s in kallsyms_lookup_name find_module register_kprobe; do
		if awk -v n="$s" '$3==n {found=1; exit} END{exit !found}' /proc/kallsyms 2>/dev/null; then
			ok "kallsyms 里有 $s"
		else
			if [ "$s" = "kallsyms_lookup_name" ]; then
				bad "kallsyms 里没有 kallsyms_lookup_name —— 无法解析模块符号"
			else
				warn "kallsyms 里没有 $s（可能被裁掉，加载时会报出来）"
			fi
		fi
	done
	if awk '$3=="copy_from_kernel_nofault" {found=1; exit} END{exit !found}' /proc/kallsyms 2>/dev/null; then
		ok "有 copy_from_kernel_nofault"
	else
		warn "没有 copy_from_kernel_nofault（被 TRIM 裁掉）→ 退回 memcpy 读串，功能不受影响"
	fi
else
	warn "读不到 /proc/kallsyms，跳过符号检查"
fi
grp_end

# ================= 8. 其它环境 =================
grp_start "其它环境"
PS="$(getconf PAGE_SIZE 2>/dev/null)"
[ -n "$PS" ] || PS="$(getconf PAGESIZE 2>/dev/null)"
if [ -n "$PS" ]; then
	if [ "$PS" = "4096" ]; then
		ok "页大小 4096（与构建目标一致）"
	else
		info "页大小 $PS（非 4096 也能用，只要指针数组不跨页；加载时会自行判断）"
	fi
fi
case "$(getenforce 2>/dev/null)" in
	Enforcing) info "SELinux=Enforcing（magisk 域通常有 sys_module 权限；若 insmod 被拦看 boot.log）" ;;
	*) ;;
esac
if [ -e /proc/sys/kernel/kptr_restrict ]; then
	info "kptr_restrict=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null)（只影响地址显示，不影响符号名）"
fi
grp_end

# ================= 结论 =================
if [ "$FAIL" -gt 0 ]; then
	ui_print ""
	ui_print "=============================="
	ui_print " 兼容性检查未通过：$FAIL 项不满足"
	ui_print " （另有警告 $WARN 项 / 备注 $INFO 项，见上面各组明细）"
	ui_print "=============================="
	if [ -n "$WARNBUF" ]; then
		ui_print " 以下警告（不拦截安装）一并给出："
		printf '%s' "$WARNBUF" | while IFS= read -r l; do ui_print "$l"; done
	fi
	abort "环境不兼容，已取消安装"
fi

ui_print ""
if [ "$WARN" -gt 0 ]; then
	ui_print " 兼容性检查通过（警告 $WARN 项 / 备注 $INFO 项）"
	if [ -n "$WARNBUF" ]; then
		printf '%s' "$WARNBUF" | while IFS= read -r l; do ui_print "$l"; done
	fi
else
	ui_print " 兼容性检查全部通过（$PG 组，备注 $INFO 项）"
fi

# ---------- 权限 ----------
# ctnd 必须显式 chmod：解包器不一定认 zip 里存的 Unix 权限位
# （实测 KernelSU 用 Info-ZIP unzip 解包，遇到 create_system=0 的条目会
#   按 DOS 属性处理，可执行位直接丢掉，解出来是 0644 —— daemon 起不来）。
# 带 #! 的脚本会被解包器/管理器补上 0755，但没有 shebang 的二进制不会。
set_perm "$MODPATH/ctn_patch.ko"     0 0 0644
set_perm "$MODPATH/ctnd"             0 0 0755
set_perm "$MODPATH/fakethreads"      0 0 0755
set_perm "$MODPATH/service.sh"       0 0 0755
set_perm "$MODPATH/uninstall.sh"     0 0 0755
set_perm "$MODPATH/action.sh"        0 0 0755
set_perm "$MODPATH/verify.sh"        0 0 0755

ui_print "- 重启后自动加载；日志 /data/adb/modules/ctn_patch/boot.log"
ui_print "- daemon 只在云控里有 ctn 时才动节点；没有就完全不碰（见 README 5.4）"
