#!/usr/bin/env python3
"""Live GNOME acceptance for capture-bound, process-owned agent cursors."""

import ast
import json
import os
import shutil
import subprocess
import sys
import time

from deskpal_client import DESKPAL, DeskpalClient, text

GDBUS_STATUS = [
    "gdbus",
    "call",
    "--session",
    "--dest",
    "org.deskpal.Indicator",
    "--object-path",
    "/org/deskpal/Indicator",
    "--method",
    "org.deskpal.Indicator.GetStatus",
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


def command_output(*args):
    return subprocess.check_output(args)


def desktop_state():
    clipboard = subprocess.run(
        ["xclip", "-selection", "clipboard", "-o"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout
    return {
        "pointer": command_output("xdotool", "getmouselocation", "--shell"),
        "focus": command_output("xprop", "-root", "_NET_ACTIVE_WINDOW"),
        "stacking": command_output("xprop", "-root", "_NET_CLIENT_LIST_STACKING"),
        "clipboard": clipboard,
    }


def move(client, capture, x, y, cursor_id, color, label):
    result = client.tool(
        "agent_cursor_move",
        {
            "captureId": capture["screenshot"]["captureId"],
            "x": x,
            "y": y,
            "cursorId": cursor_id,
            "color": color,
            "label": label,
        },
    )
    payload = json.loads(text(result))
    assert payload["verified"] is True, payload
    assert payload["indicatorMoved"] is True, payload
    assert payload["inputDelivered"] is False, payload
    return payload


def wait_until_removed(remote_id, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if all(cursor["cursorId"] != remote_id for cursor in raw_status()["cursors"]):
            return
        time.sleep(0.02)
    raise AssertionError(f"cursor survived owner exit: {remote_id}")


def run_suite():
    require_dependencies()
    baseline = desktop_state()
    existing_ids = {cursor["cursorId"] for cursor in raw_status()["cursors"]}
    owner = DeskpalClient(name="indicator-live-owner")
    peer = None
    forced = None
    owner_remote = None
    forced_remote = None
    try:
        environment = json.loads(text(owner.tool("get_environment_status")))
        assert environment["scope"] == "visible-desktop", environment
        assert environment["capabilities"]["agentCursor"]["available"] is True, environment
        assert environment["capabilities"]["backgroundPixelInput"]["available"] is False, environment
        blocker_ids = {blocker["id"] for blocker in environment["blockers"]}
        assert "non_interfering_pixel_input_unavailable" in blocker_ids, environment
        assert "agent_cursor_unavailable" not in blocker_ids, environment

        status = json.loads(text(owner.tool("agent_cursor_status")))
        assert status["available"] is True, status
        assert len(status["monitors"]) == 1, status
        monitor = status["monitors"][0]
        assert (monitor["x"], monitor["y"]) == (0, 0), monitor
        assert monitor["width"] == status["stageWidth"], status
        assert monitor["height"] == status["stageHeight"], status

        full = owner.tool("screenshot", {"fullScreen": True})
        full_meta = full["screenshot"]
        assert (full_meta["imageWidth"], full_meta["imageHeight"]) == (
            full_meta["sourceWidth"],
            full_meta["sourceHeight"],
        ), full_meta

        top_left = move(
            owner, full, 0, 0, "live-owner", "#36C5F0", "owner-a"
        )
        assert top_left["stagePosition"] == {"x": 0, "y": 0}, top_left
        bottom_right = move(
            owner,
            full,
            full_meta["imageWidth"] - 1,
            full_meta["imageHeight"] - 1,
            "live-owner",
            "#36C5F0",
            "owner-a",
        )
        edge = {"x": status["stageWidth"] - 1, "y": status["stageHeight"] - 1}
        assert bottom_right["stagePosition"] == edge, bottom_right
        assert bottom_right["cursor"]["renderedX"] == edge["x"], bottom_right
        assert bottom_right["cursor"]["renderedY"] == edge["y"], bottom_right

        restyled = move(
            owner,
            full,
            full_meta["imageWidth"] // 2,
            full_meta["imageHeight"] // 2,
            "live-owner",
            "#FF9F1C",
            "owner-a-restyled",
        )
        assert restyled["created"] is False, restyled
        assert restyled["cursor"]["color"] == "#FF9F1C", restyled
        assert restyled["cursor"]["label"] == "owner-a-restyled", restyled

        hidden = json.loads(
            text(owner.tool("agent_cursor_hide", {"cursorId": "live-owner"}))
        )
        assert hidden["hidden"] is True and hidden["verified"] is True, hidden
        downscaled = owner.tool(
            "screenshot", {"fullScreen": True, "maxWidth": 1920, "maxHeight": 1080}
        )
        down_meta = downscaled["screenshot"]
        image_x = down_meta["imageWidth"] // 3
        image_y = down_meta["imageHeight"] // 3
        down_move = move(
            owner,
            downscaled,
            image_x,
            image_y,
            "live-owner",
            "#36C5F0",
            "owner-a-downscaled",
        )
        assert down_move["stagePosition"] == {
            "x": round(image_x * status["stageWidth"] / down_meta["imageWidth"]),
            "y": round(image_y * status["stageHeight"] / down_meta["imageHeight"]),
        }, down_move

        owner_remote = next(
            cursor["cursorId"]
            for cursor in raw_status()["cursors"]
            if cursor["label"] == "owner-a-downscaled"
        )
        assert owner_remote.startswith("dp-"), owner_remote

        peer = DeskpalClient(name="indicator-live-peer")
        peer_status = json.loads(text(peer.tool("agent_cursor_status")))
        assert peer_status["cursors"] == [], peer_status
        peer_hide = json.loads(
            text(peer.tool("agent_cursor_hide", {"cursorId": "live-owner"}))
        )
        assert peer_hide["hidden"] is False, peer_hide
        denied = subprocess.check_output(
            [
                "gdbus",
                "call",
                "--session",
                "--dest",
                "org.deskpal.Indicator",
                "--object-path",
                "/org/deskpal/Indicator",
                "--method",
                "org.deskpal.Indicator.MoveCursorStyled",
                owner_remote,
                "700",
                "700",
                "#FF00FF",
                "intruder",
            ],
            text=True,
        ).strip()
        assert denied == "(false,)", denied
        owner_state = next(
            cursor for cursor in raw_status()["cursors"]
            if cursor["cursorId"] == owner_remote
        )
        assert owner_state["label"] == "owner-a-downscaled", owner_state

        peer.close()
        peer = None
        owner.close()
        owner = None
        wait_until_removed(owner_remote)

        forced = DeskpalClient(name="indicator-live-forced-death")
        forced_capture = forced.tool(
            "screenshot", {"fullScreen": True, "maxWidth": 960}
        )
        move(
            forced,
            forced_capture,
            300,
            200,
            "forced-death",
            "#A855F7",
            "forced-death",
        )
        forced_remote = next(
            cursor["cursorId"]
            for cursor in raw_status()["cursors"]
            if cursor["label"] == "forced-death"
        )
        forced.proc.kill()
        forced.proc.wait(timeout=5)
        forced = None
        wait_until_removed(forced_remote)

        after = desktop_state()
        for key, before_value in baseline.items():
            assert after[key] == before_value, {
                "changedState": key,
                "before": before_value,
                "after": after[key],
            }
        remaining_ids = {cursor["cursorId"] for cursor in raw_status()["cursors"]}
        assert remaining_ids == existing_ids, (existing_ids, remaining_ids)
        print("PASS: live capture mapping, style, isolation, death cleanup, and non-interference")
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
        if forced is not None and forced.proc.poll() is None:
            forced.proc.kill()
            forced.proc.wait(timeout=5)
        for remote_id in (owner_remote, forced_remote):
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
