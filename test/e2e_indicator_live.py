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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ACCESSIBILITY_FIXTURE = os.path.join(ROOT, "test", "fixtures", "accessibility_app.py")

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
    metadata = capture.get("screenshot", capture.get("appState"))
    result = client.tool(
        "agent_cursor_move",
        {
            "captureId": metadata["captureId"],
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


def find_semantic_nodes(payload):
    found = []

    def visit(nodes):
        for node in nodes:
            found.append(node)
            visit(node.get("children", []))

    for application in payload.get("applications", []):
        for window in application.get("windows", []):
            visit(window.get("nodes", []))
    return found


def wait_until_removed(remote_id, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if all(cursor["cursorId"] != remote_id for cursor in raw_status()["cursors"]):
            return
        time.sleep(0.02)
    raise AssertionError(f"cursor survived owner exit: {remote_id}")


def run_suite():
    require_dependencies()
    fixture_title = "Deskpal Accessibility Fixture"
    fixture_env = os.environ.copy()
    fixture_env.pop("WAYLAND_DISPLAY", None)
    fixture_env["XDG_SESSION_TYPE"] = "x11"
    fixture_env["GDK_BACKEND"] = "x11"
    fixture_env["GTK_MODULES"] = "gail:atk-bridge"
    fixture = subprocess.Popen(
        ["/usr/bin/python3", ACCESSIBILITY_FIXTURE],
        env=fixture_env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    deadline = time.monotonic() + 3
    fixture_window_id = None
    while time.monotonic() < deadline:
        found = subprocess.run(
            ["xdotool", "search", "--onlyvisible", "--name", f"^{fixture_title}$"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        if found.returncode == 0 and found.stdout.strip():
            fixture_window_id = found.stdout.splitlines()[0]
            break
        time.sleep(0.05)
    else:
        fixture.terminate()
        fixture.wait(timeout=3)
        raise AssertionError("live app-state fixture did not appear")
    subprocess.run(
        [
            "xprop", "-id", fixture_window_id, "-f", "_NET_WM_PID", "32c",
            "-set", "_NET_WM_PID", str(fixture.pid),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    time.sleep(0.3)
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
        assert environment["capabilities"]["semanticPress"]["available"] is True, environment
        assert environment["capabilities"]["semanticSetText"]["available"] is True, environment
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

        app_state_baseline = desktop_state()
        app_state_result = owner.tool(
            "get_app_state",
            {"windowName": fixture_title, "maxWidth": 210, "maxHeight": 110},
        )
        app_state = app_state_result["appState"]
        assert app_state["target"]["processId"] == fixture.pid, app_state
        assert app_state["focus"]["known"] is True, app_state
        assert app_state["consistency"]["stable"] is True, app_state
        image_x = app_state["image"]["imageWidth"] // 2
        image_y = app_state["image"]["imageHeight"] // 2
        app_move = move(
            owner,
            app_state_result,
            image_x,
            image_y,
            "app-state-live",
            "#22C55E",
            "app-state-live",
        )
        expected_app_x = app_state["transform"]["offsetX"] + round(
            image_x * app_state["transform"]["scaleX"]
        )
        expected_app_y = app_state["transform"]["offsetY"] + round(
            image_y * app_state["transform"]["scaleY"]
        )
        assert app_move["stagePosition"] == {
            "x": expected_app_x,
            "y": expected_app_y,
        }, app_move
        app_hidden = json.loads(
            text(owner.tool("agent_cursor_hide", {"cursorId": "app-state-live"}))
        )
        assert app_hidden["hidden"] is True, app_hidden
        app_state_after = desktop_state()
        for key, before_value in app_state_baseline.items():
            assert app_state_after[key] == before_value, {
                "phase": "app-state cursor",
                "changedState": key,
                "before": before_value,
                "after": app_state_after[key],
            }

        semantic_nodes = find_semantic_nodes(app_state["semantic"])
        semantic_button = next(
            node for node in semantic_nodes
            if node.get("name") == "Apply validation message"
        )
        semantic_entry = next(
            node for node in semantic_nodes
            if node.get("name") == "Validation message"
        )
        press_baseline = desktop_state()
        press_arguments = {
            "captureId": app_state["captureId"],
            "target": semantic_button["locator"],
            "action": "click",
            "verify": {
                "role": "label",
                "name": "Apply count",
                "textEquals": "Apply count: 1",
            },
            "cursorId": "semantic-live",
            "color": "#22C55E",
            "label": "semantic press",
        }
        press_result = owner.tool("agent_semantic_press", press_arguments)
        press = json.loads(text(press_result))
        assert press.get("route") == "atspi", (press, press_result)
        assert press["verified"] is True, press
        assert press["indicatorMoved"] is True, press
        assert press["actionApplied"] is True, press
        assert press["inputDelivered"] is True, press
        assert press["sharedPointerMoved"] is False, press
        assert press["stackingChanged"] is None, press
        assert press["stackingChangeUnknown"] is True, press
        assert press["clipboardChanged"] is False, press
        assert press["action"]["verified"] is True, press
        idempotent_press = json.loads(
            text(owner.tool("agent_semantic_press", press_arguments))
        )
        assert idempotent_press["verified"] is True, idempotent_press
        assert idempotent_press["actionApplied"] is False, idempotent_press
        assert idempotent_press["inputDelivered"] is False, idempotent_press
        duplicate_target = next(
            node for node in find_semantic_nodes(app_state["semantic"])
            if node.get("name") == "Duplicate action"
        )
        ambiguous_press_result = owner.tool(
            "agent_semantic_press",
            {
                **press_arguments,
                "target": duplicate_target["locator"],
                "cursorId": "semantic-ambiguous",
            },
        )
        assert ambiguous_press_result.get("isError") is True, ambiguous_press_result
        ambiguous_press = json.loads(text(ambiguous_press_result))
        assert ambiguous_press["mutationIssued"] is False, ambiguous_press
        assert ambiguous_press["inputDelivered"] is False, ambiguous_press
        assert ambiguous_press["action"]["targetMatchCount"] == 2, ambiguous_press
        owner.tool("agent_cursor_hide", {"cursorId": "semantic-ambiguous"})
        set_text_arguments = {
            "captureId": app_state["captureId"],
            "target": semantic_entry["locator"],
            "value": "semantic live value",
            "cursorId": "semantic-text-live",
            "color": "#8B5CF6",
            "label": "semantic text",
        }
        set_text_result = owner.tool(
            "agent_semantic_set_text", set_text_arguments
        )
        set_text_action = json.loads(text(set_text_result))
        assert set_text_action["route"] == "atspi", set_text_action
        assert set_text_action["operation"] == "setText", set_text_action
        assert set_text_action["verified"] is True, set_text_action
        assert set_text_action["actionApplied"] is True, set_text_action
        assert set_text_action["inputDelivered"] is True, set_text_action
        assert set_text_action["sharedPointerMoved"] is False, set_text_action
        assert set_text_action["clipboardChanged"] is False, set_text_action
        idempotent_text = json.loads(
            text(owner.tool("agent_semantic_set_text", set_text_arguments))
        )
        assert idempotent_text["verified"] is True, idempotent_text
        assert idempotent_text["actionApplied"] is False, idempotent_text
        assert idempotent_text["inputDelivered"] is False, idempotent_text
        owner.tool("agent_cursor_hide", {"cursorId": "semantic-text-live"})
        press_hidden = json.loads(
            text(owner.tool("agent_cursor_hide", {"cursorId": "semantic-live"}))
        )
        assert press_hidden["hidden"] is True, press_hidden
        press_after = desktop_state()
        for key, before_value in press_baseline.items():
            assert press_after[key] == before_value, {
                "phase": "semantic press",
                "changedState": key,
                "before": before_value,
                "after": press_after[key],
            }
        baseline = desktop_state()

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
            # Focus is checked immediately around app-state cursor movement and
            # semantic press above. GNOME may clear _NET_ACTIVE_WINDOW while
            # this longer lifecycle test runs even without Deskpal input.
            if key == "focus":
                continue
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
        if fixture.poll() is None:
            fixture.terminate()
            try:
                fixture.wait(timeout=3)
            except subprocess.TimeoutExpired:
                fixture.kill()
                fixture.wait(timeout=3)


def main():
    run_suite()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
