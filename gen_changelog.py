#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""从简短完整的 CHANGELOG.md 生成更新弹窗与包内历史。

update-changelog.md 只含所选版本；magisk/changelog.txt 保留全部版本，最新在前。
每版只允许 1–4 条短要点，详细旧记录保留在 Git 历史，不另存长日志副本。
用法：python gen_changelog.py [v3.1]，或 python gen_changelog.py --version v3.1。
省略版本时读取 magisk/module.prop；两个输出在全部验证通过后才写入。
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import NamedTuple

HERE = Path(__file__).resolve().parent
CHANGELOG = HERE / "CHANGELOG.md"
PROP = HERE / "magisk" / "module.prop"
OUT = HERE / "update-changelog.md"
PACKAGE_OUT = HERE / "magisk" / "changelog.txt"

HEADER = ("<!-- 由 gen_changelog.py 自动生成，请勿手改。"
          "要改内容请改 CHANGELOG.md 后重跑本脚本。 -->\n")
REQUIRED_HISTORY = {
    **{f"v1.{minor}": 10 + minor for minor in range(10)},
    "v2.0": 20, "v2.1": 21, "v2.2": 22, "v3.0": 30, "v3.1": 31,
}
MAX_BULLETS = 4
MAX_BULLET_CHARS = 120
MAX_BULLET_BYTES = 400
SECTION_HEADER = re.compile(r"## (v([1-9][0-9]*)\.([0-9]))（versionCode ([1-9][0-9]*)）")
CHECKSUM = re.compile(r"(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])", re.I)


class ChangelogSection(NamedTuple):
    version: str
    version_code: int
    bullets: tuple[str, ...]  # 不含 '- ' 前缀。


def current_version() -> str:
    for line in PROP.read_text(encoding="utf-8").splitlines():
        if line.startswith("version="):
            return line.split("=", 1)[1].strip()
    sys.exit("!! module.prop 里找不到 version=")


def parse_changelog(text: str) -> dict[str, ChangelogSection]:
    """校验完整简短历史，返回按版本降序的 version -> code/bullets 映射。

    禁止重复版本、错版号、长段落、校验值及超长要点；历史版本不可丢失。
    未来版本沿用 v主版本.个位次版本、versionCode = 主版本 * 10 + 次版本。
    """
    sections: dict[str, ChangelogSection] = {}
    version = ""
    code = 0
    bullets: list[str] = []
    previous: tuple[int, int] | None = None
    title_seen = False

    def finish() -> None:
        if version:
            if not bullets:
                raise ValueError(f"{version} 必须包含 1–{MAX_BULLETS} 条要点")
            sections[version] = ChangelogSection(version, code, tuple(bullets))

    for line in text.splitlines():
        if not line.strip():
            continue
        if line == "# 更新日志" and not version and not title_seen:
            title_seen = True
            continue
        match = SECTION_HEADER.fullmatch(line)
        if match:
            finish()
            version, major, minor, code_text = match.groups()
            if version in sections:
                raise ValueError(f"重复版本标题: {version}")
            key = (int(major), int(minor))
            code = int(code_text)
            if code != key[0] * 10 + key[1]:
                raise ValueError(f"{version} 的 versionCode 不正确: {code}")
            if previous is not None and key >= previous:
                raise ValueError("版本必须按数字降序排列，最新在前")
            previous = key
            bullets = []
            continue
        if not version or not line.startswith("- "):
            raise ValueError(f"只允许统一版本标题与短要点: {line}")
        bullet = line[2:]
        if not bullet or bullet != bullet.strip():
            raise ValueError(f"{version} 包含空要点或多余空白")
        if len(bullet) > MAX_BULLET_CHARS or len(line.encode("utf-8")) > MAX_BULLET_BYTES:
            raise ValueError(f"{version} 要点过长（最多 {MAX_BULLET_CHARS} 字 / {MAX_BULLET_BYTES} 字节）")
        if CHECKSUM.search(bullet):
            raise ValueError(f"{version} 不应包含 64 位校验值")
        bullets.append(bullet)
        if len(bullets) > MAX_BULLETS:
            raise ValueError(f"{version} 最多 {MAX_BULLETS} 条要点")
    finish()
    missing = REQUIRED_HISTORY.keys() - sections.keys()
    if missing:
        raise ValueError("缺少历史版本: " + ", ".join(sorted(missing)))
    return sections


def render_update_changelog(sections: dict[str, ChangelogSection], version: str) -> str:
    """更新弹窗只含所选版本，未知版本直接拒绝。"""
    if not version.startswith("v"):
        version = "v" + version
    if version not in sections:
        raise ValueError(f"CHANGELOG.md 里找不到版本: {version}")
    section = sections[version]
    return (HEADER + f"## {section.version}（versionCode {section.version_code}）\n\n"
            + "\n".join("- " + bullet for bullet in section.bullets) + "\n")


def render_package_changelog(sections: dict[str, ChangelogSection]) -> str:
    """包内全历史；段首是固定纯文本标记，段间仅空行。"""
    return "\n\n".join(
        f"CTN Patch {section.version}（versionCode {section.version_code}）更新日志\n\n"
        + "\n".join("- " + bullet for bullet in section.bullets)
        for section in sections.values()
    ) + "\n"


def extract_section(text: str, version: str) -> str:
    """保留发版脚本的单节提取 API；兼容旧正文，但拒绝重复目标标题。"""
    pat = re.compile(r"^##[^\n]*%s(?![0-9.])" % re.escape(version), re.M)
    matches = list(pat.finditer(text))
    if len(matches) > 1:
        raise ValueError(f"重复版本标题: {version}")
    if not matches:
        return ""
    match = matches[0]
    nxt = re.compile(r"^## ", re.M).search(text, match.end())
    return text[match.start():(nxt.start() if nxt else len(text))].rstrip() + "\n"


def strip_checksums(section: str) -> str:
    """保留旧发版调用的兼容 API，去掉 sha256 代码块及紧邻校验标题。"""
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


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", nargs="?", help="所选版本，默认读取 module.prop")
    parser.add_argument("--version", dest="target_version", help="显式指定所选版本")
    args = parser.parse_args(argv)
    if args.version and args.target_version:
        parser.error("版本只能指定一次：位置参数或 --version")
    try:
        version = args.target_version or args.version or current_version()
        if not version.startswith("v"):
            version = "v" + version
        sections = parse_changelog(CHANGELOG.read_text(encoding="utf-8"))
        update = render_update_changelog(sections, version)
        package = render_package_changelog(sections)
        # 先验证并渲染两个输出；未知版本或无效历史不能触碰任何输出。
        OUT.write_text(update, encoding="utf-8", newline="\n")
        PACKAGE_OUT.write_text(package, encoding="utf-8", newline="\n")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"!! {exc}\n")
    print(f"  已生成 {OUT.name}（仅 {version}）")
    print(f"  已生成 magisk/{PACKAGE_OUT.name}（{len(sections)} 个版本，最新在前，每版最多 {MAX_BULLETS} 条）")


if __name__ == "__main__":
    main()
