# GNOME agent indicator prototype

This is the first visual slice of the desktop-broker plan. It draws logical
agent cursors in GNOME Shell without moving the system pointer, focusing an app,
raising a window, or injecting input.

It currently supports GNOME Shell 42, matching the development desktop.

## Install and verify

```bash
npm run test:indicator
npm run indicator:install
npm run indicator:demo
```

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

## D-Bus contract

The session service `org.deskpal.Indicator` exports
`/org/deskpal/Indicator` with these methods:

- `Ping`
- `GetStatus`
- `ShowCursor(cursorId, x, y, color, label)`
- `MoveCursor(cursorId, x, y)`
- `MoveCursorStyled(cursorId, x, y, color, label)`
- `HideCursor(cursorId)`
- `ClearAll`
- `ListCursors`

Mutation methods enforce the caller's D-Bus ownership for production `dp-`
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
real delivery route separately.

## Deskpal and Pi hello world

A full-screen `screenshot` now returns a short-lived `captureId`. Deskpal exposes
`agent_cursor_status`, `agent_cursor_move`, and `agent_cursor_hide`; the Pi
extension registers them with the usual `deskpal_` prefix. Move coordinates are
pixels in the captured image, including when that image was downscaled. Deskpal
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
