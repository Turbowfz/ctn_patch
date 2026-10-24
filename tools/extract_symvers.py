# SPDX-License-Identifier: GPL-2.0-only
"""从设备自带的 .ko 里把 __versions 段（符号名 + CRC）全导出来，
汇成一份可用的 Module.symvers，并检查我们模块需要的那批符号覆盖率。

原理：内核开了 CONFIG_MODVERSIONS 时，每个模块都会把「它用到的每个外部符号」
的名字和 CRC 记在自己的 __versions 段里。设备上有 432 个模块，合起来基本覆盖了
普通模块会用到的大部分导出符号。这样就不必为了拿 Module.symvers 去整编一遍内核。

__versions 段元素结构（6.1）：
    struct modversion_info { unsigned long crc; char name[MODULE_NAME_LEN]; };
MODULE_NAME_LEN 在 6.1 是 56，所以一条 64 字节；这里两种长度都试，按段大小整除判断。
"""

import glob
import os
import re
import struct
import sys
from collections import defaultdict

MODDIR = r"C:/Users/User/Desktop/风驰/6.1_一加ace3pro/01_手机提取/modules"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Module.symvers.device")

# 本模块（ctn_patch.c）用到的外部符号。resolve 成 kallsyms 的不算 import，
# 但仍列出来以便核对；标 x 的表示我们依赖它必须能解析。
NEEDED = [
    # 直接 import（链接期必须找到 CRC）
    "register_kprobe", "unregister_kprobe",
    "proc_create_data", "proc_remove", "default_llseek",
    "simple_read_from_buffer",
    "vmap", "vunmap", "vmalloc_to_page",
    "copy_from_kernel_nofault", "copy_from_user",
    "synchronize_rcu", "try_module_get", "module_put",
    "strscpy", "memchr", "memcpy", "memset", "strlen", "strncmp", "scnprintf",
    "mutex_lock", "mutex_unlock", "printk", "_printk",
    "min", "memcmp",
]


def elf_sections(data):
    """返回 [(name, offset, size)]，只支持 64 位小端 ELF。"""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return []
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if not e_shoff or not e_shnum:
        return []
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        nameoff, _type, _flags, _addr, sh_off, sh_size = struct.unpack_from("<IIQQQQ", data, off)
        secs.append((nameoff, sh_off, sh_size))
    stroff = secs[e_shstrndx][1]
    out = []
    for nameoff, sh_off, sh_size in secs:
        end = data.find(b"\0", stroff + nameoff)
        nm = data[stroff + nameoff:end].decode("latin1")
        out.append((nm, sh_off, sh_size))
    return out


def parse_versions(data):
    """导出 __versions 里的 (name, crc)。"""
    for nm, off, size in elf_sections(data):
        if nm != "__versions":
            continue
        for entsz in (64, 32):  # 6.1 是 64；老的 32 位/旧内核是 32
            if size % entsz:
                continue
            res = []
            ok = True
            for i in range(size // entsz):
                base = off + i * entsz
                crc, = struct.unpack_from("<Q", data, base)
                raw = data[base + 8:base + entsz]
                n = raw.split(b"\0")[0].decode("latin1")
                if not n or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]*", n):
                    ok = False
                    break
                res.append((n, crc))
            if ok and res:
                return res
        return []
    return []


def main():
    files = sorted(glob.glob(os.path.join(MODDIR, "*.ko")))
    print(f"扫描 {len(files)} 个设备模块 ...")

    table = {}          # symbol -> (crc, 第一个见到的模块)
    per_sym_count = defaultdict(int)
    conflict = defaultdict(set)

    for f in files:
        data = open(f, "rb").read()
        for name, crc in parse_versions(data):
            if name in table:
                if table[name][0] != crc:
                    conflict[name].add((table[name][0], crc))
            else:
                table[name] = (crc, os.path.basename(f))
            per_sym_count[name] += 1

    print(f"共收集到 {len(table)} 个符号的 CRC")

    # 冲突检查：同名符号 CRC 不一致 = 解析错了或真有多个版本
    if conflict:
        print(f"\n[!] 有 {len(conflict)} 个符号 CRC 冲突，前 10 个：")
        for k in list(conflict)[:10]:
            print("   ", k, sorted(conflict[k]))
    else:
        print("CRC 一致性检查通过（同名符号在所有模块里 CRC 相同）")

    with open(OUT, "w", encoding="utf-8", newline="\n") as fp:
        for sym, (crc, _src) in sorted(table.items()):
            fp.write(f"0x{crc:08x}\t{sym}\tEXPORT_SYMBOL\t\n")
    print(f"\n已写出 {OUT}")

    # 覆盖率检查
    print("\n=== 本模块需要的符号覆盖率 ===")
    miss = []
    for s in NEEDED:
        if s in table:
            print(f"  [OK]   {s:<26} crc=0x{table[s][0]:08x}  "
                  f"({per_sym_count[s]} 个模块引用)")
        else:
            print(f"  [MISS] {s}")
            miss.append(s)
    print(f"\n缺 {len(miss)} 个: {miss}")
    return 0 if not miss else 1


if __name__ == "__main__":
    sys.exit(main())
