#!/usr/bin/env python3
"""Safe static contract checks for the GNOME logical-cursor prototype."""

import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTENSION = ROOT / "gnome-extension" / "indicator@deskpal.local"
JS = EXTENSION / "extension.js"
METADATA = EXTENSION / "metadata.json"
STYLESHEET = EXTENSION / "stylesheet.css"
SCRIPT = ROOT / "scripts" / "indicator.sh"
PI_EXTENSION = ROOT / "extensions" / "deskpal.ts"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    metadata = json.loads(METADATA.read_text())
    source = JS.read_text()
    stylesheet = STYLESHEET.read_text()
    script = SCRIPT.read_text()
    pi_extension = PI_EXTENSION.read_text()

    require(metadata["uuid"] == "indicator@deskpal.local", "unexpected UUID")
    require("42" in metadata["shell-version"], "GNOME 42 must be supported")

    methods = set(re.findall(r'<method name="([A-Za-z0-9]+)"', source))
    require(
        methods == {
            "Ping",
            "GetStatus",
            "ShowCursor",
            "MoveCursor",
            "MoveCursorStyled",
            "HideCursor",
            "ClearAll",
            "ListCursors",
            "GetCapabilities",
            "ListWindows",
            "GetMonitorLayout",
        },
        f"unexpected D-Bus methods: {sorted(methods)}",
    )
    require("org.deskpal.Indicator" in source, "missing D-Bus interface")
    require("org.deskpal.ShellBridge1" in source, "missing Shell bridge interface")
    require("SHELL_BRIDGE_PROTOCOL_VERSION = 1" in source, "missing protocol version")
    require("SHELL_BRIDGE_MAX_WINDOWS = 256" in source, "window list must be bounded")
    require(r"/[\x00-\x1F\x7F]/g" in source,
            "untrusted window strings must not inject controls")
    require("shellInstanceId" in source, "missing Shell-instance identity")
    require("surfaceId" in source and "generation" in source,
            "missing replacement-safe window identity")
    require("geometryRevision" in source, "missing geometry freshness")
    require("get_window_actors" in source, "missing native window enumeration")
    require("windowCapture: false" in source, "bridge must deny capture")
    require("foregroundWindowManagement: false" in source,
            "bridge must deny window mutation")
    require("surfaceInput: false" in source and "backgroundInput: false" in source,
            "bridge must deny input")
    require("gnome-stage-logical" in source, "missing coordinate-space identity")
    require("onComplete" in source and "sequence" in source, "missing move completion state")
    require("NameOwnerChanged" in source, "owned cursors need disconnect cleanup")
    require("invocation.get_sender()" in source, "mutations must bind to the D-Bus caller")
    require("affectsInputRegion: false" in source, "overlay must be click-through")
    require("reactive: false" in source, "overlay must not receive input")
    require("Main.activateWindow" not in source, "extension must not activate windows")
    require("move_frame" not in source and "move_resize_frame" not in source,
            "extension must not move or resize windows")
    require("Shell.Screenshot" not in source, "extension must not capture the desktop")
    require(not re.search(r"global\.display\.focus_window\s*=(?!=)", source),
            "extension must not assign focus")
    require("XTest" not in source and "uinput" not in source, "indicator must not inject input")
    require("St.DrawingArea" in source, "cursor must use a drawn pointer shape")
    require("deskpal-agent-cursor-pointer" in stylesheet, "missing pointer style")
    require("gdbus call --session" in script, "demo must use the session bus")
    require("xdotool" not in script and "ydotool" not in script, "demo must not move input")
    require('pi.on("agent_settled"' in pi_extension, "Pi must release idle control")
    require('callTool("release_control"' in pi_extension, "Pi release hook must call Deskpal")

    subprocess.run(["node", "--check", str(JS)], check=True)
    subprocess.run(["bash", "-n", str(SCRIPT)], check=True)
    print("PASS: GNOME indicator contract is narrow, click-through, and input-free")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
