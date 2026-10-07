#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# =============================================================================
# Arch(WSL) 一键全流程：依赖 → NDK → 内核源码 → Kconfig 补链 → 编 ko → 校验
# （所有子步骤脚本都可单独重跑，本脚本按顺序串一遍；已做过的步骤会自动跳过）
#
# 由 Windows 侧调起：
#   MSYS_NO_PATHCONV=1 wsl.exe -d archlinux -u root -- /bin/bash /mnt/c/.../wsl_build.sh
# =============================================================================
set -uo pipefail

HERE="/mnt/c/Users/User/Desktop/风驰/6.1_一加ace3pro/05_补丁模块/ctn_patch"
NDK="/opt/ndkroot/android-ndk-r26b/toolchains/llvm/prebuilt/linux-x86_64"
# 必须把 NDK 的 bin 加进 PATH：内核用 LLVM=1 时会把 NM/AR/OBJCOPY 等设成
# llvm-nm / llvm-ar / llvm-objcopy，这些是从 PATH 找的（Arch 没装系统 llvm，
# 只有 NDK 里有）。少了它 scripts/check-local-export 会报 "llvm-nm failed"。
# CC 是显式传全路径的，所以只有 PATH 缺了时症状很隐蔽。
export PATH="$NDK/bin:$PATH"
KERNEL=/root/ctn_build/kernel
OUT=/root/ctn_build/work/out
SRC=/root/ctn_build/src
WIN_DIR="$HERE"

step() { echo; echo "=================================================="; echo " $*"; echo "=================================================="; }

# ---------- 1. 依赖 ----------
if ! command -v make >/dev/null || ! command -v gcc >/dev/null; then
	step "1/8 装依赖"
	bash "$HERE/arch_setup.sh" || exit 1
else
	step "1/8 依赖已装（跳过）"
fi

# ---------- 2. NDK clang 17.0.2 ----------
if [ ! -x "$NDK/bin/clang" ]; then
	step "2/8 下载 NDK r26b（clang 17.0.2）"
	bash "$HERE/arch_get_ndk.sh" || exit 1
else
	step "2/8 NDK 已就绪（跳过）"
	"$NDK/bin/clang" --version | head -1 | sed 's/^/    /'
fi

# ---------- 3. 内核源码 ----------
step "3/8 内核源码"
if [ ! -f "$KERNEL/Makefile" ]; then
	git clone --depth 1 -b oneplus/sm8650_b_16.0.0_ace_3_pro \
		https://gh-proxy.com/https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650 \
		"$KERNEL" 2>&1 | tail -3 || exit 1
else
	echo "    已存在：$KERNEL"
fi
V=$(grep -E '^(VERSION|PATCHLEVEL|SUBLEVEL) = ' "$KERNEL/Makefile" | head -3 | awk '{print $3}' | paste -sd.)
echo "    源码版本 = $V（需 6.1.141）"
[ "$V" = "6.1.141" ] || { echo "    !! 版本不对"; exit 1; }

# ---------- 4. 设备配置 ----------
step "4/8 铺设备真实配置"
mkdir -p "$OUT"
cp "$HERE/device_kernel.config" "$OUT/.config"
python3 - "$OUT/.config" <<'PYEOF'
import re, sys
path = sys.argv[1]
txt = open(path, encoding="utf-8").read()

def setval(txt, key, val):
    pe = re.compile(r'^%s=.*$' % re.escape(key), re.M)
    pn = re.compile(r'^# %s is not set$' % re.escape(key), re.M)
    if pe.search(txt): return pe.sub('%s=%s' % (key, val), txt, count=1)
    if pn.search(txt): return pn.sub('%s=%s' % (key, val), txt, count=1)
    return txt + '\n%s=%s\n' % (key, val)

def unset(txt, key):
    return re.sub(r'^%s=.*$' % re.escape(key), '# %s is not set' % key, txt, count=1, flags=re.M)

# vermagic 钉死成设备模块那一串
txt = setval(txt, 'CONFIG_LOCALVERSION', '"-android14-11-o-gdc1b6a03413f"')
txt = unset(txt, 'CONFIG_LOCALVERSION_AUTO')
# 构建机上不存在的白名单路径
txt = setval(txt, 'CONFIG_UNUSED_KSYMS_WHITELIST', '""')
    # WERROR 也保留设备值（clang 17.0.2 编不会有新警告问题）
    # ★ 千万不要关 CONFIG_DEBUG_INFO_BTF / _BTF_MODULES ★
    #   它们给 struct module 加字段，一关掉 sizeof(struct module) 就从 1088
    #   掉到 1024，装载器按自己的布局越界写坏指针 → insmod 时 panic。
    # ★ 千万不要关 CONFIG_DEBUG_INFO_BTF / _BTF_MODULES ★
    #   它们给 struct module 加字段，一关掉 sizeof(struct module) 就从 1088
    #   掉到 1024，装载器按自己的布局越界写坏指针 → insmod 时 panic。
    # ★ 千万不要关 CONFIG_DEBUG_INFO_BTF / _BTF_MODULES ★
    #   它们给 struct module 加字段，一关掉 sizeof(struct module) 就从 1088
    #   掉到 1024，装载器按自己的布局越界写坏指针 → insmod 时 panic。
open(path, 'w', encoding='utf-8', newline='\n').write(txt)
print("    已钉 LOCALVERSION，其余配置沿用设备值")
PYEOF

# ---------- 5. 断链重指 + Kconfig 修补 + olddefconfig ----------
step "5/8 断链重指 + Kconfig 修补 + olddefconfig"
bash "$HERE/arch_relink.sh" || exit 1

# ---------- 6. modules_prepare ----------
# 生成内核头文件 / host 工具 / kernel.release。
# 注意：改过 .config 后必须重跑这一步，否则 autoconf.h 还是旧的
# （实测踩过：以为关了 BTF 不影响 struct module，其实是 prepare 没重跑）。
step "6/8 modules_prepare"
make -C "$KERNEL" O="$OUT" ARCH=arm64 LLVM=1 CC="$NDK/bin/clang" 	HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld -j"$(nproc)" modules_prepare 2>&1 | tail -5
echo "    kernel.release = $(cat "$OUT/include/config/kernel.release" 2>/dev/null)"

# ---------- 7. 编模块（含 symvers 重排 + .scmversion 去 + 号）----------
step "7/8 编模块"

# 先把 Windows 侧源码同步到 Linux 侧（/mnt/c 是 9p 慢盘，编译在本地盘做），
# 顺便归一 CRLF —— Windows 编辑过的文件若带回车符，内核 Makefile 会报错。
rm -rf "$SRC"
mkdir -p "$SRC"
cp -a "$HERE/." "$SRC/"
cd "$SRC"
find . -maxdepth 1 -type f -print0 | xargs -0 -r sed -i 's/\r$//'
echo "    源码已同步到 $SRC"
echo "    ctn_patch.c sha256 = $(sha256sum ctn_patch.c | cut -c1-16)"

# 清掉从 Windows 侧一起拷过来的旧编译产物。留着会让 make 走增量链接，
# 实测产出过坏 ELF（ko 少 3 字节、modinfo 报 Invalid argument），干净重编最稳。
rm -f ctn_patch.ko ctn_patch.mod ctn_patch.mod.c ctn_patch.mod.o ctn_patch.o \
      Module.symvers modules.order

touch "$KERNEL/.scmversion"
rm -f "$OUT/include/config/kernel.release"
make -C "$KERNEL" O="$OUT" ARCH=arm64 LLVM=1 CC="$NDK/bin/clang" \
	HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld -j"$(nproc)" prepare >/dev/null 2>&1
python3 - "$SRC/Module.symvers.device" "$OUT/Module.symvers" <<'PYEOF'
import sys
src, dst = sys.argv[1], sys.argv[2]
out = []
for line in open(src, encoding="utf-8"):
    s = line.rstrip("\n")
    if not s: continue
    p = s.split("\t")
    crc, sym = p[0], p[1]
    export = p[2] if len(p) > 2 else "EXPORT_SYMBOL"
    ns = p[4] if len(p) > 4 else ""
    out.append("\t".join([crc, sym, "vmlinux", export, ns]) + "\n")
open(dst, "w", newline="\n").write("".join(out))
print(f"    symvers {len(out)} 条")
PYEOF
make -C "$KERNEL" O="$OUT" M="$SRC" ARCH=arm64 LLVM=1 CC="$NDK/bin/clang" \
	HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld -j"$(nproc)" modules 2>&1 | tail -8
[ -f "$SRC/ctn_patch.ko" ] || { echo "    !! 编译失败"; exit 1; }
echo "    $(ls -la "$SRC/ctn_patch.ko")"

# ---------- 7.5 剥掉调试段（体积 304KB → ~35KB）----------
# 内核安装模块时 INSTALL_MOD_STRIP=1 做的就是这件事（strip --strip-debug）。
# 这个 .ko 里 .debug_info + 它的重定位 + .debug_str 等占了约 270KB ——
# 装载器一个都不用，留着只是让刷入包更大、写 flash 更慢。
# 只剥 .debug_*，绝不碰这些：.text / .modinfo / .gnu.linkonce.this_module /
# __versions / .rela.*（modpost 与装载器要用的）。下面的校验跑在**剥完之后**，
# 所以校验的就是最终产物。
step "7.5/8 剥调试段"
SZ_BEFORE=$(stat -c%s "$SRC/ctn_patch.ko")
"$NDK/bin/llvm-strip" --strip-debug "$SRC/ctn_patch.ko" || { echo "    !! strip 失败"; exit 1; }
SZ_AFTER=$(stat -c%s "$SRC/ctn_patch.ko")
echo "    $SZ_BEFORE -> $SZ_AFTER 字节（省 $(( (SZ_BEFORE-SZ_AFTER)/1024 ))KB）"
# 剥完必须确认关键段还在、大小没变（尤其 1088 的 this_module）
"$NDK/bin/llvm-readelf" -S "$SRC/ctn_patch.ko" | grep -q 'gnu.linkonce.this_module' 	|| { echo "    !! this_module 段没了，strip 过头了"; exit 1; }
grep -qa 'vermagic=' "$SRC/ctn_patch.ko" || { echo "    !! modinfo/vermagic 没了"; exit 1; }

# ---------- 8. 校验 + 拷回 ----------
step "8/8 校验"
VM=$(grep -aom1 'vermagic=[ -~]*' "$SRC/ctn_patch.ko" | cut -d= -f2)
EXPECT="6.1.141-android14-11-o-gdc1b6a03413f SMP preempt mod_unload modversions aarch64"
echo "    vermagic: $VM"
[ "$VM" = "$EXPECT" ] && echo "    [OK] vermagic 一致" || echo "    [!!] vermagic 不一致"
echo "    undefined 符号: $("$NDK/bin/llvm-nm" -u "$SRC/ctn_patch.ko" | grep -c ' U ' || true)"
CTN_DEV_MODDIR="/mnt/c/Users/User/Desktop/风驰/6.1_一加ace3pro/01_手机提取/modules" \
	python3 "$HERE/verify_kcfi.py" "$SRC/ctn_patch.ko" 2>&1 | tail -12 || true

cp -f "$SRC/ctn_patch.ko" "$WIN_DIR/ctn_patch.ko"
echo
echo "    已拷回 Windows 侧：$WIN_DIR/ctn_patch.ko"
echo "    下一步打包: python build_zip.py"
