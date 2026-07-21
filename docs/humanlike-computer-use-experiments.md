# Human-like computer-use experiments

Date: 2026-07-19  
Environment: Ubuntu 22.04, GNOME Shell 42.9, Wayland, 3840x2160 at 2x
display scale

This report tests the main engineering ideas behind lower-friction desktop
control. It is based on live experiments, not API availability alone.

## Executive result

AT-SPI is the highest-value next slice. It can provide semantic discovery,
true child focus, direct text mutation, actions, state, bounds, and event
feedback for accessible applications, including native-Wayland GTK windows.
It does not replace screenshots or compositor integration:

- accessibility is disabled by session policy on this machine
  (`NO_AT_BRIDGE=1`, toolkit accessibility off)
- normal Slack, VS Code, and Claude instances expose only app/frame shells
- a disposable VS Code started with renderer accessibility forced exposed 450
  semantic nodes instead of three
- native Wayland does not expose trustworthy global window placement through
  AT-SPI, so semantic bounds cannot be directly overlaid on physical desktop
  screenshots

The target architecture should be hybrid: AT-SPI first, portal capture and
input when approved, X11/Xwayland preserved, and OCR/pixels as a verified
fallback.

## Measured experiments

### Native-Wayland semantic control

A GTK3 fixture was launched with `GDK_BACKEND=wayland`. It was absent from the
X11 window tree. With the accessibility bridge disabled, it was also absent
from AT-SPI. Relaunching only the fixture with `NO_AT_BRIDGE` removed and
`GTK_MODULES=gail:atk-bridge` exposed:

- 16 semantic nodes before dynamic insertion
- exact roles and names for the entry, status label, button, and checkbox
- logical screen bounds for all visible controls
- true focus on the editable entry
- `EditableText`, `Action`, `Text`, `Component`, and state interfaces

Coordinate-free actions succeeded:

- set entry text through `EditableText`
- invoke the button's `click` action
- verify the changed status through `Text`
- move child focus through `Component.grab_focus`
- toggle a checkbox and verify `CHECKED`

This proves a native-Wayland semantic backend is feasible when an app exposes a
complete accessibility tree.

### Real-application coverage

| Application mode | Semantic nodes | Result |
|---|---:|---|
| Native-Wayland GTK fixture, bridge enabled | 19 after dynamic insertion | Rich |
| GNOME Terminal | 179 | Rich, including terminal focus and menus |
| Normal VS Code | 3 | App/frame shell only |
| Normal Slack | 2 | App/frame shell only |
| Normal Claude Desktop | 2 | App/frame shell only |
| Native-Wayland VS Code with `--force-renderer-accessibility` | 450 | Rich |

Electron accessibility is therefore an app-launch/deployment capability, not
something deskpal can assume for already-running instances.

### Focus and continuous feedback

AT-SPI emitted `object:state-changed:focused` events for entry, button, and
checkbox transitions. Events contained semantic name, role, focused state, and
object path. Gain/loss events arrived within roughly 2-9 ms of each other in
the experiment.

This can replace screenshot-based focus-ring inference for accessible apps.
Production code still needs a cached event stream, disconnect handling, and a
snapshot fallback because applications may omit events.

### Identifier stability

Object paths and semantic path hashes were stable across repeated reads, focus
changes, and one fixture process restart. They were not stable across dynamic
sibling insertion: inserting one control changed the index-derived ID of every
following sibling.

Do not expose a tree path as a permanent element ID. Use:

1. a short-lived opaque handle for one tree generation
2. a semantic locator containing application, window, role, name, attributes,
   and ancestor context
3. re-resolution before mutation
4. ambiguity errors when a locator does not resolve uniquely

AT-SPI object paths may be cached within a live application but must be treated
as invalidatable handles.

### Verified action transaction

A prototype transaction performed:

1. unique semantic resolution
2. precondition capture
3. text mutation and action invocation
4. event monitoring
5. immediate and 10 ms polling checks
6. structured postcondition result

Five idempotent runs all verified successfully in 5.4-10.2 ms (7.9 ms mean).
An event-only implementation initially waited the full three-second timeout on
idempotent actions, because no state-change event is required when the desired
state already exists. Verification must combine immediate checks, events, and
bounded polling.

An intentionally broad selector matched six buttons and was refused as
ambiguous without invoking any action.

### Wayland capture and input portals

This desktop exposes:

- ScreenCast portal v4
- monitor and window source types
- embedded, metadata, and hidden cursor modes
- RemoteDesktop portal v1
- pointer, keyboard, and touch device types

The generic Screenshot portal created a native-Wayland authorization dialog.
The dialog was fully visible through AT-SPI and its Share action succeeded,
returning a real 3840x2160 PNG. An unparented MCP process logged that the portal
window could not be associated with a parent window. Portal authorization and
session lifetime therefore need an explicit host/user UX; they are not a
headless screenshot primitive.

RemoteDesktop `CreateSession` and pointer/keyboard `SelectDevices` succeeded.
`Start` displayed a semantic authorization dialog with an "Allow remote
interaction" switch. The experiment did not grant desktop-wide control, so
Start timed out intentionally. The remaining implementation is protocol and
session engineering, plus a user-approved authorization flow, rather than API
discovery.

AT-SPI reported native-window bounds in logical coordinates with an origin of
`(0,0)`, while the portal screenshot was in physical compositor pixels and the
window appeared elsewhere. Wayland intentionally withholds global placement.
Pixel fallback needs a ScreenCast window stream or compositor-specific window
identity/placement; a global scale factor is insufficient.

## Recommended implementation slices

### Slice 1: AT-SPI capability and read-only tools (1-2 engineer-weeks)

- optional `atspi-2` Meson dependency
- long-lived accessibility backend object
- cheap backend availability reporting
- scoped `get_accessibility_tree` and `get_focused_element`
- bounded traversal, stale-object handling, and privacy filtering
- deterministic GTK fixture tests

This is the smallest useful production slice and should be implemented first.

Implemented on 2026-07-20: optional `atspi-2` build integration,
`accessibility_status`, bounded `get_accessibility_tree`, and
`get_focused_element`, with deterministic private D-Bus/AT-SPI fixture tests.
The Slice 1 inspection tools are read-only, preserve existing X11 routing, do not enable host
accessibility settings, expose semantic locators as short-lived paths, omit
text/attributes by default, redact password text, and mark semantic content as
untrusted application output.

### Slice 2: Semantic actions and verified transactions (2-3 weeks)

- semantic locator and short-lived handle model
- unique re-resolution before mutation
- action, editable-text, selection, value, and component operations
- preconditions and typed postconditions
- event cache plus immediate/poll fallback
- fail-closed ambiguity and stale-handle errors
- GTK, GNOME Terminal, and forced-accessibility Electron tests

After this slice, accessible apps should have substantially fewer focus and
coordinate failures.

Partially implemented on 2026-07-20: `accessibility_action` performs uniquely
resolved `setText`, child `focus`, and named `invoke` operations. Path selectors
carry a live bus/object/process identity so same-index replacements fail
closed, and mutation app/window scopes use exact accessible names. It captures
preconditions, requires explicit text/state verification for generic invokes,
re-resolves postcondition targets, combines immediate checks with bounded
10 ms polling, uses the visible-desktop control lock, and fails closed for
ambiguous, protected, stale, incomplete, or unverified targets. Deterministic
coverage includes idempotent text, button/checkbox actions, path locators,
verification timeout, lock contention, protected/unknown roles, and isolated
session rejection. Late mutation replies are represented as unknown outcomes;
postcondition polling continues, and an unverified unknown outcome must not be
retried blindly. DEFUNCT verification objects are rejected before false-state
postconditions can become no-op successes. Private Xvfb children share the
parent's inherited kernel lock after validating the active lock-file identity;
they do not receive an ambient flag-based bypass.

Still remaining in Slice 2: selection/value interfaces, a retained AT-SPI event
cache, richer ancestor/attribute locators, and live forced-accessibility
Electron/GNOME Terminal acceptance beyond the deterministic GTK fixture.

### Slice 3: Backend extraction and routing (2-4 weeks)

- extract current X11 operations behind a backend interface
- route per operation: semantic, X11/Xwayland, portal, then OCR/pixel fallback
- normalize app/window/control identity without pretending IDs are universal
- expose backend and verification metadata in every result
- preserve all current Xvfb and visible-X11 behavior

This carries the highest regression risk because current tool logic directly
couples resolution, capture, focus, input, and OCR.

### Slice 4: Portal ScreenCast capture (3-5 weeks)

- asynchronous request/session state machine
- user authorization and reconnect behavior
- PipeWire stream consumption and frame conversion
- monitor/window stream selection
- cursor metadata and logical/physical transform handling
- cancellation, timeout, and parent-window strategy

Single screenshots can use the Screenshot portal earlier, but repeated action
verification needs a retained ScreenCast stream.

### Slice 5: Portal RemoteDesktop input (2-4 weeks after ScreenCast)

- approved session lifecycle and device negotiation
- pointer, keyboard, scroll, and touch dispatch
- stream-relative absolute coordinates
- keycode/keysym mapping and modifier cleanup
- visible abort, ownership, and session status
- portal revocation and compositor restart handling

Portal input should complement semantic actions, not replace them.

### Slice 6: GNOME/KDE window adapters (4-8 weeks per compositor family)

Needed for capabilities portals and AT-SPI do not supply reliably:

- complete top-level window enumeration and stable app identity
- global placement and stacking
- activation/minimize/maximize where AT-SPI fails
- compositor-specific dialogs and surfaces without accessibility

This is the expensive part of "any app" support and cannot be solved by one
cross-desktop API today.

## Overall effort and limits

For one experienced Linux desktop engineer, slices 1-5 (semantic control,
verified actions, backend routing, ScreenCast, and RemoteDesktop) total
approximately 10-18 engineer-weeks, including tests and hardening. A complete
GNOME window-management adapter is another 4-8 weeks; adding KDE at comparable
quality is another 4-8 weeks. Reaching "no barrier for any app" is not a finite
compatibility claim: inaccessible custom canvases, games, protected surfaces,
remote desktops, and apps that disable accessibility will always require
pixel/input fallback and explicit capability reporting.

A realistic goal is:

- semantic, verified actions for accessible apps
- approved portal capture/input for native Wayland
- compositor adapters for window management
- current X11/Xwayland support retained
- OCR/pixels as a measured fallback
- every mutation reports what backend acted and how success was verified

That target materially approaches human-like use without claiming universal
control that the platform does not expose.