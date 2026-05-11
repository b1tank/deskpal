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
| `launch_app` | Launch a desktop app, kill existing instances, wait for window |

### How it works

- **Input**: Virtual input devices via `/dev/uinput` for Wayland-compatible mouse and keyboard (falls back to `xdotool` for right-click context menus)
- **Screenshots**: XCB `GetImage` for window capture, `gnome-screenshot` fallback for full-screen on Wayland, ImageMagick `import` fallback for transient dialogs
- **OCR**: Tesseract with 2x upscaling, normal + inverted passes for dark themes, position-based multi-word matching
- **Protocol**: JSON-RPC 2.0 over stdio (MCP 2024-11-05)

## Prerequisites

```bash
# Ubuntu/Debian — runtime
sudo apt-get install xdotool imagemagick tesseract-ocr

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

## Testing

```bash
# Canonical E2E test against GNOME System Monitor (41 tests)
GDK_BACKEND=x11 gnome-system-monitor &
python3 test/e2e_sysmon.py

# General desktop interaction test (21 tests)
python3 test/e2e_desktop.py
```

## Compatibility

- **Display**: X11 or Xwayland (GNOME Wayland with Xwayland works)
- **HiDPI**: Handles display scaling (tested at 192 DPI / 2x scale)
- **Apps**: Any X11 window — GTK, Qt, Electron, terminal apps, games
- **Dialogs**: Finds and interacts with transient/child windows
