#!/usr/bin/env python3
"""Deterministic isolated E2E coverage for deskpal's computer-use surface."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

from deskpal_client import (
    DESKPAL,
    DeskpalClient,
    png_is_opaque,
    png_size,
    start_xvfb,
    stop_process,
    text,
    tool_by_name,
)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "test", "fixtures", "computer_use_app.py")
DUPLICATE_FIXTURE = os.path.join(ROOT, "test", "fixtures", "duplicate_windows.py")
TITLE = "Deskpal Computer Use Fixture"
DUPLICATE_TITLE = "Deskpal Duplicate App State Fixture"


def require_dependencies():
    commands = ("Xvfb", "xauth", "identify", "convert")
    missing = [command for command in commands if not shutil.which(command)]
    try:
        subprocess.run(
            [sys.executable, "-c", "import tkinter"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        missing.append("python3-tk")
    if missing:
        raise SystemExit(f"FAIL: missing dependencies: {', '.join(missing)}")
    if not os.path.isfile(DESKPAL):
        raise SystemExit("FAIL: build/deskpal does not exist; run ninja -C build")


def run_suite():
    require_dependencies()
    with tempfile.TemporaryDirectory(prefix="deskpal-computer-use-") as temp_dir:
        xvfb, env = start_xvfb(temp_dir)
        fake_bin = os.path.join(temp_dir, "fake-bin")
        os.mkdir(fake_bin)
        backend_probe_output = os.path.join(temp_dir, "backend-probe-output")
        backend_probe = os.path.join(temp_dir, "backend-probe")
        with open(backend_probe, "w", encoding="ascii") as script:
            script.write(
                "#!/bin/sh\n"
                'printf "%s|%s|%s|%s|%s\\n" "${WAYLAND_DISPLAY-unset}" '
                '"${XDG_SESSION_TYPE-unset}" "${GDK_BACKEND-unset}" '
                '"${QT_QPA_PLATFORM-unset}" "${ELECTRON_OZONE_PLATFORM_HINT-unset}" '
                '> "$DESKPAL_BACKEND_PROBE"\n'
                'exec xmessage -title "deskpal-x11-backend" probe\n'
            )
        os.chmod(backend_probe, 0o755)
        hanging_convert = os.path.join(fake_bin, "convert")
        with open(hanging_convert, "w", encoding="ascii") as script:
            script.write("#!/bin/sh\nexec sleep 30\n")
        os.chmod(hanging_convert, 0o755)
        denied_client = DeskpalClient(env, args=["--no-uinput"], name="e2e-denied-exec")
        try:
            denied_launch = denied_client.tool(
                "launch_app", {"command": sys.executable, "args": [FIXTURE]}
            )
            assert denied_launch.get("isError") is True, denied_launch
            denied_exec = denied_client.tool("exec", {"command": "true"})
            assert denied_exec.get("isError") is True, denied_exec
        finally:
            denied_client.close()

        client = DeskpalClient(
            env, args=["--no-uinput", "--allow-exec"], name="e2e-computer-use"
        )
        second = None
        third = None
        duplicate_fixture = None
        try:
            tools = client.tools()
            screenshot_schema = tool_by_name(tools, "screenshot")["inputSchema"]
            list_schema = tool_by_name(tools, "list_windows")["inputSchema"]
            launch_schema = tool_by_name(tools, "launch_app")["inputSchema"]
            cursor_move_schema = tool_by_name(tools, "agent_cursor_move")["inputSchema"]
            environment_schema = tool_by_name(tools, "get_environment_status")["inputSchema"]
            app_state_schema = tool_by_name(tools, "get_app_state")["inputSchema"]
            tool_by_name(tools, "agent_cursor_status")
            tool_by_name(tools, "agent_cursor_hide")
            assert "maxWidth" in screenshot_schema["properties"]
            assert "maxHeight" in screenshot_schema["properties"]
            assert "includeAll" in list_schema["properties"]
            assert "forceX11" in launch_schema["properties"]
            assert cursor_move_schema["required"] == ["captureId", "x", "y"]
            assert cursor_move_schema["properties"]["x"]["type"] == "integer"
            assert "sessionId" in environment_schema["properties"]
            assert app_state_schema["properties"]["maxWidth"]["default"] == 1920
            assert app_state_schema["properties"]["includeText"]["default"] is False

            environment = json.loads(text(client.tool("get_environment_status")))
            assert environment["scope"] == "visible-desktop", environment
            assert environment["displayServer"] == "x11", environment
            assert environment["sharedSeat"] is True, environment
            assert environment["selectedBackends"]["pointer"] == "xtest", environment
            assert environment["capabilities"]["pointerInput"] == {
                "available": True,
                "backend": "xtest",
                "sharedSeat": True,
                "nonInterfering": False,
            }, environment
            assert environment["capabilities"]["semanticPress"]["available"] is False
            assert environment["capabilities"]["semanticSetText"]["available"] is False
            assert environment["capabilities"]["processLaunch"]["available"] is True
            assert environment["capabilities"]["filesystem"]["available"] is False
            blocker_ids = {blocker["id"] for blocker in environment["blockers"]}
            assert "non_interfering_pixel_input_unavailable" in blocker_ids, environment
            assert "agent_cursor_unavailable" in blocker_ids, environment
            assert environment["setupActions"], environment

            indicator_status = json.loads(text(client.tool("agent_cursor_status")))
            assert indicator_status["available"] is False, indicator_status
            assert indicator_status["blocker"], indicator_status
            hidden = json.loads(text(client.tool("agent_cursor_hide")))
            assert hidden["hidden"] is False and hidden["verified"] is True, hidden
            unknown_capture = client.tool(
                "agent_cursor_move", {"captureId": "capture-unknown", "x": 1, "y": 1}
            )
            assert unknown_capture.get("isError") is True, unknown_capture

            backend_launch = client.tool(
                "launch_app",
                {
                    "command": backend_probe,
                    "waitForWindow": "deskpal-x11-backend",
                    "killExisting": False,
                    "timeout": 3,
                    "env": {
                        "DESKPAL_BACKEND_PROBE": backend_probe_output,
                        "WAYLAND_DISPLAY": "wayland-test",
                        "XDG_SESSION_TYPE": "wayland",
                        "GDK_BACKEND": "wayland",
                        "QT_QPA_PLATFORM": "wayland",
                        "ELECTRON_OZONE_PLATFORM_HINT": "wayland",
                    },
                },
            )
            assert "deskpal-x11-backend" in text(backend_launch), backend_launch
            with open(backend_probe_output, encoding="ascii") as probe_file:
                assert probe_file.read().strip() == "unset|x11|x11|xcb|x11"

            launched = client.tool(
                "launch_app",
                {
                    "command": sys.executable,
                    "args": [FIXTURE],
                    "waitForWindow": TITLE,
                    "killExisting": False,
                    "timeout": 5,
                },
            )
            assert TITLE in text(launched), launched

            deadline = time.time() + 2
            listing = ""
            while time.time() < deadline:
                listing = text(client.tool("list_windows"))
                if TITLE in listing:
                    break
                time.sleep(0.05)
            assert TITLE in listing, listing
            assert 'class="Tk"' in listing, listing
            assert "Xvfb" not in listing, listing

            assert "No window found" in text(
                client.tool("find_window", {"name": "Tk"})
            )
            assert "Window not found" in text(
                client.tool("get_window_geometry", {"windowName": "Tk"})
            )

            default_count = listing.count("[0x") + listing.count("[")
            assert default_count < 5, listing

            recursive = text(client.tool("list_windows", {"includeAll": True}))
            assert TITLE in recursive, recursive

            original = client.tool("screenshot", {"windowName": TITLE})
            assert png_size(original) == (720, 520), png_size(original)
            assert png_is_opaque(original)
            assert original["screenshot"] == {
                "sourceWidth": 720,
                "sourceHeight": 520,
                "imageWidth": 720,
                "imageHeight": 520,
                "coordinateScaleX": 1,
                "coordinateScaleY": 1,
            }, original["screenshot"]

            app_state_result = client.tool(
                "get_app_state",
                {"windowName": TITLE, "maxWidth": 360, "maxHeight": 260},
            )
            assert png_size(app_state_result) == (360, 260), png_size(app_state_result)
            app_state = app_state_result["appState"]
            assert app_state == json.loads(text(app_state_result, 1)), app_state
            assert app_state["target"]["title"] == TITLE, app_state
            assert app_state["target"]["class"] == "Tk", app_state
            assert app_state["target"]["processId"] > 0, app_state
            assert app_state["target"]["geometry"] == {
                "x": 40, "y": 40, "width": 720, "height": 520,
            }, app_state
            assert app_state["image"] == {
                "sourceWidth": 720,
                "sourceHeight": 520,
                "imageWidth": 360,
                "imageHeight": 260,
                "coordinateScaleX": 2,
                "coordinateScaleY": 2,
            }, app_state
            assert app_state["transform"] == {
                "imageSpace": "window-image-pixels",
                "targetSpace": "desktop-stage-pixels",
                "offsetX": 40,
                "offsetY": 40,
                "scaleX": 2,
                "scaleY": 2,
                "supported": True,
            }, app_state
            assert app_state["captureId"].startswith("capture-"), app_state
            assert app_state["consistency"] == {
                "identityStable": True,
                "geometryStable": True,
                "focusKnown": True,
                "focusStable": True,
                "transformSupported": True,
                "stable": True,
                "retryRecommended": False,
            }, app_state
            assert app_state["semantic"]["available"] is False, app_state
            assert app_state["semantic"]["includeText"] is False, app_state
            assert app_state["semantic"]["includeAttributes"] is False, app_state
            assert app_state["inputDelivered"] is False, app_state
            by_id = client.tool(
                "get_app_state",
                {"windowId": app_state["target"]["windowId"], "maxWidth": 360},
            )
            assert by_id["appState"]["target"]["windowId"] == app_state["target"]["windowId"]
            subprocess.run(
                ["xprop", "-root", "-remove", "_NET_ACTIVE_WINDOW"],
                env=env,
                stdout=subprocess.DEVNULL,
                check=True,
            )
            unknown_focus = client.tool(
                "get_app_state", {"windowId": app_state["target"]["windowId"]}
            )["appState"]
            assert unknown_focus["focus"]["known"] is False, unknown_focus
            assert unknown_focus["consistency"]["focusKnown"] is False, unknown_focus
            assert unknown_focus["consistency"]["stable"] is False, unknown_focus
            assert "captureId" not in unknown_focus, unknown_focus
            subprocess.run(
                [
                    "xprop", "-root", "-f", "_NET_ACTIVE_WINDOW", "32x",
                    "-set", "_NET_ACTIVE_WINDOW", app_state["target"]["windowId"],
                ],
                env=env,
                stdout=subprocess.DEVNULL,
                check=True,
            )
            client.tool(
                "resize_window",
                {"windowId": app_state["target"]["windowId"], "width": 700, "height": 500},
            )
            time.sleep(0.1)
            stale_app_capture = client.tool(
                "agent_cursor_move",
                {"captureId": app_state["captureId"], "x": 100, "y": 100},
            )
            assert stale_app_capture.get("isError") is True, stale_app_capture
            assert "geometry changed" in text(stale_app_capture), stale_app_capture
            client.tool(
                "resize_window",
                {"windowId": app_state["target"]["windowId"], "width": 720, "height": 520},
            )
            for invalid_target in (
                {},
                {"windowName": TITLE, "windowId": app_state["target"]["windowId"]},
            ):
                invalid_state = client.tool("get_app_state", invalid_target)
                assert invalid_state.get("isError") is True, invalid_state
            for unavailable_name in ("Computer Use", TITLE.lower()):
                unsupported_state = client.tool(
                    "get_app_state", {"windowName": unavailable_name}
                )
                assert unsupported_state.get("isError") is True, unsupported_state
                assert unsupported_state["appStateError"]["code"] == (
                    "target_not_found_or_unsupported_backend"
                ), unsupported_state

            desktop_capture = client.tool(
                "screenshot", {"fullScreen": True, "maxWidth": 640}
            )
            assert png_size(desktop_capture) == (640, 400), png_size(desktop_capture)
            capture_metadata = desktop_capture["screenshot"]
            assert capture_metadata["sourceWidth"] == 1280, capture_metadata
            assert capture_metadata["sourceHeight"] == 800, capture_metadata
            assert capture_metadata["coordinateScaleX"] == 2, capture_metadata
            assert capture_metadata["coordinateScaleY"] == 2, capture_metadata
            assert capture_metadata["captureId"].startswith("capture-"), capture_metadata
            assert capture_metadata["captureTarget"] == "desktop", capture_metadata
            assert capture_metadata["captureCoordinateSpace"] == "image-pixels", capture_metadata
            assert capture_metadata["captureId"] in text(desktop_capture, 1), desktop_capture
            unavailable_move = client.tool(
                "agent_cursor_move",
                {"captureId": capture_metadata["captureId"], "x": 320, "y": 200},
            )
            assert unavailable_move.get("isError") is True, unavailable_move
            assert "indicator" in text(unavailable_move).lower(), unavailable_move
            next_capture = client.tool("screenshot", {"fullScreen": True})
            assert (
                next_capture["screenshot"]["captureId"]
                != capture_metadata["captureId"]
            ), next_capture["screenshot"]

            scaled = client.tool(
                "screenshot",
                {"windowName": TITLE, "maxWidth": 360, "maxHeight": 260},
            )
            assert png_size(scaled) == (360, 260), png_size(scaled)
            assert scaled["screenshot"]["coordinateScaleX"] == 2
            assert scaled["screenshot"]["coordinateScaleY"] == 2
            assert "Input-tool coordinates use source pixels" in text(scaled, 1)

            width_only = client.tool(
                "screenshot", {"windowName": TITLE, "maxWidth": 500}
            )
            assert png_size(width_only) == (500, 361), png_size(width_only)
            assert abs(width_only["screenshot"]["coordinateScaleX"] - 1.44) < 0.01
            assert abs(width_only["screenshot"]["coordinateScaleY"] - 520 / 361) < 0.01

            timeout_env = env.copy()
            timeout_env["PATH"] = fake_bin + os.pathsep + timeout_env["PATH"]
            timeout_env["DESKPAL_TEST_SCALE_TIMEOUT_MS"] = "100"
            timeout_client = DeskpalClient(
                timeout_env, args=["--no-uinput"], name="e2e-scale-timeout"
            )
            try:
                started = time.monotonic()
                timed_out = timeout_client.tool(
                    "screenshot", {"windowName": TITLE, "maxWidth": 360}
                )
                elapsed = time.monotonic() - started
                assert elapsed < 2, elapsed
                assert "downscaling failed" in text(timed_out), timed_out
            finally:
                timeout_client.close()

            initial_ocr = text(
                client.tool("read_screen_text", {"windowName": TITLE})
            )
            assert "Deskpal computer use" in initial_ocr, initial_ocr
            assert "Apply Text" in initial_ocr, initial_ocr

            clicked_entry = text(
                client.tool("click", {"windowName": TITLE, "x": 360, "y": 100})
            )
            assert "Clicked" in clicked_entry, clicked_entry
            client.tool("key_press", {"windowName": TITLE, "keys": "ctrl+a"})
            client.tool(
                "type_text",
                {"windowName": TITLE, "text": "typed by deskpal", "delay": 4},
            )
            applied = text(
                client.tool("click_text", {"windowName": TITLE, "text": "Apply Text"})
            )
            assert "Clicked" in applied, applied
            updated = text(client.tool("read_screen_text", {"windowName": TITLE}))
            assert "typed by deskpal" in updated, updated

            hovered = text(
                client.tool(
                    "hover_text",
                    {"windowName": TITLE, "text": "Hover Target", "settleMs": 700},
                )
            )
            assert "Deskpal" in hovered and "tooltip" in hovered, hovered

            for race_tool in ("click_text", "hover_text"):
                opened = text(
                    client.tool(
                        "key_press", {"windowName": TITLE, "keys": "ctrl+r"}
                    )
                )
                assert "Pressed" in opened, opened
                time.sleep(0.2)
                raced = client.tool(
                    race_tool,
                    {
                        "windowName": "Deskpal Race Window",
                        "text": "Race Target",
                        **({"settleMs": 100} if race_tool == "hover_text" else {}),
                    },
                )
                assert "Window not found" in text(raced), (race_tool, raced)
                assert "No window found" in text(
                    client.tool("find_window", {"name": "Deskpal Race Window"})
                )

            geometry = text(client.tool("get_window_geometry", {"windowName": TITLE}))
            assert "Size: 720x520" in geometry, geometry
            resized = text(
                client.tool(
                    "resize_window",
                    {"windowName": TITLE, "width": 640, "height": 440},
                )
            )
            assert "640x440" in resized, resized
            geometry = text(client.tool("get_window_geometry", {"windowName": TITLE}))
            assert "Size: 640x440" in geometry, geometry
            client.tool(
                "resize_window",
                {"windowName": TITLE, "width": 720, "height": 520},
            )

            scrolled = text(
                client.tool(
                    "scroll", {"windowName": TITLE, "direction": "down", "clicks": 3}
                )
            )
            assert "Scrolled down 3 clicks" in scrolled, scrolled

            copied = text(client.tool("set_clipboard", {"text": "deskpal clipboard"}))
            assert "Wrote" in copied, copied
            assert text(client.tool("get_clipboard")) == "deskpal clipboard"

            isolated = client.tool(
                "launch_isolated_app",
                {
                    "command": "xmessage",
                    "args": ["-title", "lock-isolation", "private"],
                    "waitForWindow": "lock-isolation",
                    "timeout": 3,
                },
            )
            isolated_id = isolated["sessionId"]
            private_click = client.tool(
                "click",
                {
                    "windowName": "lock-isolation",
                    "x": 20,
                    "y": 40,
                    "sessionId": isolated_id,
                },
            )
            assert "Clicked" in text(private_click), private_click

            contender_env = env.copy()
            contender_env["XDG_RUNTIME_DIR"] = os.path.join(temp_dir, "other-runtime")
            display_number = int(env["DISPLAY"].lstrip(":"))
            contender_env["DISPLAY"] = f"unix:{display_number:04d}.0"
            contender_env["DESKPAL_HEADLESS_ACTIVE"] = "1"
            second = DeskpalClient(
                contender_env,
                args=["--no-uinput", "--allow-exec"],
                name="e2e-lock-contender",
            )
            assert TITLE in text(second.tool("list_windows"))
            blocked = second.tool("focus_window", {"windowName": TITLE})
            assert blocked.get("isError") is True, blocked
            assert "desktop control is already held" in text(blocked), blocked

            blocked_exec = second.tool("exec", {"command": "true"})
            assert blocked_exec.get("isError") is True, blocked_exec
            assert "desktop control is already held" in text(blocked_exec)

            blocked_isolated = second.tool(
                "launch_isolated_app",
                {
                    "command": "xmessage",
                    "args": ["-title", "lock-isolation", "private"],
                    "waitForWindow": "lock-isolation",
                    "timeout": 3,
                },
            )
            assert blocked_isolated.get("isError") is True, blocked_isolated
            assert "desktop control is already held" in text(blocked_isolated)

            scoped_exec = second.tool(
                "exec", {"command": "true", "sessionId": isolated_id}
            )
            assert scoped_exec.get("isError") is True, scoped_exec
            assert "desktop control is already held" in text(scoped_exec)
            scoped_launch = second.tool(
                "launch_app",
                {
                    "command": "xmessage",
                    "args": ["-title", "scoped-launch", "blocked"],
                    "sessionId": isolated_id,
                },
            )
            assert scoped_launch.get("isError") is True, scoped_launch
            assert "desktop control is already held" in text(scoped_launch)

            client.tool("close_isolated_session", {"sessionId": isolated_id})

            stress = text(
                client.tool(
                    "click_text", {"windowName": TITLE, "text": "Open Stress Windows"}
                )
            )
            assert "Clicked" in stress, stress
            time.sleep(0.3)
            stress_listing = text(client.tool("list_windows", {"includeAll": True}))
            assert "Transient 49" in stress_listing, stress_listing
            assert len(stress_listing) <= 8191, len(stress_listing)

            transient = text(client.tool("find_window", {"name": "Transient 00"}))
            transient_id = transient.split("]", 1)[0].lstrip("[")
            time.sleep(1.6)
            stale_cases = (
                ("screenshot", {}),
                ("click", {"x": 1, "y": 1}),
                ("type_text", {"text": "must-not-type"}),
                ("key_press", {"keys": "Return"}),
                ("get_window_geometry", {}),
                ("resize_window", {"width": 300, "height": 200}),
                ("mouse_move", {"x": 1, "y": 1}),
                ("scroll", {"direction": "down"}),
                ("drag", {"fromX": 1, "fromY": 1, "toX": 2, "toY": 2}),
                ("mouse_down", {"x": 1, "y": 1}),
                ("hover_text", {"text": "missing"}),
            )
            for tool_name, arguments in stale_cases:
                stale = client.tool(
                    tool_name, {"windowId": transient_id, **arguments}
                )
                assert "Window not found" in text(stale), (tool_name, stale)

            malformed_cases = (123, None, "")
            for selector in malformed_cases:
                malformed = client.tool(
                    "click", {"windowId": selector, "x": 1, "y": 1}
                )
                assert malformed.get("isError") is True, malformed
                malformed_name = client.tool(
                    "type_text", {"windowName": selector, "text": "must-not-type"}
                )
                assert malformed_name.get("isError") is True, malformed_name

            conflicts = (
                client.tool(
                    "click",
                    {
                        "windowId": "1",
                        "windowName": TITLE,
                        "x": 1,
                        "y": 1,
                    },
                ),
                client.tool(
                    "screenshot", {"fullScreen": True, "windowName": TITLE}
                ),
            )
            for conflict in conflicts:
                assert conflict.get("isError") is True, conflict

            extreme_cases = (
                ("click", {"windowName": TITLE, "x": 2**31, "y": 1}),
                ("mouse_move", {"windowName": TITLE, "x": -(2**31), "y": 1}),
                ("scroll", {"windowName": TITLE, "direction": "down", "clicks": 2**31}),
                (
                    "drag",
                    {
                        "windowName": TITLE,
                        "fromX": 1,
                        "fromY": 1,
                        "toX": 2,
                        "toY": 2,
                        "steps": 2**31,
                    },
                ),
                ("type_text", {"windowName": TITLE, "text": "x", "delay": 2**31}),
                ("resize_window", {"windowName": TITLE, "width": 2**31, "height": 1}),
                ("screenshot", {"windowName": TITLE, "maxWidth": 1.5}),
                ("get_app_state", {"windowName": TITLE, "maxWidth": 1.5}),
                ("get_app_state", {"windowName": TITLE, "semanticMaxNodes": 2**31}),
                ("get_app_state", {"windowName": TITLE, "includeText": "yes"}),
                ("wait_for_window", {"name": "missing", "timeout": 2**31}),
            )
            for tool_name, arguments in extreme_cases:
                rejected = client.tool(tool_name, arguments)
                assert rejected.get("isError") is True, (tool_name, rejected)
            assert TITLE in text(client.tool("list_windows"))

            for empty_search in (
                client.tool("find_window", {"name": ""}),
                client.tool("wait_for_window", {"name": "", "timeout": 1}),
                client.tool(
                    "launch_app",
                    {
                        "command": "xmessage",
                        "args": ["ignored"],
                        "waitForWindow": "",
                    },
                ),
            ):
                assert empty_search.get("isError") is True, empty_search

            closed = text(
                client.tool("click_text", {"windowName": TITLE, "text": "Close Fixture"})
            )
            assert "Clicked" in closed, closed
            time.sleep(0.3)
            assert "No window found" in text(client.tool("find_window", {"name": TITLE}))

            duplicate_fixture = subprocess.Popen(
                [sys.executable, DUPLICATE_FIXTURE],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            deadline = time.time() + 3
            ambiguous_state = None
            while time.time() < deadline:
                ambiguous_state = client.tool(
                    "get_app_state", {"windowName": DUPLICATE_TITLE}
                )
                if ambiguous_state.get("appStateError", {}).get("code") == "target_ambiguous":
                    break
                if duplicate_fixture.poll() is not None:
                    raise AssertionError(
                        f"duplicate fixture exited: {duplicate_fixture.stderr.read()}"
                    )
                time.sleep(0.05)
            assert ambiguous_state is not None, ambiguous_state
            assert ambiguous_state.get("isError") is True, ambiguous_state
            assert ambiguous_state["appStateError"]["code"] == "target_ambiguous"
            duplicate_fixture.terminate()
            duplicate_fixture.wait(timeout=3)
            duplicate_fixture = None
            subprocess.run(
                ["xprop", "-root", "-remove", "_NET_CLIENT_LIST"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
            )

            client.close()
            client = None
            third = DeskpalClient(
                env,
                args=["--no-uinput", "--allow-exec"],
                name="e2e-lock-successor",
            )
            successor = third.tool(
                "launch_app",
                {
                    "command": "xmessage",
                    "args": ["-title", "lock-successor", "lock released"],
                    "waitForWindow": "lock-successor",
                    "killExisting": False,
                    "timeout": 3,
                },
            )
            assert "lock-successor" in text(successor), successor
        finally:
            if duplicate_fixture is not None:
                stop_process(duplicate_fixture)
            if third is not None:
                third.close()
            if second is not None:
                second.close()
            if client is not None:
                client.close()
            stop_process(xvfb)


def main():
    run_suite()
    print("PASS: deterministic computer-use workflow, scaling, and lock arbitration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
