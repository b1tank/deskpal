#!/usr/bin/env python3
"""Deterministic tests for the Codex Deskpal-release Stop hook."""

import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / "scripts" / "codex-release-control-hook.py"

spec = importlib.util.spec_from_file_location("codex_release_hook", HOOK)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def main():
    with tempfile.TemporaryDirectory(prefix="deskpal-hook-") as directory:
        lock = os.path.join(directory, "deskpal-test-control.lock")
        Path(lock).touch(mode=0o600)

        assert module.release_decision([lock], {1234}) == {}

        locker = subprocess.Popen(
            [
                sys.executable,
                "-c",
                (
                    "import fcntl,os,sys,time;"
                    "f=open(sys.argv[1],'r+');"
                    "fcntl.flock(f,fcntl.LOCK_EX);"
                    "f.truncate(0);f.write(f'deskpal pid {os.getpid()}');f.flush();"
                    "print(os.getpid(),flush=True);time.sleep(30)"
                ),
                lock,
            ],
            stdout=subprocess.PIPE,
            text=True,
        )
        try:
            owner_pid = int(locker.stdout.readline().strip())
            decision = module.release_decision([lock], {owner_pid})
            assert decision["decision"] == "block", decision
            assert "release_control" in decision["reason"], decision
            assert module.release_decision([lock], {owner_pid}, True) == {}
            assert module.release_decision([lock], {owner_pid + 1}) == {}
        finally:
            locker.terminate()
            locker.wait(timeout=3)

        assert module.release_decision([lock], {owner_pid}) == {}

    subprocess.run(
        [sys.executable, str(HOOK)],
        input='{"stop_hook_active":true}\n',
        text=True,
        stdout=subprocess.PIPE,
        check=True,
    )
    print("PASS: Codex hook detects only its own held Deskpal lease")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
