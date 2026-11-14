#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Isolated Linux host lifecycle tests; never use a device or production /data.

Run sequentially: python3 -I -B tests/test_service_lifecycle.py
                  CTN_TEST_SHELL=mksh python3 -I -B tests/test_service_lifecycle.py
Only package copies model Android identity/node readback. Service, Action,
verification, process identity, flock, and the compiled production ctnd are real.
The game stays idle, so ctnd never discovers/reads a production COSA database.
"""
from __future__ import annotations

import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ctnp_packaging_fixture", REPO / "tests/test_packaging.py")
assert SPEC and SPEC.loader
FIXTURES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURES)
SH = shutil.which(os.environ.get("CTN_TEST_SHELL", "sh"))
CC = shutil.which(os.environ.get("CC", "cc"))
WAIT = 20


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"fixture hook no longer matches production: {old!r}")
    return text.replace(old, new, 1)


def text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return ""


def process_identity(pid: int):
    try:
        proc = Path("/proc") / str(pid)
        fields = (proc / "stat").read_text().rsplit(") ", 1)[1].split()
        argv = tuple(os.fsdecode(arg) for arg in (proc / "cmdline").read_bytes().split(b"\0")[:-1])
        return fields[19], os.readlink(proc / "exe"), argv
    except (OSError, IndexError):
        return None


NODE_WRITE = r'''ctn_write_node() {
    local input=$1
    printf '%s\n' "$input" >> "$CTN_RUNTIME/test-writes"
    set -- $input
    [ "$#" = 1 ] || [ "$#" = 2 ] || return 1
    printf '%s:-1,%s:-1\n' "$1" "${2:-$1}" > "$CTN_NODE" || return $?
    [ "$input" = 'CTNTestMain CTNTestGfx' ] || return 0
    printf '%s\n' "$$" > "$CTN_RUNTIME/test-write.pid" || return $?
    # Synchronous only: TERM reaches verify's real trap; no background writer.
    if [ "${CTN_LIFECYCLE_HOLD:-0}" = 1 ]; then
        while :; do sleep 0.05; done
    fi
    return "${CTN_LIFECYCLE_FAIL:-0}"
}'''

HOST_COMMANDS = r'''
# Fixture-only Android facts. Never mock kill/readlink/hash/flock/sleep/ctnd.
uname() {
    case "$1" in
        -r) printf '6.1.157-android14-11-o-fixture\n' ;;
        -m) printf 'aarch64\n' ;;
        *) return 99 ;;
    esac
}
getconf() { printf '4096\n'; }
id() { [ "$#:$1" = '1:-u' ] || return 99; printf '0\n'; }
forbidden_command() {
    printf '%s\n' "$*" >> "$CTN_RUNTIME/forbidden-commands"
    ctn_error "forbidden host-test command: $*"
    return 99
}
insmod() { forbidden_command insmod "$@"; }
rmmod() { forbidden_command rmmod "$@"; }
dmesg() { forbidden_command dmesg "$@"; }
'''


@unittest.skipUnless(sys.platform.startswith("linux") and SH and CC and shutil.which("flock"),
                     "Linux /proc, a native C compiler, POSIX shell, and flock are required")
class ServiceLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Production service re-execs/backgrounds itself, including on resume.
        # Adopt only our descendants so every orphan we spawn can be waited for.
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        if libc.prctl(37, ctypes.byref(previous), 0, 0, 0) or libc.prctl(36, 1, 0, 0, 0):
            raise unittest.SkipTest("Linux child-subreaper support is required for guaranteed cleanup")
        cls.addClassCleanup(libc.prctl, 36, previous.value, 0, 0, 0)

    def setUp(self):
        import fcntl
        self.fcntl = fcntl
        self.temp = tempfile.TemporaryDirectory(prefix="ctnp-lifecycle-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.package = self.base / "package"
        self.device = self.base / "device"
        self.runtime = self.device / "data/adb/ctn_patch"
        self.node = self.device / "proc/game_opt/task_boost/critical_task_name"
        self.enable = self.node.parent / "ct_enable"
        self.game = self.device / "proc/game_opt/game_pid"
        self.daemon = self.package / "bin/ctnd"
        self.processes = {}
        self.owned = {}
        self.addCleanup(self.cleanup_processes)
        fixture = FIXTURES.Fixture(self.base / "source")
        for name, (data, mode) in fixture.collect().items():
            FIXTURES.put(self.package / name, data)
            (self.package / name).chmod(mode)
        self.runtime.parent.mkdir(parents=True)
        FIXTURES.put(self.device / "vendor/lib/modules/oplus_bsp_game_opt.ko", fixture.vendors["6.1.157"])
        FIXTURES.put(self.device / "proc/modules", "oplus_bsp_game_opt 123 0 - Live 0\n")
        FIXTURES.put(self.game, "game_pid=-1 child_num=0\n")
        FIXTURES.put(self.node, "SavedA:-1,SavedB:-1\n")
        FIXTURES.put(self.enable, "1\n")  # Non-default originals must survive, not be reset to zero.
        loaded = self.device / "sys/module/ctn_patch"
        properties = FIXTURES.PACK.module_properties((self.package / "module.prop").read_bytes())
        self.module_version = properties["version"].removeprefix("v")
        for leaf, value in {"version": self.module_version, "parameters/profile": "6.1.157",
                            "parameters/build_id": FIXTURES.sha(b"build 6.1.157"),
                            "parameters/patch_ready": "Y"}.items():
            FIXTURES.put(loaded / leaf, value + "\n")

        common = self.package / "lib/common.sh"
        script = text(common)
        for old, new in (
            ("    CTN_ROOT=\n", f"    CTN_ROOT={shlex.quote(str(self.device))}\n"),
            ('    CTN_PROC="$CTN_ROOT/proc"', "    CTN_PROC=/proc"),
            ('    CTN_NODE="$CTN_PROC/game_opt/task_boost/critical_task_name"',
             f"    CTN_NODE={shlex.quote(str(self.node))}"),
            ('    CTN_ENABLE="$CTN_PROC/game_opt/task_boost/ct_enable"',
             f"    CTN_ENABLE={shlex.quote(str(self.enable))}"),
            ('    CTN_SHELL=$(command -v sh) || {', f"    CTN_SHELL={shlex.quote(SH)} || {{"),
            ('    value=$(ctn_scalar "$CTN_PROC/game_opt/game_pid") || return 1',
             '    value=$(ctn_scalar "$CTN_ROOT/proc/game_opt/game_pid") || return 1'),
            ('    ctn_note "READ game_state=$(ctn_scalar "$CTN_PROC/game_opt/game_pid")"',
             '    ctn_note "READ game_state=$(ctn_scalar "$CTN_ROOT/proc/game_opt/game_pid")"'),
            ('"$CTN_PROC/modules"', '"$CTN_ROOT/proc/modules"'),
            ('"$CTN_PROC/game_opt/task_boost"', '"$CTN_ROOT/proc/game_opt/task_boost"'),
        ):
            script = replace_once(script, old, new)
        common.write_text(script + HOST_COMMANDS, encoding="utf-8")
        verify = self.package / "verify.sh"
        verify.write_text(replace_once(text(verify),
                          r'''ctn_write_node() { printf '%s\n' "$1" > "$CTN_NODE"; }''', NODE_WRITE),
                          encoding="utf-8")

        definitions = {"CTND_RUNTIME_DIR": self.runtime, "CTND_NAMES_PATH": self.node,
                       "CTND_ENABLE_PATH": self.enable, "CTND_GAMEPID_PATH": self.game,
                       "CTND_KMSG_PATH": Path("/dev/null")}
        command = [CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror"]
        command += [f"-D{name}={json.dumps(str(path))}" for name, path in definitions.items()]
        command += [str(REPO / "daemon" / name) for name in
                    ("ctnd.c", "ctnd_parse.c", "ctnd_state.c", "ctnd_runtime.c", "ctnd_fs.c", "ctnd_db.c")]
        result = subprocess.run(command + ["-ldl", "-o", str(self.daemon)], cwd=self.base,
                                capture_output=True, text=True, timeout=WAIT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.daemon.chmod(0o755)
        # Packaging correctly rejects a host ELF as an Android release image.
        # Generate with its ELF fixture first, then bind both manifests to the
        # actual host daemon without bypassing any runtime metadata/hash gates.
        daemon_hash = FIXTURES.sha(self.daemon.read_bytes())
        for profile in FIXTURES.PACK.PROFILES:
            directory = self.package / "kernels" / profile
            manifest = json.loads(text(directory / "manifest.json"))
            manifest["daemon_sha256"] = daemon_hash
            raw = FIXTURES.PACK.json_bytes(manifest)
            (directory / "manifest.json").write_bytes(raw)
            metadata = dict(line.split("=", 1) for line in text(directory / "metadata.prop").splitlines())
            metadata.update(daemon_sha256=daemon_hash, manifest_sha256=FIXTURES.sha(raw))
            (directory / "metadata.prop").write_text(
                "".join(f"{key}={value}\n" for key, value in sorted(metadata.items())), encoding="utf-8")

    def wait_for(self, predicate, label):
        deadline = time.monotonic() + WAIT
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.05)
        logs = "\n".join(f"{name}:\n{text(self.runtime / name)}" for name in
                         ("boot.log", "daemon.log", "action.log", "action-detail.log"))
        self.fail(f"timeout ({WAIT}s): {label}\n{logs}")

    def spawn(self, script, *args, **env):
        log = self.base / f"process-{len(self.processes)}.log"
        with log.open("wb") as output:
            proc = subprocess.Popen([SH, str(self.package / script), *args], cwd=self.base,
                                    env=dict(os.environ, **env), stdin=subprocess.DEVNULL,
                                    stdout=output, stderr=output, start_new_session=True)
        self.processes[proc.pid] = (proc, log)
        self.remember(proc.pid)
        return proc

    def run_script(self, script, *args, **env):
        proc = self.spawn(script, *args, **env)
        rc = proc.wait(timeout=WAIT)
        return rc, text(self.processes[proc.pid][1])

    def remember(self, pid):
        identity = process_identity(pid)
        if identity:
            self.owned[pid] = identity
        return identity

    def discover(self):
        scripts = {str(self.package / name) for name in ("service.sh", "action.sh", "verify.sh")}
        roots = []
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            identity = process_identity(int(entry.name))
            if identity and (identity[1] == str(self.daemon) or
                             (len(identity[2]) >= 2 and identity[2][0] == SH and identity[2][1] in scripts)):
                self.owned[int(entry.name)] = identity
                roots.append(int(entry.name))
        # Remember precise spawned utility children too, before terminating parents.
        pending = list(roots)
        while pending:
            pid = pending.pop()
            for child in text(Path(f"/proc/{pid}/task/{pid}/children")).split():
                child = int(child)
                if self.remember(child):
                    pending.append(child)
        return roots

    def wait_pid(self, pid):
        if pid in self.processes:
            return self.processes[pid][0].wait(timeout=WAIT)
        deadline = time.monotonic() + WAIT
        while time.monotonic() < deadline:
            try:
                child, status = os.waitpid(pid, os.WNOHANG)
                if child:
                    return os.waitstatus_to_exitcode(status)
            except ChildProcessError:
                pass  # A live daemon is still its service's child; never steal that wait.
            if process_identity(pid) != self.owned.get(pid):
                # Exit can happen between WNOHANG and /proc inspection. Reap the
                # exact child once more rather than discard its exit status.
                try:
                    child, status = os.waitpid(pid, 0)
                    if child:
                        return os.waitstatus_to_exitcode(status)
                except ChildProcessError:
                    return None
            time.sleep(0.05)
        raise subprocess.TimeoutExpired(f"precise child PID {pid}", WAIT)

    def terminate(self, pid):
        if process_identity(pid) == self.owned.get(pid) and self.owned.get(pid):
            os.kill(pid, signal.SIGTERM)
        return self.wait_pid(pid)

    def cleanup_processes(self):
        failures = []
        for role in ("action.sh", "verify.sh", "service.sh", "ctnd", None):
            self.discover()  # Action cleanup can have spawned a new service.
            for pid, identity in list(self.owned.items()):
                argv = identity[2]
                name = "ctnd" if identity[1] == str(self.daemon) else (
                    Path(argv[1]).name if len(argv) >= 2 and argv[0] == SH else None)
                if name != role:
                    continue
                try:
                    self.terminate(pid)
                except subprocess.TimeoutExpired:
                    # Teardown-only last resort, never a passing lifecycle assertion.
                    if process_identity(pid) == identity:
                        os.kill(pid, signal.SIGKILL)
                    self.wait_pid(pid)
                    failures.append(f"PID {pid} did not terminate gracefully")
        for proc, _ in self.processes.values():
            proc.wait(timeout=WAIT)
        self.assertFalse(self.discover(), "fixture process survived cleanup")
        self.assertEqual(failures, [])
        self.assertFalse((self.runtime / "forbidden-commands").exists(), "device command was attempted")

    def lock_available(self, name):
        path = self.runtime / name
        self.assertTrue(path.is_file(), f"real lock inode is missing: {path}")
        with path.open("rb") as lock:
            try:
                self.fcntl.flock(lock, self.fcntl.LOCK_EX | self.fcntl.LOCK_NB)
            except BlockingIOError:
                return False
            self.fcntl.flock(lock, self.fcntl.LOCK_UN)
            return True

    def active_pair(self):
        try:
            pid, start = text(self.runtime / "service.pid").split()
            service, daemon = int(pid), int(text(self.runtime / "daemon.pid"))
        except ValueError:
            return None
        service_identity, daemon_identity = process_identity(service), process_identity(daemon)
        if not service_identity or not daemon_identity:
            return None
        if service_identity != (start, str(Path(SH).resolve()), (SH, str(self.package / "service.sh"), "--run")):
            return None
        if daemon_identity[1:] != (str(self.daemon), (str(self.daemon),)):
            return None
        self.owned.update({service: service_identity, daemon: daemon_identity})
        return service, daemon

    def assert_active(self):
        pair = self.wait_for(self.active_pair, "real service and daemon identity")
        service, daemon = pair
        self.assertFalse(self.lock_available(".service.lock"))
        self.assertFalse(self.lock_available(".daemon.lock"))
        self.assertTrue(self.lock_available(".maintenance.lock"))
        # Inspect only this exact daemon, not unrelated host processes/fds.
        descriptors = []
        for fd in Path(f"/proc/{daemon}/fd").iterdir():
            try:
                descriptors.append(os.readlink(fd))
            except FileNotFoundError:
                pass
        self.assertIn(str(self.runtime / ".daemon.lock"), descriptors)
        self.assertNotIn(str(self.runtime / ".service.lock"), descriptors)
        self.assertNotIn(str(self.runtime / ".maintenance.lock"), descriptors)
        self.assertEqual(FIXTURES.sha(Path(f"/proc/{daemon}/exe").read_bytes()), FIXTURES.sha(self.daemon.read_bytes()))
        return service, daemon

    def start_service(self):
        rc, output = self.run_script("service.sh")  # Exercise the production re-exec, too.
        self.assertEqual(rc, 0, output)
        return self.assert_active()

    def assert_restored(self):
        self.assertEqual(text(self.node), "SavedA:-1,SavedB:-1\n")
        self.assertEqual(text(self.enable), "1\n")
        self.assertFalse((self.runtime / "restore.state").exists())
        self.assertFalse((self.runtime / "exercise.state").exists())

    def assert_stopped(self, pair):
        self.wait_for(lambda: all(process_identity(pid) is None for pid in pair), "service/daemon exit")
        for name in ("service.pid", "daemon.pid"):
            self.assertFalse((self.runtime / name).exists(), name)
        for name in (".service.lock", ".daemon.lock", ".maintenance.lock"):
            self.assertTrue(self.lock_available(name), name)
        self.assert_restored()

    def assert_summary(self, output, *, action=False, rc=0):
        lines = output.splitlines()
        self.assertLessEqual(len(lines), 17 if action else 15, output)
        allowed = ("[正常] ", "[注意] ", "[失败] ", "[跳过] ")
        if action:
            allowed += ("当前版本：", "时间：", "模式：", "摘要日志：", "详细日志：")
        self.assertTrue(all(line.startswith(allowed) for line in lines), output)
        self.assertNotRegex(output, r"[0-9a-f]{64}")
        for marker in ("ctn_patch:", "FILE ", "RUNNING ", "SHA256=", "build_id=", "mode=",
                       "Static checks only", "Name write/read", "SELF_CHECK_EXIT=", "exercise finished"):
            self.assertNotIn(marker, output)
        final = "[正常] 自检结果：" if rc == 0 else "[失败] 自检结果："
        self.assertEqual(sum(line.startswith(final) for line in lines), 1, output)
        if rc:
            self.assertNotIn("[正常] 自检结果：", output)

    def assert_action_logs(self, output, rc, *technical):
        summary = text(self.runtime / "action.log")
        details = text(self.runtime / "action-detail.log")
        self.assertTrue(summary, output)
        self.assertTrue(details, output)
        self.assert_summary(summary, action=True, rc=rc)
        self.assert_summary(output, action=True, rc=rc)
        self.assertEqual(output, summary + f"摘要日志：{self.runtime / 'action.log'}\n"
                                          f"详细日志：{self.runtime / 'action-detail.log'}\n")
        for message in (*technical, f"SELF_CHECK_EXIT={rc}"):
            self.assertIn(message, details)
            self.assertNotIn(message, summary)
            self.assertNotIn(message, output)
        for name in ("action.log", "action-detail.log"):
            self.assertEqual((self.runtime / name).stat().st_mode & 0o777, 0o600, name)
        self.assertFalse(list(self.runtime.glob(".action*")), "Action left a temporary log")
        return summary, details

    def test_wait_pid_reaps_exit_between_poll_and_proc_snapshot(self):
        pid = 987654
        self.owned[pid] = ("fixture-start", str(self.daemon), (str(self.daemon),))
        try:
            with mock.patch("os.waitpid", side_effect=[(0, 0), (pid, 143 << 8)]) as wait, \
                 mock.patch(__name__ + ".process_identity", return_value=None):
                self.assertEqual(self.wait_pid(pid), 143)
                self.assertEqual(wait.call_args_list,
                                 [mock.call(pid, os.WNOHANG), mock.call(pid, 0)])
        finally:
            self.owned.pop(pid, None)

    def test_default_action_passes_logs_restores_and_resumes_real_daemon(self):
        old = self.start_service()
        inodes = {name: (self.runtime / name).stat().st_ino for name in (".service.lock", ".daemon.lock")}
        rc, output = self.run_script("verify.sh")
        self.assertEqual(rc, 0, output)
        self.assert_summary(output)
        self.assertIn("[正常] 文件与兼容性：", output)
        self.assertIn("[跳过] 写入演练：只读模式", output)
        self.assertIn("[跳过] 进程样本：没有稳定登记样本", output)
        self.assertEqual(self.assert_active(), old)
        self.assertFalse((self.runtime / "test-writes").exists())

        rc, output = self.run_script("action.sh")
        self.assertEqual(rc, 0, output)
        summary, details = self.assert_action_logs(output, 0, "mode=exercise",
                         "Name write/read and rejection tests passed",
                         "original service and current daemon resumed")
        self.assertIn("模式：安全演练", summary)
        self.assertIn("[正常] 名称演练：", summary)
        self.assertIn("[正常] 原值恢复：", summary)
        self.assertIn("[正常] 服务收尾：原服务与当前后台已恢复", summary)
        self.assertIn("FILE module=v" + self.module_version, details)
        self.assertIn("KO_SHA256=", details)
        self.assertEqual(text(self.runtime / "test-writes").splitlines(),
                         ["CTNTestMain CTNTestGfx", "CTNTestSingle", "A B C", " ", "SavedA SavedB"])
        self.assert_restored()
        self.assertFalse((self.runtime / ".service.stop").exists())
        new = self.assert_active()
        self.assertTrue(all(before != after for before, after in zip(old, new)))
        self.assertTrue(all(process_identity(pid) is None for pid in old))
        for name, inode in inodes.items():
            self.assertEqual((self.runtime / name).stat().st_ino, inode, "lock inode replaced during exercise")
        self.assertEqual(self.terminate(new[0]), 143)
        self.assert_stopped(new)
        self.assertGreaterEqual(text(self.runtime / "daemon.log").count("stopped; original values restored"), 2)

    def test_failed_name_test_restores_originals_and_resumes(self):
        old = self.start_service()
        # Fail after the first real fixture-node change, not before any write.
        rc, output = self.run_script("action.sh", CTN_LIFECYCLE_FAIL="19")
        self.assertEqual(rc, 19, output)
        summary, _ = self.assert_action_logs(output, 19, "dual-name write/readback failed rc=19",
                           "exercise finished rc=19", "original service and current daemon resumed")
        self.assertIn("[失败] 名称演练：双名称写入或回读失败", summary)
        self.assertNotIn("[正常] 名称演练：", summary)
        self.assertIn("[正常] 原值恢复：", summary)
        self.assertEqual(text(self.runtime / "test-writes").splitlines(),
                         ["CTNTestMain CTNTestGfx", "SavedA SavedB"])
        self.assert_restored()
        self.assertFalse((self.runtime / ".service.stop").exists())
        new = self.assert_active()
        self.assertTrue(all(before != after for before, after in zip(old, new)))
        self.assertTrue(all(process_identity(pid) is None for pid in old))

    def test_term_action_waits_for_child_restore_and_resume_before_exit(self):
        old = self.start_service()
        action = self.spawn("action.sh", CTN_LIFECYCLE_HOLD="1")
        child = self.wait_for(lambda: text(self.runtime / "test-write.pid").strip(), "verify reached changed node")
        child = int(child)
        identity = self.remember(child)
        self.assertIsNotNone(identity)
        self.assertEqual(identity[1:], (str(Path(SH).resolve()),
                                       (SH, str(self.package / "verify.sh"), "--details", "--exercise")))
        self.assertIn(str(child), text(Path(f"/proc/{action.pid}/task/{action.pid}/children")).split())
        self.assertEqual(text(self.node), "CTNTestMain:-1,CTNTestGfx:-1\n")
        self.assertTrue((self.runtime / "exercise.state").is_file())
        self.assertTrue(self.lock_available(".service.lock"))
        self.assertTrue(self.lock_available(".daemon.lock"))
        self.assertFalse(self.lock_available(".maintenance.lock"))

        self.assertEqual(self.terminate(action.pid), 143)
        # No polling here: Action must have waited for verify's restore before exit.
        self.assert_restored()
        self.assertIsNone(process_identity(child), "Action exited while its verification child was still alive")
        self.assertIsNotNone(self.active_pair(), "Action exited before the original service was resumed")
        summary, _ = self.assert_action_logs(text(self.processes[action.pid][1]), 143,
                           "original service and current daemon resumed", "exercise finished rc=143")
        self.assertIn("[失败] 自检结果：已中断", summary)
        self.assertNotIn("[正常] 名称演练：", summary)
        new = self.assert_active()
        self.assertTrue(all(before != after for before, after in zip(old, new)))

    def test_preexisting_stop_request_is_preserved_and_not_resumed(self):
        old = self.start_service()
        stop = self.runtime / ".service.stop"
        FIXTURES.put(stop, "keep stopped\n")
        rc, output = self.run_script("verify.sh", "--exercise")
        self.assertEqual(rc, 0, output)
        self.assert_summary(output)
        self.assertIn("[正常] 名称演练：", output)
        self.assertIn("[注意] 服务收尾：保留原有停止请求", output)
        self.assertNotIn("[正常] 服务收尾：原服务与当前后台已恢复", output)
        self.assertEqual(text(stop), "keep stopped\n")
        self.assert_stopped(old)
        self.assertFalse(self.discover(), "a preexisting stop request unexpectedly restarted a process")
    def test_details_flags_are_order_independent_and_readonly_stays_readonly(self):
        before = {path.relative_to(self.device): path.read_bytes() for path in self.device.rglob("*") if path.is_file()}
        rc, output = self.run_script("verify.sh", "--details")
        self.assertEqual(rc, 0, output)
        for message in ("verification mode=read-only", "Static checks only", "KO_SHA256=", "build_id=",
                        "SKIP real PID exercise"):
            self.assertIn(message, output)
        self.assertIn("[正常] 文件与兼容性：", output)
        self.assertEqual(before, {path.relative_to(self.device): path.read_bytes()
                                 for path in self.device.rglob("*") if path.is_file()})
        self.assertFalse(self.runtime.exists(), "direct --details wrote runtime state or logs")
        for args in (("--details", "--exercise"), ("--exercise", "--details")):
            with self.subTest(args=args):
                rc, output = self.run_script("verify.sh", *args)
                self.assertEqual(rc, 0, output)
                self.assertIn("Name write/read and rejection tests passed", output)
                self.assertIn("exercise finished rc=0", output)
                self.assertIn("[正常] 原值恢复：", output)
                self.assert_restored()

    def test_readonly_action_logs_only_and_atomically_replaces_private_logs(self):
        old = self.start_service()
        snapshot = {name: (self.runtime / name).read_bytes() for name in
                    ("service.pid", "daemon.pid", ".service.lock", ".daemon.lock", ".maintenance.lock")}
        targets = {}
        for name in ("action.log", "action-detail.log"):
            path = self.runtime / name
            FIXTURES.put(path, "previous log inode\n")
            path.chmod(0o644)
            target = self.base / (name + ".previous")
            os.link(path, target)
            targets[name] = target
        rc, output = self.run_script("action.sh", "--read-only")
        self.assertEqual(rc, 0, output)
        summary, details = self.assert_action_logs(output, 0, "mode=read-only", "Static checks only")
        self.assertIn("模式：只读自检", summary)
        self.assertNotIn("Name write/read and rejection tests passed", details)
        self.assertEqual(self.assert_active(), old)
        self.assertFalse((self.runtime / "test-writes").exists())
        self.assertFalse((self.runtime / ".service.stop").exists())
        self.assertEqual(snapshot, {name: (self.runtime / name).read_bytes() for name in snapshot})
        self.assert_restored()
        for name, target in targets.items():
            self.assertEqual(text(target), "previous log inode\n")
            self.assertNotEqual(target.stat().st_ino, (self.runtime / name).stat().st_ino)
            self.assertEqual(target.stat().st_mode & 0o777, 0o644)

    def test_old_running_patch_and_hash_failure_are_not_reported_as_passed(self):
        version = self.device / "sys/module/ctn_patch/version"
        FIXTURES.put(version, "2.9\n")
        for args in ((), ("--details",), ("--exercise",)):
            with self.subTest(args=args):
                rc, output = self.run_script("verify.sh", *args)
                self.assertEqual(rc, 1, output)
                self.assertIn("磁盘包 v" + self.module_version, output)
                self.assertIn("[失败] 当前运行：v2.9", output)
                self.assertIn("正常重启", output)
                self.assertNotIn("[正常] 当前运行：", output)
                if "--details" not in args:
                    self.assert_summary(output, rc=1)
        self.assertFalse(self.runtime.exists())
        FIXTURES.put(version, self.module_version + "\n")
        vendor = self.device / "vendor/lib/modules/oplus_bsp_game_opt.ko"
        FIXTURES.put(vendor, b"unknown vendor image")
        rc, output = self.run_script("action.sh", "--read-only")
        self.assertEqual(rc, 1, output)
        summary, _ = self.assert_action_logs(output, 1, "unknown vendor identity")
        self.assertIn("[失败] ", summary)
        self.assertNotIn("[正常] 文件与兼容性：", summary)
        self.assertNotIn("[正常] 名称演练：", summary)
        self.assertFalse((self.runtime / "test-writes").exists())

    def test_restore_failure_keeps_record_stop_and_does_not_resume(self):
        old = self.start_service()
        verify = self.package / "verify.sh"
        script = replace_once(text(verify), '    local input=$1\n',
                              '    local input=$1\n    [ "$input" != "SavedA SavedB" ] || return 23\n')
        verify.write_text(script, encoding="utf-8")
        rc, output = self.run_script("action.sh")
        self.assertEqual(rc, 23, output)
        summary, _ = self.assert_action_logs(output, 23, "cannot restore original names",
                                             "exercise finished rc=23")
        self.assertIn("[失败] 原值恢复：未完成", summary)
        self.assertNotIn("[正常] 原值恢复：", summary)
        self.assertNotIn("[正常] 服务收尾：原服务与当前后台已恢复", summary)
        self.assertTrue((self.runtime / "exercise.state").is_file())
        self.assertTrue((self.runtime / ".service.stop").exists())
        self.assertFalse((self.runtime / "restore.state").exists())
        self.assertEqual(text(self.node), "CTNTestSingle:-1,CTNTestSingle:-1\n")
        self.assertEqual(text(self.enable), "1\n")
        self.assertTrue(all(process_identity(pid) is None for pid in old))
        self.assertFalse(self.discover(), "restore debt restarted a process")
        for name in (".service.lock", ".daemon.lock", ".maintenance.lock"):
            self.assertTrue(self.lock_available(name), name)

    def test_hup_action_waits_for_restoration_and_reports_failure(self):
        old = self.start_service()
        action = self.spawn("action.sh", CTN_LIFECYCLE_HOLD="1")
        child = int(self.wait_for(lambda: text(self.runtime / "test-write.pid").strip(), "verify changed node"))
        self.remember(child)
        self.assertEqual(process_identity(action.pid), self.owned[action.pid])
        os.kill(action.pid, signal.SIGHUP)
        self.assertEqual(action.wait(timeout=WAIT), 143)
        self.assert_restored()
        self.assertIsNone(process_identity(child))
        self.assertIsNotNone(self.active_pair(), "Action did not wait for resumed service")
        summary, _ = self.assert_action_logs(text(self.processes[action.pid][1]), 143, "exercise finished rc=143")
        self.assertIn("[失败] 自检结果：已中断", summary)
        self.assertTrue(all(process_identity(pid) is None for pid in old))

    def test_log_publication_failure_preserves_verify_code_and_private_old_log(self):
        common = self.package / "lib/common.sh"
        common.write_text(text(common) + r'''
# Fixture-only summary-log publication failure; all other moves stay real.
mv() {
    for argument in "$@"; do
        [ "$argument" != "$CTN_RUNTIME/action.log" ] || { printf 'fixture summary mv failure\n' >&2; return 27; }
    done
    command mv "$@"
}
''', encoding="utf-8")
        self.runtime.mkdir(exist_ok=True)
        FIXTURES.put(self.runtime / "action.log", "previous summary\n")
        (self.runtime / "action.log").chmod(0o600)
        for failure, expected in (("0", 1), ("19", 19)):
            with self.subTest(failure=failure):
                rc, output = self.run_script("action.sh", CTN_LIFECYCLE_FAIL=failure)
                self.assertEqual(rc, expected, output)
                self.assert_summary(output, action=True, rc=expected)
                self.assertIn("[失败] 自检结果：日志保存失败", output)
                self.assertNotIn("fixture summary mv failure", output)
                details = text(self.runtime / "action-detail.log")
                self.assertIn("fixture summary mv failure", details)
                self.assertIn(f"ACTION_EXIT={expected}", details)
                self.assertEqual(text(self.runtime / "action.log"), "previous summary\n")
                self.assertEqual((self.runtime / "action-detail.log").stat().st_mode & 0o777, 0o600)
                self.assertFalse(list(self.runtime.glob(".action*")))
                self.assert_restored()
    def test_late_hup_during_log_publication_cannot_leave_success_summary(self):
        common = self.package / "lib/common.sh"
        common.write_text(text(common) + r'''
# Fixture-only signal at publication, after the real read-only child has exited.
mv() {
    for argument in "$@"; do
        if [ "$argument" = "$CTN_RUNTIME/action.log" ] && [ "${CTN_TEST_LATE_HUP:-0}" = 0 ]; then
            CTN_TEST_LATE_HUP=1
            command kill -HUP "$$" || return $?
        fi
    done
    command mv "$@"
}
''', encoding="utf-8")
        rc, output = self.run_script("action.sh", "--read-only")
        self.assertEqual(rc, 143, output)
        summary, _ = self.assert_action_logs(output, 143, "VERIFY_EXIT=0", "ACTION_SIGNAL=143")
        self.assertIn("[失败] 自检结果：已中断", summary)
        self.assertFalse((self.runtime / "test-writes").exists())
        self.assert_restored()


if __name__ == "__main__":
    unittest.main(verbosity=2)
