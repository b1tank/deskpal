# Deskpal test loop

Tests are split by blast radius. Run the smallest deterministic suite first.

## Safe default

```bash
npm run build
npm test
```

`npm test` runs entirely in nested/private X displays and must not manipulate
the user's visible desktop:

- `indicator_contract.py`: static GNOME extension syntax, narrow D-Bus surface,
  caller ownership hooks, click-through behavior, and absence of input APIs.
- `e2e_isolation.py`: parent/child routing, Xvfb lifecycle, cleanup, malformed
  session IDs, missing-window safety, OCR, screenshots, and binary replacement.
- `e2e_computer_use.py`: deterministic Tk workflow for app identity, environment
  capability/blocker reporting, exact capture-bound app-state observations,
  screenshot scaling metadata, screenshot/OCR/click/type/key/hover/resize/scroll/clipboard,
  controller lock contention/release, and isolated-operation independence.
- `e2e_accessibility.py`: optional AT-SPI tool schemas, unavailable capability,
  bounded semantic tree output, filtering/truncation, logical bounds, actions,
  opt-in attributes/text, privacy redaction, completion metadata, and true
  focused-element lookup. It also covers exact app-state semantic filtering,
  privacy opt-ins, unstable geometry, target replacement, verified semantic text/focus/invoke
  mutations, path and role/name resolution, ambiguity, verification timeout,
  unknown outcomes, same-path/interleaving replacement, DEFUNCT verifier
  rejection, lock contention, protected roles, and isolated-session rejection. Semantic
  coverage runs inside
  private Xvfb and D-Bus/AT-SPI sessions and does not change host accessibility
  settings.

Environment-status coverage also verifies that isolated sessions report no
shared host seat while visible sessions disclose compatibility risks.

`e2e_isolation.py` also proves that direct `--xvfb-child`, ambient headless
markers, fake inherited descriptors, and PATH-shadowed `xvfb-run` cannot forge
private control ownership. Real children inherit the parent's locked file
description and validate it before serving tools.

All suites use `deskpal_client.py`. New protocol tests should use this client
instead of copying JSON-RPC transport code.

## Sanitizers

```bash
npm run test:asan
```

This builds `build-asan/deskpal` with AddressSanitizer and UndefinedBehavior
Sanitizer, then runs the safe suites. Use this after X11 traversal, OCR result,
PNG, process, or session-lifecycle changes.

## Focused commands

```bash
npm run test:isolation
npm run test:computer-use
npm run test:accessibility
```

Set `DESKPAL_TEST_BINARY=/absolute/path/to/deskpal` to run any suite against
an alternate build.

## Live desktop tests

These tests manipulate the active desktop and installed GNOME applications.
Close important work first.

```bash
npm run test:desktop
npm run test:sysmon
npm run test:indicator-live
# or all live suites
npm run test:live
```

They require a real X11/Xwayland display, `/dev/uinput` access for full input
coverage, and the named applications. `test:indicator-live` additionally
requires the installed GNOME extension and verifies full/downscaled capture
mapping, edge placement, restyling, a capture-bound verified semantic press,
cross-process isolation, graceful and forced-death cleanup, and
pointer/focus/stacking/clipboard non-interference.
Screenshot artifacts go under `/tmp`.
Capability-dependent cases print `BLOCK` when the visible surface is native
Wayland and therefore outside the current backend; blocked is not counted as
passed.

## Claude Desktop/Cowork host acceptance

This is a manual host-integration check, not part of `npm test`. Run Claude
Desktop with its X11 backend, create a fresh Cowork task, and ask it to use only
deskpal MCP tools to perform the deterministic fixture workflow from
`e2e_computer_use.py` inside `launch_isolated_app`.

Require the task to report and verify:

- scoped window discovery and a 720x520 to 360x260 screenshot
- initial and post-edit OCR, including exact status text
- hover tooltip, 640x440 resize, and 720x520 restoration
- three downward scroll clicks and an exact clipboard round trip
- fixture disappearance followed by `close_isolated_session`

Desktop prompts on the first use of each MCP tool type. "Allow for this task"
applies to later calls of that type, not every tool exposed by the server;
validate the displayed tool and `sessionId` before approving it. If the Cowork
task is stopped before its cleanup step, explicitly close the isolated session
or stop the deskpal MCP host, then confirm no fixture or Xvfb process remains.
The live baseline and known host gaps are recorded in
`docs/computer-use-parity.md`.

## Adding coverage

1. Prefer a deterministic Tk fixture under `test/fixtures/`.
2. Run it in nested Xvfb or through `launch_isolated_app`.
3. Assert observable state after every mutation, not only tool response text.
4. Restore clipboard/window state and close all sessions in `finally` blocks.
5. Add a regression that fails before the fix.
6. Run `npm test`, then `npm run test:asan` for memory-sensitive changes.
7. Use live tests only for behavior impossible to prove in nested Xvfb.

Never route a private test through host `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`,
D-Bus, or `/dev/uinput`. A test that can move the user's real pointer is not a
safe default test.
