#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# 把内核源码树里断掉的符号链接重指到 Windows 侧的 modules 仓（本地稀疏检出），
# 再补齐裸 source 缺失的 Kconfig，然后跑 olddefconfig。
#
# 背景：OnePlusOSS 的内核仓里，凡是 Oplus 私有的东西都留成一个符号链接指向
#   ../../../vendor/oplus/kernel/... —— 那个 vendor/ 目录在实际构建树里来自
#   modules 仓（android_kernel_modules_and_devicetree_oneplus_sm8650），
#   单独克隆内核仓时这些链接全是断的，kconfig 报
#   "can't open file kernel/oplus_cpu/sched/Kconfig" 就是这么来的。
#
# 做法：把断链换成指向 /mnt/c/.../src_original_modules/vendor/... 的链接。
#       /mnt/c 是 9p 慢盘，但链接本身只是编译时 include 路径，不影响速度。
set -uo pipefail

MODROOT="/mnt/c/Users/User/Desktop/风驰/6.1_一加ace3pro/03_源码重建/src_original_modules"
KERNEL=/root/ctn_build/kernel
NDK="/opt/ndkroot/android-ndk-r26b/toolchains/llvm/prebuilt/linux-x86_64"
export PATH="$NDK/bin:$PATH"
OUT=/root/ctn_build/work/out

[ -d "$MODROOT/vendor" ] || { echo "!! 找不到 modules 仓: $MODROOT"; exit 1; }

cd "$KERNEL" || exit 1

echo "############ 1. 重指断链 ############"
python3 - <<PYEOF
import os

MODROOT = "$MODROOT"   # modules 仓根（vendor/ 在它下面）
fixed = kept = still_bad = 0

for root, dirs, files in os.walk(".", topdown=True):
    for name in dirs + files:
        p = os.path.join(root, name)
        try:
            if not os.path.islink(p):
                continue
            t = os.readlink(p)
            full = t if os.path.isabs(t) else os.path.join(os.path.dirname(p), t)
            if os.path.exists(full):
                kept += 1
                continue
        except OSError:
            continue

        # 断链：按"内核树相对路径 -> 仓库相对路径"的层数关系重算
        #   内核树 kernel/oplus_cpu -> ../../../vendor/oplus/kernel/cpu
        #   意思是从 kernel/ 往上 3 级到树根，再进 vendor/...
        #   所以目标是 <MODROOT>/vendor/oplus/kernel/cpu
        # 通用规则：链接目标是 <kernel>/<相对层级>/vendor/<rest>
        #           rest = t 里最后一个 "vendor/" 之后的部分
        rest = None
        idx = t.rfind("vendor/")
        if idx < 0:
            # qcom 的链接是 .../vendor/qcom/... 同样处理
            idx = t.rfind("qcom/")
            if idx >= 0:
                rest = "qcom/" + t[idx + len("qcom/"):]
        else:
            rest = t[idx:]

        if rest is None:
            print("  跳过(无vendor/qcom前缀):", p, "->", t)
            still_bad += 1
            continue

        new_target = os.path.join(MODROOT, rest)
        if not os.path.exists(new_target):
            print("  目标仍不存在:", p, "->", new_target)
            still_bad += 1
            continue

        os.unlink(p)
        os.symlink(new_target, p)
        fixed += 1
        print(f"  重指 {p}\n        -> {new_target}")

print(f"\n共 {fixed} 个重指，{kept} 个本来就好，{still_bad} 个仍缺失")
PYEOF

echo
echo "############ 2. 补齐裸 source 的缺失 Kconfig ############"
python3 - <<'PYEOF'
import re, os, shutil, glob

TARGETS = [
    "mm/mm_osvelte/Kconfig",
    "drivers/base/kernelFwUpdate/Kconfig",
    "drivers/base/touchpanel_notify/Kconfig",
    "drivers/misc/vibrator/aw8697_haptic/Kconfig",
    "drivers/misc/vibrator/si_haptic/Kconfig",
    "drivers/misc/vibrator/oplus_haptic/Kconfig",
    "kernel/oplus_cpu/uad/Kconfig",
    "drivers/oplus_inject/Kconfig",
    "kernel/oplus_cpu/sched/Kconfig",
]

for f in set(glob.glob("**/Kconfig*", recursive=True) + ["Kconfig"]):
    if not os.path.isfile(f) or f.endswith(".orig_ctnpatch"):
        continue
    try:
        txt = open(f, errors="ignore").read()
    except Exception:
        continue
    changed = False
    for t in TARGETS:
        if os.path.isfile(t):
            continue                      # 已经有真实文件（重指后）就不动
        pat = re.compile(r'^(?P<i>\s*)source\s+"' + re.escape(t) + r'"\s*$', re.M)
        if pat.search(txt):
            txt = pat.sub(lambda m: m.group("i") + "# [ctn_patch 修补] 缺 " + t
                                    + "\n" + m.group("i") + '# source "' + t + '"',
                          txt, count=1)
            changed = True
    if changed:
        if not os.path.exists(f + ".orig_ctnpatch"):
            shutil.copy(f, f + ".orig_ctnpatch")
        open(f, "w").write(txt)
        print("  修补:", f)

for t in TARGETS:
    if not os.path.isfile(t):
        os.makedirs(os.path.dirname(t) or ".", exist_ok=True)
        open(t, "w").write("# empty (ctn_patch stub)\n")
        print("  建空:", t)
PYEOF

echo
echo "############ 3. olddefconfig ############"
mkdir -p "$OUT"
make -C "$KERNEL" O="$OUT" ARCH=arm64 LLVM=1 \
	CC="$NDK/bin/clang" HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld \
	olddefconfig 2>&1 | tail -8

echo
echo "############ 4. kernel.release ############"
REL=$(cat "$OUT/include/config/kernel.release" 2>/dev/null)
echo "    kernel.release = ${REL:-（未生成）}"
echo "    期望           = 6.1.141-android14-11-o-gdc1b6a03413f"
[ "$REL" = "6.1.141-android14-11-o-gdc1b6a03413f" ] && echo "    [OK] 一致" || echo "    !! 不一致"
