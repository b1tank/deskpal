#!/usr/bin/env python3
"""Reusable JSON-RPC client and assertions for deskpal E2E tests."""

import base64
import json
import os
import struct
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESKPAL = os.environ.get(
    "DESKPAL_TEST_BINARY", os.path.join(ROOT, "build", "deskpal")
)


class DeskpalClient:
    def __init__(self, env=None, executable=DESKPAL, args=None, name="deskpal-test"):
        command = [executable, *(args or [])]
        self.proc = subprocess.Popen(
            command,
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
                "clientInfo": {"name": name, "version": "1.0"},
            },
        )
        self.notify("notifications/initialized")

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
            return_code = self.proc.poll()
            stderr = self.proc.stderr.read() if return_code is not None else ""
            raise AssertionError(
                f"non-JSON data on MCP stdout: {line!r}; "
                f"returncode={return_code}; stderr={stderr!r}"
            )
        return json.loads(line)

    def notify(self, method, params=None):
        request = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params or {},
        }
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()

    def tools(self):
        return self.call("tools/list", {})["result"]["tools"]

    def tool(self, name, arguments=None):
        response = self.call(
            "tools/call", {"name": name, "arguments": arguments or {}}
        )
        return response["result"]

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        stderr = self.proc.stderr.read()
        if self.proc.returncode != 0:
            raise AssertionError(
                f"deskpal exited with {self.proc.returncode}; stderr={stderr!r}"
            )
        return stderr


def text(result, index=0):
    return result["content"][index]["text"]


def image_bytes(result):
    image = result["content"][0]
    assert image["type"] == "image", image
    assert image["mimeType"] == "image/png", image
    data = base64.b64decode(image["data"])
    assert data[:8] == bytes.fromhex("89504e470d0a1a0a"), data[:8]
    return data


def png_size(result):
    data = image_bytes(result)
    return struct.unpack(">II", data[16:24])


def png_is_opaque(result):
    identified = subprocess.run(
        ["identify", "-format", "%[opaque]", "png:-"],
        input=image_bytes(result),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return identified.stdout.strip().lower() == b"true"


def tool_by_name(tools, name):
    return next(tool for tool in tools if tool["name"] == name)


def start_xvfb(temp_dir, screen_size="1280x800"):
    display_number = next(
        number
        for number in range(190, 350)
        if not os.path.exists(f"/tmp/.X11-unix/X{number}")
    )
    display = f":{display_number}"
    auth = os.path.join(temp_dir, "Xauthority")
    open(auth, "a", encoding="ascii").close()
    cookie = "0123456789abcdef0123456789abcdef"
    subprocess.run(
        ["xauth", "-f", auth, "add", display, ".", cookie],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    process = subprocess.Popen(
        [
            "Xvfb", display, "-screen", "0", f"{screen_size}x24",
            "-auth", auth,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    env = os.environ.copy()
    for name in (
        "WAYLAND_DISPLAY",
        "DESKPAL_HEADLESS_ACTIVE",
        "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS",
        "SESSION_MANAGER",
        "AT_SPI_BUS_ADDRESS",
        "AT_SPI_BUS",
        "AT_SPI_DISPLAY",
    ):
        env.pop(name, None)
    env.update(
        {
            "DISPLAY": display,
            "XAUTHORITY": auth,
            "XDG_SESSION_TYPE": "x11",
            "GDK_BACKEND": "x11",
            "QT_QPA_PLATFORM": "xcb",
        }
    )
    deadline = time.time() + 5
    while time.time() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"Xvfb exited: {process.stderr.read()}")
        ready = subprocess.run(
            ["xdpyinfo", "-display", display],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if ready.returncode == 0:
            return process, env
        time.sleep(0.05)
    stop_process(process)
    raise AssertionError("Xvfb did not become ready")


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
