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
        for leaf, value in {"version": "3.0", "parameters/profile": "6.1.157",
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
            ('    CTN_SHELL=$(command -v sh) || return 1', f"    CTN_SHELL={shlex.quote(SH)}"),
            ('"$CTN_PROC/game_opt/game_pid"', '"$CTN_ROOT/proc/game_opt/game_pid"'),
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
        logs = "\n".join(f"{name}:\n{text(self.runtime / name)}" for name in ("boot.log", "daemon.log", "action.log"))
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

    def test_default_action_passes_logs_restores_and_resumes_real_daemon(self):
        old = self.start_service()
        inodes = {name: (self.runtime / name).stat().st_ino for name in (".service.lock", ".daemon.lock")}
        rc, output = self.run_script("verify.sh")
        self.assertEqual(rc, 0, output)
        self.assertIn("Static checks only", output)
        self.assertEqual(self.assert_active(), old)
        self.assertFalse((self.runtime / "test-writes").exists())

        rc, output = self.run_script("action.sh")
        self.assertEqual(rc, 0, output)
        log = text(self.runtime / "action.log")
        for message in ("mode=exercise", "Name write/read and rejection tests passed",
                        "original service and current daemon resumed", "SELF_CHECK_EXIT=0"):
            self.assertIn(message, log)
        self.assertIn(log, output)
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
        log = text(self.runtime / "action.log")
        for message in ("dual-name write/readback failed rc=19", "exercise finished rc=19",
                        "original service and current daemon resumed", "SELF_CHECK_EXIT=19"):
            self.assertIn(message, log)
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
                                       (SH, str(self.package / "verify.sh"), "--exercise")))
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
        log = text(self.runtime / "action.log")
        for message in ("original service and current daemon resumed", "exercise finished rc=143", "SELF_CHECK_EXIT=143"):
            self.assertIn(message, log)
        self.assertFalse(list(self.runtime.glob(".action.*")), "interrupted Action left its temporary log")
        new = self.assert_active()
        self.assertTrue(all(before != after for before, after in zip(old, new)))

    def test_preexisting_stop_request_is_preserved_and_not_resumed(self):
        old = self.start_service()
        stop = self.runtime / ".service.stop"
        FIXTURES.put(stop, "keep stopped\n")
        rc, output = self.run_script("verify.sh", "--exercise")
        self.assertEqual(rc, 0, output)
        self.assertIn("Name write/read and rejection tests passed", output)
        self.assertNotIn("original service and current daemon resumed", output)
        self.assertEqual(text(stop), "keep stopped\n")
        self.assert_stopped(old)
        self.assertFalse(self.discover(), "a preexisting stop request unexpectedly restarted a process")


if __name__ == "__main__":
    unittest.main(verbosity=2)
