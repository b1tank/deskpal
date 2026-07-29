# GNOME agent indicator prototype

This is the first visual slice of the desktop-broker plan. It draws logical
agent cursors in GNOME Shell without moving the system pointer, focusing an app,
raising a window, or injecting input.

GNOME Shell 42 is the currently runtime-accepted target, matching the
development desktop. The deterministic packager also emits a GNOME 45–50
ES-module artifact for explicit compatibility testing, but that artifact is not
a runtime support claim until its live acceptance suite passes on those Shell
versions.

## Install and verify

```bash
npm run test:indicator
npm run indicator:package
npm run indicator:install
npm run indicator:demo
```

Packaging creates:

```text
dist/deskpal-shell-extension-gnome42.zip
dist/deskpal-shell-extension-gnome45-50.zip
```

The GNOME 42 artifact retains the legacy `imports.*`/`init()` entry point. The
GNOME 45–50 artifact is generated deterministically from the same implementation
with ES-module imports and a default `Extension` export. Static tests inspect
both archives, enforce exact contents and metadata, syntax-check both entry
points, and prove repeated builds are byte-identical. Automatic installation of
the modern artifact is refused unless
`DESKPAL_EXPERIMENTAL_GNOME_EXTENSION=1` is explicitly set for testing.

GNOME Shell 42 on Wayland discovers a newly installed local extension only when
the Shell session starts and caches its JavaScript until that session ends. If
installation asks for a restart, or after changing the extension code, log out
and back in once, then run `scripts/indicator.sh enable` followed by the demo.

The demo shows blue `agent-1` and orange `agent-2` cursors moving independently.
Move the physical mouse during the demo: it must remain independent, and the
focused window must not change.

Clear or remove the prototype with:

```bash
npm run indicator:clear
scripts/indicator.sh uninstall
```

## D-Bus contracts

The package contains two deliberately separate read-only/visual services. The
session service `org.deskpal.Indicator` exports
`/org/deskpal/Indicator` with these methods:

- `Ping`
- `GetStatus`
- `ShowCursor(cursorId, x, y, color, label)`
- `MoveCursor(cursorId, x, y)`
- `MoveCursorStyled(cursorId, x, y, color, label)`
- `HideCursor(cursorId)`
- `ClearAll`
- `ListCursors`

The read-only `org.deskpal.ShellBridge1` interface exports bounded capability,
native-window, and monitor-layout metadata as specified in
[`shell-bridge-protocol.md`](shell-bridge-protocol.md). It exposes no capture,
activation, movement, resize, or input method.

Indicator mutation methods enforce the caller's D-Bus ownership for production `dp-`
IDs. `ClearAll` removes that caller's owned cursors plus manual/demo cursors; it
never clears another live process's owned cursor.

Coordinates are GNOME Shell logical stage coordinates. `GetStatus` returns the
stage and monitor geometry, scale, coordinate-space identity, and cursor state.
Each cursor reports its target and rendered coordinates, monotonic movement
sequence, and `moving` or `idle` state so callers can synchronize with the
animation. Colors must be `#RRGGBB`; invalid colors use a safe default. Cursor
IDs are limited to 64 letters, digits, dots, underscores, or hyphens. Labels
are plain text and are truncated to 48 characters.

## Acceptance

A visual run passes only when:

1. both colored cursors are visible and move independently;
2. the physical pointer does not move unless the human moves it;
3. the focused window and stacking do not change;
4. the overlay never consumes clicks;
5. `clear`, extension disable, and Shell cleanup remove every actor.

The overlay is only an ownership indicator. It must never be presented as proof
that a future action used non-interfering input; action results must report the
real delivery route separately. It must also remain input-free: do not add
virtual-device, capture, focus, activation, or surface-control methods to this
D-Bus interface. The trusted broker is a separate compositor security boundary
defined in [`broker-protocol.md`](broker-protocol.md).

## Deskpal and Pi hello world

A full-screen `screenshot` and stable exact-window `get_app_state` observation
return a short-lived `captureId`. Deskpal exposes `agent_cursor_status`,
`agent_cursor_move`, and `agent_cursor_hide`; the Pi extension registers them
with the usual `deskpal_` prefix. Move coordinates are pixels in the captured
image, including when that image was downscaled. Deskpal
rejects unknown, evicted, stale, out-of-bounds, or geometry-mismatched captures
before issuing an indicator mutation.

Cursor IDs are namespaced to one Deskpal process. Production IDs are bound to
the caller's unique D-Bus name; the Shell removes them on disconnect, including
process crashes and forced termination. Status filters out cursors owned by
other processes, and hide cannot remove them. Unprefixed IDs retain the manual
demo behavior and are not part of the production ownership contract.

Move results keep `indicatorMoved` and `inputDelivered` separate and report
mutation, unknown-outcome, verification, and desktop side-effect fields
explicitly. The first milestone supports one monitor covering the full GNOME
stage. Multi-monitor and mixed-scale layouts fail closed until Deskpal has a
per-monitor capture transform.
