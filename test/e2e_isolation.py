#!/usr/bin/env python3
"""E2E test for goal-aware desktop versus isolated Xvfb routing."""

import base64
import json
import os
import re
import signal
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESKPAL = os.path.join(ROOT, "build", "deskpal")
WINDOW_TITLE = "deskpal-isolation-e2e"
SECOND_WINDOW_TITLE = "deskpal-isolation-e2e-second"
COMPANION_WINDOW_TITLE = "deskpal-companion-e2e"


class DeskpalClient:
    def __init__(self, env):
        self.proc = subprocess.Popen(
            [DESKPAL],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        self.request_id = 0
        self.call(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "e2e-isolation", "version": "1.0"},
            },
        )

    def call(self, method, params):
        self.request_id += 1
        request = {
            "jsonrpc": "2.0",
            "id": self.request_id,
            "method": method,
            "params": params,
        }
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line.startswith("{"):
            raise AssertionError(f"non-JSON data on MCP stdout: {line!r}")
        return json.loads(line)

    def tool(self, name, arguments=None):
        response = self.call(
            "tools/call", {"name": name, "arguments": arguments or {}}
        )
        return response["result"]

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        return self.proc.stderr.read()


def text(result):
    return result["content"][0]["text"]


def png_size(result):
    image = result["content"][0]
    data = base64.b64decode(image["data"])
    assert image["type"] == "image"
    assert image["mimeType"] == "image/png"
    assert data[:8] == bytes.fromhex("89504e470d0a1a0a")
    return struct.unpack(">II", data[16:24])


def tool_by_name(tools, name):
    return next(tool for tool in tools if tool["name"] == name)


def process_table():
    table = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/stat", encoding="ascii") as stat_file:
                stat = stat_file.read()
            fields = stat[stat.rfind(")") + 2 :].split()
            table[int(entry)] = {"ppid": int(fields[1]), "pgrp": int(fields[2])}
        except (FileNotFoundError, IndexError, OSError, ValueError):
            continue
    return table


def direct_children(parent_pid):
    return {pid for pid, info in process_table().items() if info["ppid"] == parent_pid}


def process_group_members(process_group):
    return {pid for pid, info in process_table().items() if info["pgrp"] == process_group}


def assert_no_uinput_fd(process_ids):
    for pid in process_ids:
        fd_dir = f"/proc/{pid}/fd"
        try:
            descriptors = os.listdir(fd_dir)
        except (FileNotFoundError, PermissionError):
            continue
        for descriptor in descriptors:
            try:
                target = os.readlink(os.path.join(fd_dir, descriptor))
            except (FileNotFoundError, PermissionError):
                continue
            assert target != "/dev/uinput", (
                f"isolated process {pid} inherited host uinput descriptor"
            )


def process_name(pid):
    try:
        with open(f"/proc/{pid}/comm", encoding="ascii") as comm_file:
            return comm_file.read().strip()
    except (FileNotFoundError, PermissionError):
        return ""


def signal_is_ignored(pid, signal_number):
    try:
        with open(f"/proc/{pid}/status", encoding="ascii") as status_file:
            for line in status_file:
                if line.startswith("SigIgn:"):
                    ignored = int(line.split()[1], 16)
                    return bool(ignored & (1 << (signal_number - 1)))
    except (FileNotFoundError, PermissionError, ValueError):
        pass
    return False


def wait_for_process_group_exit(process_group, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not process_group_members(process_group):
            return
        time.sleep(0.05)
    remaining = process_group_members(process_group)
    raise AssertionError(f"isolated process group {process_group} survived: {remaining}")


def main():
    missing = [
        name
        for name in ("Xvfb", "xvfb-run", "xauth", "xmessage")
        if not shutil.which(name)
    ]
    if missing:
        print(f"SKIP: missing isolation test dependencies: {', '.join(missing)}")
        return 0
    if not os.environ.get("DISPLAY"):
        print("SKIP: no desktop DISPLAY for the primary Deskpal server")
        return 0
    if not os.path.isfile(DESKPAL):
        print("FAIL: build/deskpal does not exist; run ninja -C build", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="deskpal-isolation-test-") as temp_dir:
        sentinel = os.path.join(temp_dir, "host-screenshot-invoked")
        launch_sentinel = os.path.join(temp_dir, "shell-syntax-executed")
        runtime_probe_output = os.path.join(temp_dir, "runtime-probe-output")
        runtime_probe = os.path.join(temp_dir, "runtime-probe")
        fake_screenshot = os.path.join(temp_dir, "gnome-screenshot")
        with open(fake_screenshot, "w", encoding="ascii") as script:
            script.write(
                "#!/bin/sh\n"
                ': > "$DESKPAL_SCREENSHOT_SENTINEL"\n'
                "exit 1\n"
            )
        os.chmod(fake_screenshot, 0o755)
        with open(runtime_probe, "w", encoding="ascii") as script:
            script.write(
                "#!/bin/sh\n"
                'printf "%s|%s\\n" "${XDG_RUNTIME_DIR-unset}" '
                '"${WAYLAND_DISPLAY-unset}" > "$DESKPAL_RUNTIME_PROBE"\n'
                "exec xmessage \"$@\"\n"
            )
        os.chmod(runtime_probe, 0o755)

        env = os.environ.copy()
        env.pop("DESKPAL_HEADLESS_ACTIVE", None)
        env["PATH"] = temp_dir + os.pathsep + env["PATH"]
        env["DESKPAL_SCREENSHOT_SENTINEL"] = sentinel
        env["DESKPAL_RUNTIME_PROBE"] = runtime_probe_output
        env["XDG_RUNTIME_DIR"] = os.path.join(temp_dir, "host-runtime")
        env["WAYLAND_DISPLAY"] = "wayland-host"
        client = DeskpalClient(env)
        open_sessions = set()
        stderr = ""
        try:
            tools = client.call("tools/list", {})["result"]["tools"]
            isolated_tool = tool_by_name(tools, "launch_isolated_app")
            desktop_tool = tool_by_name(tools, "launch_app")
            screenshot_tool = tool_by_name(tools, "screenshot")
            assert "locally developed app" in isolated_tool["description"]
            assert "visible desktop" in desktop_tool["description"]
            assert "sessionId" in screenshot_tool["inputSchema"]["properties"]

            for malformed in (None, 42, {}, ""):
                failed = client.tool("list_windows", {"sessionId": malformed})
                assert failed.get("isError") is True, failed
                assert "sessionId must be" in text(failed), text(failed)

            failed_launch = client.tool(
                "launch_isolated_app",
                {"command": "deskpal-command-that-does-not-exist", "timeout": 1},
            )
            assert failed_launch.get("isError") is True, failed_launch
            assert "sessionId" not in failed_launch, failed_launch
            assert "No such file" in text(failed_launch), text(failed_launch)

            children_before = direct_children(client.proc.pid)
            launched = client.tool(
                "launch_isolated_app",
                {
                    "command": runtime_probe,
                    "args": [
                        "-title",
                        WINDOW_TITLE,
                        f"private verification; touch {launch_sentinel}",
                    ],
                    "waitForWindow": WINDOW_TITLE,
                    "timeout": 3,
                    "screenSize": "1024x768",
                    "env": {
                        "DISPLAY": ":12345",
                        "XAUTHORITY": "/does/not/exist",
                        "WAYLAND_DISPLAY": "wayland-do-not-use",
                        "XDG_RUNTIME_DIR": "/host/runtime/do-not-use",
                        "DESKPAL_LITERAL_ENV": f"value; touch {launch_sentinel}",
                    },
                },
            )
            session_id = launched["sessionId"]
            open_sessions.add(session_id)
            assert session_id in text(launched)
            assert not os.path.exists(launch_sentinel), (
                "launch arguments or environment were interpreted as shell syntax"
            )
            with open(runtime_probe_output, encoding="ascii") as probe_file:
                assert probe_file.read().strip() == "unset|unset"
            new_children = direct_children(client.proc.pid) - children_before
            assert len(new_children) == 1, new_children
            session_process_group = new_children.pop()
            session_members = process_group_members(session_process_group)
            assert_no_uinput_fd(session_members)
            xmessage_processes = {
                pid for pid in session_members if process_name(pid) == "xmessage"
            }
            assert xmessage_processes, session_members
            assert all(
                not signal_is_ignored(pid, signal.SIGPIPE)
                for pid in xmessage_processes
            ), "launched app inherited ignored SIGPIPE"

            children_before_second = direct_children(client.proc.pid)
            second = client.tool(
                "launch_isolated_app",
                {
                    "command": "xmessage",
                    "args": ["-title", SECOND_WINDOW_TITLE, "second session"],
                    "waitForWindow": SECOND_WINDOW_TITLE,
                    "timeout": 3,
                    "screenSize": "800x600",
                },
            )
            second_session_id = second["sessionId"]
            open_sessions.add(second_session_id)
            assert second_session_id != session_id
            new_children = direct_children(client.proc.pid) - children_before_second
            assert len(new_children) == 1, new_children
            second_process_group = new_children.pop()
            assert_no_uinput_fd(process_group_members(second_process_group))

            desktop_lookup = client.tool("find_window", {"name": WINDOW_TITLE})
            assert "No window found" in text(desktop_lookup), text(desktop_lookup)

            session_args = {"sessionId": session_id}
            isolated_lookup = client.tool(
                "find_window", {"name": WINDOW_TITLE, **session_args}
            )
            assert WINDOW_TITLE in text(isolated_lookup), text(isolated_lookup)

            companion = client.tool(
                "launch_app",
                {
                    "command": "xmessage",
                    "args": ["-title", COMPANION_WINDOW_TITLE, "companion"],
                    "waitForWindow": COMPANION_WINDOW_TITLE,
                    "timeout": 3,
                    **session_args,
                },
            )
            assert COMPANION_WINDOW_TITLE in text(companion), text(companion)
            desktop_companion = client.tool(
                "find_window", {"name": COMPANION_WINDOW_TITLE}
            )
            assert "No window found" in text(desktop_companion), text(desktop_companion)

            long_wait = client.tool(
                "wait_for_window",
                {
                    "name": "deskpal-window-that-will-not-appear",
                    "timeout": 16,
                    **session_args,
                },
            )
            assert "after 16s" in text(long_wait), text(long_wait)
            assert long_wait.get("isError") is not True, long_wait
            session_after_wait = client.tool(
                "find_window", {"name": WINDOW_TITLE, **session_args}
            )
            assert WINDOW_TITLE in text(session_after_wait), text(session_after_wait)

            typed = client.tool(
                "type_text",
                {
                    "windowName": WINDOW_TITLE,
                    "text": "test",
                    "delay": 5500,
                    **session_args,
                },
            )
            assert "Typed 4 characters" in text(typed), text(typed)
            session_after_typing = client.tool(
                "find_window", {"name": WINDOW_TITLE, **session_args}
            )
            assert WINDOW_TITLE in text(session_after_typing), text(session_after_typing)

            match = re.search(r"Size: (\d+)x(\d+)", text(launched))
            assert match, text(launched)
            window_height = int(match.group(2))

            window_image = client.tool(
                "screenshot", {"windowName": WINDOW_TITLE, **session_args}
            )
            assert png_size(window_image)[0] > 0

            root_image = client.tool(
                "screenshot", {"fullScreen": True, **session_args}
            )
            assert png_size(root_image) == (1024, 768)

            client.tool(
                "click_text",
                {
                    "windowName": WINDOW_TITLE,
                    "text": "not-present-anywhere",
                    **session_args,
                },
            )
            assert not os.path.exists(sentinel), (
                "isolated OCR invoked the host screenshot fallback"
            )

            clicked = client.tool(
                "click",
                {
                    "windowName": WINDOW_TITLE,
                    "x": 20,
                    "y": window_height - 12,
                    **session_args,
                },
            )
            assert "Clicked" in text(clicked), text(clicked)
            time.sleep(0.5)
            state = client.tool(
                "find_window", {"name": WINDOW_TITLE, **session_args}
            )
            assert "No window found" in text(state), text(state)

            closed = client.tool(
                "close_isolated_session", {"sessionId": session_id}
            )
            assert "Closed isolated session" in text(closed), text(closed)
            open_sessions.remove(session_id)
            wait_for_process_group_exit(session_process_group)

            second_state = client.tool(
                "find_window",
                {"name": SECOND_WINDOW_TITLE, "sessionId": second_session_id},
            )
            assert SECOND_WINDOW_TITLE in text(second_state), text(second_state)

            stale = client.tool(
                "find_window", {"name": WINDOW_TITLE, **session_args}
            )
            assert "Unknown or closed isolated session" in text(stale), text(stale)
            assert stale.get("isError") is True, stale

            second_closed = client.tool(
                "close_isolated_session", {"sessionId": second_session_id}
            )
            assert "Closed isolated session" in text(second_closed), text(second_closed)
            open_sessions.remove(second_session_id)
            wait_for_process_group_exit(second_process_group)

            children_before_crash = direct_children(client.proc.pid)
            crashing = client.tool(
                "launch_isolated_app",
                {
                    "command": "xmessage",
                    "args": ["-title", "deskpal-crash-test", "crash test"],
                    "waitForWindow": "deskpal-crash-test",
                    "timeout": 3,
                },
            )
            crashing_session_id = crashing["sessionId"]
            open_sessions.add(crashing_session_id)
            new_children = direct_children(client.proc.pid) - children_before_crash
            assert len(new_children) == 1, new_children
            crashing_process_group = new_children.pop()
            os.kill(crashing_process_group, signal.SIGKILL)
            time.sleep(0.1)
            failed = client.tool(
                "list_windows", {"sessionId": crashing_session_id}
            )
            assert failed.get("isError") is True, failed
            assert "exited unexpectedly" in text(failed), text(failed)
            open_sessions.remove(crashing_session_id)
            wait_for_process_group_exit(crashing_process_group)

            replacement = client.tool(
                "launch_isolated_app",
                {
                    "command": "xmessage",
                    "args": ["-title", "deskpal-reuse-test", "reuse test"],
                    "waitForWindow": "deskpal-reuse-test",
                    "timeout": 3,
                },
            )
            replacement_session_id = replacement["sessionId"]
            open_sessions.add(replacement_session_id)
            replacement_closed = client.tool(
                "close_isolated_session", {"sessionId": replacement_session_id}
            )
            assert "Closed isolated session" in text(replacement_closed)
            open_sessions.remove(replacement_session_id)
        finally:
            for session_id in open_sessions:
                try:
                    client.tool(
                        "close_isolated_session", {"sessionId": session_id}
                    )
                except (BrokenPipeError, KeyError, json.JSONDecodeError):
                    pass
            stderr = client.close()

        assert "headless display, using XTest input (1024x768)" in stderr, stderr

        termination_client = DeskpalClient(env)
        children_before = direct_children(termination_client.proc.pid)
        termination_launch = termination_client.tool(
            "launch_isolated_app",
            {
                "command": "xmessage",
                "args": ["-title", "deskpal-parent-exit-test", "parent exit"],
                "waitForWindow": "deskpal-parent-exit-test",
                "timeout": 3,
            },
        )
        assert termination_launch.get("sessionId")
        new_children = direct_children(termination_client.proc.pid) - children_before
        assert len(new_children) == 1, new_children
        termination_process_group = new_children.pop()
        termination_client.proc.terminate()
        termination_client.proc.wait(timeout=5)
        wait_for_process_group_exit(termination_process_group)
        termination_client.proc.stdin.close()
        termination_client.proc.stdout.close()
        termination_client.proc.stderr.close()

        broken_output_client = DeskpalClient(env)
        children_before = direct_children(broken_output_client.proc.pid)
        broken_output_launch = broken_output_client.tool(
            "launch_isolated_app",
            {
                "command": "xmessage",
                "args": ["-title", "deskpal-broken-output-test", "broken output"],
                "waitForWindow": "deskpal-broken-output-test",
                "timeout": 3,
            },
        )
        assert broken_output_launch.get("sessionId")
        new_children = direct_children(broken_output_client.proc.pid) - children_before
        assert len(new_children) == 1, new_children
        broken_output_process_group = new_children.pop()
        broken_output_client.proc.stdout.close()
        broken_output_client.request_id += 1
        broken_output_client.proc.stdin.write(
            json.dumps(
                {
                    "jsonrpc": "2.0",
                    "id": broken_output_client.request_id,
                    "method": "tools/list",
                    "params": {},
                }
            )
            + "\n"
        )
        broken_output_client.proc.stdin.flush()
        broken_output_client.proc.wait(timeout=5)
        wait_for_process_group_exit(broken_output_process_group)
        broken_output_client.proc.stdin.close()
        broken_output_client.proc.stderr.close()

    print("PASS: desktop default and session-scoped Xvfb verification stay separate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())