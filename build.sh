#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# =============================================================================
# ctn_patch 一键构建（在 Linux 上跑：WSL2 / 虚拟机 / GitHub Actions 都行）
#
# 为什么需要这套东西：
#   这台手机的内核开了 CONFIG_MODVERSIONS，模块里每个外部符号都要带一个 CRC，
#   而 CRC 只有「编过这个内核」才拿得到。整编一遍内核要几小时 + 几十 GB，
#   这里用两个从设备上抠出来的东西替代，把构建压到 10 分钟左右：
#     - device_kernel.config : 内核 Image 里内嵌的真实 .config（IKCONFIG）
#     - Module.symvers.device: 从设备自带的 432 个 .ko 的 __versions 段抽出的
#                              4168 个符号 CRC（同名符号 CRC 全一致，已校验）
#
# 用法：
#   bash build.sh                      # 默认在 ./build_work 下干活
#   bash build.sh --clang /path/clang  # 指定编译器
#   bash build.sh --srcdir /path/kernel  # 复用已克隆的内核源码
#   bash build.sh --jobs 8
#
# 依赖（Ubuntu/Debian）：
#   sudo apt-get install -y clang-17 lld-17 llvm-17 build-essential bc bison \
#        flex libssl-dev libelf-dev python3 git
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

KERNEL_REPO="${KERNEL_REPO:-https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650}"
# 国内直连 github.com 常常不通，这个镜像实测可用；主地址失败就自动换它。
KERNEL_REPO_MIRROR="${KERNEL_REPO_MIRROR:-https://gh-proxy.com/https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650}"
KERNEL_BRANCH="${KERNEL_BRANCH:-oneplus/sm8650_b_16.0.0_ace_3_pro}"

# 目标 vermagic。设备实锤：
#   内核自身      = 6.1.141-android14-11-o-gd2a6093d5589 SMP preempt mod_unload modversions aarch64
#   432 个厂商模块 = 6.1.141-android14-11-o-gdc1b6a03413f SMP preempt mod_unload modversions aarch64
# 两者只差 git hash，且模块确实加载成功 → 这个内核容忍 hash 尾差。
# 为求确定性，下面把 LOCALVERSION 钉成模块那串，让产物 vermagic 完全一致。
TARGET_VERMAGIC="6.1.141-android14-11-o-gdc1b6a03413f SMP preempt mod_unload modversions aarch64"
TARGET_LOCALVERSION="-android14-11-o-gdc1b6a03413f"

WORK="$HERE/build_work"
SRCDIR=""
CLANG_BIN=""
JOBS="$(nproc 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
	case "$1" in
		--srcdir) SRCDIR="$2"; shift 2 ;;
		--clang)  CLANG_BIN="$2"; shift 2 ;;
		--jobs)   JOBS="$2"; shift 2 ;;
		--work)   WORK="$2"; shift 2 ;;
		-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
		*) echo "未知参数: $1"; exit 2 ;;
	esac
done

SRC="${SRCDIR:-$WORK/kernel}"
OUT="$WORK/out"

echo "=============================================================="
echo " ctn_patch 构建"
echo "   模块源码 : $HERE"
echo "   内核源码 : $SRC"
echo "   构建输出 : $OUT"
echo "   目标 vermagic: $TARGET_VERMAGIC"
echo "=============================================================="

# ---------- 0. 找 clang ----------
# 内核必须用 clang 编（设备内核就是 clang 17.0.2 编的）。
# 注意：NDK 是给 Android 用户态（bionic）用的，不是给内核用的 —— 但 NDK 里带的
# 就是一份完整 LLVM，当 CC 用没问题。不过 Windows 版 NDK 的 prebuilt 是
# windows-x86_64 的 exe，WSL 里跑不了；要用得下 Linux 版 NDK（r26 对应 clang 17.0.2，
# 和设备完全同版本；r27+ 是 clang 18/19，能用但没那个必要）。
if [ -z "$CLANG_BIN" ]; then
	for c in "$(command -v clang 2>/dev/null)" \
	         /usr/lib/llvm-17/bin/clang \
	         /usr/lib/llvm-18/bin/clang \
	         /usr/lib/llvm-16/bin/clang; do
		[ -n "$c" ] && [ -x "$c" ] && { CLANG_BIN="$c"; break; }
	done
fi
# 兜底：扫 NDK 的 linux prebuilt（含 WSL 下挂在 /mnt/c 的）
if [ -z "$CLANG_BIN" ]; then
	for c in /mnt/c/Users/*/ndk/android-ndk-*/toolchains/llvm/prebuilt/linux-x86_64/bin/clang \
	         "$HOME"/android-ndk-*/toolchains/llvm/prebuilt/linux-x86_64/bin/clang \
	         /opt/android-ndk-*/toolchains/llvm/prebuilt/linux-x86_64/bin/clang; do
		[ -x "$c" ] && { CLANG_BIN="$c"; echo "   （用 NDK 的 clang：$c）"; break; }
	done
fi
if [ -z "$CLANG_BIN" ]; then
	echo "!! 找不到 clang。装一下：sudo apt-get install -y clang-17 lld-17 llvm-17"
	echo "   （NDK 里虽有 clang，但 Windows 版的 exe 在 WSL/Linux 里跑不了，"
	echo "     要用得下 Linux 版 NDK：android-ndk-r26b-linux.zip 对应 clang 17.0.2）"
	exit 1
fi
LLVM_BIN="$(dirname "$CLANG_BIN")"
export PATH="$LLVM_BIN:$PATH"
echo "[0/7] clang = $CLANG_BIN  ($("$CLANG_BIN" --version | head -1))"
for t in ld.lld llvm-ar llvm-nm llvm-objcopy llvm-strip llvm-objdump; do
	command -v "$t" >/dev/null 2>&1 || echo "   ! 缺少 $t（llvm-17 包里有）"
done

# 设备内核是 clang 17.0.2 编的（CONFIG_CLANG_VERSION=170002）。大版本不同时：
#   - CONFIG_WERROR=y 会把新版本的「新警告」变成编译错误 → 我们在铺配置时直接关掉 WERROR
#   - 再补一道 KCFLAGS=-Wno-error 兜底
#   - kCFI 类型号是否一致，另有 verify_kcfi.py 拿设备自带 .ko 比对
CLANG_MAJOR="$("$CLANG_BIN" -dumpversion 2>/dev/null | cut -d. -f1)"
KCFLAGS_ARG=""
if [ "$CLANG_MAJOR" != "17" ]; then
	echo "   ! clang 大版本 = $CLANG_MAJOR，设备内核用的是 17 → 自动加 -Wno-error"
	echo "     （想完全对齐可以用 NDK r26b 里的 clang 17.0.2：--clang <ndk>/.../bin/clang）"
	KCFLAGS_ARG="-Wno-error"
fi

# 主机侧工具（fixdep/genksyms/modpost 等）用原生 gcc 编，避免交叉编译器
# 默认 target 不是本机导致 host 程序跑不起来。
HOSTCC_BIN="${HOSTCC_BIN:-gcc}"
HOSTCXX_BIN="${HOSTCXX_BIN:-g++}"
HOSTLD_BIN="${HOSTLD_BIN:-ld}"
echo "     主机编译器 HOSTCC = $HOSTCC_BIN"

MAKE_COMMON=(-C "$SRC" O="$OUT" ARCH=arm64 LLVM=1 CC="$CLANG_BIN")
MAKE_COMMON+=("HOSTCC=$HOSTCC_BIN" "HOSTCXX=$HOSTCXX_BIN" "HOSTLD=$HOSTLD_BIN")
if [ -n "$KCFLAGS_ARG" ]; then
	MAKE_COMMON+=("KCFLAGS=$KCFLAGS_ARG")
fi

# ---------- 1. 内核源码 ----------
mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then
	echo "[1/7] 克隆内核源码（depth=1，约 1.5~2GB，第一次慢）..."
	if ! git clone --depth 1 -b "$KERNEL_BRANCH" "$KERNEL_REPO" "$SRC"; then
		echo "      主地址失败，换镜像：$KERNEL_REPO_MIRROR"
		rm -rf "$SRC"
		git clone --depth 1 -b "$KERNEL_BRANCH" "$KERNEL_REPO_MIRROR" "$SRC"
	fi
else
	echo "[1/7] 复用已有源码：$SRC"
fi

# 版本核对：设备内核是 6.1.141，源码树顶 Makefile 必须一致，
# 否则产物的 vermagic 前缀就不是 6.1.141，insmod 必失败。
SRC_VER="$(grep -E '^(VERSION|PATCHLEVEL|SUBLEVEL) = ' "$SRC/Makefile" \
	| head -3 | awk '{print $3}' | paste -sd.)"
echo "      内核源码版本 = $SRC_VER  （设备需要 6.1.141）"
if [ "$SRC_VER" != "6.1.141" ]; then
	echo "!! 版本不一致。换对分支重来，例如："
	echo "   git clone --depth 1 -b <正确分支> $KERNEL_REPO $SRC"
	echo "   （OnePlusOSS sm8650 的 ace_3_pro 分支实测就是 6.1.141）"
	exit 1
fi

# ---------- 2. 铺配置 ----------
echo "[2/7] 写入设备真实配置 ..."
mkdir -p "$OUT"
cp "$HERE/device_kernel.config" "$OUT/.config"

# 让 vermagic 精确命中目标串：关掉 AUTO、把 LOCALVERSION 钉死
python3 - "$OUT/.config" "$TARGET_LOCALVERSION" <<'PY'
import re, sys
path, localver = sys.argv[1], sys.argv[2]
txt = open(path, encoding='utf-8').read()

def setval(txt, key, val):
    # 已有 "CONFIG_KEY=..." 就替换；被注释掉的 "# CONFIG_KEY is not set" 也替换
    pat_eq = re.compile(r'^%s=.*$' % re.escape(key), re.M)
    pat_no = re.compile(r'^# %s is not set$' % re.escape(key), re.M)
    if pat_eq.search(txt):
        return pat_eq.sub('%s=%s' % (key, val), txt, count=1)
    if pat_no.search(txt):
        return pat_no.sub('%s=%s' % (key, val), txt, count=1)
    return txt + '\n%s=%s\n' % (key, val)

def unset(txt, key):
    pat_eq = re.compile(r'^%s=.*$' % re.escape(key), re.M)
    if pat_eq.search(txt):
        return pat_eq.sub('# %s is not set' % key, txt, count=1)
    return txt

txt = setval(txt, 'CONFIG_LOCALVERSION', '"%s"' % localver)
txt = unset(txt, 'CONFIG_LOCALVERSION_AUTO')

# 构建机上不存在的白名单路径会让 vmlinux 链接报错（我们只做 modules_prepare，但清掉更稳）
txt = setval(txt, 'CONFIG_UNUSED_KSYMS_WHITELIST', '""')

# ★ 千万不要关 CONFIG_DEBUG_INFO_BTF / _BTF_MODULES！
#   它们给 struct module 加 btf_data_size/btf_data 等字段，一关掉 sizeof(struct module)
#   就从 1088 掉到 1024，而内核装载器按自己的布局读写我们的 .gnu.linkonce.this_module
#   段 → 越界写坏指针 → mod_sysfs_setup 里野指针 panic。实测踩过这个坑。
#   WERROR 也保留设备值（用 clang 17 编不会有新警告问题）。
#   唯一需要动的是白名单路径（指向设备构建机，本地不存在）。

open(path, 'w', encoding='utf-8', newline='\n').write(txt)
print("   已设置 LOCALVERSION=%s，关闭 LOCALVERSION_AUTO（BTF 等保持设备值）" % localver)
PY

echo "      CONFIG_LOCALVERSION / AUTO 现状："
grep -E '^CONFIG_LOCALVERSION' "$OUT/.config" | sed 's/^/        /'

# ---------- 3. olddefconfig ----------
echo "[3/7] make olddefconfig ..."
make "${MAKE_COMMON[@]}" olddefconfig >/dev/null
echo "      kernel.release = $(cat "$OUT/include/config/kernel.release" 2>/dev/null || echo '(未生成)')"

# ---------- 4. modules_prepare ----------
echo "[4/7] make modules_prepare（几分钟）..."
make "${MAKE_COMMON[@]}" -j"$JOBS" modules_prepare

# ---------- 5. 塞设备符号表 ----------
echo "[5/7] 安装设备符号表 Module.symvers ..."
cp "$HERE/Module.symvers.device" "$OUT/Module.symvers"
echo "      $(wc -l < "$OUT/Module.symvers") 个符号"

# ---------- 6. 编模块 ----------
echo "[6/7] 编译 ctn_patch.ko ..."
rm -f "$HERE/ctn_patch.ko" "$HERE/ctn_patch.mod" "$HERE/ctn_patch.o" \
      "$HERE/ctn_patch.mod.c" "$HERE/Module.symvers" "$HERE/modules.order"
make "${MAKE_COMMON[@]}" M="$HERE" -j"$JOBS" modules

if [ ! -f "$HERE/ctn_patch.ko" ]; then
	echo "!! 没产出 ctn_patch.ko，看上面的报错"
	exit 1
fi

# ---------- 7. 校验 vermagic ----------
echo "[7/7] 校验 vermagic ..."
GOT="$(grep -aom1 'vermagic=[ -~]*' "$HERE/ctn_patch.ko" | cut -d= -f2)"
echo "      产物 vermagic: $GOT"
echo "      目标 vermagic: $TARGET_VERMAGIC"
if [ "$GOT" = "$TARGET_VERMAGIC" ]; then
	echo "      [OK] 完全一致"
else
	GOT4=$(echo "$GOT" | awk -F- '{print $1"-"$2"-"$3"-"$4}')
	EXP4=$(echo "$TARGET_VERMAGIC" | awk -F- '{print $1"-"$2"-"$3"-"$4}')
	if [ "$GOT4" = "$EXP4" ]; then
		echo "      [WARN] 只有 git hash 不同 —— 设备实测容忍（内核自身 hash 和"
		echo "             432 个厂商模块都不一样，照样加载），应该能用"
	else
		echo "      [FAIL] 版本前缀都不一样，insmod 会失败，把这里贴给我"
		exit 1
	fi
fi

# ---------- 8. 硬校验：struct module 布局必须和设备模块一致 ----------
echo
echo "[8/8] 校验 struct module 布局（上次崩机的根因）..."
python3 - "$HERE/ctn_patch.ko" "$HERE/device_kernel.config" <<'PYEOF'
import struct, sys, os

def sec_size(path, want):
    d = open(path, "rb").read()
    e_shoff, = struct.unpack_from("<Q", d, 0x28)
    ents, num, strx = struct.unpack_from("<HHH", d, 0x3A)
    S = []
    for i in range(num):
        o = e_shoff + i * ents
        v = struct.unpack_from("<IIQQQQIIQQ", d, o)
        S.append(dict(n=v[0], off=v[4], size=v[5]))
    stroff = S[strx]["off"]
    for s in S:
        e = d.find(b" ", stroff + s["n"])
        s["name"] = d[stroff + s["n"]:e].decode()
    for s in S:
        if s["name"] == want:
            return s["size"]
    return None

mine = sys.argv[1]
dev = os.environ.get("DEV_VENDOR_KO", "/path/to/oplus_bsp_game_opt.ko")
if os.path.exists(dev):
    a = sec_size(dev, ".gnu.linkonce.this_module")
    b = sec_size(mine, ".gnu.linkonce.this_module")
    print(f"      设备厂商模块 struct module = {a} 字节")
    print(f"      我们产物     struct module = {b} 字节")
    if a != b:
        print("      !! 布局不一致 —— 刷入必崩，请检查是不是关了 BTF 等配置")
        sys.exit(1)
    print("      [OK] 布局一致")
else:
    b = sec_size(mine, ".gnu.linkonce.this_module")
    print(f"      产物 struct module = {b} 字节（期望 1088）")
    if b != 1088:
        print("      !! 不是 1088 —— 大概率关了 CONFIG_DEBUG_INFO_BTF(_MODULES)")
        sys.exit(1)
PYEOF

echo
echo "=============================================================="
echo " 完成：$HERE/ctn_patch.ko"
ls -la "$HERE/ctn_patch.ko"
echo
echo " 下一步打包成 root 模块："
echo "   python3 build_zip.py"
echo " 刷入：Magisk/KernelSU App -> 模块 -> 从存储安装 -> 选 ctn_patch_magisk.zip"
echo "=============================================================="
