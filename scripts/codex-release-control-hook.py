#!/usr/bin/env python3
"""Codex Stop hook: request Deskpal release only when this Codex owns the lock."""

import fcntl
import glob
import json
import os
import re
import sys
from pathlib import Path

OWNER_PATTERN = re.compile(r"^deskpal pid ([0-9]+)$")


def process_parent(pid):
    try:
        return int(Path(f"/proc/{pid}/stat").read_text().split()[3])
    except (OSError, ValueError, IndexError):
        return 0


def process_command(pid):
    try:
        data = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0", 1)[0]
        return os.path.basename(data.decode("utf-8", "replace"))
    except OSError:
        return ""


def find_codex_ancestor(pid):
    for _ in range(16):
        if pid <= 1:
            return 0
        if process_command(pid) == "codex":
            return pid
        pid = process_parent(pid)
    return 0


def descendants(root_pid):
    result = set()
    pending = [root_pid]
    while pending:
        parent = pending.pop()
        try:
            raw = Path(
                f"/proc/{parent}/task/{parent}/children"
            ).read_text().split()
        except OSError:
            continue
        for value in raw:
            try:
                child = int(value)
            except ValueError:
                continue
            if child not in result:
                result.add(child)
                pending.append(child)
    return result


def lock_is_held_by(path, owned_pids):
    try:
        fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
    except OSError:
        return False
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            fcntl.flock(fd, fcntl.LOCK_UN)
            return False
        try:
            owner = os.pread(fd, 96, 0).decode("ascii", "replace").strip()
        except OSError:
            return False
        match = OWNER_PATTERN.fullmatch(owner)
        return bool(match and int(match.group(1)) in owned_pids)
    finally:
        os.close(fd)


def release_decision(lock_paths, owned_pids, stop_hook_active=False):
    if stop_hook_active:
        return {}
    if any(lock_is_held_by(path, owned_pids) for path in lock_paths):
        return {
            "decision": "block",
            "reason": (
                "Your Deskpal MCP process still owns visible-desktop control. "
                "Call the Deskpal release_control MCP tool now. If release is "
                "refused, first close owned isolated sessions or release any "
                "Deskpal-held mouse button. Do not repeat an unknown-outcome "
                "application mutation. Then finish without further desktop work."
            ),
        }
    return {}


def default_lock_paths():
    uid = os.getuid()
    paths = glob.glob(f"/run/user/{uid}/deskpal-*-control.lock")
    paths.extend(glob.glob(os.path.expanduser("~/.deskpal/deskpal-*-control.lock")))
    return paths


def main():
    try:
        event = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError):
        event = {}
    codex_pid = find_codex_ancestor(os.getppid())
    owned = descendants(codex_pid) if codex_pid else set()
    decision = release_decision(
        default_lock_paths(),
        owned,
        bool(event.get("stop_hook_active", False)),
    )
    json.dump(decision, sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
