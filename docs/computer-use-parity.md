# Computer-use parity roadmap

Deskpal is a Linux MCP server for controlling X11/Xwayland applications. This
roadmap compares it with Claude Code and Claude Desktop computer use as
documented on 2026-07-17:

- <https://code.claude.com/docs/en/computer-use>
- <https://code.claude.com/docs/en/desktop#let-claude-use-your-computer>

The comparison is about the computer-control engine, not Claude's surrounding
product UI, account entitlements, browser connector, or model-side safety
classifier.

## Current parity

| Capability | Claude computer use | Deskpal | Status |
|---|---|---|---|
| Screenshot screen/window | Yes, automatically downscaled | Yes; optional `maxWidth`/`maxHeight` with source-coordinate metadata | Parity+ |
| Click, type, key chords, scroll, drag | Yes | Yes | Parity |
| Launch and focus applications | Yes | Yes | Parity |
| Resize windows | Yes | Yes | Parity |
| Window/app discovery | Approved apps only | EWMH top-level windows with `WM_CLASS`; `includeAll` for recursive dialog/helper discovery | Different, now cleaner |
| One controller at a time | Machine-wide session lock | Lazy per-user machine lock for visible-desktop mutations | Parity for arbitration |
| Stop current action | Global Esc or Ctrl+C | Client cancellation/stdio shutdown and per-tool timeouts | Missing global hotkey |
| App approval | Prompt once per app/session | MCP-host approval only; no per-app prompt | Missing |
| App permission tiers | Browser view-only; terminal/IDE click-only; other apps full control | All visible apps get the configured tool surface | Missing |
| Denied apps | Configurable in Desktop | Not implemented | Missing |
| Hide unrelated apps | Yes, restore after turn/session | Private Xvfb sessions avoid the visible desktop; visible mode does not hide apps | Different |
| Exclude agent terminal from screenshots | Yes | Not implemented | Missing |
| Clipboard approval | Requested separately | Clipboard tools are always exposed; host MCP policy may still prompt | Missing app-level policy |
| Prompt-injection safety | Model-side classifier and action review | No content classifier; relies on model/host policy | Outside server alone |
| Native Wayland windows | N/A on Linux (built-in computer use unavailable) | Input can use uinput, but discovery/capture target X11/Xwayland only | Major Linux gap |
| Private verification environment | No documented equivalent | Goal-aware Xvfb sessions with scoped tools and process-group cleanup | Deskpal advantage |
| OCR text targeting | Vision model points/clicks | Local Tesseract `click_text`, `read_screen_text`, and tooltip diffing | Deskpal advantage |
| Headless/automation use | Interactive Claude sessions only | Standard MCP stdio server and deterministic E2E harness | Deskpal advantage |
| Third-party/Linux availability | Built-in is unavailable on Linux/3P | Native Linux MCP server | Deskpal advantage |

## Shipped architecture

- **Desktop backend:** X11/Xwayland discovery and screenshots, uinput/XTest input.
- **Private backend:** child deskpal server under Xvfb, routed by `sessionId`.
- **App identity:** `_NET_CLIENT_LIST`, title, `WM_CLASS`, PID, geometry.
- **Arbitration:** first visible-desktop mutation or process launch acquires a
  per-user, per-display advisory lock until the MCP process exits. Read-only
  tools and interactions inside an existing isolated session do not claim it.
- **Vision:** screenshots remain full resolution unless bounds are requested.
  A downscaled result reports source/image dimensions and coordinate scale.
- **Safety floor:** explicit missing windows never fall back to another active
  app; arbitrary file and shell access remain opt-in flags.

## North-star milestones

### M1: Policy engine and app identity

Build policy as a separate layer above display backends. It must make decisions
on a stable app identity (backend, class/app-id, executable, PID, title), not on
window title alone.

Deliverables:

- `policy.c` with `view`, `click`, and `full` tiers.
- Config schema for denied apps and fixed tier overrides.
- Tool-dispatch authorization before any screenshot/input/clipboard action.
- Policy decisions returned as structured MCP errors.
- Tests proving denied apps cannot be targeted indirectly by window ID.

An MCP server cannot display Claude's native approval card by itself. A
production per-app prompt needs either an MCP elicitation/client capability or
a small trusted local approval UI. Until that exists, fail closed for rules
that require approval rather than silently auto-approving.

### M2: Operator control and lifecycle

- Global Esc abort monitor that consumes the key and cancels the current
  action without releasing the session lock.
- Explicit `release_control`/status tools or MCP lifecycle integration.
- Owner metadata robust enough to identify the competing host/session.
- Optional notification while control is held.
- Cancellation-aware OCR, waits, and long input sequences.

### M3: Visible-session privacy

- Snapshot stacking/minimized state.
- Hide or minimize non-approved apps while controlling the desktop.
- Restore state reliably on normal completion, abort, crash, and parent death.
- Mask denied/sensitive windows in full-screen captures.
- Exclude the controlling terminal/IDE from screenshots when requested.

This requires a compositor/window-manager adapter. Do not implement it as
best-effort `xdotool` calls without crash recovery and state restoration tests.

### M4: Native Wayland backend

Create a backend interface before adding compositor-specific code:

- window enumeration and app identity
- capture screen/window/region
- focus/activate/resize where supported
- pointer and keyboard injection
- capability reporting

Target order:

1. X11 backend extracted from current code.
2. `xdg-desktop-portal` ScreenCast/Screenshot capture and RemoteDesktop input.
3. GNOME Shell integration for window enumeration/activation where portals do
   not expose per-window control.
4. KDE/KWin adapter.
5. Capability-aware fallback with explicit unsupported errors.

Portal sessions are asynchronous and permission-bearing, so they belong in a
long-lived backend object rather than shell-command fallbacks.

Current GNOME evidence: System Monitor's main window runs under Xwayland, but
"Search for Open Files" is rendered as a native-Wayland dialog. X11 exposes
only an unmapped compatibility window, so discovery, capture, OCR, and direct
dialog input are reported as BLOCKED in `test/e2e_desktop.py` rather than
misreported as product regressions.

### M5: Safety-aware orchestration

Some Claude behaviors are model/client responsibilities, not display-server
features:

- prefer MCP/Bash/browser tools before generic screen control
- classify prompt injection in visual content
- require confirmation for external side effects
- warn when an approved app implies shell/filesystem/system-setting access

Deskpal should expose enough structured identity and action metadata for a host
to enforce these rules. It should not claim equivalent safety merely because
it can move the pointer.

## Test gates for every milestone

Each milestone must add deterministic tests before enabling behavior by
default:

- unit or protocol tests for policy and schemas
- nested-Xvfb E2E tests for interaction and crash recovery
- sanitizer run (`npm run test:asan`)
- live X11/Xwayland smoke test when backend behavior changes
- backend-specific native Wayland test with a clear skip reason when the
  compositor capability is unavailable

A skip is not a pass for release claims. Record the environment and report the
capability as unverified.

## Validation baseline (2026-07-17)

- Deterministic nested-display suites: all pass.
- ASan + UBSan deterministic suites: all pass.
- General live desktop suite: 16 pass, 0 fail, 5 BLOCKED (all native-Wayland
  dialog operations).
- Canonical System Monitor suite: 39 direct passes, 0-1 compositor-popup OCR
  cases BLOCKED per run, and no deterministic failures after keyboard fallback
  for the focused `Refresh` row. Which popup row is visible to X11 capture can
  vary by compositor frame; deterministic OCR interaction remains covered by
  the nested Tk suite.
