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
        # 指向 update-changelog.md（只有当前这一版），**不是** CHANGELOG.md。
        # 更新弹窗会把这里的内容整段渲染成 markdown（KernelSU 的
        # ModuleViewModel.kt:437-486，不截断），喂全量历史会变成一大段墙。
        # 那个文件由 gen_changelog.py 从 CHANGELOG.md 里抽出来生成。
        "changelog": "%s/raw/main/update-changelog.md" % GITEE,
    }
    open(UPD, "w", encoding="utf-8", newline="\n").write(
        json.dumps(d, ensure_ascii=False, indent=2) + "\n")
    return d


def sync_changelog():
    """调 gen_changelog.py 生成 update-changelog.md（失败不致命，只提醒）。"""
    import subprocess
    r = subprocess.run([sys.executable, os.path.join(HERE, "gen_changelog.py")],
                       capture_output=True, text=True)
    if r.returncode == 0:
        print(r.stdout.rstrip())
        return True
    print("  !! update-changelog.md 没生成（%s）" % r.stdout.strip() or r.stderr.strip())
    print("     先在 CHANGELOG.md 顶部补上这一版，再跑 python gen_changelog.py")
    return False


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
            print("  changelog   : %s" % u["changelog"])
            same = (u["version"] == ver and u["versionCode"] == vcode)
            print("  两者一致    :", "是" if same else "!!! 否 —— 跑一次 --sync 修好")
        return

    if args[0] == "--sync":
        # 不改版本号，只按 module.prop 现有的版本重写 update.json，
        # 并顺手重新生成 update-changelog.md。改过 update.json 的字段
        # （比如换了 changelog 地址）之后用它对齐，不用为了改个地址硬升版本。
        lines, ver, vcode = read_prop()
        d = write_update(ver, vcode)
        print("  按 module.prop 重写 update.json：version=%s versionCode=%d" % (ver, vcode))
        print("    zipUrl    : %s" % d["zipUrl"])
        print("    changelog : %s" % d["changelog"])
        sync_changelog()
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
    print("    2. python gen_changelog.py    （抽出这一版喂给更新弹窗）")
    print("    3. python build_zip.py")
    print("    4. git add -A && git commit -m '%s' && git push gitee main && git push github main" % new_ver)
    print("    5. git tag -a %s -m '%s' && git push gitee %s && git push github %s" % (new_ver, new_ver, new_ver, new_ver))
    print("    6. GITEE_TOKEN=xxx python make_release.py   （建 Release + 传附件）")
    print("       （zip 地址必须是 %s 这种形式，否则管理器下载会 404）" % d["zipUrl"])
    print()
    print("  ★ 第 2 步别漏：update.json 的 changelog 指向 update-changelog.md，"
          "漏了的话更新弹窗里的更新日志是上一版的内容。")
    # 顺手试一次：如果 CHANGELOG 里已经有这一版，直接生成掉
    sync_changelog()


if __name__ == "__main__":
    main()
