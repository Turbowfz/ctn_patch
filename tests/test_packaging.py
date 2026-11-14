#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-only packaging and shell regressions; all device paths/commands are mocked.

Run: python -I -B tests/test_packaging.py
No device, root access, real insmod, package installation, or downloads are needed.
"""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import struct
import subprocess
import tempfile
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ctn_build_zip", REPO / "build_zip.py")
assert SPEC and SPEC.loader
PACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACK)
SH = shutil.which(os.environ.get("CTN_TEST_SHELL", "sh"))


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def put(path: Path, data: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode("utf-8") if isinstance(data, str) else data)


def elf(executable: bool = False, marker: bytes = b"fixture", *,
        modinfo: bytes | None = None, duplicate_modinfo: bool = False) -> bytes:
    if executable:
        # Keep the daemon image used by the runtime fixtures byte-for-byte unchanged.
        data = bytearray(128)
        data[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<HH", data, 16, 3, 183)
        return bytes(data) + marker
    data = bytearray(64)
    names = b"\0.shstrtab\0.modinfo\0"
    names_offset = len(data)
    data.extend(names)
    modinfo_offset = len(data)
    if modinfo is not None:
        data.extend(modinfo)
    data.extend(bytes(-len(data) % 8))
    section_offset = len(data)
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0),
    ]
    if modinfo is not None:
        sections.append((11, 1, 0, 0, modinfo_offset, len(modinfo), 0, 0, 1, 0))
        if duplicate_modinfo:
            sections.append(sections[-1])
    for section in sections:
        data.extend(struct.pack("<IIQQQQIIQQ", *section))
    struct.pack_into("<16sHHIQQQIHHHHHH", data, 0,
                     b"\x7fELF\x02\x01\x01" + bytes(9), 1, 183, 1, 0, 0,
                     section_offset, 0, 64, 0, 0, 64, len(sections), 1)
    return bytes(data) + marker


def shell_path(path: Path) -> str:
    value = path.resolve().as_posix()
    if os.name == "nt" and len(value) > 2 and value[1] == ":":
        return "/" + value[0].lower() + value[2:]
    return value


class Fixture:
    def __init__(self, root: Path):
        self.root = root
        for relative, _ in PACK.SOURCES.values():
            put(root / relative, (REPO / relative).read_bytes())
        put(root / "README.md", "repository development README, not the package README\n")
        put(root / "LICENSE", "source license stays in source\n")
        put(root / "out/user/ctnd", elf(True, b"ctnd-3.0"))
        self.vendors: dict[str, bytes] = {}
        for version in PACK.PROFILES:
            vendor = b"verified vendor fixture " + version.encode()
            self.vendors[version] = vendor
            profile = {
                "schema": 1, "id": version, "kernel_version": version,
                "arch": "aarch64", "page_size": 4096,
                "runtime_releases": [version + "-android14-11-o-fixture"],
                "vendor": {
                    "name": "oplus_bsp_game_opt", "paths": sorted(PACK.VENDOR_PATHS),
                    "sha256": [sha(vendor)],
                    "vermagic": version + "-android14-11-o-vendor SMP preempt mod_unload modversions aarch64",
                },
                "native_node": False,
                "abi": {"test_fixture": True}, "source": {"status": "synthetic-host-test"},
            }
            raw = PACK.json_bytes(profile)
            put(root / f"profiles/{version}/profile.json", raw)
            ko = elf(marker=version.encode(), modinfo=self.modinfo(version))
            put(root / f"out/{version}/ctn_patch.ko", ko)
            record = {
                "schema": 1, "profile_id": version, "profile_sha256": sha(raw),
                "module_sha256": sha(ko), "build_id": sha(b"build " + version.encode()),
                "static": "passed", "runtime": "passed",
                "checks": [{"name": name, "passed": True} for name in sorted(PACK.REQUIRED_CHECKS)],
            }
            put(root / f"out/{version}/verification.json", PACK.json_bytes(record))

    def modinfo(self, profile_id: str = "6.1.157", **changes: str) -> bytes:
        profile = json.loads((self.root / f"profiles/{profile_id}/profile.json").read_bytes())
        values = {
            "version": "3.0", "ctn_profile": profile_id,
            "ctn_build_id": sha(b"build " + profile_id.encode()),
            "vermagic": profile["vendor"]["vermagic"],
        }
        values.update(changes)
        return b"".join(f"{key}={value}\0".encode() for key, value in values.items())

    def module(self, modinfo: bytes | None, version: str = "6.1.157", *,
               duplicate_modinfo: bool = False, marker: bytes | None = None) -> bytes:
        ko = elf(marker=version.encode() if marker is None else marker,
                 modinfo=modinfo, duplicate_modinfo=duplicate_modinfo)
        put(self.root / f"out/{version}/ctn_patch.ko", ko)
        self.record(version, module_sha256=sha(ko))
        return ko

    def record(self, version: str = "6.1.157", **changes: object) -> None:
        path = self.root / f"out/{version}/verification.json"
        value = json.loads(path.read_bytes())
        value.update(changes)
        put(path, PACK.json_bytes(value))

    def profile(self, version: str = "6.1.157", **changes: object) -> None:
        path = self.root / f"profiles/{version}/profile.json"
        value = json.loads(path.read_bytes())
        value.update(changes)
        raw = PACK.json_bytes(value)
        put(path, raw)
        self.record(version, profile_sha256=sha(raw))

    def collect(self, **kwargs: object) -> dict[str, tuple[bytes, int]]:
        return PACK.collect_entries(self.root, **kwargs)[0]


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ctnp-package-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixture = Fixture(self.root)

    def build(self, **kwargs: object) -> Path:
        with contextlib.redirect_stdout(io.StringIO()):
            return PACK.build_package(self.root, **kwargs)

    def test_full_layout_permissions_hashes_and_package_readme(self):
        put(self.root / "magisk/action.sh", (self.root / "magisk/action.sh").read_bytes().replace(b"\n", b"\r\n"))
        output = self.build()
        self.assertEqual(output.name, "ctn_patch-v3.0-full.zip")
        entries = self.fixture.collect()
        with zipfile.ZipFile(output) as z:
            self.assertEqual(set(z.namelist()), set(entries))
            self.assertEqual(z.read("README.md"), (self.root / "magisk/README.md").read_bytes())
            self.assertNotEqual(z.read("README.md"), (self.root / "README.md").read_bytes())
            self.assertFalse({"LICENSE", "conf", "fakethreads", "bin/fakethreads", "ctn_patch.ko"} & set(z.namelist()))
            for name, (data, mode) in entries.items():
                self.assertEqual(z.read(name), data)
                info = z.getinfo(name)
                self.assertEqual(info.create_system, 3)
                self.assertTrue(stat.S_ISREG(info.external_attr >> 16))
                self.assertEqual(stat.S_IMODE(info.external_attr >> 16), mode)
                if name.endswith((".sh", ".json", ".prop", ".md")):
                    self.assertNotIn(b"\r", z.read(name))
            for version in PACK.PROFILES:
                prefix = f"kernels/{version}/"
                metadata = dict(line.split("=", 1) for line in z.read(prefix + "metadata.prop").decode().splitlines())
                self.assertEqual(metadata["module_sha256"], sha(z.read(prefix + "ctn_patch.ko")))
                self.assertEqual(metadata["manifest_sha256"], sha(z.read(prefix + "manifest.json")))
                self.assertEqual(metadata["profile_sha256"], sha((self.root / f"profiles/{version}/profile.json").read_bytes()))
                self.assertEqual(metadata["daemon_sha256"], sha(z.read("bin/ctnd")))
                self.assertEqual(json.loads(z.read(prefix + "manifest.json"))["verification"]["runtime"], "passed")

    def test_missing_each_required_artifact_refuses_without_touching_output(self):
        required = [relative for relative, _ in PACK.SOURCES.values()] + ["out/user/ctnd"]
        required += [f"{base}/{version}/{name}" for version in PACK.PROFILES
                     for base, name in (("profiles", "profile.json"), ("out", "ctn_patch.ko"), ("out", "verification.json"))]
        output = self.root / "keep.zip"
        put(output, b"old package must survive refusal")
        for name in required:
            with self.subTest(name=name):
                path = self.root / name
                data = path.read_bytes()
                path.unlink()
                with self.assertRaises(PACK.PackageError):
                    self.build(output=output)
                self.assertEqual(output.read_bytes(), b"old package must survive refusal")
                put(path, data)

    def test_legacy_artifacts_are_not_fallbacks(self):
        (self.root / "out/user/ctnd").unlink()
        put(self.root / "daemon/ctnd", elf(True))
        put(self.root / "magisk/ctnd", elf(True))
        put(self.root / "ctn_patch.ko", elf())
        with self.assertRaisesRegex(PACK.PackageError, "user/ctnd"):
            self.fixture.collect()

    def test_full_release_requires_both_runtime_passes(self):
        for version in PACK.PROFILES:
            with self.subTest(version=version):
                self.fixture.record(version, runtime="not_tested")
                with self.assertRaisesRegex(PACK.PackageError, "runtime verification not passed"):
                    self.fixture.collect()
                self.fixture.record(version, runtime="passed")

    def test_development_and_single_profile_are_explicitly_labelled(self):
        self.fixture.record(runtime="not_tested")
        path = self.build(development=True)
        self.assertIn("development", path.name)
        with zipfile.ZipFile(path) as z:
            self.assertIn(b"DEVELOPMENT PACKAGE", z.read("README.md"))
            self.assertIn("kernels/6.1.141/ctn_patch.ko", z.namelist())
        shutil.rmtree(self.root / "out/6.1.141")
        path = self.build(profile="6.1.157")
        with zipfile.ZipFile(path) as z:
            self.assertNotIn("kernels/6.1.141/ctn_patch.ko", z.namelist())
            self.assertIn(b"package_mode=development", z.read("kernels/6.1.157/metadata.prop"))
        self.fixture.record(runtime="failed")
        with self.assertRaises(PACK.PackageError):
            self.fixture.collect(profile="6.1.157")

    def test_module_prop_only_version_bump_refuses_untouched_ko(self):
        path = self.root / "magisk/module.prop"
        changelog = self.root / "magisk/changelog.txt"
        put(path, path.read_bytes().replace(b"version=v3.0\n", b"version=v3.1\n")
            .replace(b"versionCode=30\n", b"versionCode=31\n"))
        # Isolate the embedded-KO gate: the display changelog follows the bump.
        put(changelog, changelog.read_bytes()
            .replace("v3.0（versionCode 30）".encode("utf-8"), "v3.1（versionCode 31）".encode("utf-8")))
        untouched = {version: (self.root / f"out/{version}/ctn_patch.ko").read_bytes()
                     for version in PACK.PROFILES}
        output = self.root / "keep.zip"
        for options in ({}, {"development": True}, {"profile": "6.1.157"}):
            with self.subTest(options=options):
                put(output, b"keep existing package on identity refusal")
                with self.assertRaisesRegex(PACK.PackageError, "modinfo version mismatch"):
                    self.build(output=output, **options)
                self.assertEqual(output.read_bytes(), b"keep existing package on identity refusal")
                for version, ko in untouched.items():
                    self.assertEqual((self.root / f"out/{version}/ctn_patch.ko").read_bytes(), ko)

    def test_module_prop_version_without_v_prefix_matches_ko(self):
        path = self.root / "magisk/module.prop"
        put(path, path.read_bytes().replace(b"version=v3.0\n", b"version=3.0\n"))
        entries = self.fixture.collect()
        for version in PACK.PROFILES:
            self.assertIn(b"module_version=3.0\n", entries[f"kernels/{version}/metadata.prop"][0])

    def test_valid_hex_record_build_id_must_match_embedded_ko(self):
        original = (self.root / "out/6.1.157/ctn_patch.ko").read_bytes()
        wrong = sha(b"different recorded build identity")
        self.assertRegex(wrong, PACK.SHA256)
        self.fixture.record(build_id=wrong)
        for options in ({}, {"development": True}, {"profile": "6.1.157"}):
            with self.subTest(options=options):
                with self.assertRaisesRegex(PACK.PackageError, "modinfo ctn_build_id mismatch"):
                    self.fixture.collect(**options)
        self.assertEqual((self.root / "out/6.1.157/ctn_patch.ko").read_bytes(), original)

    def test_wrong_embedded_identity_refused_even_with_matching_hash(self):
        for version in PACK.PROFILES:
            profile = json.loads((self.root / f"profiles/{version}/profile.json").read_bytes())
            wrong_profile = next(item for item in PACK.PROFILES if item != version)
            wrong = (
                ("version", "2.9"), ("version", "v3.0"), ("ctn_profile", wrong_profile),
                ("ctn_build_id", sha(b"different embedded build identity")),
                ("vermagic", profile["vendor"]["vermagic"].replace("-o-vendor", "-o-other")),
                ("vermagic", profile["runtime_releases"][0] +
                 " SMP preempt mod_unload modversions aarch64"),
                ("vermagic", profile["vendor"]["vermagic"].replace(" preempt", "")),
            )
            for key, value in wrong:
                with self.subTest(version=version, key=key, value=value):
                    self.fixture.module(self.fixture.modinfo(version, **{key: value}), version)
                    with self.assertRaisesRegex(PACK.PackageError, f"modinfo {key} mismatch"):
                        self.fixture.collect(profile=version)
                self.fixture.module(self.fixture.modinfo(version), version)

    def test_missing_or_duplicate_modinfo_identity_fields_refused(self):
        raw = self.fixture.modinfo()
        items = [item for item in raw.split(b"\0") if item]
        for item in items:
            key = item.split(b"=", 1)[0].decode()
            missing = b"".join(other + b"\0" for other in items if other != item)
            for label, value in (("missing", missing), ("duplicate", raw + item + b"\0")):
                with self.subTest(key=key, label=label):
                    # Correct identity outside .modinfo must not substitute for section data.
                    self.fixture.module(value, marker=raw)
                    with self.assertRaisesRegex(PACK.PackageError, "modinfo"):
                        self.fixture.collect(profile="6.1.157")

    def test_missing_empty_or_duplicate_modinfo_section_refused(self):
        raw = self.fixture.modinfo()
        for label, value, duplicate in (("missing", None, False), ("empty", b"", False),
                                        ("duplicate", raw, True)):
            with self.subTest(label=label):
                self.fixture.module(value, duplicate_modinfo=duplicate, marker=raw)
                with self.assertRaisesRegex(PACK.PackageError, "modinfo"):
                    self.fixture.collect(profile="6.1.157")

    def test_malformed_modinfo_records_refused(self):
        raw = self.fixture.modinfo()
        for value in (raw[:-1], raw + b"no-equals\0", raw + b"=empty-key\0",
                      raw + b"bad\nkey=value\0", raw + b"description=\xff\0"):
            with self.subTest(value=value):
                self.fixture.module(value)
                with self.assertRaisesRegex(PACK.PackageError, "modinfo"):
                    self.fixture.collect(profile="6.1.157")

    def test_modinfo_elf_section_headers_are_bounded(self):
        path = self.root / "out/6.1.157/ctn_patch.ko"
        raw = path.read_bytes()
        section_offset = struct.unpack_from("<Q", raw, 40)[0]
        for label, offset, format_, value in (
                ("table outside file", 40, "<Q", len(raw) + 64),
                ("wrong entry size", 58, "<H", 63),
                ("no section headers", 60, "<H", 0),
                ("bad section name", section_offset + 128, "<I", 1024),
                ("NOBITS modinfo", section_offset + 132, "<I", 8),
                ("not PROGBITS", section_offset + 132, "<I", 3),
                ("modinfo outside file", section_offset + 152, "<Q", len(raw) + 64)):
            with self.subTest(label=label):
                bad = bytearray(raw)
                struct.pack_into(format_, bad, offset, value)
                put(path, bytes(bad))
                self.fixture.record(module_sha256=sha(bad))
                with self.assertRaisesRegex(PACK.PackageError, "ELF|modinfo"):
                    self.fixture.collect(profile="6.1.157")

    def test_profile_vermagic_requires_full_runtime_flags_in_order(self):
        path = self.root / "profiles/6.1.157/profile.json"
        vendor = json.loads(path.read_bytes())["vendor"]
        full = vendor["vermagic"]
        invalid = [full.replace(" " + flag, "")
                   for flag in ("SMP", "preempt", "mod_unload", "modversions")]
        invalid += [full.replace("SMP preempt", "preempt SMP"), full + " extra",
                    full.replace(" SMP", "  SMP"), full.replace(" aarch64", " arm64")]
        for value in invalid:
            with self.subTest(vermagic=value):
                self.fixture.profile(vendor=dict(vendor, vermagic=value))
                self.fixture.module(self.fixture.modinfo())
                with self.assertRaisesRegex(PACK.PackageError, "full vermagic mismatch"):
                    self.fixture.collect(profile="6.1.157")
        self.fixture.profile(vendor=vendor)
        self.fixture.module(self.fixture.modinfo())
        self.fixture.collect()

    def test_same_size_ko_tamper_is_rejected_by_hash(self):
        path = self.root / "out/6.1.157/ctn_patch.ko"
        data = path.read_bytes()
        put(path, data[:-1] + bytes([data[-1] ^ 1]))
        with self.assertRaisesRegex(PACK.PackageError, "module hash mismatch"):
            self.fixture.collect()

    def test_profile_hash_is_raw_bytes_not_reserialized_json(self):
        path = self.root / "profiles/6.1.157/profile.json"
        put(path, path.read_bytes() + b"\n")
        with self.assertRaisesRegex(PACK.PackageError, "profile hash mismatch"):
            self.fixture.collect()

    def test_in_module_changelog_is_concise_and_version_bound(self):
        entries = self.fixture.collect()
        self.assertIn("changelog.txt", entries)
        data, mode = entries["changelog.txt"]
        self.assertEqual(mode, 0o644)
        self.assertLessEqual(len(data), 2048)
        text = data.decode("utf-8")
        self.assertIn("v3.0", text)
        self.assertIn("versionCode 30", text)
        path = self.root / "magisk/changelog.txt"
        raw = path.read_bytes()
        for label, content in (("stale version", text.replace("v3.0", "v9.9").encode("utf-8")),
                               ("stale code", text.replace("versionCode 30", "versionCode 31").encode("utf-8")),
                               ("oversized", ("v3.0（versionCode 30）\n" + "x" * 2100).encode("utf-8"))):
            with self.subTest(label=label):
                put(path, content)
                with self.assertRaisesRegex(PACK.PackageError, "changelog"):
                    self.fixture.collect()
        put(path, raw)
        self.fixture.collect()

    def test_status_without_all_six_checks_is_rejected(self):
        self.fixture.record(checks=[])
        with self.assertRaises(PACK.PackageError):
            self.fixture.collect()
        checks = [{"name": name, "passed": True} for name in sorted(PACK.REQUIRED_CHECKS)]
        for missing in checks:
            with self.subTest(missing=missing):
                self.fixture.record(checks=[item for item in checks if item != missing])
                with self.assertRaisesRegex(PACK.PackageError, "missing required"):
                    self.fixture.collect()

    def test_false_duplicate_or_string_checks_are_rejected(self):
        checks = [{"name": name, "passed": True} for name in sorted(PACK.REQUIRED_CHECKS)]
        for bad in ([*checks, checks[0]], [dict(checks[0], passed="true"), *checks[1:]],
                    [dict(checks[0], passed=False), *checks[1:]]):
            with self.subTest(checks=bad):
                self.fixture.record(checks=bad)
                with self.assertRaises(PACK.PackageError):
                    self.fixture.collect()

    def test_malformed_identity_fields_fail_closed(self):
        for changes in ({"schema": True}, {"profile_id": "6.1.141"}, {"module_sha256": "x"},
                        {"profile_sha256": "a" * 64}, {"build_id": ""}, {"static": "not_tested"}):
            with self.subTest(changes=changes):
                path = self.root / "out/6.1.157/verification.json"
                original = path.read_bytes()
                self.fixture.record(**changes)
                with self.assertRaises(PACK.PackageError):
                    self.fixture.collect()
                put(path, original)

    def test_duplicate_json_keys_and_unsupported_native_profile_refused(self):
        path = self.root / "profiles/6.1.157/profile.json"
        raw = path.read_bytes()
        put(path, raw.replace(b'"schema": 1', b'"schema": 1, "schema": 1'))
        with self.assertRaisesRegex(PACK.PackageError, "duplicate JSON"):
            self.fixture.collect()
        put(path, raw)
        self.fixture.profile(native_node=True)
        with self.assertRaisesRegex(PACK.PackageError, "native-node"):
            self.fixture.collect()

    def test_elf_machine_not_size_or_magic_substring(self):
        for filename in ("out/user/ctnd", "out/6.1.157/ctn_patch.ko"):
            with self.subTest(filename=filename):
                path = self.root / filename
                raw = path.read_bytes()
                bad = bytearray(raw)
                struct.pack_into("<H", bad, 18, 62)
                put(path, bytes(bad))
                with self.assertRaisesRegex(PACK.PackageError, "not AArch64"):
                    self.fixture.collect()
                put(path, raw)

    def test_archive_whitelist_rejects_extra_entries(self):
        path = self.build()
        with zipfile.ZipFile(path, "a") as z:
            z.writestr("LICENSE", b"not allowed in runtime package")
        with self.assertRaisesRegex(PACK.PackageError, "whitelist"):
            PACK.validate_archive(path, self.fixture.collect())

    def test_repeated_build_is_deterministic(self):
        one = self.build(output=self.root / "one.zip").read_bytes()
        two = self.build(output=self.root / "two.zip").read_bytes()
        self.assertEqual(one, two)


MOCKS = r'''
MOCK_ROOT=@@ROOT@@
MOCK_PACKAGE=@@PACKAGE@@
MOCK_LOG=@@LOG@@
mock_event() { printf '%s\n' "$*" >> "$MOCK_LOG"; }
uname() {
    case "$1" in -r) printf '%s\n' "${MOCK_RELEASE:-6.1.157-android14-11-o-fixture}" ;;
    -m) printf '%s\n' "${MOCK_ARCH:-aarch64}" ;; *) return 1 ;; esac
}
getconf() { printf '%s\n' "${MOCK_PAGE:-4096}"; }
id() { printf '0\n'; }
readlink() {
    local path=$1
    case "$path" in "$MOCK_ROOT"/proc/*/exe)
        [ -f "${path%/exe}/alive" ] || return 1
        command cat "${path%/exe}/exe-path"; return $? ;;
    esac
    command readlink "$@"
}
sha256sum() {
    case "$1" in "$MOCK_ROOT"/proc/*/exe)
        command sha256sum "${1%/exe}/image" ;;
    *) command sha256sum "$@" ;; esac
}
kill() {
    local flag=$1 pid=$2
    case "$flag" in
    -0) [ -f "$MOCK_ROOT/proc/$pid/alive" ] ;;
    -TERM)
        [ -f "$MOCK_ROOT/proc/$pid/alive" ] || return 1
        mock_event "kill -TERM $pid"
        if [ "${MOCK_TERM_TIMEOUT:-0}" = 1 ]; then return 0; fi
        command rm -f "$MOCK_ROOT/proc/$pid/alive"
        if [ "${MOCK_RESTORE_FAIL:-0}" != 1 ]; then command rm -f "$MOCK_ROOT/data/adb/ctn_patch/restore.state"; fi
        ;;
    *) mock_event "FORBIDDEN kill $*"; return 99 ;;
    esac
}
flock() {
    case "${MOCK_FLOCK_ERROR:-0}" in 1|"$2") return 2 ;; esac
    if [ "${MOCK_REAL_FLOCK:-0}" = 1 ]; then
        case "$2" in
            5) command flock "$@" 5>&5 ;;
            6) command flock "$@" 6>&6 ;;
            7) command flock "$@" 7>&7 ;;
            8) command flock "$@" 8>&8 ;;
            9) command flock "$@" 9>&9 ;;
            *) return 2 ;;
        esac
        return $?
    fi
    case "$1:$2:${MOCK_LOCK_HELD:-}" in -n:7:daemon|-n:6:service|-n:9:service|-n:8:maintenance) return 1 ;; esac
    if [ "$1:$2" = -n:6 ] && [ "${MOCK_LEGACY_LOCK_HELD:-0}" = 1 ] &&
       [ "$(command readlink /proc/self/fd/6 6>&6)" = "$MOCK_PACKAGE/.ctnd.lock" ]; then
        return 1
    fi
    if [ "$1:$2" = -n:6 ] || [ "$1:$2" = -n:9 ]; then
        local service_pid
        service_pid=$(command cut -d' ' -f1 "$MOCK_ROOT/data/adb/ctn_patch/service.pid" 2>/dev/null) || service_pid=
        [ ! -f "$MOCK_ROOT/proc/$service_pid/alive" ] || return 1
    fi
    if [ "$1:$2" = -n:7 ] && [ "${MOCK_DAEMON_UNLOCKED:-0}" != 1 ]; then
        local pid
        pid=$(command cat "$MOCK_ROOT/data/adb/ctn_patch/daemon.pid" 2>/dev/null) || pid=
        [ ! -f "$MOCK_ROOT/proc/$pid/alive" ] || {
            local exe
            exe=$(command cat "$MOCK_ROOT/proc/$pid/exe-path" 2>/dev/null) || exe=
            case "$exe" in "$MOCK_PACKAGE/bin/ctnd"|"$MOCK_PACKAGE/bin/ctnd (deleted)") return 1 ;; esac
        }
    fi
    return 0
}
sleep() {
    if [ "${MOCK_STOP_MANAGED:-0}" = 1 ] && [ -e "$MOCK_ROOT/data/adb/ctn_patch/.service.stop" ]; then
        local service_pid
        service_pid=$(command cut -d' ' -f1 "$MOCK_ROOT/data/adb/ctn_patch/service.pid" 2>/dev/null) || service_pid=
        command rm -f "$MOCK_ROOT/proc/$service_pid/alive" "$MOCK_ROOT/data/adb/ctn_patch/service.pid"
    fi
    if [ "${MOCK_GAME_START_AFTER_PAUSE:-0}" = 1 ] && [ -e "$MOCK_ROOT/data/adb/ctn_patch/.service.stop" ]; then
        printf 'game_pid=222 child_num=0\n' > "$MOCK_ROOT/proc/game_opt/game_pid"
    fi
    if [ "${MOCK_STOP_ON_SLEEP:-0}" = 1 ]; then
        : > "$MOCK_ROOT/data/adb/ctn_patch/.service.stop"
    fi
}
insmod() {
    mock_event "insmod $*"
    [ "${MOCK_INSMOD_RC:-0}" = 0 ] || return "$MOCK_INSMOD_RC"
    command mkdir -p "$MOCK_ROOT/sys/module/ctn_patch/parameters"
    printf '%s\n' "$CTN_MODULE_VERSION" > "$MOCK_ROOT/sys/module/ctn_patch/version"
    printf '%s\n' "$CTN_PROFILE" > "$MOCK_ROOT/sys/module/ctn_patch/parameters/profile"
    printf '%s\n' "$CTN_BUILD_ID" > "$MOCK_ROOT/sys/module/ctn_patch/parameters/build_id"
    printf 'Y\n' > "$MOCK_ROOT/sys/module/ctn_patch/parameters/patch_ready"
    printf 'SavedA:-1,SavedB:-1\n' > "$MOCK_ROOT/proc/game_opt/task_boost/critical_task_name"
}
rmmod() {
    mock_event "rmmod $*"
    [ "${MOCK_RMMOD_RC:-0}" = 0 ] || return "$MOCK_RMMOD_RC"
    command rm -rf "$MOCK_ROOT/sys/module/ctn_patch"
    command rm -f "$MOCK_ROOT/proc/game_opt/task_boost/critical_task_name"
}
ui_print() { printf '%s\n' "$*"; }
abort() { printf 'ABORT: %s\n' "$*" >&2; exit 1; }
set_perm() { mock_event "set_perm $1 $4"; }
# Fake /proc entries for the fixture shell itself; never signal a host process.
command mkdir -p "$MOCK_ROOT/proc/$$"
printf '%s (sh) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 12345 0\n' "$$" > "$MOCK_ROOT/proc/$$/stat"
: > "$MOCK_ROOT/proc/$$/alive"
command readlink -f "$(command -v sh)" > "$MOCK_ROOT/proc/$$/exe-path"
printf '%s\000%s\000%s\000' "$(command -v sh)" "$MOCK_PACKAGE/service.sh" --run > "$MOCK_ROOT/proc/$$/cmdline"
'''

MOCK_NODE_WRITE = r'''ctn_write_node() {
    mock_event "write_names $1"
    if [ "${MOCK_FAIL_TEST:-0}" = 1 ] && [ "$1" = 'CTNTestMain CTNTestGfx' ]; then return "${MOCK_FAIL_TEST_RC:-1}"; fi
    if [ "${MOCK_FAIL_RESTORE:-0}" = 1 ] && [ "$1" = 'SavedA SavedB' ]; then return 1; fi
    if [ "${MOCK_SIGNAL_TEST:-0}" = 1 ] && [ "$1" = 'CTNTestMain CTNTestGfx' ]; then command kill -TERM "$$"; fi
    set -- $1
    [ "$#" = 1 ] || [ "$#" = 2 ] || return 1
    printf '%s:-1,%s:-1\n' "$1" "${2:-$1}" > "$CTN_NODE"
}'''


@unittest.skipUnless(SH, "POSIX sh is required (Git Bash works on Windows)")
class RuntimeShellTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ctnp-shell-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / "source"
        self.fixture = Fixture(self.source)
        self.package = self.base / "package"
        self.device = self.base / "device"
        self.runtime = self.device / "data/adb/ctn_patch"
        # Android's manager already provides /data/adb. Daemon fixtures used to
        # create it incidentally through daemon.pid; no-daemon tests must model
        # the same prerequisite without precreating the module runtime itself.
        self.runtime.parent.mkdir(parents=True)
        self.log = self.base / "commands.log"
        self.node = self.device / "proc/game_opt/task_boost/critical_task_name"
        self.enable = self.node.parent / "ct_enable"
        self.preamble = (MOCKS.replace("@@ROOT@@", shlex.quote(shell_path(self.device)))
                         .replace("@@PACKAGE@@", shlex.quote(shell_path(self.package)))
                         .replace("@@LOG@@", shlex.quote(shell_path(self.log))))
        entries = self.fixture.collect()
        for name, (data, mode) in entries.items():
            if name == "lib/common.sh":
                data = data.replace(b"    CTN_ROOT=\n", ("    CTN_ROOT=" + shlex.quote(shell_path(self.device)) + "\n").encode())
            elif name.endswith(".sh"):
                data = data.replace(b"#!/system/bin/sh\n", ("#!/bin/sh\n" + self.preamble).encode(), 1)
            if name == "verify.sh":
                data = data.replace(b'''ctn_write_node() { printf '%s\\n' "$1" > "$CTN_NODE"; }''', MOCK_NODE_WRITE.encode())
            put(self.package / name, data)
            (self.package / name).chmod(mode)
        self.vendor = self.device / "vendor/lib/modules/oplus_bsp_game_opt.ko"
        put(self.vendor, self.fixture.vendors["6.1.157"])
        put(self.device / "proc/modules", "oplus_bsp_game_opt 123 0 - Live 0\n")
        put(self.enable, "0\n")
        put(self.device / "proc/game_opt/game_pid", "game_pid=-1 child_num=0\n")
        put(self.device / "proc/sys/kernel/random/boot_id", "01234567-89ab-cdef-0123-456789abcdef\n")

    def run_script(self, name: str, *args: str, **env: str) -> subprocess.CompletedProcess:
        settings = dict(os.environ, **env)
        if name == "customize.sh":
            settings["MODPATH"] = shell_path(self.package)
        return subprocess.run([SH, shell_path(self.package / name), *args],
                              cwd=self.base, env=settings, text=True, encoding="utf-8", errors="replace",
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)

    def common(self, body: str, **env: str) -> subprocess.CompletedProcess:
        driver = self.base / "driver.sh"
        put(driver, "#!/bin/sh\n" + self.preamble +
            '\n. "$MOCK_PACKAGE/lib/common.sh"\nctn_init "$MOCK_PACKAGE" || exit 91\n' + body + "\n")
        return subprocess.run([SH, shell_path(driver)], cwd=self.base, env=dict(os.environ, **env),
                              text=True, encoding="utf-8", errors="replace", stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=30)

    def events(self) -> str:
        return self.log.read_text(encoding="utf-8") if self.log.exists() else ""

    def loaded(self, version: str = "6.1.157", legacy: bool = False, ready: str = "Y") -> None:
        root = self.device / "sys/module/ctn_patch"
        put(root / "version", "3.0\n")
        if not legacy:
            put(root / "parameters/profile", version + "\n")
            put(root / "parameters/build_id", sha(b"build " + version.encode()) + "\n")
            put(root / "parameters/patch_ready", ready + "\n")
        put(self.node, "SavedA:-1,SavedB:-1\n")

    def daemon(self, pid: int = 765432, legacy: bool = False, unrelated: bool = False,
               old_image: bool = False, deleted: bool = False) -> None:
        relative = "ctnd" if legacy else "bin/ctnd"
        exe = shell_path(self.package / relative)
        if unrelated:
            exe = "/some/unrelated/ctnd"
        if deleted:
            exe += " (deleted)"
        proc = self.device / f"proc/{pid}"
        put(proc / "exe-path", exe + "\n")
        put(proc / "alive", "")
        image = b"old daemon image" if old_image else (self.package / "bin/ctnd").read_bytes()
        put(proc / "image", image)
        put(proc / "stat", f"{pid} (ctnd odd name) S " + "0 " * 18 + "45678 0\n")
        if legacy:
            put(self.package / "ctnd", image)
            put(self.package / "ctnd.pid", str(pid) + "\n")
            put(self.package / ".ctnd.lock", "")
        else:
            put(self.runtime / ".daemon.lock", "")
            put(self.runtime / "daemon.pid", str(pid) + "\n")

    def service(self, pid: int = 765433) -> None:
        proc = self.device / f"proc/{pid}"
        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        put(proc / "exe-path", shell_path(Path(shell).resolve()) + "\n")
        put(proc / "alive", "")
        put(proc / "stat", f"{pid} (sh) S " + "0 " * 18 + "45678 0\n")
        put(proc / "cmdline", (shell + "\0" + shell_path(self.package / "service.sh") + "\0--run\0").encode())
        put(self.runtime / ".service.lock", "")
        put(self.runtime / "service.pid", f"{pid} 45678\n")

    def test_legacy_stop_requires_idle_lock_with_missing_or_unrelated_pid(self):
        for unrelated in (False, True):
            with self.subTest(unrelated=unrelated):
                self.daemon(legacy=True, unrelated=unrelated)
                if not unrelated:
                    (self.package / "ctnd.pid").unlink()
                put(self.package / ".ctnd.lock", "")
                self.log.unlink(missing_ok=True)
                result = self.common("ctn_stop_legacy", MOCK_LEGACY_LOCK_HELD="1")
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("kill -TERM", self.events())

    def test_service_rejects_old_loaded_identity_before_stopping_legacy(self):
        self.loaded(legacy=True)
        self.daemon(legacy=True)
        result = self.run_script("service.sh", "--run")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("kill -TERM", self.events())
        self.assertFalse((self.package / ".stop").exists())
        self.assertTrue((self.device / "proc/765432/alive").exists())

    def test_exercise_checks_idle_and_rechecks_after_service_pause(self):
        for starts_during_pause in (False, True):
            with self.subTest(starts_during_pause=starts_during_pause):
                self.loaded()
                self.log.unlink(missing_ok=True)
                put(self.device / "proc/game_opt/game_pid",
                    "game_pid=-1 child_num=0\n" if starts_during_pause else "game_pid=222 child_num=0\n")
                env = {}
                if starts_during_pause:
                    self.service()
                    put(self.runtime / ".service.stop", "pending stop\n")
                    env = {"MOCK_STOP_MANAGED": "1", "MOCK_GAME_START_AFTER_PAUSE": "1"}
                result = self.run_script("verify.sh", "--exercise", **env)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("write_names", self.events())
                self.assertEqual(self.node.read_text(), "SavedA:-1,SavedB:-1\n")

    def test_exercise_does_not_resume_existing_stop_request(self):
        self.loaded()
        self.service()
        put(self.runtime / ".service.stop", "keep stopped\n")
        result = self.run_script("verify.sh", "--exercise", MOCK_STOP_MANAGED="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.runtime / ".service.stop").read_text(), "keep stopped\n")
        self.assertNotIn("service did not resume", result.stderr)
        self.assertFalse((self.runtime / "service.pid").exists())

    def test_failed_exercise_recovery_blocks_same_boot_restart(self):
        self.loaded()
        result = self.run_script("verify.sh", "--exercise", MOCK_FAIL_RESTORE="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.runtime / "exercise.state").is_file())
        self.log.unlink(missing_ok=True)
        result = self.run_script("service.sh", "--run", MOCK_STOP_ON_SLEEP="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.runtime / ".service.stop").exists())
        self.assertTrue((self.runtime / "exercise.state").exists())
        self.assertNotIn("insmod", self.events())
        self.assertNotIn("daemon started", result.stdout)

    def test_successful_exercise_removes_own_recovery_record(self):
        self.loaded()
        result = self.run_script("verify.sh", "--exercise")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.runtime / "exercise.state").exists())

    def test_exercise_refuses_unresolved_recovery_and_symlink(self):
        self.loaded()
        put(self.runtime / "exercise.state", "boot=unknown\nnames=SavedA SavedB\nenable=0\n")
        result = self.run_script("verify.sh", "--exercise")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("write_names", self.events())
        (self.runtime / "exercise.state").unlink()
        outside = self.base / "recovery-keep"
        put(outside, "untouched")
        (self.runtime / "exercise.state").symlink_to(outside)
        result = self.run_script("verify.sh", "--exercise")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(outside.read_text(), "untouched")

    def test_previous_boot_exercise_record_is_discarded_without_node_writes(self):
        self.loaded()
        put(self.runtime / "exercise.state",
            "boot_id=fedcba98-7654-3210-fedc-ba9876543210\nnames=OldA OldB\nenable=1\n")
        result = self.common("ctn_runtime_prepare && ctn_exercise_guard")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.runtime / "exercise.state").exists())
        self.assertEqual(self.node.read_text(), "SavedA:-1,SavedB:-1\n")
        self.assertEqual(self.events(), "")

    def test_existing_boot_exercise_record_refuses_service_before_stop_clear(self):
        self.loaded()
        put(self.runtime / "exercise.state",
            "boot_id=01234567-89ab-cdef-0123-456789abcdef\nnames=SavedA SavedB\nenable=0\n")
        put(self.runtime / ".service.stop", "keep stopped\n")
        result = self.run_script("service.sh", "--run")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unresolved self-check restoration", result.stderr)
        self.assertEqual((self.runtime / ".service.stop").read_text(), "keep stopped\n")
        self.assertTrue((self.runtime / "exercise.state").exists())
        self.assertNotIn("insmod", self.events())

    def test_standalone_shell_lookup_uses_absolute_system_interpreter(self):
        common = self.package / "lib/common.sh"
        raw = common.read_bytes()
        put(common, raw.replace(b"    CTN_SHELL=$(command -v sh) || return 1\n",
                                b"    CTN_SHELL=sh\n", 1))
        system_shell = self.device / "system/bin/sh"
        system_shell.parent.mkdir(parents=True)
        system_shell.symlink_to(Path(shutil.which("sh")).resolve())
        result = self.common('printf "SHELL=%s\\nEXE=%s\\n" "$CTN_SHELL" "$CTN_SHELL_EXE"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SHELL=" + shell_path(system_shell.parent) + "/sh", result.stdout)
        self.assertIn("EXE=" + shell_path(Path(shutil.which("sh")).resolve()), result.stdout)

    def test_bare_shell_lookup_never_uses_working_directory_executable(self):
        common = self.package / "lib/common.sh"
        raw = common.read_bytes()
        put(common, raw.replace(b"    CTN_SHELL=$(command -v sh) || return 1\n",
                                b"    CTN_SHELL=sh\n", 1))
        decoy = self.base / "sh"
        put(decoy, "#!/bin/sh\nexit 0\n")
        decoy.chmod(0o755)
        result = self.common('printf "SHELL=%s\\n" "$CTN_SHELL"')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("SHELL=sh", result.stdout)

    def test_selector_accepts_exactly_both_profiles(self):
        for version in PACK.PROFILES:
            with self.subTest(version=version):
                put(self.vendor, self.fixture.vendors[version])
                result = self.common('ctn_select && printf "SELECTED=%s\\n" "$CTN_PROFILE"',
                                     MOCK_RELEASE=version + "-android14-11-o-fixture")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("SELECTED=" + version, result.stdout)
        self.assertEqual(self.events(), "")
        self.assertFalse(self.runtime.exists())

    def test_unknown_release_arch_or_pages_refuses_without_insmod(self):
        for env in ({"MOCK_RELEASE": "6.1.142-android14"}, {"MOCK_RELEASE": "6.1.157-android14-11-o-unknown"},
                    {"MOCK_ARCH": "x86_64"}, {"MOCK_PAGE": "16384"}, {"MOCK_PAGE": ""}):
            with self.subTest(env=env):
                result = self.common("ctn_select", **env)
                if env == {"MOCK_PAGE": ""}:
                    continue  # mock's :- default intentionally supplies 4096
                self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("insmod", self.events())

    def test_vendor_and_target_ko_tamper_rejected_even_same_size(self):
        for path in (self.vendor, self.package / "kernels/6.1.157/ctn_patch.ko", self.package / "bin/ctnd"):
            with self.subTest(path=path):
                raw = path.read_bytes()
                put(path, raw[:-1] + bytes([raw[-1] ^ 1]))
                result = self.common("ctn_select")
                self.assertNotEqual(result.returncode, 0)
                put(path, raw)
        self.assertEqual(self.events(), "")

    def test_missing_vendor_or_conflicting_second_path_refused(self):
        self.vendor.unlink()
        self.assertNotEqual(self.common("ctn_select").returncode, 0)
        put(self.vendor, self.fixture.vendors["6.1.157"])
        second = self.device / "vendor_dlkm/lib/modules/oplus_bsp_game_opt.ko"
        put(second, self.fixture.vendors["6.1.141"])
        result = self.common("ctn_select")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown vendor identity", result.stderr)

    def test_metadata_unknown_duplicate_or_shell_expression_is_never_executed(self):
        path = self.package / "kernels/6.1.157/metadata.prop"
        raw = path.read_bytes()
        marker = shell_path(self.base / "executed")
        for suffix in (b"unknown_key=true\n", b"schema=1\n", ("shell=$(touch " + marker + ")\n").encode()):
            with self.subTest(suffix=suffix):
                put(path, raw + suffix)
                self.assertNotEqual(self.common("ctn_select").returncode, 0)
                self.assertFalse((self.base / "executed").exists())
        put(path, raw)

    def test_manifest_hash_mismatch_is_refused(self):
        path = self.package / "kernels/6.1.157/manifest.json"
        put(path, path.read_bytes() + b"\n")
        result = self.common("ctn_select")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("manifest hash mismatch", result.stderr)

    def test_install_displays_changelog_and_packaged_kernels(self):
        result = self.run_script("customize.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("更新日志", result.stdout)
        self.assertIn("支持 6.1.141 / 6.1.157", result.stdout)
        self.assertIn("本包支持内核: 6.1.141 6.1.157", result.stdout)
        changelog = self.package / "changelog.txt"
        raw = changelog.read_bytes()
        put(changelog, raw.replace(b"v3.0", b"v9.9", 1))  # Display file, not a trust anchor.
        result = self.run_script("customize.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("已跳过显示", result.stdout)
        self.assertNotIn("v9.9", result.stdout)
        put(changelog, raw)

    def test_install_never_uses_loaded_old_module_as_compatibility_evidence(self):
        self.loaded(legacy=True)
        result = self.run_script("customize.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("NOT evidence", result.stdout)
        self.assertNotIn("insmod", self.events())
        self.assertNotIn("rmmod", self.events())
        put(self.vendor, b"wrong")
        self.assertNotEqual(self.run_script("customize.sh").returncode, 0)

    def test_upgrade_preserves_disable_and_remove(self):
        old = self.device / "data/adb/modules/ctn_patch"
        for flag in ("disable", "remove"):
            put(old / flag, "")
        result = self.run_script("customize.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.package / "disable").exists())
        self.assertTrue((self.package / "remove").exists())
        self.assertTrue((old / "disable").exists())
        self.assertTrue((old / "remove").exists())

    def test_unknown_native_node_blocks_install_and_loading(self):
        put(self.node, "NativeA:-1,NativeB:-1\n")
        self.assertNotEqual(self.run_script("customize.sh").returncode, 0)
        result = self.common("ctn_select && ctn_ensure_module")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("insmod", self.events())

    def test_disabled_service_action_and_exercise_do_not_enable(self):
        put(self.package / "disable", "keep disabled\n")
        result = self.run_script("service.sh", "--run")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_script("action.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_script("verify.sh", "--exercise")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.package / "disable").read_text(), "keep disabled\n")
        self.assertEqual(self.events(), "")
        self.assertTrue((self.runtime / "action.log").is_file())
        self.assertIn("read-only", (self.runtime / "action.log").read_text())

    def test_default_verify_and_readonly_action_propagate_errors(self):
        result = self.run_script("verify.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.runtime.exists())
        result = self.run_script("action.sh", "--read-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("FILE module=v3.0", result.stdout)
        self.assertIn("RUNNING module=not_loaded", result.stdout)
        self.assertIn("No runtime exercise", result.stdout)
        self.assertEqual(self.events(), "")
        log = self.runtime / "action.log"
        self.assertIn("SELF_CHECK_EXIT=0", log.read_text())
        self.assertIn(str(log), result.stdout)
        put(self.vendor, b"bad vendor")
        result = self.run_script("action.sh", "--read-only")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown vendor identity", log.read_text())
        self.assertIn("SELF_CHECK_EXIT=1", log.read_text())

    def test_action_default_exercises_and_logs(self):
        for version in PACK.PROFILES:
            with self.subTest(profile=version):
                self.log.unlink(missing_ok=True)
                put(self.vendor, self.fixture.vendors[version])
                self.loaded(version=version)
                result = self.run_script("action.sh", MOCK_RELEASE=version + "-android14-11-o-fixture")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("write_names CTNTestMain CTNTestGfx", self.events())
                self.assertIn("write_names SavedA SavedB", self.events())
                self.assertEqual(self.node.read_text(), "SavedA:-1,SavedB:-1\n")
                log = (self.runtime / "action.log").read_text()
                self.assertIn("SELF_CHECK_EXIT=0", log)
                self.assertIn("profile=" + version, log)

    def test_action_running_game_is_readonly(self):
        self.loaded()
        put(self.device / "proc/game_opt/game_pid", "game_pid=24680 child_num=1\n")
        result = self.run_script("action.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("write_names", self.events())
        self.assertIn("mode=read-only", (self.runtime / "action.log").read_text())

    def test_action_preserves_exercise_failure_code(self):
        self.loaded()
        result = self.run_script("action.sh", MOCK_FAIL_TEST="1", MOCK_FAIL_TEST_RC="19")
        self.assertEqual(result.returncode, 19, result.stderr)
        self.assertEqual(self.node.read_text(), "SavedA:-1,SavedB:-1\n")
        self.assertIn("SELF_CHECK_EXIT=19", (self.runtime / "action.log").read_text())

    def test_action_refuses_symlink_log(self):
        target = self.base / "keep.log"
        put(target, "keep this file\n")
        self.runtime.mkdir(exist_ok=True)
        (self.runtime / "action.log").symlink_to(target)
        result = self.run_script("action.sh", "--read-only")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_text(), "keep this file\n")

    def test_old_loaded_instance_is_never_reported_as_new(self):
        self.loaded(legacy=True)
        result = self.run_script("verify.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("profile=unknown", result.stdout)
        self.assertIn("reboot required", result.stderr)
        self.assertNotIn("insmod", self.events())
        result = self.run_script("service.sh", "--run")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("insmod", self.events())

    def test_new_running_identity_requires_profile_build_and_ready(self):
        self.loaded()
        self.assertEqual(self.common("ctn_select && ctn_running_identity").returncode, 0)
        params = self.device / "sys/module/ctn_patch/parameters"
        for key, value in (("profile", "6.1.141"), ("build_id", "f" * 64), ("patch_ready", "N")):
            with self.subTest(key=key):
                path = params / key
                raw = path.read_bytes()
                put(path, value + "\n")
                self.assertNotEqual(self.common("ctn_select && ctn_running_identity").returncode, 0)
                put(path, raw)

    def test_no_node_means_no_daemon_and_single_insmod_failure_is_preserved(self):
        result = self.common("ctn_select && ctn_ensure_module", MOCK_INSMOD_RC="17")
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertEqual(self.events().count("insmod"), 1)
        self.loaded()
        self.node.unlink()
        result = self.common("ctn_select && ctn_ensure_module")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no usable node", result.stderr)

    def test_daemon_identity_uses_exact_exe_not_comm_and_hash(self):
        self.daemon(unrelated=True)
        self.assertNotEqual(self.common("ctn_select && ctn_daemon_pid").returncode, 0)
        self.assertEqual(self.common("ctn_runtime_prepare && ctn_stop_daemon").returncode, 0)
        self.assertNotIn("kill", self.events())
        self.daemon(old_image=True, deleted=True)
        result = self.common("ctn_select && ctn_daemon_pid && ctn_daemon_current")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("765432", result.stdout)
        self.daemon()
        self.assertEqual(self.common("ctn_select && ctn_daemon_current").returncode, 0)
        self.assertNotEqual(self.common("ctn_select && ctn_daemon_current",
                                        MOCK_DAEMON_UNLOCKED="1").returncode, 0)
        (self.runtime / ".daemon.lock").unlink()
        self.assertNotEqual(self.common("ctn_select && ctn_daemon_current").returncode, 0)
        self.assertFalse((self.runtime / ".daemon.lock").exists())
        put(self.runtime / ".daemon.lock", "lock inode must not be truncated")
        result = self.common('''exec 5>"$CTN_RUNTIME/.daemon.lock" || exit 91
command flock -n 5 5>&5 || exit 91
printf '%s' 'lock inode must not be truncated' >&5
ctn_daemon_idle && exit 92
exec 5>&-
ctn_daemon_idle''', MOCK_REAL_FLOCK="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.runtime / ".daemon.lock").read_text(), "lock inode must not be truncated")

    def test_pid_hint_is_strict_and_starttime_is_parsed_after_spaced_comm(self):
        self.daemon()
        result = self.common("ctn_process_start 765432")
        self.assertEqual(result.stdout.strip(), "45678")
        for value in ("765432evil", "765432\n999", "0", "1", "-1"):
            with self.subTest(value=value):
                put(self.runtime / "daemon.pid", value + "\n")
                self.assertNotEqual(self.common("ctn_daemon_pid").returncode, 0)

    def test_legacy_migration_only_signals_own_old_exe_and_keeps_stop(self):
        self.daemon(legacy=True, unrelated=True)
        self.assertEqual(self.common("ctn_stop_legacy").returncode, 0)
        self.assertNotIn("kill", self.events())
        self.daemon(legacy=True, old_image=True, deleted=True)
        result = self.common("ctn_stop_legacy")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events().count("kill -TERM 765432"), 1)
        self.assertTrue((self.package / ".stop").exists())

    def test_service_reuses_current_daemon_and_stops_gracefully(self):
        self.loaded()
        self.daemon()
        result = self.run_script("service.sh", "--run", MOCK_STOP_ON_SLEEP="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("existing current daemon", result.stdout)
        self.assertNotIn("insmod", self.events())
        self.assertIn("kill -TERM 765432", self.events())
        self.assertFalse((self.runtime / "service.pid").exists())

    def test_duplicate_service_lock_never_loads_or_signals(self):
        self.loaded()
        result = self.run_script("service.sh", "--run", MOCK_LOCK_HELD="service")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("another service", result.stdout)
        self.assertEqual(self.events(), "")
        result = self.run_script("service.sh", "--run", MOCK_FLOCK_ERROR="9")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertNotIn("another service", result.stdout)
        self.assertEqual(self.events(), "")

    def test_uninstall_restores_daemon_before_rmmod_and_removes_only_runtime(self):
        self.loaded()
        self.daemon()
        put(self.runtime / "restore.state", "opaque daemon-only debt")
        put(self.runtime / "action.log", "SELF_CHECK_EXIT=0\n")
        put(self.package / "disable", "")
        outside = self.device / "data/adb/other-module/keep"
        put(outside, "untouched")
        result = self.run_script("uninstall.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        events = self.events()
        self.assertLess(events.index("kill -TERM"), events.index("rmmod"))
        self.assertFalse(self.runtime.exists())
        self.assertEqual(outside.read_text(), "untouched")
        self.assertTrue((self.package / "disable").exists())

    def test_uninstall_restore_failure_timeout_or_lock_conflict_prevents_rmmod(self):
        for env in ({"MOCK_RESTORE_FAIL": "1"}, {"MOCK_TERM_TIMEOUT": "1"}, {"MOCK_LOCK_HELD": "daemon"}):
            with self.subTest(env=env):
                self.loaded()
                self.daemon()
                put(self.runtime / "restore.state", "keep debt")
                self.log.unlink(missing_ok=True)
                result = self.run_script("uninstall.sh", **env)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("rmmod", self.events())
                self.assertTrue(self.runtime.exists())
                if env != {"MOCK_LOCK_HELD": "daemon"}:
                    self.assertTrue((self.runtime / "restore.state").exists())

    def test_uninstall_does_not_unload_with_selfcheck_recovery_debt(self):
        self.loaded()
        put(self.runtime / "exercise.state",
            "boot_id=01234567-89ab-cdef-0123-456789abcdef\nnames=SavedA SavedB\nenable=0\n")
        result = self.run_script("uninstall.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("rmmod", self.events())
        self.assertTrue((self.runtime / "exercise.state").exists())
        self.assertTrue((self.device / "sys/module/ctn_patch").exists())

    def test_uninstall_rmmod_failure_is_not_masked(self):
        self.loaded()
        result = self.run_script("uninstall.sh", MOCK_RMMOD_RC="23")
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertTrue(self.runtime.exists())

    def test_exercise_restores_exact_names_and_enable_without_rt_info(self):
        self.loaded()
        put(self.enable, "1\n")
        result = self.run_script("verify.sh", "--exercise")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.node.read_text(), "SavedA:-1,SavedB:-1\n")
        self.assertEqual(self.enable.read_text(), "1\n")
        self.assertNotIn("rmmod", self.events())
        self.assertNotIn("rt_info", self.events())
        self.assertIn("write_names SavedA SavedB", self.events())
        self.assertFalse((self.runtime / ".service.stop").exists())

    def test_exercise_failure_and_term_trap_restore_original_values(self):
        for env, code in (({"MOCK_FAIL_TEST": "1"}, 1),
                          ({"MOCK_FAIL_TEST": "1", "MOCK_FAIL_TEST_RC": "19"}, 19),
                          ({"MOCK_SIGNAL_TEST": "1"}, 143)):
            with self.subTest(env=env):
                self.loaded()
                result = self.run_script("verify.sh", "--exercise", **env)
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertEqual(self.node.read_text(), "SavedA:-1,SavedB:-1\n")

    def test_exercise_only_unloads_module_it_loaded(self):
        result = self.run_script("verify.sh", "--exercise")
        self.assertEqual(result.returncode, 0, result.stderr)
        events = self.events()
        self.assertLess(events.index("insmod"), events.index("write_names"))
        self.assertLess(events.rindex("write_names SavedA SavedB"), events.index("rmmod"))
        self.assertFalse((self.device / "sys/module/ctn_patch").exists())
        self.log.unlink()
        result = self.run_script("verify.sh", "--exercise", MOCK_RMMOD_RC="23")
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(self.events().count("rmmod"), 1)
        self.assertTrue((self.device / "sys/module/ctn_patch").exists())
        self.assertTrue((self.runtime / ".service.stop").exists())

    def test_exercise_restore_failure_keeps_test_ko_and_reports_failure(self):
        result = self.run_script("verify.sh", "--exercise", MOCK_FAIL_RESTORE="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("rmmod", self.events())
        self.assertTrue((self.runtime / ".service.stop").exists())
        self.assertIn("cannot restore original names", result.stderr)

    def test_exercise_refuses_unmanaged_daemon_and_unrestorable_names(self):
        self.loaded()
        self.daemon()
        result = self.run_script("verify.sh", "--exercise")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("standalone daemon", result.stderr)
        self.assertNotIn("kill", self.events())
        (self.device / "proc/765432/alive").unlink()
        put(self.node, "ambiguous:value:3,a:3,extra:4\n")
        result = self.run_script("verify.sh", "--exercise")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("write_names", self.events())

    def test_missing_flock_is_fail_closed(self):
        result = self.run_script("service.sh", "--run", MOCK_FLOCK_ERROR="1")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(self.events(), "")

    def test_shell_sources_are_syntax_valid_and_no_global_process_scan(self):
        for file in ("common.sh", "service.sh", "customize.sh", "action.sh", "uninstall.sh", "../verify.sh"):
            path = REPO / "magisk" / file
            result = subprocess.run([SH, "-n", shell_path(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        text = (REPO / "magisk/common.sh").read_text(encoding="utf-8")
        self.assertNotRegex(text, r"for\s+\S+\s+in\s+/proc/")
        self.assertNotRegex(text, r"\b(?:pkill|killall)\s")
        self.assertNotRegex((REPO / "verify.sh").read_text(encoding="utf-8"), r"dmesg\s+-[cC]")


if __name__ == "__main__":
    unittest.main(verbosity=2)
