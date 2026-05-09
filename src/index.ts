#!/usr/bin/env node
/**
 * deskpal — Playwright for the desktop
 * MCP server giving AI agents eyes and hands for native Linux apps.
 *
 * Copyright (c) 2026 deskpal contributors
 * SPDX-License-Identifier: MIT
 */
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { exec, spawn } from "./shell.js";

const server = new McpServer({
  name: "deskpal",
  version: "0.1.0",
});

// ── screenshot ──────────────────────────────────────────────────────────────

server.tool(
  "screenshot",
  "Capture a screenshot of a window or the entire screen. Returns the image as base64 PNG.",
  {
    windowId: z
      .string()
      .optional()
      .describe("X11 window ID (hex or decimal). Omit for the active window."),
    windowName: z
      .string()
      .optional()
      .describe(
        "Window name/title substring to search for. Ignored if windowId is provided."
      ),
    fullScreen: z
      .boolean()
      .optional()
      .default(false)
      .describe("Capture the entire screen instead of a single window."),
  },
  async ({ windowId, windowName, fullScreen }) => {
    let wid = windowId;

    if (!wid && windowName) {
      wid = await findWindow(windowName);
      if (!wid) {
        return { content: [{ type: "text", text: `No window found matching "${windowName}"` }] };
      }
    }

    const tmpFile = `/tmp/deskpal_${Date.now()}.png`;

    try {
      if (fullScreen) {
        await exec(`import -window root ${tmpFile}`);
      } else if (wid) {
        await exec(`import -window ${wid} ${tmpFile}`);
      } else {
        // Active window
        const activeWid = (await exec("xdotool getactivewindow")).trim();
        await exec(`import -window ${activeWid} ${tmpFile}`);
      }

      const base64 = (await exec(`base64 -w0 ${tmpFile}`)).trim();
      await exec(`rm -f ${tmpFile}`);

      return {
        content: [{ type: "image", data: base64, mimeType: "image/png" }],
      };
    } catch (e: unknown) {
      return { content: [{ type: "text", text: `Screenshot failed: ${(e as Error).message}` }] };
    }
  }
);

// ── list_windows ────────────────────────────────────────────────────────────

server.tool(
  "list_windows",
  "List all visible windows with their IDs, titles, geometry, and PID.",
  {},
  async () => {
    const raw = await exec(
      `xdotool search --onlyvisible --name "" 2>/dev/null || true`
    );
    const wids = raw.trim().split("\n").filter(Boolean);

    const windows: { id: string; title: string; geometry: string; pid: string }[] = [];

    for (const wid of wids.slice(0, 50)) {
      try {
        const title = (await exec(`xdotool getwindowname ${wid} 2>/dev/null`)).trim();
        const geo = (await exec(`xdotool getwindowgeometry ${wid} 2>/dev/null`)).trim();
        const pid = (await exec(`xdotool getwindowpid ${wid} 2>/dev/null`)).trim();
        if (title) {
          windows.push({ id: wid, title, geometry: geo, pid });
        }
      } catch { /* skip */ }
    }

    return {
      content: [
        {
          type: "text",
          text: windows.length
            ? windows
                .map((w) => `[${w.id}] "${w.title}" pid=${w.pid}\n  ${w.geometry}`)
                .join("\n\n")
            : "No visible windows found",
        },
      ],
    };
  }
);

// ── find_window ─────────────────────────────────────────────────────────────

server.tool(
  "find_window",
  "Find a window by title substring. Returns the window ID.",
  {
    name: z.string().describe("Window title substring to search for"),
  },
  async ({ name }) => {
    const wid = await findWindow(name);
    if (!wid) {
      return { content: [{ type: "text", text: `No window found matching "${name}"` }] };
    }
    const title = (await exec(`xdotool getwindowname ${wid} 2>/dev/null`)).trim();
    const geo = (await exec(`xdotool getwindowgeometry ${wid} 2>/dev/null`)).trim();
    return {
      content: [{ type: "text", text: `Window found: [${wid}] "${title}"\n${geo}` }],
    };
  }
);

// ── focus_window ────────────────────────────────────────────────────────────

server.tool(
  "focus_window",
  "Bring a window to the front and give it focus.",
  {
    windowId: z.string().optional().describe("X11 window ID"),
    windowName: z.string().optional().describe("Window title substring"),
  },
  async ({ windowId, windowName }) => {
    let wid = windowId;
    if (!wid && windowName) {
      wid = await findWindow(windowName);
    }
    if (!wid) {
      return { content: [{ type: "text", text: "Window not found" }] };
    }
    await exec(`xdotool windowactivate --sync ${wid}`);
    return { content: [{ type: "text", text: `Focused window ${wid}` }] };
  }
);

// ── click ───────────────────────────────────────────────────────────────────

server.tool(
  "click",
  "Click at a position in a window. Coordinates are relative to the window's top-left corner.",
  {
    windowId: z.string().optional().describe("X11 window ID. Omit for active window."),
    windowName: z.string().optional().describe("Window title substring"),
    x: z.number().describe("X coordinate relative to window"),
    y: z.number().describe("Y coordinate relative to window"),
    button: z
      .number()
      .optional()
      .default(1)
      .describe("Mouse button: 1=left, 2=middle, 3=right"),
    doubleClick: z.boolean().optional().default(false),
  },
  async ({ windowId, windowName, x, y, button, doubleClick }) => {
    let wid = windowId;
    if (!wid && windowName) {
      wid = await findWindow(windowName);
    }

    // Focus and get window position
    if (wid) {
      await exec(`xdotool windowactivate --sync ${wid}`);
      await sleep(100);
    }

    const targetWid = wid || (await exec("xdotool getactivewindow")).trim();
    const geoRaw = await exec(`xdotool getwindowgeometry ${targetWid}`);
    const posMatch = geoRaw.match(/Position: (\d+),(\d+)/);
    if (!posMatch) {
      return { content: [{ type: "text", text: "Could not get window position" }] };
    }
    const absX = parseInt(posMatch[1]!) + x;
    const absY = parseInt(posMatch[2]!) + y;

    const clicks = doubleClick ? 2 : 1;
    await exec(
      `xdotool mousemove --sync ${absX} ${absY} click --repeat ${clicks} ${button}`
    );

    return {
      content: [
        {
          type: "text",
          text: `Clicked (${x}, ${y}) in window ${targetWid} [abs: ${absX}, ${absY}]`,
        },
      ],
    };
  }
);

// ── type_text ───────────────────────────────────────────────────────────────

server.tool(
  "type_text",
  "Type text into the focused window. Optionally focus a window first.",
  {
    text: z.string().describe("Text to type"),
    windowId: z.string().optional().describe("Window to focus before typing"),
    windowName: z.string().optional().describe("Window title to focus before typing"),
    delay: z
      .number()
      .optional()
      .default(12)
      .describe("Delay between keystrokes in ms"),
  },
  async ({ text, windowId, windowName, delay }) => {
    let wid = windowId;
    if (!wid && windowName) {
      wid = await findWindow(windowName);
    }
    if (wid) {
      await exec(`xdotool windowactivate --sync ${wid}`);
      await sleep(100);
    }

    // Escape single quotes for shell
    const escaped = text.replace(/'/g, "'\\''");
    await exec(`xdotool type --delay ${delay} '${escaped}'`);

    return {
      content: [{ type: "text", text: `Typed ${text.length} characters` }],
    };
  }
);

// ── key_press ───────────────────────────────────────────────────────────────

server.tool(
  "key_press",
  "Send keyboard shortcuts or key presses. Uses xdotool key names (e.g. 'Return', 'ctrl+s', 'alt+F4').",
  {
    keys: z
      .string()
      .describe("Key combination (e.g. 'Return', 'ctrl+a', 'shift+Tab')"),
    windowId: z.string().optional().describe("Window to target"),
    windowName: z.string().optional().describe("Window title to target"),
  },
  async ({ keys, windowId, windowName }) => {
    let wid = windowId;
    if (!wid && windowName) {
      wid = await findWindow(windowName);
    }
    if (wid) {
      await exec(`xdotool windowactivate --sync ${wid}`);
      await sleep(100);
    }
    await exec(`xdotool key ${keys}`);
    return { content: [{ type: "text", text: `Pressed: ${keys}` }] };
  }
);

// ── get_window_geometry ─────────────────────────────────────────────────────

server.tool(
  "get_window_geometry",
  "Get the position and size of a window.",
  {
    windowId: z.string().optional(),
    windowName: z.string().optional(),
  },
  async ({ windowId, windowName }) => {
    let wid = windowId;
    if (!wid && windowName) {
      wid = await findWindow(windowName);
    }
    if (!wid) {
      wid = (await exec("xdotool getactivewindow")).trim();
    }
    const geo = (await exec(`xdotool getwindowgeometry ${wid}`)).trim();
    const size = (await exec(`xdotool getwindowgeometry --shell ${wid}`)).trim();
    return { content: [{ type: "text", text: `${geo}\n${size}` }] };
  }
);

// ── resize_window ───────────────────────────────────────────────────────────

server.tool(
  "resize_window",
  "Resize a window to the specified dimensions.",
  {
    width: z.number().describe("New width in pixels"),
    height: z.number().describe("New height in pixels"),
    windowId: z.string().optional(),
    windowName: z.string().optional(),
  },
  async ({ width, height, windowId, windowName }) => {
    let wid = windowId;
    if (!wid && windowName) {
      wid = await findWindow(windowName);
    }
    if (!wid) {
      wid = (await exec("xdotool getactivewindow")).trim();
    }
    await exec(`xdotool windowsize ${wid} ${width} ${height}`);
    return {
      content: [{ type: "text", text: `Resized window ${wid} to ${width}x${height}` }],
    };
  }
);

// ── wait_for_window ─────────────────────────────────────────────────────────

server.tool(
  "wait_for_window",
  "Wait for a window with the given title to appear. Times out after the specified duration.",
  {
    name: z.string().describe("Window title substring to wait for"),
    timeout: z
      .number()
      .optional()
      .default(10)
      .describe("Timeout in seconds"),
  },
  async ({ name, timeout }) => {
    const deadline = Date.now() + timeout * 1000;
    while (Date.now() < deadline) {
      const wid = await findWindow(name);
      if (wid) {
        const title = (await exec(`xdotool getwindowname ${wid} 2>/dev/null`)).trim();
        return {
          content: [{ type: "text", text: `Window appeared: [${wid}] "${title}"` }],
        };
      }
      await sleep(500);
    }
    return {
      content: [{ type: "text", text: `Timeout: no window matching "${name}" after ${timeout}s` }],
    };
  }
);

// ── mouse_move ──────────────────────────────────────────────────────────────

server.tool(
  "mouse_move",
  "Move the mouse to a position relative to a window (or absolute if no window specified).",
  {
    x: z.number(),
    y: z.number(),
    windowId: z.string().optional(),
    windowName: z.string().optional(),
  },
  async ({ x, y, windowId, windowName }) => {
    let wid = windowId;
    if (!wid && windowName) {
      wid = await findWindow(windowName);
    }
    if (wid) {
      const geoRaw = await exec(`xdotool getwindowgeometry ${wid}`);
      const posMatch = geoRaw.match(/Position: (\d+),(\d+)/);
      if (posMatch) {
        const absX = parseInt(posMatch[1]!) + x;
        const absY = parseInt(posMatch[2]!) + y;
        await exec(`xdotool mousemove --sync ${absX} ${absY}`);
        return { content: [{ type: "text", text: `Mouse moved to (${x}, ${y}) in window` }] };
      }
    }
    await exec(`xdotool mousemove --sync ${x} ${y}`);
    return { content: [{ type: "text", text: `Mouse moved to absolute (${x}, ${y})` }] };
  }
);

// ── scroll ──────────────────────────────────────────────────────────────────

server.tool(
  "scroll",
  "Scroll up or down in the focused or specified window.",
  {
    direction: z.enum(["up", "down"]).describe("Scroll direction"),
    clicks: z.number().optional().default(3).describe("Number of scroll clicks"),
    windowId: z.string().optional(),
    windowName: z.string().optional(),
  },
  async ({ direction, clicks, windowId, windowName }) => {
    let wid = windowId;
    if (!wid && windowName) {
      wid = await findWindow(windowName);
    }
    if (wid) {
      await exec(`xdotool windowactivate --sync ${wid}`);
      await sleep(50);
    }
    const button = direction === "up" ? 4 : 5;
    await exec(`xdotool click --repeat ${clicks} ${button}`);
    return {
      content: [{ type: "text", text: `Scrolled ${direction} ${clicks} clicks` }],
    };
  }
);

// ── Helpers ─────────────────────────────────────────────────────────────────

async function findWindow(name: string): Promise<string | undefined> {
  try {
    const raw = await exec(
      `xdotool search --name "${name.replace(/"/g, '\\"')}" 2>/dev/null`
    );
    const ids = raw.trim().split("\n").filter(Boolean);
    return ids.length > 0 ? ids[0] : undefined;
  } catch {
    return undefined;
  }
}

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

// ── Start ───────────────────────────────────────────────────────────────────

const transport = new StdioServerTransport();
await server.connect(transport);
