# deskpal

**Playwright for the desktop** — a C-native MCP server that gives AI agents eyes and hands for any Linux desktop app.

## What it does

deskpal controls the Linux desktop via the [Model Context Protocol](https://modelcontextprotocol.io/). It can launch apps, find windows, take screenshots, read text via OCR, click buttons by name, type, scroll, drag — anything a human can do with a mouse and keyboard.

### Tools

| Tool | Description |
|------|-------------|
| `screenshot` | Capture any window or full screen as PNG; optionally downscale with source-coordinate metadata |
| `list_windows` | List top-level app windows with IDs, titles, `WM_CLASS`, geometry, PID; optionally include recursive helpers/dialogs |
| `accessibility_status` | Cheaply report whether the optional AT-SPI backend is compiled and connected |
| `get_accessibility_tree` | Return a scoped, bounded semantic tree with roles, names, states, logical bounds, actions, optional attributes/text, and short-lived locators |
| `get_focused_element` | Return the true focused AT-SPI child within a required application/window scope; ambiguous results fail closed |
| `accessibility_action` | Perform verified semantic `setText`, `focus`, or named `invoke` on one uniquely resolved accessible element |
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
- **Arbitration**: the first visible-desktop mutation acquires a per-user
  machine lock until that deskpal MCP process exits; read-only tools and
  private Xvfb sessions remain available to other clients

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

Optional semantic accessibility support is built when `libatspi2.0-dev` is
installed:

```bash
sudo apt-get install libatspi2.0-dev libglib2.0-dev
```

Deskpal never enables accessibility globally. `accessibility_status` cheaply
reports backend availability; scoped tree results report `empty`, `semantic`,
or `error`. Electron apps may need launch-time accessibility
support such as `--force-renderer-accessibility` before they expose useful
descendants. Accessibility paths are short-lived tree locations, not permanent
element IDs; re-query before relying on them. Accessible names, optional
attributes, and optional text are application-controlled, potentially private
or adversarial content. Text and attributes are omitted unless explicitly
requested, and password/unknown-role text is never returned.

`accessibility_action` is visible-desktop-only and takes the same machine-wide
control lock as pixel input. Its `application` and `window` scopes are exact
accessible names (copy them from the tree locator), while read-only inspection
filters remain substring matches. It re-resolves a unique role/name or short-lived
path before acting. Path targets must include the locator's `busName`,
`objectPath`, and `processId`, so a replacement at the same tree index is
rejected. `setText` and `focus` verify their own result; generic
`invoke` requires an explicit text and/or state postcondition. Ambiguous,
protected, stale, incomplete, or unverified actions fail closed. An action may
have been applied even when its postcondition later fails, so inspect the
structured `mutationIssued`, `actionApplied`, `actionOutcomeUnknown`, and
`verified` fields separately. A timed-out RPC may have reached the app: if the
outcome is unknown and verification fails, do not retry blindly. If the
postcondition is already satisfied, the tool returns a verified no-op success
without issuing the mutation again. Observed verification text is never echoed.
JSON strings containing U+0000 are rejected at parse time so hidden suffixes
cannot bypass C-string validation or exact postcondition comparison.
DEFUNCT verification elements are unreadable even when the expected state is
`false`; a missing state on a dead object cannot satisfy a postcondition.

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

### Pi extension

Deskpal ships a Pi extension that starts one persistent MCP process per Pi
session and registers every server tool with a `deskpal_` prefix. Screenshot
results are returned as Pi image blocks rather than base64 text.

```bash
# From this checkout
pi -e ./extensions/deskpal.ts

# Or install the checkout as a local Pi package
pi install /absolute/path/to/deskpal
```

The extension resolves `build/deskpal` relative to the checkout. Override it
when needed:

```bash
DESKPAL_BINARY=/absolute/path/to/deskpal pi -e ./extensions/deskpal.ts
```

Process-launch and filesystem tools remain disabled by default, matching the
server's security model. Opt in explicitly when starting Pi:

```bash
DESKPAL_PI_ALLOW_EXEC=1 DESKPAL_PI_ALLOW_FS=1 pi -e ./extensions/deskpal.ts
```

Use `/deskpal-status` to verify the bridge. The extension closes the server and
its isolated sessions when the Pi session shuts down. Before choosing a desktop
interaction route, call `deskpal_get_environment_status` to inspect active
backends, blockers, shared-seat risks, and concrete setup actions. Use
`deskpal_get_app_state` with an exact `windowId` or `windowName` to obtain one
capture-bound image, target/focus/transform metadata, and bounded untrusted
AT-SPI state. Pass a retained `previousCaptureId` for a privacy-safe structural
revision and bounded same-target element diff; see
[docs/app-state.md](docs/app-state.md).

On GNOME 42, the optional logical-cursor extension adds
`deskpal_agent_cursor_status`, `deskpal_agent_cursor_move`, and
`deskpal_agent_cursor_hide`. Take a full-screen screenshot first, then pass its
short-lived `captureId` and image-pixel coordinates to the move tool. The
indicator is visual only: results explicitly report `inputDelivered: false`.
Installation and acceptance steps are in [docs/gnome-indicator.md](docs/gnome-indicator.md).
For accessible controls, `deskpal_agent_semantic_press`,
`deskpal_agent_semantic_set_text`, `deskpal_agent_semantic_set_value`, and
`deskpal_agent_semantic_select`, and
`deskpal_agent_semantic_replace_text_range` connect a stable app observation,
logical-cursor motion, verified AT-SPI mutation, and an explicit postcondition
without shared-pointer, keyboard, or clipboard fallback; see
[docs/semantic-actions.md](docs/semantic-actions.md) for its contract and
limitations. Chromium/Electron session setup evidence and its native-Wayland
boundary are documented in
[docs/chromium-accessibility.md](docs/chromium-accessibility.md).

### Goal-aware isolation

Deskpal remains connected to the user's desktop and creates Xvfb sessions only
for tasks that should not interrupt it. The agent chooses the launch tool from
the goal:

When `launch_app` is given `waitForWindow`, it defaults to launching through
XWayland so the resulting window can be discovered and controlled by Deskpal's
X11 backend. Set `forceX11: false` to permit a native Wayland surface when
window matching and control are not required. Applications that delegate to an
already-running native Wayland process may still need that process closed or a
separate application profile.

Private children inherit the parent's already-locked control-file descriptor
and validate its inode, ownership, permissions, and kernel lock before serving
tools. Supplying `--xvfb-child`, headless environment variables, a fake file
descriptor, or a PATH-shadowed launcher cannot escape arbitration.

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

Visible-desktop mutations and all process-launch tools are serialized across deskpal processes. If another
MCP host already holds control, the tool returns an error naming the owner
instead of racing the pointer or keyboard. The lock is lazy: window listing,
OCR, screenshots, and interactions inside an existing private session do not
claim it; creating that private session does.

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

`--allow-exec` also gates `launch_app` and `launch_isolated_app`, because both
execute an arbitrary command with structured arguments. Without the flags the tools are still listed in `tools/list` but
return a "disabled. Start deskpal with `--allow-…`" message. Even
with `--allow-fs`, paths under `/etc/shadow`, `/etc/sudoers`,
`/root/`, and `/proc/self/maps` are refused.

## Testing

```bash
# Build and run all deterministic, desktop-safe tests
npm run build
npm test

# Repeat the safe suite with ASan + UBSan
npm run test:asan

# Focused safe suites
npm run test:isolation
npm run test:computer-use

# Live GNOME tests (manipulate the visible desktop)
npm run test:live
```

See [test/README.md](test/README.md) for prerequisites and test-writing
guidance. The detailed comparison with Claude's built-in computer use and the
north-star architecture plan are in
[docs/computer-use-parity.md](docs/computer-use-parity.md).

## Compatibility

- **Display**: X11, Xwayland (GNOME Wayland with Xwayland works), or isolated Xvfb. Native Wayland-only windows are not yet discoverable/capturable.
- **HiDPI**: Handles display scaling (tested at 192 DPI / 2x scale)
- **Apps**: Any X11 window — GTK, Qt, Electron, terminal apps, games
- **Dialogs**: Finds and interacts with transient/child windows
