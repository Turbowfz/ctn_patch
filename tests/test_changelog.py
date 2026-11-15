#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-only changelog regressions. Run: python -I -B tests/test_changelog.py."""
from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ctn_gen_changelog", REPO / "gen_changelog.py")
assert SPEC and SPEC.loader
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)
EXPECTED = [
    ("v3.2", 32), ("v3.1", 31), ("v3.0", 30), ("v2.2", 22), ("v2.1", 21), ("v2.0", 20),
    *((f"v1.{minor}", 10 + minor) for minor in range(9, -1, -1)),
]
PACKAGE_HEADER = re.compile(r"CTN Patch (v[0-9]+\.[0-9]+)（versionCode ([0-9]+)）更新日志")
ROOT_HEADER = re.compile(r"## (v[0-9]+\.[0-9]+)（versionCode ([0-9]+)）")
SOURCE = (REPO / "CHANGELOG.md").read_text(encoding="utf-8")


class ChangelogTests(unittest.TestCase):
    def assert_sections(self, text: str, header_pattern: re.Pattern[str]) -> None:
        headings = []
        bullets = []
        for line in text.splitlines():
            if not line:
                continue
            match = header_pattern.fullmatch(line)
            if match:
                if headings:
                    self.assertTrue(1 <= len(bullets) <= 4)
                headings.append((match[1], int(match[2])))
                bullets = []
                continue
            self.assertTrue(headings, line)
            self.assertTrue(line.startswith("- "), line)
            self.assertTrue(line[2:] and line[2:] == line[2:].strip(), line)
            self.assertLessEqual(len(line[2:]), 120)
            self.assertLessEqual(len(line.encode("utf-8")), 400)
            self.assertNotRegex(line, r"[0-9a-fA-F]{64}")
            bullets.append(line)
        self.assertTrue(1 <= len(bullets) <= 4)
        self.assertEqual(headings, EXPECTED)
        self.assertEqual(len({version for version, _ in headings}), 16)

    def test_root_complete_unique_descending_concise_history(self):
        self.assertTrue(SOURCE.startswith("# 更新日志\n\n"))
        self.assert_sections(SOURCE.removeprefix("# 更新日志\n\n"), ROOT_HEADER)
        parsed = GEN.parse_changelog(SOURCE)
        self.assertEqual([(version, section.version_code) for version, section in parsed.items()], EXPECTED)

    def test_package_generation_has_exact_16_sections_and_no_extra_details(self):
        parsed = GEN.parse_changelog(SOURCE)
        package = GEN.render_package_changelog(parsed)
        self.assert_sections(package, PACKAGE_HEADER)
        expected = "\n\n".join(
            f"CTN Patch {version}（versionCode {code}）更新日志\n\n"
            + "\n".join("- " + bullet for bullet in parsed[version].bullets)
            for version, code in EXPECTED
        ) + "\n"
        self.assertEqual(package, expected)
        self.assertNotIn("\r", package)

    def test_checked_in_outputs_match_generation(self):
        parsed = GEN.parse_changelog(SOURCE)
        package = (REPO / "magisk/changelog.txt").read_bytes()
        update = (REPO / "update-changelog.md").read_bytes()
        self.assertEqual(package, GEN.render_package_changelog(parsed).encode("utf-8"))
        self.assertEqual(update, GEN.render_update_changelog(parsed, "v3.2").encode("utf-8"))
        self.assert_sections(package.decode("utf-8"), PACKAGE_HEADER)

    def test_update_contains_only_requested_current_section(self):
        parsed = GEN.parse_changelog(SOURCE)
        update = GEN.render_update_changelog(parsed, "v3.1")
        self.assertTrue(update.startswith(GEN.HEADER + "## v3.1（versionCode 31）\n\n"))
        self.assertEqual(re.findall(r"^## .+$", update, re.M), ["## v3.1（versionCode 31）"])
        self.assertEqual(update, GEN.HEADER + GEN.extract_section(SOURCE, "v3.1"))
        self.assertEqual(len(re.findall(r"^- ", update, re.M)), 4)
        self.assertNotIn("## v3.0", update)
        self.assertNotIn("CTN Patch", update)

    def test_current_no_manual_configuration_and_historical_override_is_past_only(self):
        parsed = GEN.parse_changelog(SOURCE)
        current = "\n".join(parsed["v3.2"].bullets)
        self.assertIn("不支持 ctn.conf 手动添加", current)
        self.assertIn("141 仍无实机验证", current)
        self.assertIn("当时支持本地覆盖（已移除）", "\n".join(parsed["v1.1"].bullets))
        self.assertIn("移除本地覆盖文件 ctn.conf", "\n".join(parsed["v2.0"].bullets))

    def test_duplicate_version_headings_rejected(self):
        duplicate = SOURCE + "\n## v2.0（versionCode 20）\n\n- 重复修订。\n"
        with self.assertRaisesRegex(ValueError, "重复版本标题: v2.0"):
            GEN.parse_changelog(duplicate)
        with self.assertRaisesRegex(ValueError, "重复版本标题: v2.0"):
            GEN.extract_section(duplicate, "v2.0")

    def test_wrong_version_code_rejected(self):
        with self.assertRaisesRegex(ValueError, "versionCode 不正确"):
            GEN.parse_changelog(SOURCE.replace("## v2.2（versionCode 22）", "## v2.2（versionCode 23）"))

    def test_non_numeric_order_rejected(self):
        v31 = GEN.extract_section(SOURCE, "v3.1")
        v30 = GEN.extract_section(SOURCE, "v3.0")
        reordered = SOURCE.replace(v31 + "\n" + v30, v30 + "\n" + v31)
        self.assertNotEqual(reordered, SOURCE)
        with self.assertRaisesRegex(ValueError, "数字降序"):
            GEN.parse_changelog(reordered)

    def test_missing_historical_version_rejected(self):
        with self.assertRaisesRegex(ValueError, "缺少历史版本: v1.0"):
            GEN.parse_changelog(SOURCE.replace(GEN.extract_section(SOURCE, "v1.0"), ""))

    def test_nonconcise_content_rejected(self):
        marker = "## v3.1（versionCode 31）\n\n"
        additions = [
            ("- 第五条要点。\n", "最多 4 条"),
            ("- " + "长" * 121 + "\n", "要点过长"),
            ("- " + "𠮷" * 100 + "\n", "要点过长"),
            ("- \n", "空要点"),
            ("- " + "a" * 64 + "\n", "64 位校验值"),
            ("长技术段落不能进入简短历史。\n", "只允许"),
            ("### 详细修订\n", "只允许"),
        ]
        for addition, message in additions:
            with self.subTest(message=message, addition=addition), self.assertRaisesRegex(ValueError, message):
                GEN.parse_changelog(SOURCE.replace(marker, marker + addition, 1))

    def test_future_versions_must_follow_same_concise_contract(self):
        future = ("# 更新日志\n\n## v3.3（versionCode 33）\n\n- 简短新版本要点。\n\n"
                  + SOURCE.removeprefix("# 更新日志\n\n"))
        parsed = GEN.parse_changelog(future)
        self.assertEqual(len(parsed), 17)
        self.assertEqual(next(iter(parsed)), "v3.3")
        self.assertEqual(parsed["v3.3"].version_code, 33)
        self.assertTrue(GEN.render_package_changelog(parsed).startswith("CTN Patch v3.3（versionCode 33）更新日志\n\n"))

    def test_extract_section_keeps_legacy_boundary_and_missing_behavior(self):
        text = "## v1.10（versionCode 110）\n\n- 不应匹配。\n\n## v1.1（versionCode 11）\n\n- 所选版本。\n"
        self.assertEqual(GEN.extract_section(text, "v1.1"), "## v1.1（versionCode 11）\n\n- 所选版本。\n")
        self.assertEqual(GEN.extract_section(text, "v9.9"), "")

    def test_strip_checksums_legacy_import_compatibility(self):
        concise = "## v3.1（versionCode 31）\n\n- 保留要点。\n"
        legacy = concise + "\n**校验值**\n```\nctnd sha256 " + "a" * 64 + "\n```\n"
        self.assertEqual(GEN.strip_checksums(legacy), concise)
        self.assertEqual(GEN.strip_checksums(concise), concise)
        code = concise + "\n```text\n非校验示例\n```\n"
        self.assertEqual(GEN.strip_checksums(code), code)


class GeneratorCLITests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ctnp-changelog-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "CHANGELOG.md"
        self.prop = self.root / "module.prop"
        self.update = self.root / "update-changelog.md"
        self.package = self.root / "changelog.txt"
        self.source.write_text(SOURCE, encoding="utf-8", newline="\n")
        self.prop.write_text("version=v3.1\nversionCode=31\n", encoding="utf-8", newline="\n")
        patch = mock.patch.multiple(GEN, CHANGELOG=self.source, PROP=self.prop, OUT=self.update, PACKAGE_OUT=self.package)
        patch.start()
        self.addCleanup(patch.stop)

    def generate(self, argv: list[str]) -> None:
        with contextlib.redirect_stdout(io.StringIO()):
            GEN.main(argv)

    def assert_outputs_untouched(self, argv: list[str], message: str) -> None:
        self.update.write_bytes(b"keep update output\r\n")
        self.package.write_bytes(b"keep package output\r\n")
        before = [(path.read_bytes(), path.stat().st_mtime_ns) for path in (self.update, self.package)]
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as raised:
            self.generate(argv)
        self.assertNotEqual(raised.exception.code, 0)
        self.assertIn(message, stderr.getvalue())
        self.assertEqual(before, [(path.read_bytes(), path.stat().st_mtime_ns) for path in (self.update, self.package)])

    def test_default_cli_generates_both_outputs(self):
        self.generate([])
        parsed = GEN.parse_changelog(SOURCE)
        self.assertEqual(self.update.read_bytes(), GEN.render_update_changelog(parsed, "v3.1").encode("utf-8"))
        self.assertEqual(self.package.read_bytes(), GEN.render_package_changelog(parsed).encode("utf-8"))

    def test_positional_and_version_flag_preserve_target_selection(self):
        for argv in (["v2.2"], ["--version", "2.2"]):
            with self.subTest(argv=argv):
                self.generate(argv)
                self.assertEqual(re.findall(r"^## .+$", self.update.read_text(encoding="utf-8"), re.M), ["## v2.2（versionCode 22）"])
                self.assertTrue(self.package.read_text(encoding="utf-8").startswith("CTN Patch v3.2（versionCode 32）更新日志\n\n"))
                self.assertEqual(len(re.findall(r"^CTN Patch ", self.package.read_text(encoding="utf-8"), re.M)), 16)

    def test_unknown_or_unsupported_target_rejected_before_output_touch(self):
        for target in ("v9.9", "v1.10", "not-a-version"):
            with self.subTest(target=target):
                self.assert_outputs_untouched(["--version", target], "找不到版本")

    def test_unknown_target_does_not_create_missing_outputs(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.generate(["--version", "v9.9"])
        self.assertFalse(self.update.exists())
        self.assertFalse(self.package.exists())

    def test_duplicate_heading_rejected_before_output_touch(self):
        self.source.write_text(SOURCE + "\n## v3.1（versionCode 31）\n\n- 重复。\n", encoding="utf-8", newline="\n")
        self.assert_outputs_untouched([], "重复版本标题")


if __name__ == "__main__":
    unittest.main(verbosity=2)
