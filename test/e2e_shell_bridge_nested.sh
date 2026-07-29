#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UUID=indicator@deskpal.local

if [[ ${DESKPAL_NESTED_GNOME_CHILD:-} == 1 ]]; then
    gsettings set org.gnome.shell disable-user-extensions false
    gsettings set org.gnome.shell enabled-extensions "['$UUID']"
    gnome-shell --nested --wayland --no-x11 >"$TEST_ROOT/shell.log" 2>&1 &
    shell_pid=$!
    fixture_pid=
    cleanup_child() {
        [[ -z ${fixture_pid:-} ]] || kill "$fixture_pid" 2>/dev/null || true
        kill "$shell_pid" 2>/dev/null || true
        wait "$shell_pid" 2>/dev/null || true
    }
    trap cleanup_child EXIT

    bridge_call() {
        gdbus call --session --dest org.deskpal.ShellBridge \
            --object-path /org/deskpal/ShellBridge \
            --method "org.deskpal.ShellBridge1.$1"
    }
    for _ in $(seq 1 150); do
        if bridge_call GetCapabilities >"$TEST_ROOT/capabilities.txt" 2>/dev/null; then
            break
        fi
        kill -0 "$shell_pid" 2>/dev/null || exit 1
        sleep 0.1
    done
    test -s "$TEST_ROOT/capabilities.txt"

    WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland \
        zenity --info --title='Deskpal Native Bridge Fixture' \
        --text='native bridge fixture' >"$TEST_ROOT/fixture.log" 2>&1 &
    fixture_pid=$!
    for _ in $(seq 1 100); do
        bridge_call ListWindows >"$TEST_ROOT/windows.txt"
        grep -q 'Deskpal Native Bridge Fixture' "$TEST_ROOT/windows.txt" && break
        sleep 0.1
    done
    grep -q 'Deskpal Native Bridge Fixture' "$TEST_ROOT/windows.txt"
    bridge_call GetMonitorLayout >"$TEST_ROOT/monitors.txt"

    PROJECT_ROOT="$ROOT" python3 - <<'PY'
import ast
import json
import os
import subprocess
import sys

root = os.environ["PROJECT_ROOT"]
sys.path.insert(0, os.path.join(root, "test"))
from deskpal_client import DeskpalClient, text


def bridge(method):
    output = subprocess.check_output([
        "gdbus", "call", "--session", "--dest", "org.deskpal.ShellBridge",
        "--object-path", "/org/deskpal/ShellBridge",
        "--method", f"org.deskpal.ShellBridge1.{method}",
    ], text=True).strip()
    return json.loads(ast.literal_eval(output)[0])


def desktop_state():
    return {
        "pointer": subprocess.check_output(
            ["xdotool", "getmouselocation", "--shell"]),
        "focus": subprocess.check_output(
            ["xprop", "-root", "_NET_ACTIVE_WINDOW"]),
        "stacking": subprocess.check_output(
            ["xprop", "-root", "_NET_CLIENT_LIST_STACKING"]),
    }

caps = bridge("GetCapabilities")
windows = bridge("ListWindows")
monitors = bridge("GetMonitorLayout")
assert caps["protocolVersion"] == 1, caps
assert caps["capabilities"] == {
    "windowEnumeration": True,
    "monitorLayout": True,
    "windowCapture": False,
    "foregroundWindowManagement": False,
    "surfaceInput": False,
    "backgroundInput": False,
}, caps
assert windows["shellInstanceId"] == caps["shellInstanceId"], windows
assert monitors["shellInstanceId"] == caps["shellInstanceId"], monitors
assert windows["complete"] is True, windows
fixture = [window for window in windows["windows"]
           if window["title"] == "Deskpal Native Bridge Fixture"]
assert len(fixture) == 1 and fixture[0]["clientType"] == "wayland", windows
assert monitors["complete"] is True and len(monitors["monitors"]) == 1, monitors

before = desktop_state()
client = DeskpalClient(
    executable=os.path.join(root, "build", "deskpal"),
    name="nested-shell-bridge",
)
try:
    listing = text(client.tool("list_windows"))
    assert "Deskpal Native Bridge Fixture" in listing, listing
    assert "backend=gnome-shell-extension client=wayland" in listing, listing
    environment = json.loads(text(client.tool("get_environment_status")))
    discovery = environment["capabilities"]["nativeWaylandWindowDiscovery"]
    assert discovery["available"] is True, environment
    assert environment["selectedBackends"]["shellBridge"] == "gnome-shell-dbus"
finally:
    client.close()
assert desktop_state() == before
print(caps["shellInstanceId"])
PY

    # Disable/enable creates a fresh bridge instance and invalidates old IDs.
    gdbus call --session --dest org.gnome.Shell.Extensions \
        --object-path /org/gnome/Shell/Extensions \
        --method org.gnome.Shell.Extensions.DisableExtension "$UUID" >/dev/null
    gdbus call --session --dest org.gnome.Shell.Extensions \
        --object-path /org/gnome/Shell/Extensions \
        --method org.gnome.Shell.Extensions.EnableExtension "$UUID" >/dev/null
    for _ in $(seq 1 50); do
        bridge_call GetCapabilities >"$TEST_ROOT/restarted-capabilities.txt" 2>/dev/null && break
        sleep 0.1
    done
    test -s "$TEST_ROOT/restarted-capabilities.txt"
    exit 0
fi

for command in xvfb-run dbus-run-session gsettings gnome-shell gdbus zenity xdotool xprop; do
    command -v "$command" >/dev/null || {
        echo "SKIP: nested GNOME Shell bridge test requires $command"
        exit 0
    }
done
[[ -x "$ROOT/build/deskpal" ]] || {
    echo "FAIL: run npm run build before the nested GNOME test" >&2
    exit 1
}

test_root=$(mktemp -d -t deskpal-shell-bridge-XXXXXX)
cleanup() { rm -rf "$test_root"; }
trap cleanup EXIT
mkdir -p "$test_root/home/.local/share/gnome-shell/extensions" \
         "$test_root/config" "$test_root/runtime"
chmod 700 "$test_root/runtime"
cp -a "$ROOT/gnome-extension/$UUID" \
      "$test_root/home/.local/share/gnome-shell/extensions/"

if ! timeout 35s xvfb-run -a -s '-screen 0 1280x720x24' \
    dbus-run-session -- env \
        DESKPAL_NESTED_GNOME_CHILD=1 \
        TEST_ROOT="$test_root" \
        HOME="$test_root/home" \
        XDG_DATA_HOME="$test_root/home/.local/share" \
        XDG_CONFIG_HOME="$test_root/config" \
        XDG_RUNTIME_DIR="$test_root/runtime" \
        GSETTINGS_BACKEND=keyfile \
        "$0" >"$test_root/inner-python.log" 2>"$test_root/outer.log"; then
    cat "$test_root/outer.log" >&2
    cat "$test_root/shell.log" >&2 || true
    exit 1
fi

python3 - "$test_root/capabilities.txt" "$test_root/restarted-capabilities.txt" <<'PY'
import ast
import json
import sys


def load(path):
    return json.loads(ast.literal_eval(open(path, encoding="utf-8").read())[0])

before = load(sys.argv[1])
after = load(sys.argv[2])
assert before["shellInstanceId"] != after["shellInstanceId"], (before, after)
PY

echo "PASS: nested GNOME 42 bridge enumerates native Wayland without desktop side effects"
