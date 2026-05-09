# deskpal

**Playwright for the desktop** — an MCP server that gives AI agents eyes and hands for native Linux apps.

## What it does

| Tool | Description |
|------|-------------|
| `screenshot` | Capture any window or full screen as PNG |
| `list_windows` | List all visible windows with IDs, titles, geometry |
| `find_window` | Find a window by title substring |
| `focus_window` | Bring a window to front |
| `click` | Click at (x, y) relative to a window |
| `type_text` | Type text into the focused window |
| `key_press` | Send keyboard shortcuts (e.g. `ctrl+s`) |
| `mouse_move` | Move cursor to position |
| `scroll` | Scroll up/down |
| `get_window_geometry` | Get window position and size |
| `resize_window` | Resize a window |
| `wait_for_window` | Wait for a window to appear |

## Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install xdotool x11-utils imagemagick
```

## Install

```bash
npm install
npm run build
```

## Usage as MCP server

Add to your VS Code `settings.json`:

```json
{
  "mcp": {
    "servers": {
      "deskpal": {
        "command": "node",
        "args": ["/path/to/deskpal/dist/index.js"]
      }
    }
  }
}
```

Works with any X11 application — GTK, Qt, Electron, terminal apps, anything with a window.
