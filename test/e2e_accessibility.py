#!/usr/bin/env python3
"""Deterministic coverage for deskpal's optional AT-SPI inspection and action tools."""

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
MUTATION_TITLE = "Deskpal Accessibility Mutation Fixture"
INTERLEAVE_TITLE = "Deskpal Accessibility Interleave Fixture"
DEFUNCT_TITLE = "Deskpal Accessibility Defunct Fixture"


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
    auxiliary_xvfbs = []
    auxiliary_directories = []
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
                    "application": "accessibility_app.py",
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
                "application": "accessibility_app.py",
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
        assert "name" not in password["locator"], password

        button = next(
            node for node in nodes if node["name"] == "Apply validation message"
        )
        assert button["role"] == "push button", button
        assert "click" in button["actions"], button
        checkbox_node = next(
            node for node in nodes if node["name"] == "Approval checkbox"
        )

        focused_result = client.tool(
            "get_focused_element",
            {"application": "accessibility_app.py", "window": TITLE},
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
                "application": "accessibility_app.py",
                "window": TITLE,
                "includeText": True,
            },
        )
        focused_with_text = accessibility_payload(focused_with_text)
        assert focused_with_text["element"]["text"] == "", focused_with_text

        def visible_text(name):
            snapshot = accessibility_payload(
                client.tool(
                    "get_accessibility_tree",
                    {
                        "application": "accessibility_app.py",
                        "window": TITLE,
                        "includeText": True,
                        "maxNodes": 100,
                    },
                )
            )
            return next(
                node["text"] for node in find_nodes(snapshot)
                if node["name"] == name
            )

        set_text_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {"role": "text", "name": "Validation message"},
                "operation": "setText",
                "value": "semantic action value",
            },
        )
        assert set_text_result.get("isError") is not True, set_text_result
        set_text = accessibility_payload(set_text_result)
        assert set_text["success"] is True, set_text
        assert set_text["actionApplied"] is True, set_text
        assert set_text["verified"] is True, set_text
        assert set_text["verification"]["actualTextObserved"] is True

        idempotent_set_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {"role": "text", "name": "Validation message"},
                "operation": "setText",
                "value": "semantic action value",
            },
        )
        assert idempotent_set_result.get("isError") is not True
        idempotent_set = accessibility_payload(idempotent_set_result)
        assert idempotent_set["success"] is True, idempotent_set
        assert idempotent_set["actionApplied"] is False, idempotent_set
        assert idempotent_set["verified"] is True, idempotent_set
        assert idempotent_set["verification"]["alreadySatisfied"] is True

        clear_text_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {"role": "text", "name": "Validation message"},
                "operation": "setText",
                "value": "",
            },
        )
        assert clear_text_result.get("isError") is not True, clear_text_result
        assert accessibility_payload(clear_text_result)["success"] is True
        restore_text_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {"role": "text", "name": "Validation message"},
                "operation": "setText",
                "value": "semantic action value",
            },
        )
        assert restore_text_result.get("isError") is not True, restore_text_result

        focus_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": checkbox_node["locator"],
                "operation": "focus",
            },
        )
        assert focus_result.get("isError") is not True, focus_result
        focus_action = accessibility_payload(focus_result)
        assert focus_action["success"] is True, focus_action
        assert focus_action["verification"]["actualStateValue"] is True

        invoke_button_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Apply validation message",
                },
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "label",
                    "name": "Fixture status",
                    "textEquals": "Status: semantic action value",
                },
            },
        )
        assert invoke_button_result.get("isError") is not True, invoke_button_result
        invoke_button = accessibility_payload(invoke_button_result)
        assert invoke_button["success"] is True, invoke_button
        assert invoke_button["actionMatchCount"] == 1, invoke_button
        assert invoke_button["verification"]["satisfied"] is True
        assert visible_text("Apply count") == "Apply count: 1"

        idempotent_invoke_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Apply validation message",
                },
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "label",
                    "name": "Fixture status",
                    "textEquals": "Status: semantic action value",
                },
            },
        )
        assert idempotent_invoke_result.get("isError") is not True
        idempotent_invoke = accessibility_payload(idempotent_invoke_result)
        assert idempotent_invoke["success"] is True, idempotent_invoke
        assert idempotent_invoke["actionApplied"] is False, idempotent_invoke
        assert idempotent_invoke["verified"] is True, idempotent_invoke
        assert idempotent_invoke["verification"]["alreadySatisfied"] is True
        assert visible_text("Apply count") == "Apply count: 1"

        invoke_checkbox_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {"role": "check box", "name": "Approval checkbox"},
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "check box",
                    "name": "Approval checkbox",
                    "state": "checked",
                    "stateValue": True,
                },
            },
        )
        assert invoke_checkbox_result.get("isError") is not True, invoke_checkbox_result
        invoke_checkbox = accessibility_payload(invoke_checkbox_result)
        assert invoke_checkbox["success"] is True, invoke_checkbox
        assert invoke_checkbox["verification"]["actualStateValue"] is True

        failed_verification_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Apply validation message",
                },
                "operation": "invoke",
                "action": "click",
                "timeoutMs": 750,
                "verify": {
                    "role": "label",
                    "name": "Fixture status",
                    "textEquals": "Status: value that will not appear",
                },
            },
        )
        assert failed_verification_result.get("isError") is True
        failed_verification = accessibility_payload(failed_verification_result)
        assert failed_verification["actionApplied"] is True, failed_verification
        assert failed_verification["verified"] is False, failed_verification
        assert failed_verification["errorCode"] == "verification_failed", failed_verification
        assert visible_text("Apply count") == "Apply count: 2"

        truncated_verification_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Apply validation message",
                },
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "label",
                    "name": "Long verification",
                    "textEquals": "x" * 2048,
                },
            },
        )
        assert truncated_verification_result.get("isError") is True
        truncated_verification = accessibility_payload(
            truncated_verification_result
        )
        assert truncated_verification["errorCode"] == "verification_unreadable"
        assert truncated_verification["actionApplied"] is False
        assert visible_text("Apply count") == "Apply count: 2"

        ambiguous_target_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {"role": "push button", "name": "Duplicate action"},
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "label",
                    "name": "Fixture status",
                    "textEquals": "never",
                },
            },
        )
        assert ambiguous_target_result.get("isError") is True
        ambiguous_target = accessibility_payload(ambiguous_target_result)
        assert ambiguous_target["errorCode"] == "target_ambiguous", ambiguous_target
        assert ambiguous_target["actionApplied"] is False, ambiguous_target
        assert visible_text("Apply count") == "Apply count: 2"

        ambiguous_verification_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Apply validation message",
                },
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "push button",
                    "name": "Duplicate action",
                    "state": "enabled",
                    "stateValue": True,
                },
            },
        )
        assert ambiguous_verification_result.get("isError") is True
        ambiguous_verification = accessibility_payload(
            ambiguous_verification_result
        )
        assert ambiguous_verification["errorCode"] == "verification_unresolved"
        assert ambiguous_verification["actionApplied"] is False
        assert visible_text("Apply count") == "Apply count: 2"

        stale_path_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "path": [999],
                    "busName": button["locator"]["busName"],
                    "objectPath": button["locator"]["objectPath"],
                    "processId": button["locator"]["processId"],
                },
                "operation": "focus",
            },
        )
        assert stale_path_result.get("isError") is True
        stale_path = accessibility_payload(stale_path_result)
        assert stale_path["errorCode"] == "target_not_found", stale_path

        protected_target_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": password["locator"],
                "operation": "setText",
                "value": "must-not-write",
            },
        )
        assert protected_target_result.get("isError") is True
        protected_target = accessibility_payload(protected_target_result)
        assert protected_target["errorCode"] == "protected_target", protected_target

        missing_verification = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Apply validation message",
                },
                "operation": "invoke",
                "action": "click",
            },
        )
        assert missing_verification.get("isError") is True, missing_verification

        orphan_state_value = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Apply validation message",
                },
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "label",
                    "name": "Fixture status",
                    "stateValue": True,
                    "textEquals": "unused",
                },
            },
        )
        assert orphan_state_value.get("isError") is True, orphan_state_value

        invalid_action_path = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "target": {"role": "text", "path": [-1]},
                "operation": "focus",
            },
        )
        assert invalid_action_path.get("isError") is True, invalid_action_path

        substring_scope = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app",
                "window": TITLE,
                "target": {"role": "text", "name": "Validation message"},
                "operation": "focus",
            },
        )
        assert substring_scope.get("isError") is True, substring_scope
        substring_scope_payload = accessibility_payload(substring_scope)
        assert substring_scope_payload["errorCode"] == "target_not_found"

        unknown_action_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Apply validation message",
                },
                "operation": "invoke",
                "action": "not-an-action",
                "verify": {
                    "role": "label",
                    "name": "Fixture status",
                    "textEquals": "unchanged",
                },
            },
        )
        assert unknown_action_result.get("isError") is True
        unknown_action = accessibility_payload(unknown_action_result)
        assert unknown_action["errorCode"] == "action_not_found", unknown_action
        assert visible_text("Apply count") == "Apply count: 2"

        original_entry_locator = entry["locator"]
        replace_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": {
                    "role": "push button",
                    "name": "Replace validation field",
                },
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "label",
                    "name": "Entry generation",
                    "textEquals": "Entry generation: 2",
                },
            },
        )
        assert replace_result.get("isError") is not True, replace_result
        assert accessibility_payload(replace_result)["verified"] is True
        replacement_snapshot = accessibility_payload(
            client.tool(
                "get_accessibility_tree",
                {
                    "application": "accessibility_app.py",
                    "window": TITLE,
                    "includeText": True,
                    "maxNodes": 100,
                },
            )
        )
        replacement = next(
            node for node in find_nodes(replacement_snapshot)
            if node["name"] == "Replacement message"
        )
        assert replacement["path"] == entry["path"], replacement
        assert replacement["locator"]["objectPath"] != original_entry_locator["objectPath"]
        stale_reused_path = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": TITLE,
                "target": original_entry_locator,
                "operation": "setText",
                "value": "must-not-reach-replacement",
            },
        )
        assert stale_reused_path.get("isError") is True, stale_reused_path
        stale_reused_payload = accessibility_payload(stale_reused_path)
        assert stale_reused_payload["errorCode"] == "target_not_found"
        replacement_after = accessibility_payload(
            client.tool(
                "get_accessibility_tree",
                {
                    "application": "accessibility_app.py",
                    "window": TITLE,
                    "includeText": True,
                    "maxNodes": 100,
                },
            )
        )
        replacement_node = next(
            node for node in find_nodes(replacement_after)
            if node["name"] == "Replacement message"
        )
        assert replacement_node["text"] == "", replacement_node

        with tempfile.TemporaryDirectory(
            prefix="deskpal-accessibility-contender-"
        ) as contender_dir:
            contender_xvfb, contender_env = start_xvfb(contender_dir, "800x600")
            contender_env["DBUS_SESSION_BUS_ADDRESS"] = env[
                "DBUS_SESSION_BUS_ADDRESS"
            ]
            contender = DeskpalClient(
                contender_env,
                args=["--no-uinput"],
                name="e2e-accessibility-lock-contender",
            )
            try:
                blocked_action = contender.tool(
                    "accessibility_action",
                    {
                        "application": "accessibility_app.py",
                        "window": TITLE,
                        "target": {
                            "role": "text",
                            "name": "Replacement message",
                        },
                        "operation": "focus",
                    },
                )
                assert blocked_action.get("isError") is True, blocked_action
                assert "control" in text(blocked_action).lower(), blocked_action
            finally:
                contender.close()
                stop_process(contender_xvfb)

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
                                "application": "accessibility_app.py",
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
                {"application": "accessibility_app.py", "window": BOUNDARY_TITLE},
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
                {"application": "accessibility_app.py", "window": DEEP_TITLE},
            )
        )
        assert deep_focus["incomplete"] is True, deep_focus
        assert deep_focus["completed"] is False, deep_focus
        assert deep_focus["matchCountExact"] is False, deep_focus
        assert "element" not in deep_focus, deep_focus

        for _ in range(2):
            ambiguous_dir = tempfile.TemporaryDirectory(
                prefix="deskpal-accessibility-ambiguous-"
            )
            auxiliary_directories.append(ambiguous_dir)
            ambiguous_xvfb, ambiguous_env = start_xvfb(
                ambiguous_dir.name, "800x600"
            )
            auxiliary_xvfbs.append(ambiguous_xvfb)
            ambiguous_env["DBUS_SESSION_BUS_ADDRESS"] = env[
                "DBUS_SESSION_BUS_ADDRESS"
            ]
            ambiguous_env.pop("NO_AT_BRIDGE", None)
            ambiguous_env["GTK_MODULES"] = "gail:atk-bridge"
            ambiguous_env["DESKPAL_A11Y_FIXTURE_MODE"] = "ambiguous"
            auxiliary_fixtures.append(
                subprocess.Popen(
                    ["/usr/bin/python3", FIXTURE],
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
                        "application": "accessibility_app.py",
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
                "application": "accessibility_app.py",
                "window": TITLE,
                "maxNodes": 100,
                "includeText": True,
            },
        )
        tree_with_text = accessibility_payload(tree_with_text)
        text_nodes = find_nodes(tree_with_text)
        text_entry = next(
            node for node in text_nodes
            if node["name"] == "Replacement message"
        )
        assert text_entry["text"] == "", text_entry
        protected = next(node for node in text_nodes if node["name"] == "Protected text")
        assert "text" not in protected, protected
        assert "semantic-secret" not in json.dumps(tree_with_text), tree_with_text

        start_fixture_mode("unknown", UNKNOWN_TITLE)
        unknown_tree = accessibility_payload(
            client.tool(
                "get_accessibility_tree",
                {
                    "application": "accessibility_app.py",
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
        unknown_node = protected_unknown[-1]
        unknown_mutation = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": UNKNOWN_TITLE,
                "target": unknown_node["locator"],
                "operation": "setText",
                "value": "must-not-write",
            },
        )
        assert unknown_mutation.get("isError") is True, unknown_mutation
        unknown_mutation_payload = accessibility_payload(unknown_mutation)
        assert unknown_mutation_payload["errorCode"] == "protected_target"

        start_fixture_mode("mutation", MUTATION_TITLE)
        unknown_outcome_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": MUTATION_TITLE,
                "target": {"role": "text", "name": "Slow mutation field"},
                "operation": "setText",
                "value": "completed after timeout",
                "timeoutMs": 3000,
            },
        )
        assert unknown_outcome_result.get("isError") is not True, unknown_outcome_result
        unknown_outcome = accessibility_payload(unknown_outcome_result)
        assert unknown_outcome["mutationIssued"] is True, unknown_outcome
        assert unknown_outcome["actionApplied"] is False, unknown_outcome
        assert unknown_outcome["actionOutcomeUnknown"] is True, unknown_outcome
        assert unknown_outcome["verified"] is True, unknown_outcome
        assert unknown_outcome["success"] is True, unknown_outcome

        start_fixture_mode("interleave", INTERLEAVE_TITLE)
        interleaved_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": INTERLEAVE_TITLE,
                "target": {
                    "role": "push button",
                    "name": "Interleave target",
                },
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "text",
                    "name": "Interleave verification",
                    "textEquals": "original clicked",
                },
            },
        )
        assert interleaved_result.get("isError") is True, interleaved_result
        interleaved = accessibility_payload(interleaved_result)
        assert interleaved["errorCode"] == "target_changed", interleaved
        assert interleaved["actionApplied"] is False, interleaved
        interleave_snapshot = accessibility_payload(
            client.tool(
                "get_accessibility_tree",
                {
                    "application": "accessibility_app.py",
                    "window": INTERLEAVE_TITLE,
                    "includeText": True,
                    "maxNodes": 100,
                },
            )
        )
        interleave_nodes = find_nodes(interleave_snapshot)
        interleave_count = next(
            node for node in interleave_nodes
            if node["name"] == "Interleave count"
        )
        assert interleave_count["text"] == "Interleave count: 0", interleave_count

        start_fixture_mode("defunct", DEFUNCT_TITLE)
        defunct_result = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "window": DEFUNCT_TITLE,
                "target": {"role": "push button", "name": "Defunct target"},
                "operation": "invoke",
                "action": "click",
                "verify": {
                    "role": "text",
                    "name": "Defunct verifier",
                    "state": "editable",
                    "stateValue": False,
                },
            },
        )
        assert defunct_result.get("isError") is True, defunct_result
        defunct_payload = accessibility_payload(defunct_result)
        assert defunct_payload["errorCode"] == "verification_unresolved", defunct_payload
        assert defunct_payload["mutationIssued"] is False
        assert defunct_payload["actionApplied"] is False
        defunct_snapshot = accessibility_payload(
            client.tool(
                "get_accessibility_tree",
                {
                    "application": "accessibility_app.py",
                    "window": DEFUNCT_TITLE,
                    "includeText": True,
                    "maxNodes": 100,
                },
            )
        )
        defunct_count = next(
            node for node in find_nodes(defunct_snapshot)
            if node["name"] == "Defunct count"
        )
        assert defunct_count["text"] == "Defunct count: 0", defunct_count

        default_attributes = client.tool(
            "get_accessibility_tree",
            {"application": "accessibility_app.py", "window": TITLE, "maxNodes": 100},
        )
        default_attributes = accessibility_payload(default_attributes)
        default_entry = next(
            node for node in find_nodes(default_attributes)
            if node["name"] == "Replacement message"
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
            {"application": "accessibility_app.py", "window": TITLE, "maxNodes": 2},
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
            {"application": "accessibility_app.py", "includeText": "yes"},
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
        assert "sessionId" in isolated, (client.proc.pid, isolated)
        isolated_id = isolated["sessionId"]
        routed = client.tool(
            "get_accessibility_tree",
            {"application": "accessibility_app.py", "sessionId": isolated_id},
        )
        assert routed.get("isError") is True, routed
        assert "visible desktop only" in text(routed), routed
        routed_action = client.tool(
            "accessibility_action",
            {
                "application": "accessibility_app.py",
                "target": {"role": "text", "name": "Validation message"},
                "operation": "focus",
                "sessionId": isolated_id,
            },
        )
        assert routed_action.get("isError") is True, routed_action
        assert "visible desktop only" in text(routed_action), routed_action
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
                    {"application": "accessibility_app.py", "window": TITLE},
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
                        "application": "accessibility_app.py",
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
        for auxiliary_xvfb in auxiliary_xvfbs:
            stop_process(auxiliary_xvfb)
        for auxiliary_directory in auxiliary_directories:
            auxiliary_directory.cleanup()
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
                    "accessibility_action",
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
                action_schema = tool_by_name(
                    tools, "accessibility_action"
                )["inputSchema"]
                assert action_schema["properties"]["timeoutMs"]["type"] == "integer"
                assert action_schema["properties"]["target"]["properties"]["path"]["maxItems"] == 32
                assert action_schema["properties"]["target"]["properties"]["busName"]["maxLength"] == 255
                assert action_schema["properties"]["target"]["properties"]["objectPath"]["maxLength"] == 1024
                assert action_schema["properties"]["target"]["properties"]["processId"]["type"] == "integer"
                assert action_schema["properties"]["verify"]["properties"]["stateValue"]["type"] == "boolean"
                target_conditions = action_schema["properties"]["target"]["allOf"]
                assert {
                    "busName", "objectPath", "processId"
                }.issubset(target_conditions[0]["then"]["required"])
                verify_conditions = action_schema["properties"]["verify"]["allOf"]
                assert {
                    "busName", "objectPath", "processId"
                }.issubset(verify_conditions[0]["then"]["required"])
                operation_conditions = action_schema["allOf"]
                assert "value" in operation_conditions[0]["then"]["required"]
                assert {"action", "verify"}.issubset(
                    operation_conditions[1]["then"]["required"]
                )
                for nul_arguments in (
                    {
                        "application": "anything",
                        "target": {"role": "text", "name": "anything"},
                        "operation": "setText",
                        "value": "safe\x00hidden",
                    },
                    {
                        "application": "anything",
                        "target": {"role": "push button", "name": "anything"},
                        "operation": "invoke",
                        "action": "click",
                        "verify": {
                            "role": "label",
                            "name": "status",
                            "textEquals": "saved\x00hidden",
                        },
                    },
                ):
                    nul_response = unavailable.call(
                        "tools/call",
                        {"name": "accessibility_action", "arguments": nul_arguments},
                    )
                    assert nul_response["error"]["code"] == -32700, nul_response
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
                unavailable_action = unavailable.tool(
                    "accessibility_action",
                    {
                        "application": "anything",
                        "target": {"role": "text", "name": "anything"},
                        "operation": "focus",
                    },
                )
                assert unavailable_action.get("isError") is True, unavailable_action
                unavailable_action_payload = accessibility_payload(unavailable_action)
                assert unavailable_action_payload["errorCode"] == "backend_unavailable"
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
    print("PASS: optional AT-SPI inspection and verified semantic actions")


if __name__ == "__main__":
    main()
