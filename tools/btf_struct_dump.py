# SPDX-License-Identifier: GPL-2.0-only
"""解析设备内核的 BTF，dump 出 struct module 的真实成员布局。
设备 /sys/kernel/btf/vmlinux 是 CONFIG_DEBUG_INFO_BTF=y 的产物，含运行内核全部结构体定义。
BTF 格式（v1）：
  header: magic(2)=0xeb9f ver(1) flags(1) hdr_len(4) type_off(4) type_len(4) str_off(4) str_len(4)
  type:   name_off(4) info(4) size_or_type(4)  ；info = vlen:16 | unused:8 | kind:5 | kind_flag:1
  struct 成员: name_off(4) type(4) offset(4)
"""
import struct
import sys

KIND_STRUCT = 4
KIND_UNION = 5


def parse(path):
    d = open(path, "rb").read()
    magic, ver, flags, hdr_len, type_off, type_len, str_off, str_len = struct.unpack_from(
        "<HBBIIIII", d, 0)
    assert magic == 0xEB9F, f"BTF magic 不对: {magic:#x}"
    base = hdr_len
    types = d[base + type_off: base + type_off + type_len]
    strs = d[base + str_off: base + str_off + str_len]

    def s(off):
        e = strs.find(b"\0", off)
        return strs[off:e].decode("utf-8", "replace")

    out = []
    p = 0
    idx = 1
    while p < len(types):
        name_off, info, size_type = struct.unpack_from("<III", types, p)
        p += 12
        vlen = info & 0xFFFF
        kind = (info >> 24) & 0x1F
        kflag = (info >> 31) & 1
        members = []
        if kind in (KIND_STRUCT, KIND_UNION):
            for _ in range(vlen):
                m_name, m_type, m_off = struct.unpack_from("<III", types, p)
                p += 12
                members.append((s(m_name), m_type, m_off, kflag))
        elif kind == 1:      # INT
            p += 4
        elif kind == 3:      # ARRAY
            p += 12
        elif kind in (6, 13, 14, 15):  # ENUM/FUNC_PROTO/VAR/DATASEC
            if kind == 6:
                p += vlen * 8
            elif kind == 13:
                p += vlen * 8
            elif kind == 14:
                p += 4
            elif kind == 15:
                p += vlen * 12
        out.append(dict(idx=idx, name=s(name_off), kind=kind, vlen=vlen,
                        size=size_type, members=members))
        idx += 1
    return out


def main():
    btf = parse(sys.argv[1])
    tgt = [t for t in btf if t["kind"] == KIND_STRUCT and t["name"] == "module"]
    if not tgt:
        print("!! BTF 里没有 struct module")
        return
    t = tgt[0]
    print(f"=== 设备内核 BTF 里的 struct module ===")
    print(f"size = {t['size']} 字节   成员数 = {t['vlen']}")
    print()
    for name, mtype, moff, kflag in t["members"]:
        off = (moff & 0xFFFFFF) // 8 if kflag else moff // 8
        # 位域：kflag=1 时高 8 位是位宽
        print(f"  +{off:5d}  {name}")


if __name__ == "__main__":
    main()
