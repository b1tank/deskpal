#!/usr/bin/env python3
"""Safe static contract checks for the GNOME logical-cursor prototype."""

import hashlib
import json
import re
import subprocess
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTENSION = ROOT / "gnome-extension" / "indicator@deskpal.local"
JS = EXTENSION / "extension.js"
METADATA = EXTENSION / "metadata.json"
STYLESHEET = EXTENSION / "stylesheet.css"
SCRIPT = ROOT / "scripts" / "indicator.sh"
PI_EXTENSION = ROOT / "extensions" / "deskpal.ts"
PACKAGER = ROOT / "scripts" / "package-gnome-extension.py"


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
    require("Shell, St} = imports.gi" in source,
            "legacy bridge must import Shell.WindowTracker")
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
    require("bridge_call GetCapabilities" in script,
            "installer must verify both packaged services")
    require("xdotool" not in script and "ydotool" not in script, "demo must not move input")
    require('pi.on("agent_settled"' in pi_extension, "Pi must release idle control")
    require('callTool("release_control"' in pi_extension, "Pi release hook must call Deskpal")

    subprocess.run(["node", "--check", str(JS)], check=True)
    subprocess.run(["bash", "-n", str(SCRIPT)], check=True)

    with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
        subprocess.run([str(PACKAGER), "--output", first], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([str(PACKAGER), "--output", second], check=True,
                       stdout=subprocess.DEVNULL)
        names = {
            "deskpal-shell-extension-gnome42.zip": ["42"],
            "deskpal-shell-extension-gnome45-50.zip":
                ["45", "46", "47", "48", "49", "50"],
        }
        for name, versions in names.items():
            first_zip = Path(first) / name
            second_zip = Path(second) / name
            require(first_zip.is_file(), f"missing extension artifact: {name}")
            require(hashlib.sha256(first_zip.read_bytes()).digest() ==
                    hashlib.sha256(second_zip.read_bytes()).digest(),
                    f"extension artifact is not deterministic: {name}")
            with zipfile.ZipFile(first_zip) as archive:
                require(set(archive.namelist()) ==
                        {"extension.js", "metadata.json", "stylesheet.css"},
                        f"unexpected artifact contents: {name}")
                packaged_metadata = json.loads(archive.read("metadata.json"))
                packaged_source = archive.read("extension.js").decode()
                require(packaged_metadata["uuid"] == "indicator@deskpal.local",
                        f"unexpected packaged UUID: {name}")
                require(packaged_metadata["shell-version"] == versions,
                        f"unexpected packaged Shell versions: {name}")
                if versions == ["42"]:
                    require("function init()" in packaged_source and
                            "imports.ui.main" in packaged_source,
                            "GNOME 42 artifact lost its legacy entry point")
                else:
                    require("export default class DeskpalIndicatorExtension" in
                            packaged_source,
                            "modern artifact lacks default Extension export")
                    require("imports.ui.main" not in packaged_source and
                            "function init()" not in packaged_source,
                            "modern artifact retained legacy entry points")
                    modern_path = Path(first) / "extension.mjs"
                    modern_path.write_text(packaged_source)
                    subprocess.run(["node", "--check", str(modern_path)], check=True)
    print("PASS: GNOME indicator and Shell bridge artifacts are narrow and deterministic")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
