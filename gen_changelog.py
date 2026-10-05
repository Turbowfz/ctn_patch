#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""从 CHANGELOG.md 抽出「当前版本那一节」，写成 update-changelog.md。

为什么要有这个文件：
    update.json 里的 changelog 是给**更新弹窗**看的。KernelSU 会把那个地址
    的内容整段当 markdown 渲染出来（源码 ModuleViewModel.kt:437-486，不截断）。
    直接指向 CHANGELOG.md 的话，弹窗里会是「从 v1.0 到最新版」的全量历史 ——
    版本一多发版弹窗就成一大段墙。所以给更新器单独喂一份「只有这一版」的。

    CHANGELOG.md 本身保持完整历史（给人看、给仓库看），不删不改。

文件名为什么叫 update-changelog.md 而不是 changelog.md：
    仓库在 Windows 上，changelog.md 和 CHANGELOG.md 是**同一个文件**（大小写
    不敏感），会互相覆盖。必须取个明显不同的名字。

用法：
    python gen_changelog.py            # 按 module.prop 的版本抽
    python gen_changelog.py v1.1       # 指定版本
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHANGELOG = HERE / "CHANGELOG.md"
PROP = HERE / "magisk" / "module.prop"
OUT = HERE / "update-changelog.md"

HEADER = ("<!-- 由 gen_changelog.py 自动生成，请勿手改。"
          "要改内容请改 CHANGELOG.md 后重跑本脚本。 -->\n")


def current_version() -> str:
    for line in PROP.read_text(encoding="utf-8").splitlines():
        if line.startswith("version="):
            return line.split("=", 1)[1].strip()
    sys.exit("!! module.prop 里找不到 version=")


def extract_section(text: str, version: str) -> str:
    """取 CHANGELOG 里标题含 version 的那一节（到下一个 '## ' 为止）。

    标题形如 '## v1.1（versionCode 11）'。用「含 version 且 version 后紧跟
    非数字字符」来匹配，免得 v1.1 误命中 v1.10 / v1.11。
    """
    pat = re.compile(r"^##[^\n]*%s(?![0-9.])" % re.escape(version), re.M)
    m = pat.search(text)
    if not m:
        return ""
    start = m.start()
    nxt = re.compile(r"^## ", re.M).search(text, m.end())
    return text[start:(nxt.start() if nxt else len(text))].rstrip() + "\n"


def strip_checksums(section: str) -> str:
    """去掉「校验值」那一段（含 ``` 代码块和它上面的小标题）。

    为什么只在这里去、CHANGELOG.md 里留着：
      - 更新弹窗是给「这次更新改了什么」看的。管理器**不校验** sha256，
        用户也不会拿着 64 位哈希去比对，摆在弹窗里就是噪音。
      - 但 CHANGELOG.md 里留着有价值：那是仓库里唯一记着「每一版发出去的
        到底是哪个二进制」的地方（`expected_vendor.txt` 记的是厂商模块，
        不是我们的产物）。要核对下载到的东西对不对，就靠它。
    """
    lines = section.splitlines()
    out = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.strip().startswith("```"):
            j = i + 1
            body = []
            while j < len(lines) and not lines[j].strip().startswith("```"):
                body.append(lines[j])
                j += 1
            if "sha256" in "\n".join(body):
                # 把紧邻上面的「**校验值**」小标题和空行一起去掉
                while out and out[-1].strip() == "":
                    out.pop()
                if out and "校验" in out[-1] and len(out[-1]) < 20:
                    out.pop()
                while out and out[-1].strip() == "":
                    out.pop()
                i = j + 1
                continue
            out.extend(lines[i:j + 1])
            i = j + 1
            continue
        out.append(ln)
        i += 1
    return "\n".join(out).rstrip() + "\n"


def main() -> None:
    version = sys.argv[1] if len(sys.argv) > 1 else current_version()
    if not version.startswith("v"):
        version = "v" + version

    text = CHANGELOG.read_text(encoding="utf-8")
    section = extract_section(text, version)
    if not section:
        sys.exit("!! CHANGELOG.md 里找不到 %s 那一节 —— 先在 CHANGELOG.md 顶部"
                 "把它写上，再跑本脚本" % version)

    stripped = strip_checksums(section)
    dropped = len(section.splitlines()) - len(stripped.splitlines())

    OUT.write_text(HEADER + stripped, encoding="utf-8", newline="\n")
    print("  已生成 %s（%s，%d 行%s）"
          % (OUT.name, version, len(stripped.splitlines()),
             "，去掉了 %d 行校验值" % dropped if dropped else ""))
    print("  更新器看到的就是这一节；CHANGELOG.md 保持完整历史（含校验值）不变")


if __name__ == "__main__":
    main()
