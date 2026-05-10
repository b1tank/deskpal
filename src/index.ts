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
  version: "0.2.0",
});

// ── Helpers ─────────────────────────────────────────────────────────────────

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

/**
 * Get the display scale factor (1 for normal, 2 for HiDPI).
 * Tries multiple detection methods.
 */
async function getScaleFactor(): Promise<number> {
  try {
    const raw = await exec(
      `gsettings get org.gnome.desktop.interface text-scaling-factor 2>/dev/null || echo "1.0"`
    );
    // Also check GDK_SCALE env
    const gdkScale = process.env.GDK_SCALE;
    if (gdkScale) return parseInt(gdkScale);

    // Try xrandr to detect actual vs logical
    const xrandr = await exec(`xrandr --query 2>/dev/null | head -5`);
    const physMatch = xrandr.match(/(\d+)x(\d+)\+/);
    const currMatch = xrandr.match(/current (\d+) x (\d+)/);
    if (physMatch && currMatch) {
      const ratio = parseInt(physMatch[1]!) / parseInt(currMatch[1]!);
      if (ratio > 1.5) return 2;
    }

    return Math.round(parseFloat(raw.trim()) || 1);
  } catch {
    return 1;
  }
}

interface WindowInfo {
  id: string;
  title: string;
  x: number;
  y: number;
  width: number;
  height: number;
  screen: number;
  pid: string;
}

/**
 * Get detailed window info. Returns parsed position and geometry.
 */
async function getWindowInfo(wid: string): Promise<WindowInfo | null> {
  try {
    const [nameRaw, geoRaw, pidRaw] = await Promise.all([
      exec(`xdotool getwindowname ${wid} 2>/dev/null`),
      exec(`xdotool getwindowgeometry --shell ${wid} 2>/dev/null`),
      exec(`xdotool getwindowpid ${wid} 2>/dev/null`),
    ]);

    const vars: Record<string, string> = {};
    for (const line of geoRaw.trim().split("\n")) {
      const [k, v] = line.split("=");
      if (k && v) vars[k] = v;
    }

    return {
      id: wid,
      title: nameRaw.trim(),
      x: parseInt(vars["X"] || "0"),
      y: parseInt(vars["Y"] || "0"),
      width: parseInt(vars["WIDTH"] || "0"),
      height: parseInt(vars["HEIGHT"] || "0"),
      screen: parseInt(vars["SCREEN"] || "0"),
      pid: pidRaw.trim(),
    };
  } catch {
    return null;
  }
}

/**
 * Find window by name. Filters out tiny windows (< 10x10),
 * prefers exact title match, falls back to substring.
 */
async function findWindow(name: string): Promise<string | undefined> {
  try {
    const raw = await exec(
      `xdotool search --onlyvisible --name "${name.replace(/"/g, '\\"')}" 2>/dev/null`
    );
    const ids = raw.trim().split("\n").filter(Boolean);
    if (ids.length === 0) return undefined;

    // Filter out tiny windows and find best match
    let bestMatch: WindowInfo | null = null;

    for (const wid of ids.slice(0, 20)) {
      const info = await getWindowInfo(wid);
      if (!info) continue;

      // Skip tiny windows (hidden GDK helpers, etc.)
      if (info.width < 10 || info.height < 10) continue;

      // Exact title match is preferred
      if (info.title === name) return wid;

      // Keep the largest window as fallback
      if (!bestMatch || info.width * info.height > bestMatch.width * bestMatch.height) {
        bestMatch = info;
      }
    }

    return bestMatch?.id;
  } catch {
    return undefined;
  }
}

/**
 * Resolve a windowId from either direct ID or name search.
 */
async function resolveWindow(
  windowId?: string,
  windowName?: string
): Promise<string | undefined> {
  if (windowId) return windowId;
  if (windowName) return findWindow(windowName);
  return undefined;
}

// ── OCR Helpers ─────────────────────────────────────────────────────────────

interface TextBox {
  text: string;
  x: number;
  y: number;
  width: number;
  height: number;
  confidence: number;
}

/**
 * Run tesseract OCR on an image file. Returns text with bounding boxes.
 * Uses TSV output for position data.
 */
async function ocrImage(imagePath: string): Promise<TextBox[]> {
  const tsvPath = `/tmp/deskpal_ocr_${Date.now()}`;
  try {
    await exec(`tesseract "${imagePath}" "${tsvPath}" -l eng --psm 3 tsv 2>/dev/null`);
    const tsv = await exec(`cat "${tsvPath}.tsv"`);
    const lines = tsv.trim().split("\n");

    const boxes: TextBox[] = [];
    for (let i = 1; i < lines.length; i++) {
      const cols = lines[i]!.split("\t");
      if (cols.length < 12) continue;
      const text = cols[11]!.trim();
      const conf = parseInt(cols[10]!);
      if (!text || conf < 30) continue; // skip low confidence and empty

      boxes.push({
        text,
        x: parseInt(cols[6]!),
        y: parseInt(cols[7]!),
        width: parseInt(cols[8]!),
        height: parseInt(cols[9]!),
        confidence: conf,
      });
    }

    await exec(`rm -f "${tsvPath}.tsv"`);
    return boxes;
  } catch {
    await exec(`rm -f "${tsvPath}.tsv" 2>/dev/null`);
    return [];
  }
}

/**
 * Find all occurrences of a text string in OCR results.
 * Handles multi-word matches by finding consecutive boxes.
 */
function findTextInBoxes(
  boxes: TextBox[],
  searchText: string
): { x: number; y: number; width: number; height: number }[] {
  const results: { x: number; y: number; width: number; height: number }[] = [];
  const searchLower = searchText.toLowerCase();
  const searchWords = searchLower.split(/\s+/);

  if (searchWords.length === 1) {
    // Single word: direct match
    for (const box of boxes) {
      if (box.text.toLowerCase().includes(searchLower)) {
        results.push({ x: box.x, y: box.y, width: box.width, height: box.height });
      }
    }
  } else {
    // Multi-word: find consecutive boxes matching the words
    for (let i = 0; i <= boxes.length - searchWords.length; i++) {
      let match = true;
      for (let j = 0; j < searchWords.length; j++) {
        if (!boxes[i + j]!.text.toLowerCase().includes(searchWords[j]!)) {
          match = false;
          break;
        }
      }
      if (match) {
        const first = boxes[i]!;
        const last = boxes[i + searchWords.length - 1]!;
        const x = first.x;
        const y = Math.min(first.y, last.y);
        const width = last.x + last.width - first.x;
        const height = Math.max(first.y + first.height, last.y + last.height) - y;
        results.push({ x, y, width, height });
      }
    }
  }

  return results;
}

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
      .describe("Window name/title substring to search for."),
    fullScreen: z
      .boolean()
      .optional()
      .default(false)
      .describe("Capture the entire screen instead of a single window."),
  },
  async ({ windowId, windowName, fullScreen }) => {
    const wid = await resolveWindow(windowId, windowName);

    const tmpFile = `/tmp/deskpal_${Date.now()}.png`;

    try {
      if (fullScreen) {
        await exec(`import -window root ${tmpFile}`);
      } else if (wid) {
        await exec(`import -window ${wid} ${tmpFile}`);
      } else {
        const activeWid = (await exec("xdotool getactivewindow")).trim();
        await exec(`import -window ${activeWid} ${tmpFile}`);
      }

      const base64 = (await exec(`base64 -w0 ${tmpFile}`)).trim();
      await exec(`rm -f ${tmpFile}`);

      return {
        content: [{ type: "image", data: base64, mimeType: "image/png" }],
      };
    } catch (e: unknown) {
      return {
        content: [{ type: "text", text: `Screenshot failed: ${(e as Error).message}` }],
      };
    }
  }
);

// ── list_windows ────────────────────────────────────────────────────────────

server.tool(
  "list_windows",
  "List all visible windows with their IDs, titles, geometry, PID, and display scale factor.",
  {
    name: z
      .string()
      .optional()
      .describe("Optional title filter — only show windows matching this substring."),
  },
  async ({ name }) => {
    const raw = await exec(
      `xdotool search --onlyvisible --name "" 2>/dev/null || true`
    );
    const wids = raw.trim().split("\n").filter(Boolean);
    const scale = await getScaleFactor();

    const windows: WindowInfo[] = [];

    for (const wid of wids.slice(0, 50)) {
      const info = await getWindowInfo(wid);
      if (!info || !info.title) continue;
      // Filter tiny windows
      if (info.width < 10 || info.height < 10) continue;
      // Apply name filter if given
      if (name && !info.title.toLowerCase().includes(name.toLowerCase())) continue;
      windows.push(info);
    }

    const text = windows.length
      ? windows
          .map(
            (w) =>
              `[${w.id}] "${w.title}" pid=${w.pid}\n` +
              `  Window ${w.id}\n` +
              `  Position: ${w.x},${w.y} (screen: ${w.screen})\n` +
              `  Geometry: ${w.width}x${w.height}`
          )
          .join("\n\n") + `\n\nDisplay scale: ${scale}x`
      : "No visible windows found";

    return { content: [{ type: "text", text }] };
  }
);

// ── find_window ─────────────────────────────────────────────────────────────

server.tool(
  "find_window",
  "Find a window by title substring. Filters out tiny helper windows and prefers exact matches.",
  {
    name: z.string().describe("Window title substring to search for"),
  },
  async ({ name }) => {
    const wid = await findWindow(name);
    if (!wid) {
      return {
        content: [{ type: "text", text: `No window found matching "${name}"` }],
      };
    }
    const info = await getWindowInfo(wid);
    return {
      content: [
        {
          type: "text",
          text: info
            ? `[${info.id}] "${info.title}"\n  Position: ${info.x},${info.y}\n  Size: ${info.width}x${info.height}\n  PID: ${info.pid}`
            : `Window found: ${wid}`,
        },
      ],
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
    const wid = await resolveWindow(windowId, windowName);
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
  "Click at a pixel position relative to a window's top-left corner. Coordinates are in the same unit as the window geometry reported by list_windows.",
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
    const wid = await resolveWindow(windowId, windowName);

    if (wid) {
      await exec(`xdotool windowactivate --sync ${wid}`);
      await sleep(100);
    }

    const targetWid = wid || (await exec("xdotool getactivewindow")).trim();
    const info = await getWindowInfo(targetWid);
    if (!info) {
      return { content: [{ type: "text", text: "Could not get window position" }] };
    }

    const absX = info.x + x;
    const absY = info.y + y;

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

// ── click_text ──────────────────────────────────────────────────────────────

server.tool(
  "click_text",
  "Find visible text on screen using OCR and click its center. " +
    "This is the most reliable way to click buttons, tabs, menu items, and labels — " +
    "no pixel coordinate guessing needed.",
  {
    text: z.string().describe("The text to find and click (case-insensitive)"),
    windowId: z.string().optional().describe("X11 window ID to search in"),
    windowName: z.string().optional().describe("Window title to search in"),
    occurrence: z
      .number()
      .optional()
      .default(1)
      .describe("Which occurrence to click (1-based) if text appears multiple times"),
    button: z
      .number()
      .optional()
      .default(1)
      .describe("Mouse button: 1=left, 3=right"),
    offset: z
      .object({
        x: z.number().optional().default(0),
        y: z.number().optional().default(0),
      })
      .optional()
      .describe("Pixel offset from the text center (e.g. to click an icon next to a label)"),
  },
  async ({ text, windowId, windowName, occurrence, button, offset }) => {
    const wid = await resolveWindow(windowId, windowName);
    const targetWid = wid || (await exec("xdotool getactivewindow")).trim();

    // Focus the window
    await exec(`xdotool windowactivate --sync ${targetWid}`);
    await sleep(150);

    // Screenshot
    const tmpFile = `/tmp/deskpal_ocr_click_${Date.now()}.png`;
    try {
      await exec(`import -window ${targetWid} ${tmpFile}`);
    } catch (e: unknown) {
      return {
        content: [{ type: "text", text: `Screenshot failed: ${(e as Error).message}` }],
      };
    }

    // OCR
    const boxes = await ocrImage(tmpFile);
    await exec(`rm -f ${tmpFile}`);

    if (boxes.length === 0) {
      return {
        content: [
          {
            type: "text",
            text: `OCR found no text in window. Is tesseract-ocr installed? (sudo apt install tesseract-ocr)`,
          },
        ],
      };
    }

    // Find the target text
    const matches = findTextInBoxes(boxes, text);

    if (matches.length === 0) {
      // Show what was found to help debugging
      const found = boxes
        .map((b) => b.text)
        .filter((t, i, a) => a.indexOf(t) === i)
        .slice(0, 30)
        .join(", ");
      return {
        content: [
          {
            type: "text",
            text: `Text "${text}" not found on screen.\n\nVisible text: ${found}`,
          },
        ],
      };
    }

    const idx = Math.min(occurrence - 1, matches.length - 1);
    const match = matches[idx]!;

    // Click the center of the matched text
    const info = await getWindowInfo(targetWid);
    if (!info) {
      return { content: [{ type: "text", text: "Could not get window position" }] };
    }

    const clickX = match.x + match.width / 2 + (offset?.x || 0);
    const clickY = match.y + match.height / 2 + (offset?.y || 0);
    const absX = info.x + clickX;
    const absY = info.y + clickY;

    await exec(`xdotool mousemove --sync ${absX} ${absY} click ${button}`);

    return {
      content: [
        {
          type: "text",
          text:
            `Clicked "${text}" (occurrence ${idx + 1}/${matches.length}) ` +
            `at (${Math.round(clickX)}, ${Math.round(clickY)}) ` +
            `in window ${targetWid}\n` +
            `Text bbox: ${match.x},${match.y} ${match.width}x${match.height}`,
        },
      ],
    };
  }
);

// ── read_screen_text ────────────────────────────────────────────────────────

server.tool(
  "read_screen_text",
  "Read all visible text from a window or screen region using OCR. " +
    "Returns text with positions. Useful for verifying UI state.",
  {
    windowId: z.string().optional().describe("X11 window ID"),
    windowName: z.string().optional().describe("Window title substring"),
    region: z
      .object({
        x: z.number(),
        y: z.number(),
        width: z.number(),
        height: z.number(),
      })
      .optional()
      .describe(
        "Crop region within the window (pixel coordinates). Omit to read entire window."
      ),
  },
  async ({ windowId, windowName, region }) => {
    const wid = await resolveWindow(windowId, windowName);
    const targetWid = wid || (await exec("xdotool getactivewindow")).trim();

    const tmpFile = `/tmp/deskpal_ocr_read_${Date.now()}.png`;
    const cropFile = `/tmp/deskpal_ocr_crop_${Date.now()}.png`;

    try {
      await exec(`import -window ${targetWid} ${tmpFile}`);

      let ocrTarget = tmpFile;
      if (region) {
        // Crop the region
        await exec(
          `convert ${tmpFile} -crop ${region.width}x${region.height}+${region.x}+${region.y} +repage ${cropFile}`
        );
        ocrTarget = cropFile;
      }

      const boxes = await ocrImage(ocrTarget);
      await exec(`rm -f ${tmpFile} ${cropFile}`);

      if (boxes.length === 0) {
        return {
          content: [{ type: "text", text: "No text detected in the specified area." }],
        };
      }

      // Group by approximate Y position (same line)
      const lines: { y: number; items: TextBox[] }[] = [];
      for (const box of boxes) {
        const existing = lines.find((l) => Math.abs(l.y - box.y) < box.height * 0.5);
        if (existing) {
          existing.items.push(box);
        } else {
          lines.push({ y: box.y, items: [box] });
        }
      }

      // Sort lines top-to-bottom, items left-to-right
      lines.sort((a, b) => a.y - b.y);
      for (const line of lines) {
        line.items.sort((a, b) => a.x - b.x);
      }

      const text = lines
        .map(
          (line) =>
            `[y=${line.y}] ${line.items.map((b) => b.text).join(" ")}`
        )
        .join("\n");

      return {
        content: [
          {
            type: "text",
            text: `OCR results (${boxes.length} words):\n\n${text}`,
          },
        ],
      };
    } catch (e: unknown) {
      await exec(`rm -f ${tmpFile} ${cropFile}`);
      return {
        content: [{ type: "text", text: `OCR failed: ${(e as Error).message}` }],
      };
    }
  }
);

// ── launch_app ──────────────────────────────────────────────────────────────

server.tool(
  "launch_app",
  "Launch a desktop application, handling GApplication D-Bus delegation. " +
    "Kills any existing instance, launches fresh, and waits for the window to appear.",
  {
    command: z.string().describe("Command to launch (e.g. 'gnome-system-monitor')"),
    args: z
      .array(z.string())
      .optional()
      .default([])
      .describe("Command arguments"),
    waitForWindow: z
      .string()
      .optional()
      .describe("Window title to wait for. Defaults to command basename."),
    timeout: z
      .number()
      .optional()
      .default(10)
      .describe("Seconds to wait for window to appear"),
    killExisting: z
      .boolean()
      .optional()
      .default(true)
      .describe("Kill existing instances before launching"),
    env: z
      .record(z.string(), z.string())
      .optional()
      .describe("Extra environment variables (e.g. {GDK_BACKEND: 'x11'})"),
  },
  async ({ command, args, waitForWindow, timeout, killExisting, env }) => {
    const basename = command.split("/").pop()!;

    // Kill existing instances
    if (killExisting) {
      await exec(`pkill -f "${basename}" 2>/dev/null || true`);
      await sleep(500);
    }

    // Build env string
    const envParts: string[] = ["DISPLAY=:0"];
    if (env) {
      for (const [k, v] of Object.entries(env)) {
        envParts.push(`${k}=${v}`);
      }
    }
    // Force new GApplication instance by clearing D-Bus address
    envParts.push('DBUS_SESSION_BUS_ADDRESS=""');

    const fullCmd = `${envParts.join(" ")} ${command} ${args.join(" ")}`;
    spawn("/bin/sh", ["-c", `${fullCmd} &>/dev/null &`]);

    // Wait for window
    const searchTitle = waitForWindow || basename;
    const deadline = Date.now() + timeout * 1000;

    while (Date.now() < deadline) {
      await sleep(500);
      const wid = await findWindow(searchTitle);
      if (wid) {
        const info = await getWindowInfo(wid);
        return {
          content: [
            {
              type: "text",
              text: info
                ? `Launched "${command}"\n[${info.id}] "${info.title}"\n  Position: ${info.x},${info.y}\n  Size: ${info.width}x${info.height}\n  PID: ${info.pid}`
                : `Launched "${command}", window: ${wid}`,
            },
          ],
        };
      }
    }

    return {
      content: [
        {
          type: "text",
          text: `Launched "${command}" but no window matching "${searchTitle}" appeared after ${timeout}s`,
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
    const wid = await resolveWindow(windowId, windowName);
    if (wid) {
      await exec(`xdotool windowactivate --sync ${wid}`);
      await sleep(100);
    }

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
  "Send keyboard shortcuts or key presses (e.g. 'Return', 'ctrl+s', 'alt+F4').",
  {
    keys: z.string().describe("Key combination (e.g. 'Return', 'ctrl+a', 'shift+Tab')"),
    windowId: z.string().optional().describe("Window to target"),
    windowName: z.string().optional().describe("Window title to target"),
  },
  async ({ keys, windowId, windowName }) => {
    const wid = await resolveWindow(windowId, windowName);
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
  "Get the position, size, and display scale factor of a window.",
  {
    windowId: z.string().optional(),
    windowName: z.string().optional(),
  },
  async ({ windowId, windowName }) => {
    let wid = await resolveWindow(windowId, windowName);
    if (!wid) {
      wid = (await exec("xdotool getactivewindow")).trim();
    }
    const info = await getWindowInfo(wid);
    const scale = await getScaleFactor();

    if (!info) {
      return { content: [{ type: "text", text: "Window not found" }] };
    }

    return {
      content: [
        {
          type: "text",
          text:
            `Window: [${info.id}] "${info.title}"\n` +
            `Position: ${info.x},${info.y}\n` +
            `Size: ${info.width}x${info.height}\n` +
            `Scale: ${scale}x\n` +
            `PID: ${info.pid}`,
        },
      ],
    };
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
    let wid = await resolveWindow(windowId, windowName);
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
        const info = await getWindowInfo(wid);
        return {
          content: [
            {
              type: "text",
              text: info
                ? `Window appeared: [${info.id}] "${info.title}" (${info.width}x${info.height})`
                : `Window appeared: ${wid}`,
            },
          ],
        };
      }
      await sleep(500);
    }
    return {
      content: [
        { type: "text", text: `Timeout: no window matching "${name}" after ${timeout}s` },
      ],
    };
  }
);

// ── mouse_move ──────────────────────────────────────────────────────────────

server.tool(
  "mouse_move",
  "Move the mouse to a position relative to a window.",
  {
    x: z.number(),
    y: z.number(),
    windowId: z.string().optional(),
    windowName: z.string().optional(),
  },
  async ({ x, y, windowId, windowName }) => {
    const wid = await resolveWindow(windowId, windowName);
    if (wid) {
      const info = await getWindowInfo(wid);
      if (info) {
        const absX = info.x + x;
        const absY = info.y + y;
        await exec(`xdotool mousemove --sync ${absX} ${absY}`);
        return {
          content: [{ type: "text", text: `Mouse moved to (${x}, ${y}) in window [abs: ${absX}, ${absY}]` }],
        };
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
    const wid = await resolveWindow(windowId, windowName);
    if (wid) {
      await exec(`xdotool windowactivate --sync ${wid}`);
      await sleep(50);
    }
    const btn = direction === "up" ? 4 : 5;
    await exec(`xdotool click --repeat ${clicks} ${btn}`);
    return {
      content: [{ type: "text", text: `Scrolled ${direction} ${clicks} clicks` }],
    };
  }
);

// ── Start ───────────────────────────────────────────────────────────────────

const transport = new StdioServerTransport();
await server.connect(transport);
