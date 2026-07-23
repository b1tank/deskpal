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


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    metadata = json.loads(METADATA.read_text())
    source = JS.read_text()
    stylesheet = STYLESHEET.read_text()
    script = SCRIPT.read_text()

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
        },
        f"unexpected D-Bus methods: {sorted(methods)}",
    )
    require("org.deskpal.Indicator" in source, "missing D-Bus interface")
    require("gnome-stage-logical" in source, "missing coordinate-space identity")
    require("onComplete" in source and "sequence" in source, "missing move completion state")
    require("NameOwnerChanged" in source, "owned cursors need disconnect cleanup")
    require("invocation.get_sender()" in source, "mutations must bind to the D-Bus caller")
    require("affectsInputRegion: false" in source, "overlay must be click-through")
    require("reactive: false" in source, "overlay must not receive input")
    require("Main.activateWindow" not in source, "indicator must not activate windows")
    require("global.display.focus_window" not in source, "indicator must not alter focus")
    require("XTest" not in source and "uinput" not in source, "indicator must not inject input")
    require("St.DrawingArea" in source, "cursor must use a drawn pointer shape")
    require("deskpal-agent-cursor-pointer" in stylesheet, "missing pointer style")
    require("gdbus call --session" in script, "demo must use the session bus")
    require("xdotool" not in script and "ydotool" not in script, "demo must not move input")

    subprocess.run(["node", "--check", str(JS)], check=True)
    subprocess.run(["bash", "-n", str(SCRIPT)], check=True)
    print("PASS: GNOME indicator contract is narrow, click-through, and input-free")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
