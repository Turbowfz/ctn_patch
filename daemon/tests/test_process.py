#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Real daemon processes; only node readback is modelled by temporary files."""
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import sys
import time


def wait_for(predicate, label, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"timeout: {label}")


def main():
    binary, fakepkg, directory = sys.argv[1:]
    root = Path(directory)
    runtime = root / "runtime"
    names = root / "names"
    enable = root / "enable"
    pid_node = root / "game_pid"
    database = root / "game.db"
    state = runtime / "restore.state"
    pidfile = runtime / "daemon.pid"
    children = []
    logs = []
    count = 0

    def check(condition, label):
        nonlocal count
        count += 1
        if not condition:
            raise AssertionError(label)

    def text(path):
        try:
            return path.read_text()
        except FileNotFoundError:
            return ""

    def write(path, value):
        path.write_text(value)

    def applied(value="Game Render"):
        return text(names).startswith(value) and state.exists()

    def readback():
        # proc reads expose names plus PIDs; regular host files cannot do that.
        # No parser/state code is duplicated: the actual daemon parses this.
        write(names, "Game:-1,Render:-1\n")

    def start(*args):
        log = root / f"process-{len(logs)}.log"
        logs.append(log)
        with log.open("wb") as output:
            proc = subprocess.Popen([binary, "-d", str(database), *args], stdout=output, stderr=output)
        children.append(proc)
        return proc

    def stop(proc, expected=0):
        proc.send_signal(signal.SIGTERM)
        check(proc.wait(timeout=10) == expected, f"SIGTERM expected exit {expected}")

    conn = sqlite3.connect(database)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA wal_autocheckpoint=0")
    conn.execute("CREATE TABLE PackageConfigBean(package_name TEXT,game_config TEXT)")
    conn.execute("INSERT INTO PackageConfigBean VALUES (?,?)", ("com.test.cli", '{"ctn":"Game Render","ctb":1}'))
    conn.commit()
    package = subprocess.Popen([fakepkg, "com.test.cli"])
    children.append(package)

    def reset(ctb=1, original_enable="0"):
        write(names, "CustomOne:-1,CustomTwo:-1\n")
        write(enable, original_enable + "\n")
        write(pid_node, f"game_pid={package.pid} child_num=1\n")
        conn.execute("UPDATE PackageConfigBean SET game_config=?", (json.dumps({"ctn": "Game Render", "ctb": ctb}),))
        conn.commit()

    try:
        reset()
        proc = start("-r", "0")
        wait_for(applied, "initial application")
        wait_for(lambda: text(pidfile).strip() == str(proc.pid), "exact runtime pidfile")
        check(text(enable) == "0\n", "CTB defaults off")
        check((state.stat().st_mode & 0o777) == 0o600, "private journal")
        second = root / "second-location"
        second.mkdir()
        other_binary = second / "ctnd"
        shutil.copyfile(binary, other_binary)
        other_binary.chmod(0o700)
        result = subprocess.run([str(other_binary), "-d", str(database)], capture_output=True, timeout=10)
        check(result.returncode == 3, "same lock for different executable directories")
        check(text(pidfile).strip() == str(proc.pid), "duplicate cannot overwrite owner pid")
        readback()
        stop(proc)
        check(text(names).startswith("CustomOne CustomTwo"), "SIGTERM restores custom names")
        check(not state.exists() and not pidfile.exists(), "clean shutdown removes debt and pid only")
        check((runtime / ".daemon.lock").exists(), "persistent flock inode")

        reset()
        proc = start("-r", "0")
        wait_for(applied, "application before crash")
        readback()
        proc.kill()
        check(proc.wait(timeout=10) == -signal.SIGKILL, "crash simulated")
        check(state.exists(), "crash preserves recovery journal")
        orphan = runtime / ".snapshot-Pro001"
        orphan.mkdir(mode=0o700)
        for leaf in ("db", "db-wal", "db-shm"):
            artifact = orphan / leaf
            artifact.write_bytes(b"crash residue")
            artifact.chmod(0o600)
        protected = runtime / ".snapshot-Prt001"
        protected.mkdir(mode=0o700)
        for leaf in ("db", "unknown"):
            artifact = protected / leaf
            artifact.write_bytes(b"untouched")
            artifact.chmod(0o600)
        write(pid_node, "game_pid=-1 child_num=0\n")
        restarted = start("-r", "0")
        wait_for(lambda: text(names).startswith("CustomOne CustomTwo") and not state.exists(), "restart recovery")
        check(not orphan.exists(), "main reclaims orphan snapshots under shared lock")
        check((protected / "db").read_bytes() == b"untouched" and (protected / "unknown").exists(),
              "main preserves whole snapshot with unknown entries")
        stop(restarted)
        shutil.rmtree(protected)

        reset()
        proc = start("-r", "0")
        wait_for(applied, "application before HAL recovery yield")
        readback()
        proc.kill()
        check(proc.wait(timeout=10) == -signal.SIGKILL, "same-boot HAL crash simulated")
        write(names, "HALNew:-1,HALRender:-1\n")
        restarted = start("-r", "0")
        wait_for(lambda: not state.exists() and text(pidfile).strip() == str(restarted.pid),
                 "HAL recovery relinquishes durable debt")
        time.sleep(6)
        check(text(names) == "HALNew:-1,HALRender:-1\n", "first positive PID preserves recovery HAL yield")
        check(not state.exists(), "HAL recovery does not reacquire debt on retry")
        replacement = subprocess.Popen([fakepkg, "com.test.cli"])
        children.append(replacement)
        write(pid_node, f"game_pid={replacement.pid} child_num=1\n")
        wait_for(applied, "real PID switch releases recovery yield")
        readback()
        stop(restarted)
        check(text(names).startswith("HALNew HALRender"), "next game restores HAL baseline")

        reset()
        proc = start("-r", "0")
        wait_for(applied, "application before idle HAL change")
        readback()
        write(pid_node, "game_pid=-1 child_num=0\n")
        wait_for(lambda: text(names).startswith("CustomOne CustomTwo") and not state.exists(),
                 "game A exit restoration")
        write(names, "HALIdle:-1,HALRender:-1\n")
        write(pid_node, f"game_pid={replacement.pid} child_num=1\n")
        wait_for(applied, "game B applies after idle HAL change")
        readback()
        stop(proc)
        check(text(names).startswith("HALIdle HALRender"), "game B captures fresh idle observation")

        reset()
        proc = start("-r", "0")
        wait_for(applied, "application before restore failure")
        names.unlink()
        stop(proc, expected=1)
        check(state.exists(), "failed restore blocks shell unload through existing debt")
        readback()
        write(pid_node, "game_pid=-1 child_num=0\n")
        restarted = start("-r", "0")
        wait_for(lambda: text(names).startswith("CustomOne CustomTwo") and not state.exists(), "retry failed restore on restart")
        stop(restarted)

        reset(ctb=0, original_enable="1")
        proc = start("-e", "-r", "5")
        wait_for(lambda: applied() and text(enable).startswith("0"), "opt-in CTB application")
        readback()
        conn.execute("UPDATE PackageConfigBean SET game_config=?", ('{"ctn":"Game Render","ctb":1}',))
        conn.commit()
        wait_for(lambda: text(enable).startswith("1"), "CTB-only database update", seconds=12)
        check(text(names) == "Game:-1,Render:-1\n", "unchanged CTN is not rewritten on CTB-only update")
        stop(proc)
        check(text(names).startswith("CustomOne CustomTwo"), "opt-in exit restores custom names")
        check(text(enable).startswith("1") and not state.exists(), "opt-in preserves original enable value")
        print(f"PASS process integration: {count} checks (SIGTERM, crash/restart, shared lock, CTB)")
    except BaseException:
        for log in logs:
            print(f"--- {log.name} ---", file=sys.stderr)
            print(log.read_text(), file=sys.stderr)
        raise
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.kill()
            child.wait(timeout=10)
        conn.close()


if __name__ == "__main__":
    main()
