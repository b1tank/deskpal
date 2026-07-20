#!/usr/bin/env python3
"""Deterministic coverage for deskpal's optional AT-SPI read tools."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from deskpal_client import (
    DESKPAL,
    DeskpalClient,
    start_xvfb,
    stop_process,
    text,
    tool_by_name,
)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "test", "fixtures", "accessibility_app.py")
TITLE = "Deskpal Accessibility Fixture"
DEEP_TITLE = "Deskpal Accessibility Deep Fixture"
BOUNDARY_TITLE = "Deskpal Accessibility Boundary Fixture"
AMBIGUOUS_TITLE = "Deskpal Accessibility Ambiguous Fixture"
STALLED_TITLE = "Deskpal Accessibility Stalled Fixture"
UNKNOWN_TITLE = "Deskpal Accessibility Unknown Fixture"


def require_dependencies():
    missing = [
        command
        for command in ("Xvfb", "xauth")
        if not shutil.which(command)
    ]
    if missing:
        raise SystemExit(f"FAIL: missing dependencies: {', '.join(missing)}")
    if not os.path.isfile(DESKPAL):
        raise SystemExit("FAIL: build/deskpal does not exist; run npm run build")


def start_accessibility_bus(env):
    process = subprocess.Popen(
        ["/usr/libexec/at-spi-bus-launcher", "--launch-immediately"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    deadline = time.time() + 5
    while time.time() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"AT-SPI bus exited: {process.stderr.read()}")
        check = subprocess.run(
            [
                "gdbus", "call", "--session", "--dest", "org.a11y.Bus",
                "--object-path", "/org/a11y/bus", "--method", "org.a11y.Bus.GetAddress",
            ],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if check.returncode == 0:
            return process
        time.sleep(0.05)
    stop_process(process)
    raise AssertionError("AT-SPI bus did not become ready")


def accessibility_bus_address(env):
    result = subprocess.run(
        [
            "gdbus", "call", "--session", "--dest", "org.a11y.Bus",
            "--object-path", "/org/a11y/bus", "--method", "org.a11y.Bus.GetAddress",
        ],
        env=env,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.split("'", 2)[1]


def find_nodes(tree):
    return [
        node
        for application in tree["applications"]
        for window in application["windows"]
        for node in window["nodes"]
    ]


def accessibility_payload(result):
    return json.loads(text(result))


def run_rich_suite(env):
    env = env.copy()
    env["DBUS_SESSION_BUS_ADDRESS"] = os.environ["DBUS_SESSION_BUS_ADDRESS"]
    bus = None
    fixture = None
    auxiliary_fixtures = []
    client = None
    try:
        bus = start_accessibility_bus(env)
        fixture_env = env.copy()
        fixture_env.pop("NO_AT_BRIDGE", None)
        fixture_env["GTK_MODULES"] = "gail:atk-bridge"
        fixture = subprocess.Popen(
            ["/usr/bin/python3", FIXTURE],
            env=fixture_env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.time() + 5
        while time.time() < deadline:
            focused = subprocess.run(
                [
                    "xdotool", "search", "--onlyvisible", "--name",
                    f"^{TITLE}$", "windowfocus", "%@",
                ],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if focused.returncode == 0:
                break
            if fixture.poll() is not None:
                raise AssertionError(f"fixture exited: {fixture.stderr.read()}")
            time.sleep(0.05)
        else:
            raise AssertionError("accessibility fixture window did not appear")
        client = DeskpalClient(
            env, args=["--no-uinput", "--allow-exec"], name="e2e-accessibility-rich"
        )
        deadline = time.time() + 5
        tree_result = None
        while time.time() < deadline:
            tree_result = client.tool(
                "get_accessibility_tree",
                {
                    "application": "accessibility_app",
                    "window": TITLE,
                    "maxNodes": 100,
                    "includeAttributes": True,
                },
            )
            tree = accessibility_payload(tree_result)
            if tree.get("nodeCount", 0) >= 5:
                break
            if fixture.poll() is not None:
                raise AssertionError(f"fixture exited: {fixture.stderr.read()}")
            time.sleep(0.05)
        assert tree_result is not None
        tree = accessibility_payload(tree_result)
        assert tree["available"] is True, tree
        assert tree["capability"] == "semantic", tree
        assert tree["untrustedContent"] is True, tree
        assert "application-controlled" in tree["contentWarning"], tree
        assert tree["matchedApplicationCount"] == 1, tree
        assert tree["matchedWindowCount"] == 1, tree
        assert tree["nodeCount"] >= 5, tree
        assert tree["truncated"] is False, tree
        assert tree["completed"] is True, tree
        assert tree["partial"] is False, tree
        assert tree["queryFailed"] is False, tree
        nodes = find_nodes(tree)

        exact_limit = client.tool(
            "get_accessibility_tree",
            {
                "application": "accessibility_app",
                "window": TITLE,
                "maxNodes": tree["nodeCount"],
            },
        )
        exact_limit = accessibility_payload(exact_limit)
        assert exact_limit["nodeCount"] == tree["nodeCount"], exact_limit
        assert exact_limit["truncated"] is False, exact_limit
        assert exact_limit["completed"] is True, exact_limit

        entry = next(node for node in nodes if node["name"] == "Validation message")
        assert entry["role"] == "text", entry
        assert entry["untrustedContent"] is True, entry
        assert entry["states"]["focused"] is True, entry
        assert entry["states"]["editable"] is True, entry
        assert entry["attributes"]["placeholder-text"] == "Enter a semantic value"
        assert entry["bounds"]["width"] > 0 and entry["bounds"]["height"] > 0
        assert entry["locator"]["application"] == "accessibility_app.py"
        assert entry["locator"]["window"] == TITLE
        assert isinstance(entry["path"], list)
        assert entry["hasText"] is True, entry
        assert "text" not in entry, entry

        password = next(node for node in nodes if node["name"] == "Protected text")
        assert password["role"] == "password text", password
        assert password["textProtected"] is True, password
        assert "text" not in password, password
        assert password["locator"]["name"] == "", password

        button = next(
            node for node in nodes if node["name"] == "Apply validation message"
        )
        assert button["role"] == "push button", button
        assert "click" in button["actions"], button

        focused_result = client.tool(
            "get_focused_element",
            {"application": "accessibility_app", "window": TITLE},
        )
        focused = accessibility_payload(focused_result)
        assert focused["matchCount"] == 1, focused
        assert focused["ambiguous"] is False, focused
        assert focused["matchCountExact"] is True, focused
        assert focused["completed"] is True, focused
        assert focused["element"]["name"] == "Validation message", focused
        assert focused["element"]["states"]["focused"] is True, focused
        assert "text" not in focused["element"], focused

        focused_with_text = client.tool(
            "get_focused_element",
            {
                "application": "accessibility_app",
                "window": TITLE,
                "includeText": True,
            },
        )
        focused_with_text = accessibility_payload(focused_with_text)
        assert focused_with_text["element"]["text"] == "", focused_with_text

        def start_fixture_mode(mode, title):
            mode_env = fixture_env.copy()
            mode_env["DESKPAL_A11Y_FIXTURE_MODE"] = mode
            process = subprocess.Popen(
                ["/usr/bin/python3", FIXTURE],
                env=mode_env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            auxiliary_fixtures.append(process)
            deadline = time.time() + 5
            while time.time() < deadline:
                focused_window = subprocess.run(
                    [
                        "xdotool", "search", "--onlyvisible", "--name",
                        f"^{title}$", "windowfocus", "%@",
                    ],
                    env=env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                if focused_window.returncode == 0:
                    registration = accessibility_payload(
                        client.tool(
                            "get_accessibility_tree",
                            {
                                "application": "accessibility_app",
                                "window": title,
                                "maxDepth": 1,
                                "maxNodes": 2,
                            },
                        )
                    )
                    if registration["matchedWindowCount"] == 1:
                        time.sleep(0.25)
                        return process
                if process.poll() is not None:
                    raise AssertionError(
                        f"{mode} fixture exited: {process.stderr.read()}"
                    )
                time.sleep(0.05)
            raise AssertionError(f"{mode} fixture window did not appear")

        start_fixture_mode("boundary", BOUNDARY_TITLE)
        boundary_focus = accessibility_payload(
            client.tool(
                "get_focused_element",
                {"application": "accessibility_app", "window": BOUNDARY_TITLE},
            )
        )
        assert boundary_focus["matchCount"] == 1, boundary_focus
        assert boundary_focus["completed"] is True, boundary_focus
        assert boundary_focus["matchCountExact"] is True, boundary_focus
        assert boundary_focus["element"]["name"] == "Deep focus target", boundary_focus

        start_fixture_mode("deep", DEEP_TITLE)
        deep_focus = accessibility_payload(
            client.tool(
                "get_focused_element",
                {"application": "accessibility_app", "window": DEEP_TITLE},
            )
        )
        assert deep_focus["incomplete"] is True, deep_focus
        assert deep_focus["completed"] is False, deep_focus
        assert deep_focus["matchCountExact"] is False, deep_focus
        assert "element" not in deep_focus, deep_focus

        for _ in range(2):
            ambiguous_env = fixture_env.copy()
            ambiguous_env["DESKPAL_A11Y_FIXTURE_MODE"] = "ambiguous"
            auxiliary_fixtures.append(
                subprocess.Popen(
                    [
                        "xvfb-run", "--auto-servernum", "--server-args",
                        "-screen 0 800x600x24", "/usr/bin/python3", FIXTURE,
                    ],
                    env=ambiguous_env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            )
        deadline = time.time() + 5
        ambiguous_focus = None
        while time.time() < deadline:
            ambiguous_focus = accessibility_payload(
                client.tool(
                    "get_focused_element",
                    {
                        "application": "accessibility_app",
                        "window": AMBIGUOUS_TITLE,
                    },
                )
            )
            if ambiguous_focus["matchCount"] >= 2:
                break
            time.sleep(0.05)
        assert ambiguous_focus is not None
        assert ambiguous_focus["matchCount"] >= 2, ambiguous_focus
        assert ambiguous_focus["ambiguous"] is True, ambiguous_focus
        assert ambiguous_focus["completed"] is False, ambiguous_focus
        assert ambiguous_focus["matchCountExact"] is False, ambiguous_focus
        assert ambiguous_focus["capability"] == "ambiguous", ambiguous_focus
        assert "element" not in ambiguous_focus, ambiguous_focus

        tree_with_text = client.tool(
            "get_accessibility_tree",
            {
                "application": "accessibility_app",
                "window": TITLE,
                "maxNodes": 100,
                "includeText": True,
            },
        )
        tree_with_text = accessibility_payload(tree_with_text)
        text_nodes = find_nodes(tree_with_text)
        text_entry = next(node for node in text_nodes if node["name"] == "Validation message")
        assert text_entry["text"] == "", text_entry
        protected = next(node for node in text_nodes if node["name"] == "Protected text")
        assert "text" not in protected, protected
        assert "semantic-secret" not in json.dumps(tree_with_text), tree_with_text

        start_fixture_mode("unknown", UNKNOWN_TITLE)
        unknown_tree = accessibility_payload(
            client.tool(
                "get_accessibility_tree",
                {
                    "application": "accessibility_app",
                    "window": UNKNOWN_TITLE,
                    "includeText": True,
                    "includeAttributes": True,
                    "maxNodes": 100,
                },
            )
        )
        unknown_json = json.dumps(unknown_tree)
        assert "unknown-role-secret" not in unknown_json, unknown_tree
        assert "Unknown role secret name" not in unknown_json, unknown_tree
        protected_unknown = [
            node for node in find_nodes(unknown_tree)
            if node["name"] == "Protected text"
        ]
        assert protected_unknown, unknown_tree
        assert all(node["textProtected"] for node in protected_unknown)

        default_attributes = client.tool(
            "get_accessibility_tree",
            {"application": "accessibility_app", "window": TITLE, "maxNodes": 100},
        )
        default_attributes = accessibility_payload(default_attributes)
        default_entry = next(
            node for node in find_nodes(default_attributes)
            if node["name"] == "Validation message"
        )
        assert default_entry["attributes"] == {}, default_entry

        unbounded_focus = client.tool("get_focused_element")
        assert unbounded_focus.get("isError") is True, unbounded_focus
        assert "application or window" in text(unbounded_focus), unbounded_focus

        missing = client.tool(
            "get_accessibility_tree",
            {"application": "does-not-exist", "maxNodes": 20},
        )
        missing = accessibility_payload(missing)
        assert missing["matchedApplicationCount"] == 0, missing
        assert missing["nodeCount"] == 0, missing

        truncated = client.tool(
            "get_accessibility_tree",
            {"application": "accessibility_app", "window": TITLE, "maxNodes": 2},
        )
        truncated = accessibility_payload(truncated)
        assert truncated["nodeCount"] == 2, truncated
        assert truncated["truncated"] is True, truncated
        assert truncated["completed"] is False, truncated
        assert truncated["partial"] is True, truncated
        assert "visitedNodeCount" in truncated, truncated
        assert "incomplete" in truncated, truncated

        invalid = client.tool("get_accessibility_tree", {"maxDepth": 0})
        assert invalid.get("isError") is True, invalid
        assert "maxDepth" in text(invalid), invalid
        malformed = client.tool(
            "get_accessibility_tree", {"application": 123, "maxNodes": 10}
        )
        assert malformed.get("isError") is True, malformed
        malformed_boolean = client.tool(
            "get_accessibility_tree",
            {"application": "accessibility_app", "includeText": "yes"},
        )
        assert malformed_boolean.get("isError") is True, malformed_boolean
        oversized_filter = client.tool(
            "get_accessibility_tree", {"application": "x" * 513}
        )
        assert oversized_filter.get("isError") is True, oversized_filter
        unscoped = client.tool("get_accessibility_tree", {"maxNodes": 10})
        assert unscoped.get("isError") is True, unscoped
        isolated = client.tool(
            "launch_isolated_app",
            {
                "command": "xmessage",
                "args": ["-title", "accessibility-routing", "private"],
                "waitForWindow": "accessibility-routing",
                "timeout": 3,
            },
        )
        isolated_id = isolated["sessionId"]
        routed = client.tool(
            "get_accessibility_tree",
            {"application": "accessibility_app", "sessionId": isolated_id},
        )
        assert routed.get("isError") is True, routed
        assert "visible desktop only" in text(routed), routed
        client.tool("close_isolated_session", {"sessionId": isolated_id})

        direct_env = env.copy()
        direct_env["AT_SPI_BUS_ADDRESS"] = accessibility_bus_address(env)
        direct_env.pop("DBUS_SESSION_BUS_ADDRESS", None)
        direct_client = DeskpalClient(
            direct_env, args=["--no-uinput"], name="e2e-accessibility-direct-bus"
        )
        try:
            direct_status = accessibility_payload(
                direct_client.tool("accessibility_status")
            )
            assert direct_status["available"] is True, direct_status
            direct_tree = accessibility_payload(
                direct_client.tool(
                    "get_accessibility_tree",
                    {"application": "accessibility_app", "window": TITLE},
                )
            )
            assert direct_tree["nodeCount"] >= 5, direct_tree
        finally:
            direct_client.close()

        stalled_fixture = start_fixture_mode("stalled", STALLED_TITLE)
        stalled_result = {}
        stalled_error = {}

        def query_slow_peer():
            try:
                stalled_result["value"] = client.tool(
                    "get_accessibility_tree",
                    {
                        "application": "accessibility_app",
                        "window": STALLED_TITLE,
                        "includeText": True,
                        "includeAttributes": True,
                        "maxNodes": 100,
                    },
                )
            except BaseException as error:
                stalled_error["value"] = error

        started = time.monotonic()
        worker = threading.Thread(target=query_slow_peer, daemon=True)
        worker.start()
        worker.join(timeout=3.5)
        elapsed = time.monotonic() - started
        if worker.is_alive():
            client.proc.kill()
            client.proc.wait(timeout=2)
            client = None
            raise AssertionError("AT-SPI query exceeded external 3.5s watchdog")
        if stalled_error:
            raise stalled_error["value"]
        stalled = stalled_result["value"]
        assert elapsed < 3.5, elapsed
        assert stalled.get("isError") is not True, stalled
        stalled_payload = accessibility_payload(stalled)
        assert stalled_payload["incomplete"] is True, stalled_payload
        assert stalled_payload["completed"] is False, stalled_payload
        assert stalled_payload["queryErrorCount"] > 0, stalled_payload
    finally:
        if client is not None:
            client.close()
        if fixture is not None:
            stop_process(fixture)
        for auxiliary in auxiliary_fixtures:
            stop_process(auxiliary)
        if bus is not None:
            stop_process(bus)


def main():
    require_dependencies()
    with tempfile.TemporaryDirectory(prefix="deskpal-accessibility-") as temp_dir:
        xvfb, base_env = start_xvfb(temp_dir)
        try:
            unavailable_env = base_env.copy()
            unavailable_env.pop("DBUS_SESSION_BUS_ADDRESS", None)
            unavailable = DeskpalClient(
                unavailable_env, args=["--no-uinput"], name="e2e-accessibility-unavailable"
            )
            try:
                tools = unavailable.tools()
                for name in (
                    "accessibility_status",
                    "get_accessibility_tree",
                    "get_focused_element",
                ):
                    tool = tool_by_name(tools, name)
                    assert "sessionId" not in tool["inputSchema"]["properties"], tool
                tree_schema = tool_by_name(
                    tools, "get_accessibility_tree"
                )["inputSchema"]
                tree_properties = tree_schema["properties"]
                assert tree_properties["maxDepth"]["type"] == "integer"
                assert tree_properties["maxNodes"]["type"] == "integer"
                assert tree_properties["application"]["minLength"] == 1
                assert tree_properties["application"]["maxLength"] == 512
                assert tree_properties["window"]["minLength"] == 1
                assert tree_properties["window"]["maxLength"] == 512
                assert tree_properties["includeText"]["default"] is False
                assert tree_properties["includeAttributes"]["default"] is False
                focused_schema = tool_by_name(
                    tools, "get_focused_element"
                )["inputSchema"]
                assert focused_schema["properties"]["application"]["maxLength"] == 512
                assert focused_schema["properties"]["window"]["maxLength"] == 512
                status = unavailable.tool("accessibility_status")
                status_payload = accessibility_payload(status)
                compiled = status_payload["compiled"]
                assert status_payload["available"] is False, status
                assert status_payload["capability"] == "unavailable", status
                unavailable_tree = accessibility_payload(
                    unavailable.tool(
                        "get_accessibility_tree", {"application": "anything"}
                    )
                )
                assert unavailable_tree["available"] is False, unavailable_tree
                assert unavailable_tree["nodeCount"] == 0, unavailable_tree
                unavailable_focus = accessibility_payload(
                    unavailable.tool(
                        "get_focused_element", {"application": "anything"}
                    )
                )
                assert unavailable_focus["available"] is False, unavailable_focus
                assert unavailable_focus["matchCount"] == 0, unavailable_focus
            finally:
                unavailable.close()

            if not compiled:
                print("PASS: no-AT-SPI fallback contracts report backend unavailable")
                return

            optional_missing = []
            if not shutil.which("dbus-run-session"):
                optional_missing.append("dbus-run-session")
            if not os.path.exists("/usr/libexec/at-spi-bus-launcher"):
                optional_missing.append("at-spi-bus-launcher")
            try:
                subprocess.run(
                    ["/usr/bin/python3", "-c", "import gi; gi.require_version('Gtk','3.0')"],
                    check=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            except subprocess.CalledProcessError:
                optional_missing.append("python3-gi/GTK3")
            if optional_missing:
                raise SystemExit(
                    "BLOCKED: accessibility backend compiled but rich fixture "
                    f"dependencies are missing: {', '.join(optional_missing)}"
                )
                return

            command = [
                "dbus-run-session", "--", sys.executable, "-c",
                (
                    "import json, os, sys; "
                    "sys.path.insert(0, os.path.join(os.getcwd(), 'test')); "
                    "from e2e_accessibility import run_rich_suite; "
                    "run_rich_suite(json.loads(os.environ['DESKPAL_A11Y_ENV']))"
                ),
            ]
            rich_env = base_env.copy()
            rich_env.pop("DBUS_SESSION_BUS_ADDRESS", None)
            rich_env.pop("NO_AT_BRIDGE", None)
            rich_env["DESKPAL_A11Y_ENV"] = json.dumps(rich_env)
            subprocess.run(command, env=rich_env, cwd=ROOT, check=True)
        finally:
            stop_process(xvfb)
    print("PASS: optional AT-SPI status, bounded tree, and focused element")


if __name__ == "__main__":
    main()
