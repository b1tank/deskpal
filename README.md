# deskpal

**Playwright for the desktop** — a C-native MCP server that gives AI agents eyes and hands for any Linux desktop app.

## What it does

deskpal controls the Linux desktop via the [Model Context Protocol](https://modelcontextprotocol.io/). It can launch apps, find windows, take screenshots, read text via OCR, click buttons by name, type, scroll, drag — anything a human can do with a mouse and keyboard.

### Tools

| Tool | Description |
|------|-------------|
| `screenshot` | Capture any window or full screen as PNG |
| `list_windows` | List all visible windows with IDs, titles, geometry, PID |
| `find_window` | Find a window by title substring |
| `focus_window` | Bring a window to front and give it input focus |
| `click` | Click at (x, y) relative to a window. Supports left/right/middle buttons |
| `click_text` | **OCR-powered** — find visible text and click its center. No coordinate guessing |
| `read_screen_text` | Read all visible text from a window via OCR, with positions |
| `type_text` | Type text into the focused window via virtual keyboard |
| `key_press` | Send keyboard shortcuts (e.g. `ctrl+s`, `alt+F4`, `Return`) |
| `mouse_move` | Move cursor to position relative to a window |
| `scroll` | Scroll up or down in a window |
| `drag` | Click-and-drag between two points (sliders, selections, panels) |
| `mouse_down` / `mouse_up` | Press/release mouse buttons for complex gestures |
| `get_window_geometry` | Get window position, size, and display scale |
| `resize_window` | Resize a window to specified dimensions |
| `wait_for_window` | Wait for a window with a given title to appear |
| `launch_app` | Launch an app on the user's visible desktop |
| `launch_isolated_app` | Launch an app in a private Xvfb verification session |
| `close_isolated_session` | Close an Xvfb session and all apps running in it |
| `get_clipboard` / `set_clipboard` | Read or write the OS clipboard (auto-detects `wl-clipboard` / `xclip` / `xsel`) |
| `hover_text` | Move the mouse over OCR-located text and return just the tooltip text that became visible |
| `read_file` | Read a file from disk. Requires `--allow-fs` |
| `exec` | Run a short shell command and capture stdout+stderr with timeout. Requires `--allow-exec` |

### How it works

- **Input**: Virtual input devices via `/dev/uinput` for Wayland-compatible mouse and keyboard; isolated Xvfb sessions use display-local XTest input only
- **Screenshots**: XCB `GetImage` for window capture, `gnome-screenshot` fallback for full-screen on Wayland, ImageMagick `import` fallback for transient dialogs
- **OCR**: Tesseract with 2x upscaling, normal + inverted passes for dark themes, position-based multi-word matching
- **Protocol**: JSON-RPC 2.0 over stdio (MCP 2024-11-05)

## Prerequisites

```bash
# Ubuntu/Debian — runtime
sudo apt-get install xdotool imagemagick tesseract-ocr xvfb xauth

# Ubuntu/Debian — build
sudo apt-get install meson ninja-build gcc \
  libxcb1-dev libxcb-shm0-dev libx11-dev libx11-xcb-dev \
  libxdo-dev libpng-dev libdbus-1-dev \
  libtesseract-dev libleptonica-dev
```

Your user must have access to `/dev/uinput` for virtual input devices:

```bash
sudo usermod -aG input $USER
# or: sudo chmod 0666 /dev/uinput
```

## Build

```bash
meson setup build
ninja -C build
```

The binary is `build/deskpal`.

## Usage as MCP server

Add to your VS Code `settings.json`:

```json
{
  "mcp": {
    "servers": {
      "deskpal": {
        "command": "/path/to/deskpal/build/deskpal"
      }
    }
  }
}
```

Or test directly via stdin:

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1"}}}' | ./build/deskpal
```

### Goal-aware isolation

Deskpal remains connected to the user's desktop and creates Xvfb sessions only
for tasks that should not interrupt it. The agent chooses the launch tool from
the goal:

| Goal | Tool and routing |
|------|------------------|
| Verify a locally developed GUI or browser app | Use `launch_isolated_app`, then pass its `sessionId` to every screenshot, window, clipboard, and input tool |
| Inspect or control an app already open on the user's desktop | Use `list_windows` / `find_window` and the normal tools without `sessionId` |
| Launch an app that the user wants on their visible desktop | Use `launch_app` without `sessionId` |
| Launch another app beside one under isolated verification | Use `launch_app` with the existing `sessionId` |

An isolated launch returns output like:

```text
Isolated session: xvfb-1
Pass sessionId="xvfb-1" to every subsequent UI tool for this app.
```

Calls without `sessionId` always continue to target the user's desktop. Call
`close_isolated_session` when verification is complete; Deskpal also closes
all remaining sessions when the MCP server exits.

Isolated children clear host Wayland/session routing, disable `/dev/uinput`,
and use XTest only against their private X server. Display, Xauthority,
Wayland, D-Bus, and GUI-backend overrides passed through the tool API are
ignored in isolated sessions. Commands, arguments, and environment values are
launched as structured values rather than interpreted as shell syntax. These
controls keep Deskpal's own capture and input routing on the private display
and prevent accidental reconnection to the visible desktop.

Xvfb is not a security sandbox. An application launched in an isolated session
still runs as the same OS user and can access that user's files, network, and
discoverable runtime sockets. Do not use this feature to run untrusted code;
use a container or OS sandbox when adversarial isolation is required.

An Xvfb session cannot see windows that are already open on the real desktop.
Applications with single-instance or shared-profile behavior may need an
isolated profile argument such as `--user-data-dir=/tmp/deskpal-profile` to
prevent them from forwarding the request to an existing host process. Xvfb
also has no window manager by default; applications requiring EWMH behavior or
decorations may need a lightweight window manager in the isolated session.

### Security flags

Two tools are gated behind off-by-default CLI flags because they
expand deskpal's blast radius beyond "drive the desktop":

| Flag | Tool | What it does |
|------|------|--------------|
| `--allow-fs` | `read_file` | Read arbitrary files from disk |
| `--allow-exec` | `exec` | Run short shell commands with a timeout |

Without the flags the tools are still listed in `tools/list` but
return a "disabled. Start deskpal with `--allow-…`" message. Even
with `--allow-fs`, paths under `/etc/shadow`, `/etc/sudoers`,
`/root/`, and `/proc/self/maps` are refused.

## Testing

```bash
# Hybrid desktop/Xvfb routing test (does not manipulate the active desktop)
npm run test:isolation

# Canonical E2E test against GNOME System Monitor (41 tests)
GDK_BACKEND=x11 gnome-system-monitor &
python3 test/e2e_sysmon.py

# General desktop interaction test (21 tests)
python3 test/e2e_desktop.py
```

## Compatibility

- **Display**: X11, Xwayland (GNOME Wayland with Xwayland works), or isolated Xvfb
- **HiDPI**: Handles display scaling (tested at 192 DPI / 2x scale)
- **Apps**: Any X11 window — GTK, Qt, Electron, terminal apps, games
- **Dialogs**: Finds and interacts with transient/child windows
