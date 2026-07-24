#!/usr/bin/env python3
"""Short live GNOME indicator lifecycle test that never focuses an app."""

import ast
import json
import os
import shutil
import subprocess
import time

from deskpal_client import DESKPAL, DeskpalClient, text

GDBUS_STATUS = [
    "gdbus", "call", "--session",
    "--dest", "org.deskpal.Indicator",
    "--object-path", "/org/deskpal/Indicator",
    "--method", "org.deskpal.Indicator.GetStatus",
]


def require_dependencies():
    required = ("gdbus", "xclip", "xdotool", "xprop")
    missing = [command for command in required if not shutil.which(command)]
    if missing:
        raise SystemExit(f"FAIL: missing dependencies: {', '.join(missing)}")
    if not os.path.isfile(DESKPAL):
        raise SystemExit("FAIL: build/deskpal does not exist; run npm run build")


def raw_status():
    output = subprocess.check_output(GDBUS_STATUS, text=True).strip()
    return json.loads(ast.literal_eval(output)[0])


def pointer_coordinates():
    output = subprocess.check_output(
        ["xdotool", "getmouselocation", "--shell"]
    )
    return b"\n".join(
        line for line in output.splitlines()
        if line.startswith((b"X=", b"Y=", b"SCREEN="))
    )


def desktop_state():
    clipboard = subprocess.run(
        ["xclip", "-selection", "clipboard", "-o"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout
    return {
        "pointer": pointer_coordinates(),
        "focus": subprocess.check_output(
            ["xprop", "-root", "_NET_ACTIVE_WINDOW"]
        ),
        "stacking": subprocess.check_output(
            ["xprop", "-root", "_NET_CLIENT_LIST_STACKING"]
        ),
        "clipboard": clipboard,
    }


def wait_until_removed(remote_id, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if all(cursor["cursorId"] != remote_id for cursor in raw_status()["cursors"]):
            return
        time.sleep(0.02)
    raise AssertionError(f"cursor survived owner exit: {remote_id}")


def assert_unchanged(before, after):
    for key, value in before.items():
        assert after[key] == value, {
            "changedState": key,
            "before": value,
            "after": after[key],
        }


def run_suite():
    require_dependencies()
    baseline = desktop_state()
    existing_ids = {cursor["cursorId"] for cursor in raw_status()["cursors"]}
    owner = DeskpalClient(name="indicator-background-owner")
    peer = None
    remote_id = None
    try:
        status = json.loads(text(owner.tool("agent_cursor_status")))
        assert status["available"] is True, status
        assert len(status["monitors"]) == 1, status

        capture = owner.tool(
            "screenshot", {"fullScreen": True, "maxWidth": 960, "maxHeight": 540}
        )
        move_result = owner.tool(
            "agent_cursor_move",
            {
                "captureId": capture["screenshot"]["captureId"],
                "x": 12,
                "y": 12,
                "cursorId": "background-check",
                "color": "#36C5F0",
                "label": "Deskpal check",
            },
        )
        moved = json.loads(text(move_result))
        assert moved["verified"] is True, moved
        assert moved["inputDelivered"] is False, moved
        assert_unchanged(baseline, desktop_state())

        remote_id = next(
            cursor["cursorId"]
            for cursor in raw_status()["cursors"]
            if cursor["label"] == "Deskpal check"
        )
        peer = DeskpalClient(name="indicator-background-peer")
        peer_status = json.loads(text(peer.tool("agent_cursor_status")))
        assert peer_status["cursors"] == [], peer_status
        peer_hide = json.loads(
            text(peer.tool("agent_cursor_hide", {"cursorId": "background-check"}))
        )
        assert peer_hide["hidden"] is False, peer_hide
        assert any(
            cursor["cursorId"] == remote_id for cursor in raw_status()["cursors"]
        )

        peer.close()
        peer = None
        owner.proc.kill()
        owner.proc.wait(timeout=5)
        owner = None
        wait_until_removed(remote_id)
        remote_id = None
        assert_unchanged(baseline, desktop_state())
        remaining_ids = {cursor["cursorId"] for cursor in raw_status()["cursors"]}
        assert remaining_ids == existing_ids, (existing_ids, remaining_ids)
        print("PASS: background indicator ownership, cleanup, and non-interference")
    finally:
        if peer is not None:
            try:
                peer.close()
            except Exception:
                pass
        if owner is not None:
            try:
                owner.close()
            except Exception:
                pass
        if remote_id:
            try:
                wait_until_removed(remote_id)
            except Exception:
                pass


def main():
    run_suite()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
