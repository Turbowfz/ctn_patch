#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a verified, allowlisted Magisk/KernelSU/APatch package (never publish it).

Default: both profiles, including successful runtime verification of these exact KOs.
--development: allow statically verified, not-yet-runtime-tested development artifacts.
--profile VERSION: explicitly build a development-only single-profile package.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import struct
import tempfile
import zipfile
import zlib

HERE = Path(__file__).resolve().parent
# Load the bounded reader from this repository, also under isolated Python (-I).
_ABI_SPEC = importlib.util.spec_from_file_location("ctn_package_abi", HERE / "tools/abi.py")
assert _ABI_SPEC and _ABI_SPEC.loader
_ABI = importlib.util.module_from_spec(_ABI_SPEC)
_ABI_SPEC.loader.exec_module(_ABI)
PROFILES = ("6.1.141", "6.1.157")
REQUIRED_CHECKS = frozenset(
    ("vermagic", "symbol_crcs", "kcfi", "module_layout", "undefined_symbols", "elf_sections")
)
VENDOR_PATHS = frozenset(
    ("/vendor/lib/modules/oplus_bsp_game_opt.ko", "/vendor_dlkm/lib/modules/oplus_bsp_game_opt.ko")
)
# No fallback to the repository README, legacy root KO, or daemon/ build products.
SOURCES = {
    "module.prop": ("magisk/module.prop", 0o644),
    "customize.sh": ("magisk/customize.sh", 0o755),
    "service.sh": ("magisk/service.sh", 0o755),
    "action.sh": ("magisk/action.sh", 0o755),
    "uninstall.sh": ("magisk/uninstall.sh", 0o755),
    "verify.sh": ("verify.sh", 0o755),
    "lib/common.sh": ("magisk/common.sh", 0o755),
    "changelog.txt": ("magisk/changelog.txt", 0o644),
    "README.md": ("magisk/README.md", 0o644),
}
BINARY_SOURCES = {
    "oplus.png": ("magisk/oplus.png", 0o644),
}
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
TOKEN = re.compile(r"[A-Za-z0-9_.+-]+\Z")


class PackageError(ValueError):
    """An input or the finished archive did not satisfy the package contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PackageError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_required(root: Path, relative: str) -> bytes:
    path = root / relative
    require(not any(p.is_symlink() for p in (path, *path.parents) if p != root.parent),
            f"symlink input is not allowed: {relative}")
    require(path.is_file(), f"missing required input: {relative}")
    return path.read_bytes()


def text_bytes(data: bytes, label: str) -> bytes:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PackageError(f"invalid UTF-8: {label}") from exc
    require("\x00" not in text and not text.startswith("\ufeff"), f"invalid text: {label}")
    return (text.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n") + "\n").encode("utf-8")


def json_object(data: bytes, label: str) -> dict:
    def pairs(items: list[tuple[str, object]]) -> dict:
        result: dict = {}
        for key, value in items:
            require(key not in result, f"duplicate JSON key {key}: {label}")
            result[key] = value
        return result

    def invalid_constant(value: str) -> None:
        raise PackageError(f"invalid JSON constant {value}: {label}")

    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=pairs,
                           parse_constant=invalid_constant)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PackageError(f"invalid JSON: {label}: {exc}") from exc
    require(isinstance(value, dict), f"expected JSON object: {label}")
    return value


def hash_field(value: object, label: str) -> str:
    require(isinstance(value, str) and SHA256.fullmatch(value) is not None,
            f"invalid SHA256: {label}")
    return value


def string_list(value: object, label: str) -> list[str]:
    require(isinstance(value, list) and bool(value), f"missing/non-list {label}")
    require(all(isinstance(item, str) and item for item in value), f"invalid {label}")
    require(len(set(value)) == len(value), f"duplicate {label}")
    return value


def elf_check(data: bytes, label: str, executable: bool = False) -> None:
    require(len(data) >= 64 and data[:7] == b"\x7fELF\x02\x01\x01",
            f"not a little-endian ELF64 input: {label}")
    kind, machine = struct.unpack_from("<HH", data, 16)
    require(machine == 183, f"not AArch64: {label}")
    require(kind in ((2, 3) if executable else (1,)), f"wrong ELF type: {label}")


def png_check(data: bytes) -> None:
    require(8 < len(data) <= 4 * 1024 * 1024 and data[:8] == b"\x89PNG\r\n\x1a\n",
            "invalid/oversized module banner PNG")
    offset = 8
    seen_header = seen_data = False
    while offset < len(data):
        require(offset + 12 <= len(data), "truncated module banner PNG")
        size, = struct.unpack_from(">I", data, offset)
        end = offset + 12 + size
        require(end <= len(data), "module banner PNG chunk outside file")
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:end - 4]
        crc, = struct.unpack_from(">I", data, end - 4)
        require(zlib.crc32(data[offset + 4:end - 4]) & 0xffffffff == crc,
                "module banner PNG checksum mismatch")
        if not seen_header:
            require(kind == b"IHDR" and size == 13, "module banner PNG lacks IHDR")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", body)
            require(0 < width <= 4096 and 0 < height <= 4096 and depth == 8 and
                    color in (2, 6) and compression == filtering == interlace == 0,
                    "unsupported module banner PNG layout")
            seen_header = True
        elif kind == b"IHDR":
            raise PackageError("duplicate module banner PNG header")
        if kind == b"IDAT":
            seen_data = True
        if kind == b"IEND":
            require(size == 0 and seen_data and end == len(data), "invalid module banner PNG end")
            return
        offset = end
    raise PackageError("module banner PNG lacks IEND")


def module_properties(data: bytes) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in data.decode("utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        require(bool(sep) and key not in values, "malformed/duplicate module.prop key")
        values[key] = value
    require(values.get("id") == "ctn_patch", "module.prop id must remain ctn_patch")
    require(bool(TOKEN.fullmatch(values.get("version", ""))), "invalid module.prop version")
    require(bool(re.fullmatch(r"[0-9]+", values.get("versionCode", ""))),
            "invalid module.prop versionCode")
    require(bool(re.fullmatch(r"v?[0-9]+(?:\.[0-9]+)+", values["version"])),
            "module.prop version cannot be mapped to the kernel version string")
    return values


def validate_profile(profile: dict, version: str) -> None:
    require(type(profile.get("schema")) is int and profile["schema"] == 1,
            f"unsupported profile schema: {version}")
    require(profile.get("id") == version and profile.get("kernel_version") == version,
            f"profile identity mismatch: {version}")
    require(profile.get("arch") == "aarch64" and type(profile.get("page_size")) is int
            and profile["page_size"] == 4096, f"unsupported architecture/page size: {version}")
    releases = string_list(profile.get("runtime_releases"), f"{version}.runtime_releases")
    require(all(TOKEN.fullmatch(r) and (r == version or r.startswith(version + "-"))
                for r in releases), f"unsafe/cross-profile runtime release: {version}")
    require(profile.get("native_node") is False,
            f"no verified native-node/daemon-only profile is supported yet: {version}")
    vendor = profile.get("vendor")
    require(isinstance(vendor, dict) and vendor.get("name") == "oplus_bsp_game_opt",
            f"invalid vendor identity: {version}")
    paths = string_list(vendor.get("paths"), f"{version}.vendor.paths")
    require(set(paths) <= VENDOR_PATHS, f"unapproved vendor path: {version}")
    for value in string_list(vendor.get("sha256"), f"{version}.vendor.sha256"):
        hash_field(value, f"{version}.vendor.sha256")
    vermagic = vendor.get("vermagic")
    require(isinstance(vermagic, str) and
            re.fullmatch(re.escape(version) +
                         r"-[A-Za-z0-9_.+-]+ SMP preempt mod_unload modversions aarch64",
                         vermagic) is not None,
            f"missing/full vermagic mismatch: {version}")
    for key in ("abi", "source"):
        require(isinstance(profile.get(key), dict) and bool(profile[key]),
                f"missing profile {key} evidence: {version}")


def validate_verification(record: dict, version: str, profile_bytes: bytes,
                          module_bytes: bytes, development: bool, *,
                          module_version: str, vermagic: str) -> None:
    require(type(record.get("schema")) is int and record["schema"] == 1,
            f"unsupported verification schema: {version}")
    require(record.get("profile_id") == version, f"verification profile mismatch: {version}")
    require(hash_field(record.get("profile_sha256"), "profile_sha256") == digest(profile_bytes),
            f"profile hash mismatch: {version}")
    require(hash_field(record.get("module_sha256"), "module_sha256") == digest(module_bytes),
            f"module hash mismatch: {version}")
    build_id = hash_field(record.get("build_id"), f"{version}.build_id")
    # Hashes bind files, but cannot make stale embedded identity match new metadata.
    # Parse only section-backed .modinfo from the exact bytes being packaged.
    try:
        ko = _ABI.Elf(f"{version}/ctn_patch.ko", data=module_bytes)
        section = ko.by_name.get(".modinfo")
        require(section is not None and section["type"] == 1,
                f"missing/invalid ELF .modinfo section: {version}")
        raw_info = ko.blob(section)
        require(raw_info.endswith(b"\0"), f"unterminated/empty .modinfo: {version}")
        info = ko.info()
    except (_ABI.FormatError, UnicodeDecodeError, struct.error) as exc:
        raise PackageError(f"invalid ELF/.modinfo: {version}: {exc}") from exc
    require(all(re.fullmatch(r"[A-Za-z0-9_]+", key) for key in info),
            f"malformed .modinfo key: {version}")
    for key, expected in (("version", module_version), ("ctn_profile", version),
                          ("ctn_build_id", build_id), ("vermagic", vermagic)):
        values = info.get(key, [])
        require(len(values) == 1, f"missing/duplicate modinfo {key}: {version}")
        require(values[0] == expected, f"modinfo {key} mismatch: {version}")
    require(record.get("static") == "passed", f"static verification not passed: {version}")
    require(record.get("runtime") in ("passed", "not_tested"),
            f"failed/invalid runtime verification: {version}")
    require(development or record["runtime"] == "passed",
            f"runtime verification not passed for this KO: {version}; use --development explicitly")
    checks = record.get("checks")
    require(isinstance(checks, list) and bool(checks), f"missing verification checks: {version}")
    names: set[str] = set()
    for check in checks:
        require(isinstance(check, dict) and isinstance(check.get("name"), str)
                and bool(check["name"]) and check.get("passed") is True,
                f"failed/malformed verification check: {version}")
        require(check["name"] not in names, f"duplicate verification check: {version}")
        names.add(check["name"])
    require(REQUIRED_CHECKS <= names,
            f"missing required verification checks: {version}: {sorted(REQUIRED_CHECKS - names)}")


def validate_changelog(data: bytes, props: dict[str, str]) -> None:
    require(len(data) <= 32768, "in-module changelog history is too large")
    lines = data.decode("utf-8").splitlines()
    expected = f"CTN Patch {props['version']}（versionCode {props['versionCode']}）更新日志"
    require(bool(lines) and lines[0] == expected,
            "in-module changelog does not match the packaged version")
    versions: set[str] = set()
    section_bytes = bullets = 0
    previous: tuple[int, int] | None = None
    for line in lines:
        match = re.fullmatch(r"CTN Patch v?(\d+)\.(\d+)（versionCode (\d+)）更新日志", line)
        if match:
            version = (int(match[1]), int(match[2]))
            name = f"v{version[0]}.{version[1]}"
            require(name not in versions and (previous is None or version < previous),
                    "duplicate or unordered in-module changelog version")
            require(int(match[3]) == version[0] * 10 + version[1], "invalid changelog versionCode")
            require(previous is None or 1 <= bullets <= 4, "changelog sections need 1-4 concise bullets")
            versions.add(name)
            previous = version
            section_bytes = len(line.encode("utf-8"))
            bullets = 0
        elif line:
            require(previous is not None and line.startswith("- ") and len(line) <= 122,
                    "changelog must contain only concise bullet lines")
            require(re.search(r"[0-9a-fA-F]{64}", line) is None,
                    "full hashes belong in verification reports, not changelog")
            bullets += 1
            section_bytes += len(line.encode("utf-8")) + 1
        require(section_bytes <= 2048 and bullets <= 4, "changelog section must stay concise")
    require(previous is not None and 1 <= bullets <= 4, "changelog sections need 1-4 concise bullets")
    historical = {f"v1.{minor}" for minor in range(10)} | {"v2.0", "v2.1", "v2.2", "v3.0"}
    require(historical <= versions, "in-module changelog is missing historical versions")


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def collect_entries(root: Path, profile: str | None = None,
                    development: bool = False,
                    artifact_root: Path | None = None) -> tuple[dict[str, tuple[bytes, int]], dict[str, str]]:
    root = root.resolve()
    artifact_root = (artifact_root or root / "out").resolve()
    require(profile is None or profile in PROFILES, "unknown --profile")
    development = development or profile is not None
    selected = (profile,) if profile else PROFILES
    mode = "development" if development else "release"
    entries = {name: (text_bytes(read_required(root, path), path), perms)
               for name, (path, perms) in SOURCES.items()}
    props = module_properties(entries["module.prop"][0])
    require(props.get("banner") == "oplus.png", "module banner must be the packaged relative oplus.png")
    for name, (relative, permissions) in BINARY_SOURCES.items():
        data = read_required(root, relative)
        png_check(data)
        entries[name] = (data, permissions)
    # Every historical section stays concise; the first one belongs to this package.
    validate_changelog(entries["changelog.txt"][0], props)
    daemon = read_required(artifact_root, "user/ctnd")
    elf_check(daemon, "user/ctnd", executable=True)
    entries["bin/ctnd"] = (daemon, 0o755)
    for version in selected:
        profile_bytes = read_required(root, f"profiles/{version}/profile.json")
        spec = json_object(profile_bytes, f"profiles/{version}/profile.json")
        validate_profile(spec, version)
        ko = read_required(artifact_root, f"{version}/ctn_patch.ko")
        elf_check(ko, f"{version}/ctn_patch.ko")
        record = json_object(read_required(artifact_root, f"{version}/verification.json"),
                             f"{version}/verification.json")
        validate_verification(record, version, profile_bytes, ko, development,
                              module_version=props["version"].removeprefix("v"),
                              vermagic=spec["vendor"]["vermagic"])
        metadata = {
            "schema": "1",
            "profile_id": version,
            "kernel_version": version,
            "arch": "aarch64",
            "page_size": "4096",
            "runtime_kernel_release_list": "|".join(spec["runtime_releases"]),
            "vendor_paths": "|".join(spec["vendor"]["paths"]),
            "vendor_sha256": "|".join(spec["vendor"]["sha256"]),
            "vendor_vermagic": spec["vendor"]["vermagic"],
            "module_sha256": digest(ko),
            "profile_sha256": digest(profile_bytes),
            "build_id": record["build_id"],
            "module_version": props["version"].removeprefix("v"),
            "daemon_sha256": digest(daemon),
            "native_node": "0",
            "package_mode": mode,
            "static_verification": "passed",
            "runtime_verification": record["runtime"],
        }
        manifest = dict(metadata, schema=1, page_size=4096, native_node=False,
                        runtime_kernel_release_list=spec["runtime_releases"],
                        vendor_paths=spec["vendor"]["paths"], vendor_sha256=spec["vendor"]["sha256"],
                        profile=spec, verification=record)
        manifest_bytes = json_bytes(manifest)
        metadata["manifest_sha256"] = digest(manifest_bytes)
        prop_bytes = ("".join(f"{key}={value}\n" for key, value in sorted(metadata.items()))).encode("utf-8")
        prefix = f"kernels/{version}/"
        entries[prefix + "ctn_patch.ko"] = (ko, 0o644)
        entries[prefix + "manifest.json"] = (manifest_bytes, 0o644)
        entries[prefix + "metadata.prop"] = (prop_bytes, 0o644)
    if development:
        warning = ("# DEVELOPMENT PACKAGE — not a fully runtime-verified release\n\n"
                   f"Profiles included: {', '.join(selected)}. This package was explicitly built in "
                   "development mode; it must not be presented as a fully tested release. "
                   "See each manifest's runtime verification status.\n\n").encode("utf-8")
        entries["README.md"] = (warning + entries["README.md"][0], 0o644)
    props = dict(props, package_mode=mode, profiles="-".join(selected))
    return entries, props


def validate_archive(path: Path, entries: dict[str, tuple[bytes, int]]) -> None:
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) == len(set(names)) and set(names) == set(entries),
                "ZIP whitelist mismatch (missing, extra, or duplicate entries)")
        require(archive.testzip() is None, "ZIP CRC check failed")
        for name, (data, permissions) in entries.items():
            item = archive.getinfo(name)
            mode = item.external_attr >> 16
            require(item.create_system == 3 and stat.S_ISREG(mode)
                    and stat.S_IMODE(mode) == permissions, f"ZIP Unix permission mismatch: {name}")
            actual = archive.read(name)
            require(digest(actual) == digest(data), f"ZIP content hash mismatch: {name}")
            if name.endswith((".sh", ".prop", ".json", ".md")):
                require(b"\r" not in actual and actual.endswith(b"\n"), f"ZIP text is not LF: {name}")


def build_package(root: Path, output: Path | None = None, profile: str | None = None,
                  development: bool = False, artifact_root: Path | None = None) -> Path:
    entries, props = collect_entries(root, profile, development, artifact_root)
    suffix = "full" if props["package_mode"] == "release" else f"development-{props['profiles']}"
    output = output or root / f"{props['id']}-{props['version']}-{suffix}.zip"
    output = output.resolve()
    require(output.suffix.lower() == ".zip", "--out must name a .zip file")
    require(output.parent.is_dir(), f"output directory does not exist: {output.parent}")
    # Gather/verify everything BEFORE touching even a pre-existing output package.
    fd, temporary = tempfile.mkstemp(prefix=".ctn-package-", suffix=".zip", dir=output.parent)
    os.close(fd)
    temp_path = Path(temporary)
    try:
        with zipfile.ZipFile(temp_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, (data, permissions) in sorted(entries.items()):
                item = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                item.create_system = 3
                item.compress_type = zipfile.ZIP_DEFLATED
                item.external_attr = (stat.S_IFREG | permissions) << 16
                archive.writestr(item, data)
        validate_archive(temp_path, entries)
        os.replace(temp_path, output)
    finally:
        temp_path.unlink(missing_ok=True)
    print(f"{props['package_mode']}: {output}")
    print(f"SHA256 {digest(output.read_bytes())}")
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, help="output .zip (parent directory must exist)")
    parser.add_argument("--profile", choices=PROFILES, help="development-only single-profile package")
    parser.add_argument("--artifact-root", type=Path, help="isolated verified build output root; default out/")
    parser.add_argument("--development", action="store_true",
                        help="explicitly allow not-yet-runtime-tested development KOs")
    args = parser.parse_args(argv)
    try:
        build_package(HERE, args.out, args.profile, args.development, args.artifact_root)
    except (PackageError, OSError, zipfile.BadZipFile) as exc:
        parser.exit(1, f"package refused: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
