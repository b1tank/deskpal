# Computer-use parity roadmap

Deskpal is a Linux MCP server for controlling X11/Xwayland applications and
semantically accessible native-Wayland controls through optional AT-SPI. This
roadmap compares it with Claude Code and Claude Desktop computer use as
documented on 2026-07-17:

- <https://code.claude.com/docs/en/computer-use>
- <https://code.claude.com/docs/en/desktop#let-claude-use-your-computer>

The comparison is about the computer-control engine, not Claude's surrounding
product UI, account entitlements, browser connector, or model-side safety
classifier.

See [humanlike-computer-use-experiments.md](humanlike-computer-use-experiments.md)
for measured AT-SPI, native-Wayland portal, semantic identity, focus-event,
and verified-action prototypes with production effort estimates.
The product north star and retirement policy live in [plan.md](plan.md). This
document records current parity and validation evidence; where older
compatibility mechanisms differ from that plan, `plan.md` is authoritative.

## Current parity

| Capability | Claude computer use | Deskpal | Status |
|---|---|---|---|
| Screenshot screen/window | Yes, automatically downscaled | Yes; optional `maxWidth`/`maxHeight` with source-coordinate metadata | Parity+ |
| Click, type, key chords, scroll, drag | Yes | Yes | Parity |
| Launch and focus applications | Yes | Yes | Parity |
| Resize windows | Yes | Yes | Parity |
| Window/app discovery | Approved apps only | EWMH top-level windows with `WM_CLASS`; `includeAll` for recursive dialog/helper discovery | Different, now cleaner |
| Semantic element discovery/action | Native app accessibility integration | Bounded AT-SPI inspection plus capture-bound press/toggle/expand, whole/range text, numeric value, and direct-child selection mutation without pointer/keyboard/clipboard fallback | Partial parity |
| One controller at a time | Machine-wide session lock | Lazy per-user machine lock for visible-desktop mutations | Parity for arbitration |
| Stop current action | Global Esc or Ctrl+C | Client cancellation/stdio shutdown and per-tool timeouts | Missing global hotkey |
| App approval | Prompt once per app/session | MCP-host approval only; no per-app prompt | Missing |
| App permission tiers | Browser view-only; terminal/IDE click-only; other apps full control | All visible apps get the configured tool surface | Missing |
| Denied apps | Configurable in Desktop | Not implemented | Missing |
| Hide unrelated apps | Yes, restore after turn/session | Private Xvfb sessions avoid the visible desktop; visible mode does not hide apps | Different |
| Exclude agent terminal from screenshots | Yes | Not implemented | Missing |
| Clipboard approval | Requested separately | Clipboard tools are always exposed; host MCP policy may still prompt | Missing app-level policy |
| Prompt-injection safety | Model-side classifier and action review | No content classifier; relies on model/host policy | Outside server alone |
| Native Wayland windows | N/A on Linux (built-in computer use unavailable) | Accessible controls can be inspected/acted through AT-SPI; complete window discovery and pixel capture still require portals/compositor adapters | Major Linux gap narrowed |
| Private verification environment | No documented equivalent | Goal-aware Xvfb sessions with scoped tools and process-group cleanup | Deskpal advantage |
| OCR text targeting | Vision model points/clicks | Local Tesseract `click_text`, `read_screen_text`, and tooltip diffing | Deskpal advantage |
| Headless/automation use | Interactive Claude sessions only | Standard MCP stdio server and deterministic E2E harness | Deskpal advantage |
| Third-party/Linux availability | Built-in is unavailable on Linux/3P | Native Linux MCP server | Deskpal advantage |

## Shipped architecture

- **Compatibility desktop backend:** X11/Xwayland discovery and screenshots,
  with shared-seat uinput/XTest input. This is shipped behavior, not the target
  interaction architecture.
- **Semantic backend:** optional AT-SPI inspection and verified actions for
  accessible controls. Locators are short-lived and re-resolved before every
  mutation; protected/ambiguous/incomplete targets fail closed. The first
  cursor-coupled press route is limited to exact X11/Xwayland identity,
  one-monitor transform verification, named accessible controls, and explicit
  postconditions; see [semantic-actions.md](semantic-actions.md).
- **Private backend:** child deskpal server under Xvfb, routed by `sessionId`.
- **App identity:** `_NET_CLIENT_LIST`, title, `WM_CLASS`, PID, geometry.
- **Arbitration:** first visible-desktop mutation or process launch acquires a
  per-user machine-wide advisory lock until the MCP process exits. Read-only
  tools and interactions inside an existing isolated session do not claim it.
- **Vision:** screenshots remain full resolution unless bounds are requested.
  A downscaled result reports source/image dimensions and coordinate scale.
- **Safety floor:** explicit missing windows never fall back to another active
  app; arbitrary file and shell access remain opt-in flags. Semantic names and
  optional text/attributes are untrusted application output. DEFUNCT semantic
  verifiers cannot satisfy false-state postconditions. Private children share
  the parent's validated inherited kernel lock rather than bypassing it.

## Direction

The product roadmap is maintained in [plan.md](plan.md). In short: prefer
verified semantic actions, then a trusted compositor broker that addresses an
exact surface without changing the human pointer, focus, or stacking. The
backend-neutral security and evidence contract is specified in
[broker-protocol.md](broker-protocol.md). The stock-GNOME source audit in
[gnome-broker-feasibility.md](gnome-broker-feasibility.md) confirms that
ScreenCast window coordinates still feed globally picked virtual input, so a
reviewed Mutter change is required for the covered-window gate. Keep
shared-seat X11, portal input, forced focus, and forced XWayland only as
explicit compatibility fallbacks while those routes are replaced and retired.

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
- Canonical System Monitor suite: 39 direct passes, 0-2 compositor-popup OCR
  cases BLOCKED per run, and no deterministic failures after keyboard fallback
  for the focused `Refresh` row. Which popup row is visible to X11 capture can
  vary by compositor frame; deterministic OCR interaction remains covered by
  the nested Tk suite.

## Claude Desktop Cowork acceptance (2026-07-18)

A fresh Claude Desktop Cowork task exercised deskpal through the actual MCP
bridge, rather than calling the server directly. The environment was Claude
Desktop 1.22209.0 with embedded Claude Code 2.1.209 on Ubuntu GNOME Wayland.
Claude Desktop was launched with its X11 backend because native-Wayland windows
are outside deskpal's current discovery and capture backend.

The task used `launch_isolated_app`, passed `sessionId="xvfb-1"` to every
subsequent tool, and reported 10/10 PASS:

| Scenario | Observed result |
|---|---|
| Isolated launch and scoped discovery | Fixture found at 720x520 |
| Bounded screenshot | Source 720x520, image 360x260, scale 2.0x/2.0x |
| Initial OCR | Exact `Status: ready` match |
| Click, key chord, typing, and click-by-text | Exact `claude desktop deskpal ok` status |
| Tooltip OCR | `Deskpal tooltip` appeared after the scoped hover |
| Geometry and resize | 640x440 and restored 720x520 both verified |
| Scroll | Three downward clicks completed |
| Clipboard | Exact `claude-desktop-clipboard` round trip |
| Fixture close | Scoped window list confirmed the window was gone |
| Session close | Fixture and Xvfb processes both exited |

The run also confirmed two host-boundary gaps:

- Claude Desktop's Cowork permission bridge remained authoritative for local
  MCP calls. A user-level Claude Code rule for `mcp__deskpal__*` did not
  suppress Desktop prompts. "Allow for this task" approved subsequent calls
  of the same tool type, but did not blanket-approve other tools from the
  deskpal server; each newly used tool type still prompted. Classic
  `mcpServers` configuration has no supported per-server `toolPolicy`, so this
  cannot be changed in deskpal's MCP config.
- Stopping a permission-blocked Cowork task did not call
  `close_isolated_session` and did not terminate the shared deskpal MCP host.
  The fixture and Xvfb process group therefore remained alive until explicitly
  cleaned up. Explicit session close and MCP-process exit still clean up
  correctly. A robust automatic fix needs task cancellation/ownership from the
  host or a carefully designed session lease; eagerly closing all sessions on
  any interrupted request could break another client using the shared server.

For current integrations, request explicit session close in a cleanup step and
verify that no fixture/Xvfb process remains after an interrupted host task.
