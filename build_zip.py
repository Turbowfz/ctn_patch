#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""把 magisk/ 目录打包成可刷入的 root 模块 zip（Magisk / KernelSU 通用）。

用法（在 05_补丁模块/ctn_patch/ 下）:
    python build_zip.py                     # 自动找当前目录的 ctn_patch.ko
    python build_zip.py /path/to/ctn_patch.ko
    python build_zip.py --out 自定义名字.zip

要点:
  - Magisk 要求 module.prop 在 zip 根目录（不能多套一层文件夹），这里直接平铺。
  - 所有文本文件强制转成 LF —— Windows 下编辑的 sh 脚本带 CRLF 到手机上
    会报 "no such file or directory" 之类的怪错，这里在打包时归一掉。
  - 没找到 ctn_patch.ko 也能打包（会警告），装的时候 customize.sh 会拦住
    并提示先编译，防止误刷空模块。
"""

import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAGISK = HERE / "magisk"

# 文件名 -> (来源目录候选, 是否可执行位, 是否必须存在)
# verify.sh 在工程根目录（和 ctn_patch.c 同级），打包时一起带上，
# 这样装进模块后 action.sh 才能直接调它。
FILES = {
    "module.prop": (MAGISK, False, True),
    "customize.sh": (MAGISK, True, True),
    "service.sh": (MAGISK, True, True),
    "uninstall.sh": (MAGISK, True, True),
    "action.sh": (MAGISK, True, True),
    "verify.sh": (HERE, True, True),
    "README.md": (MAGISK, False, False),
    "LICENSE": (HERE, False, True),   # GPL 要求：分发二进制须随附许可
    "ctn_patch.ko": (None, False, True),  # 编译产物，单独找
    # ctnd：没有它节点永远停在默认值（HAL 不写这个节点）
    "ctnd": (HERE / "daemon", True, True),
}

TEXT_SUFFIXES = {".sh", ".prop", ".md", ".txt", ".example"}


def find_ko() -> Path | None:
    if len(sys.argv) > 1 and not sys.argv[1].startswith("--"):
        p = Path(sys.argv[1])
        if not p.is_file():
            sys.exit(f"指定的 .ko 不存在: {p}")
        return p
    for cand in (HERE / "ctn_patch.ko", MAGISK / "ctn_patch.ko"):
        if cand.is_file():
            return cand
    return None


def main() -> None:
    out_name = "ctn_patch.zip"
    if "--out" in sys.argv:
        out_name = sys.argv[sys.argv.index("--out") + 1]
    out = HERE / out_name

    ko = find_ko()

    missing = []
    for n, (src_dir, _, must) in FILES.items():
        if not must or n == "ctn_patch.ko":
            continue
        cands = [Path(src_dir) / n if isinstance(src_dir, str) else src_dir / n,
                 MAGISK / n, HERE / n]
        if not any(c.is_file() for c in cands):
            missing.append(n)
    if missing:
        sys.exit(f"缺文件: {missing}")

    if ko is None:
        print("[警告] 没找到 ctn_patch.ko —— 照样打包，但刷入时 customize.sh "
              "会拒绝安装。请先按 README 编译 .ko 再重新打包。")

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, (srcdir, executable, _) in FILES.items():
            if name == "ctn_patch.ko":
                src = ko
                if src is None:
                    continue
            else:
                if isinstance(srcdir, str):
                    src = Path(srcdir) / name
                else:
                    src = srcdir / name
                if not src.is_file():
                    src = MAGISK / name
                    if not src.is_file():
                        src = HERE / name
            mode = 0o755 if executable else 0o644
            info = zipfile.ZipInfo(name)
            # create_system=3 表示「这个 zip 是 Unix 上打的」。少了它，条目会被
            # 当成 DOS 文件，解包器（KernelSU 用的 Info-ZIP unzip）就只看 DOS
            # 属性、无视下面那个 Unix 权限位 —— 可执行位会整条丢掉，ctnd 解出来
            # 是 0644，daemon 根本起不来（真机上踩过）。带 #! 的脚本侥幸没事，
            # 是因为管理器会给有 shebang 的文件补 0755；裸二进制没有这层照顾。
            info.create_system = 3
            info.external_attr = (mode << 16) | 0o100000  # 常规文件
            data = src.read_bytes()
            if src.suffix in TEXT_SUFFIXES:
                # 归一 LF：去掉 \r，统一 \n 结尾
                text = data.decode("utf-8")
                text = text.replace("\r\n", "\n").replace("\r", "\n")
                data = text.encode("utf-8")
            z.writestr(info, data)
            print(f"  + {name}  ({len(data)} 字节, {'0755' if executable else '0644'})")

    # 回读校验：确认写进去的权限位真的能读出来，而且 create_system=3。
    # 这一步是为了挡住上面那个坑 —— 权限位写错了 zip 照样打得出来，
    # 只有装到手机上才会以「daemon 起不来」的形式暴露，太晚。
    bad = []
    with zipfile.ZipFile(out) as z:
        for name, (_, executable, _) in FILES.items():
            try:
                info = z.getinfo(name)
            except KeyError:
                continue
            want = 0o755 if executable else 0o644
            got = (info.external_attr >> 16) & 0o7777
            if info.create_system != 3 or got != want:
                bad.append(f"{name}: create_system={info.create_system} mode={got:o}（要 3/{want:o}）")
    if bad:
        sys.exit("!! 权限位写错了（解包器会丢掉可执行位）:\n  " + "\n  ".join(bad))

    print(f"\n完成: {out}")
    print("刷入: Magisk/KSU App -> 模块 -> 从存储安装 -> 选这个 zip -> 重启")


if __name__ == "__main__":
    main()
