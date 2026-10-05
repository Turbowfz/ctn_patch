#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ★ 最终构建配方 ★
#   配置：忠实设备（BTF/BTF_MODULES/WERROR 全保留），只改三处且都无副作用：
#     - UNUSED_KSYMS_WHITELIST=""      （设备构建机路径不存在）
#     - LOCALVERSION="-android14-11-o-gdc1b6a03413f" + 关 LOCALVERSION_AUTO
#       （为了 vermagic 精确；MODULE_SCMVERSION 虽随之关闭但不影响 struct module）
#     - 空 .scmversion                  （去掉 setlocalversion 附加的 + 号）
#   校验：struct module 段大小必须与设备模块一致（这就是上次崩机的根因）
set -uo pipefail
NDK=/opt/ndkroot/android-ndk-r26b/toolchains/llvm/prebuilt/linux-x86_64
export PATH="$NDK/bin:$PATH"
K=/root/ctn_build/kernel
O=/root/ctn_build/work/out
SRC=/root/ctn_build/src
WIN=/mnt/c/Users/User/Desktop/风驰/6.1_一加ace3pro/05_补丁模块/ctn_patch
DEVMOD="$WIN/../01_手机提取/modules/oplus_bsp_game_opt.ko"

echo "############ 1. 铺配置 ############"
sed 's/\r$//' "$WIN/device_kernel.config" > "$O/.config"
python3 - "$O/.config" <<'PYEOF'
import re, sys
p = sys.argv[1]; txt = open(p, encoding="utf-8").read()
def setval(t, k, v):
    pe = re.compile(r'^%s=.*$' % re.escape(k), re.M)
    pn = re.compile(r'^# %s is not set$' % re.escape(k), re.M)
    if pe.search(t): return pe.sub('%s=%s' % (k, v), t, count=1)
    if pn.search(t): return pn.sub('%s=%s' % (k, v), t, count=1)
    return t + '\n%s=%s\n' % (k, v)
def unset(t, k):
    return re.sub(r'^%s=.*$' % re.escape(k), '# %s is not set' % k, t, count=1, flags=re.M)
txt = setval(txt, 'CONFIG_LOCALVERSION', '"-android14-11-o-gdc1b6a03413f"')
txt = unset(txt, 'CONFIG_LOCALVERSION_AUTO')
txt = setval(txt, 'CONFIG_UNUSED_KSYMS_WHITELIST', '""')
# BTF / BTF_MODULES / WERROR / MODULE_SIG_PROTECT 等一律保留设备值！
open(p, "w", encoding="utf-8", newline="\n").write(txt)
print("  已改：LOCALVERSION + 关 AUTO + 白名单置空；BTF 等全部保留设备值")
PYEOF

echo
echo "############ 2. olddefconfig + modules_prepare ############"
make -C "$K" O="$O" ARCH=arm64 LLVM=1 CC="$NDK/bin/clang" \
	HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld olddefconfig >/dev/null 2>&1
make -C "$K" O="$O" ARCH=arm64 LLVM=1 CC="$NDK/bin/clang" \
	HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld -j"$(nproc)" modules_prepare >/tmp/prep.log 2>&1
echo "  modules_prepare rc=$?（resolve_btfids 失败不影响模块）"
for k in CONFIG_DEBUG_INFO_BTF_MODULES CONFIG_DEBUG_INFO_BTF CONFIG_WERROR; do
	printf "  autoconf %-36s " "$k"
	grep -q "define $k" "$O/include/generated/autoconf.h" && echo 已定义 || echo 未定义
done
echo "  kernel.release = $(cat "$O/include/config/kernel.release" 2>/dev/null)"

echo
echo "############ 3. 同步源码 + symvers ############"
rm -rf "$SRC"; mkdir -p "$SRC"; cp -a "$WIN/." "$SRC/"
cd "$SRC"; find . -maxdepth 1 -type f -print0 | xargs -0 -r sed -i 's/\r$//'
rm -f ctn_patch.ko ctn_patch.mod ctn_patch.mod.c ctn_patch.mod.o ctn_patch.o Module.symvers modules.order
python3 - "$SRC/Module.symvers.device" "$O/Module.symvers" <<'PYEOF'
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
print(f"  symvers {len(out)} 条（module 列=vmlinux）")
PYEOF

echo
echo "############ 4. 编模块 ############"
make -C "$K" O="$O" M="$SRC" ARCH=arm64 LLVM=1 CC="$NDK/bin/clang" \
	HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld -j"$(nproc)" modules 2>&1 | tail -5
[ -f "$SRC/ctn_patch.ko" ] || { echo "  !! 编译失败"; exit 1; }
cp -f "$SRC/ctn_patch.ko" "$WIN/ctn_patch.ko"
echo "  已拷回 Windows 侧"

echo
echo "############ 5. 校验（在 WSL 侧读 /mnt/c 路径）############"
python3 - "$DEVMOD" "$WIN/ctn_patch.ko" <<'PYEOF'
import struct, sys
def sec(path, want):
    d = open(path, "rb").read()
    e_shoff,=struct.unpack_from("<Q",d,0x28)
    ents,num,strx=struct.unpack_from("<HHH",d,0x3A)
    S=[]
    for i in range(num):
        o=e_shoff+i*ents
        v=struct.unpack_from("<IIQQQQIIQQ",d,o)
        S.append(dict(n=v[0],off=v[4],size=v[5]))
    stroff=S[strx]["off"]
    for s in S:
        e=d.find(b"\0",stroff+s["n"]); s["name"]=d[stroff+s["n"]:e].decode()
    for s in S:
        if s["name"]==want: return s["size"]
    return None
a = sec(sys.argv[1], ".gnu.linkonce.this_module")
b = sec(sys.argv[2], ".gnu.linkonce.this_module")
print(f"  设备 oplus_bsp_game_opt.ko : {a} 字节")
print(f"  我们 ctn_patch.ko          : {b} 字节")
print("  " + ("★★ struct module 布局一致 —— 加载安全" if a==b else "!!!! 不一致 —— 绝不能刷，会崩"))

# .modinfo
d = open(sys.argv[2],"rb").read()
e_shoff,=struct.unpack_from("<Q",d,0x28)
ents,num,strx=struct.unpack_from("<HHH",d,0x3A)
S=[]
for i in range(num):
    o=e_shoff+i*ents
    v=struct.unpack_from("<IIQQQQIIQQ",d,o)
    S.append(dict(n=v[0],off=v[4],size=v[5]))
stroff=S[strx]["off"]
for s in S:
    e=d.find(b"\0",stroff+s["n"]); s["name"]=d[stroff+s["n"]:e].decode()
print()
print("  .modinfo:")
for s in S:
    if s["name"]==".modinfo":
        for x in d[s["off"]:s["off"]+s["size"]].split(b"\0"):
            if x:
                t=x.decode('latin1')
                mark="   <<< 依赖应为空" if t.startswith("depends=") and t!="depends=" else ""
                print(f"    {t}{mark}")
PYEOF

echo
echo "############ 6. vermagic ############"
grep -aom1 'vermagic=[ -~]*' "$WIN/ctn_patch.ko" | sed 's/^/  /'
