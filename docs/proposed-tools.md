# Proposed deskpal tools

Tracks tool gaps surfaced by real-world automation work. Each entry has a
use case, a proposed signature, and rough implementation notes so the
gap can be picked up later without re-deriving context.

If you're an agent and you hit a gap not listed here while running the
[OTelux self-verify skill](https://github.com/b1tank/otelux/blob/main/.agents/skills/self-verify/SKILL.md)
(or any other deskpal-driven automation), append a new section below.

---

## 1. `get_clipboard` / `set_clipboard` ✅ shipped

**Status**: shipped in [commit](../csrc/tools.c) — see `tool_get_clipboard` /
`tool_set_clipboard`. Shells out to `wl-paste`/`wl-copy` (Wayland) →
`xclip` → `xsel` in priority order; returns an "install one of…" error
if none are on PATH. No CLI flag required.

**Use case**: Verify that a "click to copy" UI actually wrote to the OS
clipboard. Today the agent has to drop out of deskpal entirely and
shell out to `xclip`/`wl-paste`, which is brittle across X11/Wayland and
splits the run between two tool surfaces.

**Surfaced by**: OTelux self-verify §2.2 (EndpointBar URL copy).

**Shipped signature**:

```jsonc
{
  "name": "get_clipboard",
  "inputSchema": {
    "type": "object",
    "properties": {
      "selection": { "type": "string", "enum": ["clipboard", "primary"], "default": "clipboard" }
    }
  },
  "returns": { "text": "string", "isImage": false }
}

{
  "name": "set_clipboard",
  "inputSchema": {
    "type": "object",
    "required": ["text"],
    "properties": {
      "text": { "type": "string" },
      "selection": { "type": "string", "enum": ["clipboard", "primary"], "default": "clipboard" }
    }
  }
}
```

**Implementation notes**:
- X11: use XCB selection requests (`xcb_convert_selection`,
  `xcb_get_selection_owner`) against `CLIPBOARD`. Easier first pass:
  shell out to `xclip -selection clipboard -o` / `-i` and
  `wl-paste` / `wl-copy` (Wayland), guarded by env detection (already
  done elsewhere in tools.c — see screenshot fallbacks).
- Should NOT block waiting for an owner; return empty string + ok=true
  if no owner is set.

---

## 2. `click_image` / `click_at_window_coords` / `click_aria_label`

**Use case**: Click icon-only buttons OCR can't read reliably (⚙, ✕, →,
▶, GTK header-bar buttons, hamburger menus). Today `click_text("⚙")`
fails about half the time on dark themes, and the agent has to compute
coordinates from `get_window_geometry` + `read_screen_text` arithmetic.

**Surfaced by**: OTelux self-verify §2.4 (cog), §3.1 (modal ✕).

**Proposed signatures**:

```jsonc
// Option A — image template matching
{
  "name": "click_image",
  "inputSchema": {
    "type": "object",
    "required": ["template"],
    "properties": {
      "template": { "type": "string", "description": "path to a small PNG template (e.g. the cog icon)" },
      "window": { "type": "string" },
      "threshold": { "type": "number", "default": 0.8, "description": "match confidence 0..1" },
      "button": { "type": "string", "enum": ["left","right","middle"], "default": "left" }
    }
  }
}

// Option B — known window coordinates (cheapest to implement)
{
  "name": "click_at_window_coords",
  "inputSchema": {
    "type": "object",
    "required": ["window", "x", "y"],
    "properties": {
      "window": { "type": "string" },
      "x": { "type": "integer" },
      "y": { "type": "integer" },
      "button": { "type": "string", "enum": ["left","right","middle"], "default": "left" }
    }
  }
}

// Option C — Electron/web only: aria-label
{
  "name": "click_aria_label",
  "inputSchema": {
    "type": "object",
    "required": ["label"],
    "properties": {
      "label": { "type": "string" },
      "window": { "type": "string" }
    }
  },
  "description": "For Electron/web windows: connect to the window's --remote-debugging-port, find the element by [aria-label], click via CDP synthetic events. Best for icons we authored with aria-label."
}
```

**Implementation notes**:
- B is trivial — `click` already exists with absolute screen coords;
  this just adds window-relative-to-screen translation, which
  `get_window_geometry` already returns. Easiest 80% win.
- A is the most general; use OpenCV `matchTemplate` (already linked? if
  not, leptonica's `pixCorrelationBinary` could work) or implement a
  small SSD/SAD matcher. Need to handle HiDPI (template captured at 1x,
  screen rendered at 2x).
- C requires the target app to expose a CDP endpoint, which is
  Electron-specific. Useful but not general.

---

## 3. `read_file` (and maybe `write_file`) ✅ shipped (read-only)

**Status**: `read_file` shipped — see `tool_read_file`. Gated behind
`deskpal --allow-fs`; off by default. Hard-coded deny list for
`/etc/shadow`, `/etc/sudoers`, `/root/`, `/proc/self/maps` even with
`--allow-fs`. `maxBytes` capped at 16 MiB and reports `TRUNCATED` in
the header when output is clipped. `write_file` not implemented.

**Use case**: Verify the contents of a config file the app just wrote
(e.g. `~/.config/otelux/settings.json`). Today the agent has to shell
out to `cat`, which means the run isn't pure-deskpal.

**Surfaced by**: OTelux self-verify §5 (settings persistence).

**Proposed signature**:

```jsonc
{
  "name": "read_file",
  "inputSchema": {
    "type": "object",
    "required": ["path"],
    "properties": {
      "path": { "type": "string" },
      "encoding": { "type": "string", "enum": ["utf8", "base64"], "default": "utf8" },
      "maxBytes": { "type": "integer", "default": 1048576 }
    }
  },
  "returns": { "content": "string", "size": "integer", "truncated": "boolean" }
}
```

**Security**: This expands deskpal's blast radius — an MCP server that
can read arbitrary files is sensitive. Consider:
- Allowlist a base directory the agent declared up-front, OR
- Refuse paths under `~/.ssh`, `~/.aws`, `/etc/shadow`, etc., OR
- Make this a launch-time `--allow-fs <prefix>` flag.

**Implementation notes**: ~20 lines of C. The interesting work is the
security boundary above.

---

## 4. `exec` (run a short shell command) ✅ shipped

**Status**: shipped — see `tool_exec`. Gated behind
`deskpal --allow-exec`; off by default. Runs via `fork`/`execl
"/bin/sh", "-c", …` so it's not affected by popen-shell escaping
quirks. Deadline enforced with `setitimer(SIGALRM)` + `SIGTERM`;
`timeoutMs` is capped at 60 000. Output (stdout + stderr merged) is
capped at 64 KiB; the header reports byte count, exit code, and
whether the command timed out.

**Use case**: Run `ss -ltnp`, `curl`, `pkill`, etc. from inside deskpal
so the whole run is a single MCP session. Today the agent splits between
the deskpal MCP server and a separate bash tool, which is awkward when
deskpal is the user-facing automation surface.

**Surfaced by**: OTelux self-verify §1.1 (port check), §6/§10 (curl),
§11/§12 (`ss`).

**Proposed signature**:

```jsonc
{
  "name": "exec",
  "inputSchema": {
    "type": "object",
    "required": ["command"],
    "properties": {
      "command": { "type": "string" },
      "timeoutMs": { "type": "integer", "default": 5000 },
      "cwd": { "type": "string" }
    }
  },
  "returns": { "stdout": "string", "stderr": "string", "exitCode": "integer", "timedOut": "boolean" }
}
```

**Security**: Same concern as `read_file`. Same mitigation options.
**Strong recommendation**: ship this gated behind a CLI flag
(`deskpal --allow-exec`) that's off by default.

**Implementation notes**: `popen`/`waitpid` with a `SIGALRM` deadline.
~40 lines.

---

## 5. `hover_text` (and a tooltip-aware variant) ✅ shipped

**Status**: shipped — see `tool_hover_text`. Locates the target word
with OCR inside the named window, snapshots OCR before/after the hover,
and returns only the words that became newly visible (matched by text
+ ±20 px centre drift to absorb OCR jitter). The after-snapshot OCRs
the full screen because tooltips render as separate override-redirect
windows that fall outside the host window's bounds. Default
`settleMs=800`.

**Use case**: Move the cursor over an OCR-found element and wait long
enough for a tooltip to render, then return what's newly visible. Today
the agent does `mouse_move` + `sleep N` + `screenshot` + `read_screen_text`
which is verbose and order-sensitive.

**Surfaced by**: OTelux self-verify §2.1 (status-dot tooltip).

**Proposed signature**:

```jsonc
{
  "name": "hover_text",
  "inputSchema": {
    "type": "object",
    "required": ["text"],
    "properties": {
      "text": { "type": "string" },
      "window": { "type": "string" },
      "settleMs": { "type": "integer", "default": 800, "description": "how long to wait for tooltip" }
    }
  },
  "returns": {
    "moved": "boolean",
    "newText": "array of {x,y,w,h,text} for text that became visible while hovering"
  }
}
```

**Implementation notes**:
- Reuse OCR machinery to locate the target.
- Snapshot `read_screen_text` *before* the move, sleep `settleMs`,
  snapshot *after*, return the diff. Returning the diff (vs. the full
  text) tells the agent exactly what the tooltip is.

---

## 6. `get_focused_element` / `accessibility_action` ✅ shipped

**Use case**: After `key_press("Tab")` ×N, assert which control is now
focused. OCR can sometimes see focus rings on themes with strong focus
styling, but it's brittle and Electron's focus ring is often a 1-px
accent border that OCR can't resolve.

**Surfaced by**: OTelux self-verify §3.6 (Tab cycle through modal).

**Shipped focused-element signature**:

```jsonc
{
  "name": "get_focused_element",
  "inputSchema": {
    "type": "object",
    "properties": {
      "application": { "type": "string", "minLength": 1, "maxLength": 512 },
      "window": { "type": "string", "minLength": 1, "maxLength": 512 },
      "includeText": { "type": "boolean", "default": false }
    },
    "anyOf": [
      { "required": ["application"] },
      { "required": ["window"] }
    ]
  },
  "returns": "Bounded JSON in MCP text content. Ambiguous or incomplete lookups omit element."
}
```

**Implementation notes**:
- X11 AT-SPI (`libatspi-2.0`) gives accessibility-tree introspection
  including focused element. Most GTK/Qt apps participate.
- Electron exposes rich AT-SPI trees when renderer accessibility is enabled,
  for example with `--force-renderer-accessibility`; otherwise it may expose
  only an application/frame shell.
- Accessible names and optional text are application-controlled, untrusted
  content. Password and unknown-role text are never returned.

`accessibility_action` extends the same scoped locator model with verified
mutation:

```jsonc
{
  "application": "accessibility_app",
  "window": "Settings",
  "target": { "role": "push button", "name": "Apply" },
  "operation": "invoke",
  "action": "click",
  "verify": {
    "role": "label",
    "name": "Status",
    "textEquals": "Saved"
  },
  "timeoutMs": 1000
}
```

Also supported:

- `setText` with automatic exact text verification
- `focus` with automatic focused-state verification
- short-lived `path` selectors returned by `get_accessibility_tree`, together
  with the locator's `busName`, `objectPath`, and `processId`

Every mutation binds to a live AT-SPI object identity, re-resolves that exact
target immediately before acting, and takes
the visible-desktop control lock. Generic invokes are rejected without an
explicit `textEquals` and/or state postcondition. Password/unknown-role,
DEFUNCT, ambiguous, stale, incomplete, or unverified operations return errors
rather than falling back to coordinates. A missing state on a DEFUNCT object
cannot satisfy an expected-false postcondition.

RPC timeouts are modeled explicitly: `mutationIssued` records whether the call
was sent, `actionOutcomeUnknown` records a late/failed reply, and verification
still polls the bound postcondition. A verified postcondition can make an
unknown transport outcome successful; otherwise callers are warned not to
retry blindly. Observed text is compared internally and is not echoed.

Mutation `application` and `window` scopes are exact accessible names, unlike
the substring filters used for read-only tree inspection. Copy exact scope and
identity fields from a returned locator when using a path selector.
Semantic mutations share the machine-wide visible-desktop lock with pixel
input. Private Xvfb children inherit the parent's locked descriptor and
validate the active lock-file identity; `--xvfb-child`, ambient variables,
fake descriptors, and PATH shadowing are insufficient.

---

## 7. `tail_log` *(nice-to-have)* — subsumed by `exec`

`exec` is shipped, so `tail_log` is fully covered by
`exec({command: "tail -n 20 /tmp/foo.log"})`. Closing without
implementing a dedicated tool.

---

## 8. `click_text` / `click` don't reach Chromium DOM under Wayland ✅ fixed

**Status**: fixed in commit `741ad96` (verified end-to-end against
OTelux). Notes kept because the underlying input-routing pitfalls are
worth being aware of for future tools.

**Symptom (pre-fix)**: deskpal's `click` and `click_text` moved the
cursor to the target on screen but the Electron/Chromium renderer
never received a DOM `mousedown`/`click` event. Confirmed via CDP by
installing a capture-phase listener
(`document.addEventListener("click", …, true)`) and observing `[]`
after a `click_text` that deskpal reported as successful with the
correct bbox. Side-by-side, `xdotool mousemove … click 1` at the same
screen coordinate produced a DOM event with `isTrusted: true`, so the
renderer was reachable — just not by deskpal.

**Two distinct bugs were hiding behind one symptom**:

1. *Delivery.* deskpal's click path was `uinput` (kernel input device)
   → `libinput` → mutter → `wl_pointer` → client. Under Wayland the
   click only dispatches if the compositor has already transitioned
   *pointer focus* to the target window; uinput's motion+click
   sequence delivered the motion fine (visible cursor) but the
   immediate `BTN_LEFT` press/release did not always race-win the
   focus transition for Xwayland clients. `xdotool` works because it
   talks `XTEST` directly to the X server, which synthesizes events
   on a specific X window without depending on Wayland-side focus
   state.

2. *Aim.* `xdo_get_window_location` returns a coordinate that does
   not match the renderer's content origin for Mutter-managed
   Electron windows (observed ~90 screen-px offset on HiDPI). Adding
   `info.x`/`info.y` to OCR-relative coords therefore aimed the
   uinput cursor at the wrong place — *even when* uinput's click
   would have been delivered. The OCR screenshot of the window is
   captured in content-origin coordinates, but xdo_get_window_location
   returns a different reference frame.

**Side note**: native modal dialogs (Electron's "Error" `MessageBox`)
**did** receive deskpal clicks pre-fix — those go through GTK/X11
directly, not through Chromium's renderer pipeline.

**Fix**: on Wayland (`$WAYLAND_DISPLAY` set), route every click /
mousemove through `xdotool mousemove --window WID X Y click …`.
xdotool uses the X server's own coordinate system for the target
window, which is the same space our screenshots/OCR work in, so both
problems disappear at once. Pure-X11 sessions keep the existing
uinput fast path.

Touched functions: `x11_is_wayland`, `x11_window_click`,
`x11_window_mouse_move`, `x11_click`, `tool_click`, `tool_click_text`
(Pass 1), `tool_mouse_move`, `tool_hover_text`.

**Open follow-up**: investigate `libei` (XDG RemoteDesktop portal),
which is the supported Wayland path for synthetic input, so that we
can remove the xdotool dependency on pure-Wayland sessions where no X
server is present.

---

## Conventions for new entries

- One section per gap. Same headings: **Use case**, **Surfaced by**,
  **Proposed signature**, **Implementation notes**.
- Cite the concrete automation work that surfaced the gap so reviewers
  can see how often it actually bites.
- Don't pre-bikeshed: a rough sketch is fine; the API can be refined
  during implementation.
- If a gap turns out to be a duplicate of an existing tool or a usage
  mistake, delete the section rather than crossing it out.
