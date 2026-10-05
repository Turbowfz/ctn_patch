#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""版本号同步工具：改一处，module.prop 与 update.json 一起更新。

云更新靠两个文件配合，手工改很容易漏一个：
  - magisk/module.prop 的 version / versionCode  ← 装在手机上的版本
  - update.json 的 version / versionCode         ← 管理器拉取的远端版本
管理器比较两者的 versionCode，远端更大就提示更新。versionCode 必须递增。

用法：
    python bump.py 1.1              # 版本改成 v1.1，versionCode 自动 +1
    python bump.py 1.1 20           # 同时指定 versionCode
    python bump.py --show           # 只看当前版本
    python bump.py 1.1 --zip-url <url>   # 指定新的 zip 下载地址

改完后按提示构建 + 发 Release（zip 地址依赖 tag，所以要先确定 tag 名）。
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROP = os.path.join(HERE, "magisk", "module.prop")
UPD = os.path.join(HERE, "update.json")

GITEE = "https://gitee.com/turbowfz/ctn_patch"


def read_prop():
    """返回 (行列表, version, versionCode)"""
    lines = open(PROP, encoding="utf-8").read().splitlines()
    ver = vcode = None
    for l in lines:
        if l.startswith("version="):
            ver = l.split("=", 1)[1]
        elif l.startswith("versionCode="):
            vcode = int(l.split("=", 1)[1])
    if ver is None or vcode is None:
        sys.exit("!! module.prop 里找不到 version / versionCode")
    return lines, ver, vcode


def set_prop(lines, ver, vcode):
    out = []
    for l in lines:
        if l.startswith("version="):
            out.append("version=" + ver)
        elif l.startswith("versionCode="):
            out.append("versionCode=%d" % vcode)
        else:
            out.append(l)
    open(PROP, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")


def write_update(ver, vcode, zip_url=None):
    tag = ver  # 约定：tag 名 == version
    if zip_url is None:
        zip_url = "%s/releases/download/%s/ctn_patch.zip" % (GITEE, tag)
    d = {
        "version": ver,
        "versionCode": vcode,
        "zipUrl": zip_url,
        "changelog": "%s/raw/main/CHANGELOG.md" % GITEE,
    }
    open(UPD, "w", encoding="utf-8", newline="\n").write(
        json.dumps(d, ensure_ascii=False, indent=2) + "\n")
    return d


def main():
    args = [a for a in sys.argv[1:]]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return
    if args[0] == "--show":
        _, ver, vcode = read_prop()
        print("  module.prop : version=%s  versionCode=%d" % (ver, vcode))
        if os.path.exists(UPD):
            u = json.load(open(UPD, encoding="utf-8"))
            print("  update.json : version=%s  versionCode=%d" % (u["version"], u["versionCode"]))
            print("  zipUrl      : %s" % u["zipUrl"])
            same = (u["version"] == ver and u["versionCode"] == vcode)
            print("  两者一致    :", "是" if same else "!!! 否 —— 跑一次 bump.py 修好")
        return

    zip_url = None
    if "--zip-url" in args:
        i = args.index("--zip-url")
        zip_url = args[i + 1]
        del args[i:i + 2]

    new_ver = args[0]
    if not re.fullmatch(r"v?\d+(\.\d+)*", new_ver):
        sys.exit("!! 版本号格式不对（示例：1.1 或 v1.1）")
    if not new_ver.startswith("v"):
        new_ver = "v" + new_ver

    lines, old_ver, old_code = read_prop()
    new_code = int(args[1]) if len(args) > 1 else old_code + 1
    if new_code <= old_code:
        sys.exit("!! versionCode 必须递增（当前 %d，你给的是 %d）" % (old_code, new_code))

    set_prop(lines, new_ver, new_code)
    d = write_update(new_ver, new_code, zip_url)

    print("  版本: %s(%d) → %s(%d)" % (old_ver, old_code, new_ver, new_code))
    print("  module.prop 已更新")
    print("  update.json 已更新")
    print("    zipUrl    : %s" % d["zipUrl"])
    print("    changelog : %s" % d["changelog"])
    print()
    print("  接下来：")
    print("    1. 在 CHANGELOG.md 顶部补一段 %s 的说明" % new_ver)
    print("    2. python build_zip.py")
    print("    3. git add -A && git commit -m '%s' && git push github main && git push gitee main" % new_ver)
    print("    4. 在 Gitee/GitHub 建 %s 的 Release，把 ctn_patch.zip 传为附件" % new_ver)
    print("       （zip 地址必须是 %s 这种形式，否则管理器下载会 404）" % d["zipUrl"])


if __name__ == "__main__":
    main()
