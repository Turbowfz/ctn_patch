# SPDX-License-Identifier: GPL-2.0-only
"""校验 kCFI 类型号（type id）是否和设备的编译器一致。

背景：内核开了 CONFIG_CFI_CLANG（6.1 用的是 kCFI）。kCFI 的规则是：
    - 每个函数入口前面 4 字节放一个「类型号」，由函数原型算出来
    - 内核在间接调用点（比如 proc 层调 proc_ops->proc_read）会拿目标函数的
      类型号和自己期望的比，不一致就 brk 崩掉
所以我们的模块里那些会被内核间接调用的回调（proc_read / proc_write），
类型号必须和设备内核一致。类型号是编译器按原型算的，编译器大版本不同
理论上可能算得不一样 —— 这个脚本就是来实锤这件事的。

方法（先做对照实验，再比我们的产物）：
  1. 扫设备全部 432 个 .ko，找名字以 proc_read / proc_write 结尾的函数，
     读它们入口前 4 字节。如果这些函数原型相同，类型号就应该全部相同 ——
     一致就证明「读取方法正确」且「类型号是原型的稳定函数」。
  2. 若 ctn_patch.ko 已生成，读我们两个回调的类型号，和设备的值比。

用法：
    python verify_kcfi.py            # 只做对照实验（看设备侧期望值）
    python verify_kcfi.py ctn_patch.ko   # 顺便比我们的产物
"""

import glob
import os
import struct
import sys
from collections import Counter, defaultdict

DEV_MODDIR = os.environ.get(
    "CTN_DEV_MODDIR",
    r"C:/Users/User/Desktop/风驰/6.1_一加ace3pro/01_手机提取/modules")

SHT_SYMTAB = 2


def elf_sections(d):
    if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
        return None
    e_shoff, = struct.unpack_from("<Q", d, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    if not e_shoff or not e_shnum:
        return None
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        (nameoff, stype, flags, addr, off, size,
         link, info, align, entsize) = struct.unpack_from("<IIQQQQIIQQ", d, o)
        secs.append(dict(nameoff=nameoff, type=stype, addr=addr, off=off,
                         size=size, link=link, entsize=entsize))
    stroff = secs[e_shstrndx]["off"]
    for s in secs:
        e = d.find(b"\0", stroff + s["nameoff"])
        s["name"] = d[stroff + s["nameoff"]:e].decode("latin1")
    return secs


def symbols(d):
    """返回 [(name, shndx, value)]"""
    secs = elf_sections(d)
    if not secs:
        return []
    out = []
    for s in secs:
        if s["type"] != SHT_SYMTAB:
            continue
        strtab = secs[s["link"]]["off"]
        entsize = s["entsize"] or 24
        for i in range(s["size"] // entsize):
            o = s["off"] + i * entsize
            st_name, st_info, st_other, st_shndx = struct.unpack_from("<IBBH", d, o)
            st_value, = struct.unpack_from("<Q", d, o + 8)
            if not st_name:
                continue
            e = d.find(b"\0", strtab + st_name)
            nm = d[strtab + st_name:e].decode("latin1")
            out.append((nm, st_shndx, st_value))
    return out


def kcfi_typeid(d, secs, shndx, value):
    """读函数入口前 4 字节 = kCFI 类型号。"""
    if shndx == 0 or shndx >= len(secs):
        return None
    sec = secs[shndx]
    fileoff = sec["off"] + (value - sec["addr"])
    if fileoff < 4 or fileoff + 4 > len(d):
        return None
    return struct.unpack_from("<I", d, fileoff - 4)[0]


def collect(path, want_suffix=("proc_read", "proc_write")):
    d = open(path, "rb").read()
    secs = elf_sections(d)
    if not secs:
        return {}
    res = {}
    for nm, shndx, value in symbols(d):
        # 名字里带点的（xxx.register_trace / xxx.cold）是编译器生成的轮廓函数，
        # 入口前 4 字节不是类型号，直接跳过
        if "." in nm:
            continue
        if any(nm.endswith(s) for s in want_suffix):
            tid = kcfi_typeid(d, secs, shndx, value)
            if tid is not None:
                res[nm] = tid
    return res


def main():
    ours = sys.argv[1] if len(sys.argv) > 1 else None
    # 设备模块的回调名字都带 _proc_read/_proc_write 后缀；我们的回调名字不带，
    # 所以产物侧用"函数原型"来对：直接比对已知的那两个回调名。
    MINE_READ = "critical_task_name_read"
    MINE_WRITE = "critical_task_name_write"

    print("=" * 74)
    print(" 对照实验：扫设备全部 .ko，看同名原型函数的 kCFI 类型号是否一致")
    print("=" * 74)
    files = sorted(glob.glob(os.path.join(DEV_MODDIR, "*.ko")))
    print(f"扫描 {len(files)} 个设备模块 ...\n")

    by_tid = defaultdict(list)     # 类型号 -> [函数名@模块]
    for f in files:
        for nm, tid in collect(f).items():
            by_tid[tid].append(f"{nm}@{os.path.basename(f)}")

    print(f"共找到 {sum(len(v) for v in by_tid.values())} 个 proc_read/proc_write 回调，"
          f"落在 {len(by_tid)} 个不同的类型号上：\n")
    for tid, names in sorted(by_tid.items(), key=lambda kv: -len(kv[1])):
        print(f"  类型号 0x{tid:08x}   共 {len(names)} 个，例如：")
        for n in names[:4]:
            print(f"        {n}")

    if len(by_tid) == 1:
        print("\n>>> 全部一致：证明读取方法正确，且类型号是原型的稳定函数。")
        print(">>> 期望值（proc_read / proc_write 用同一个类型号，因为形参列表相同）：")
        for tid in by_tid:
            print(f"        0x{tid:08x}")
    else:
        print(f"\n>>> 有 {len(by_tid)} 种类型号。若形参列表确实相同却分了多组，")
        print(">>> 说明我的读取方法有问题（类型号前还有别的字节），需要换读法。")

    if not ours:
        print("\n（未提供我们的 .ko，跳过产物比对）")
        return 0

    print()
    print("=" * 74)
    print(f" 比对产物：{ours}")
    print("=" * 74)
    if not os.path.exists(ours):
        print(f"!! 文件不存在：{ours}")
        return 1

    d = open(ours, "rb").read()
    secs = elf_sections(d)
    if not secs:
        print("!! 不是可解析的 ELF64")
        return 1
    mine = {nm: (shndx, val) for nm, shndx, val in symbols(d)
            if nm in (MINE_READ, MINE_WRITE)}
    if not mine:
        print(f"!! 产物里没找到 {MINE_READ}/{MINE_WRITE} 符号")
        return 1
    expect_read = 0xE866E2F4    # ssize_t (struct file*, char __user*, size_t, loff_t*)
    expect_write = 0x9A660EA0   # ssize_t (struct file*, const char __user*, size_t, loff_t*)
    ok = True
    for nm, (shndx, val) in sorted(mine.items()):
        tid = kcfi_typeid(d, secs, shndx, val)
        want = expect_read if nm == MINE_READ else expect_write
        mark = "OK  " if tid == want else "不一致!"
        if tid != want:
            ok = False
        print(f"  [{mark}] {nm:<34} 0x{tid:08x}（设备同原型期望 0x{want:08x}）")
    print()
    if ok:
        print(">>> 类型号和设备一致 → kCFI 兼容，模块可以安全加载。")
        return 0
    print(">>> 类型号不一致 → 内核间接调用我们的回调时会 CFI failure（崩）。")
    print(">>> 必须换成设备同版本的编译器（clang 17.0.2，即 NDK r26b）重编。")
    return 2


if __name__ == "__main__":
    sys.exit(main())
