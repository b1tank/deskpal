# Chromium and Electron accessibility setup

Date verified: 2026-07-24
Environment: Ubuntu 22.04, GNOME 42 Wayland, AT-SPI enabled

Deskpal never changes desktop accessibility settings or relaunches user
applications silently. This document records an explicit user-run Slack
experiment and a read-only VS Code Insiders baseline.

## Session prerequisites

The host already had the required GNOME session settings:

```text
org.gnome.desktop.interface toolkit-accessibility = true
NO_AT_BRIDGE unset
GTK_MODULES=gail:atk-bridge
```

No global setting was changed.

## Slack experiment

Before restart, Slack exposed one accessible frame node and one actionable
object even with a depth-10, 300-node query. Renderer controls were absent.

The user explicitly quit/restarted Slack with:

```bash
/usr/lib/slack/slack --force-renderer-accessibility
```

The new main process was confirmed to contain the flag. After restart, the same
bounded query exposed 24 nodes and 24 actionable objects, including:

- native title-bar controls;
- a menu bar and named menu buttons with `open` actions;
- nested panels;
- a `document web` subtree and web sections.

This proves that `--force-renderer-accessibility` materially improves Slack's
AT-SPI coverage for the current session. Starting the command while an old
Slack process is still alive may delegate to that instance and not change its
renderer; a complete application restart is required.

### Renderer-accessibility-only limitations

- The flag was applied for that launch only. No desktop file, autostart entry,
  wrapper, or profile setting was modified.
- With `--ozone-platform=wayland`, the semantic tree became rich but Deskpal
  could not bind it to exact X11/Xwayland image identity for `get_app_state`.
- Xwayland validation was therefore performed as a separate approved
  experiment, recorded below.
- Deep Chromium trees can exceed bounded depth/node budgets. Partial/truncated
  metadata must be honored; callers should narrow exact application/window
  scopes and request only the depth they need.
- Accessible names and all optional text/attributes are application-controlled,
  potentially sensitive, and untrusted.

To return to the normal launch configuration, fully quit Slack and launch it
without the flag.

## Slack Xwayland experiment

With separate explicit approval, Deskpal gracefully stopped Slack and relaunched
the same profile for one session with:

```bash
env -u WAYLAND_DISPLAY \
  XDG_SESSION_TYPE=x11 \
  ELECTRON_OZONE_PLATFORM_HINT=x11 \
  /usr/lib/slack/slack \
  --force-renderer-accessibility \
  --ozone-platform=x11
```

The process tree and renderer both reported `--ozone-platform=x11`. Deskpal then
resolved the exact Slack window as:

```text
windowId: 0xe00004
class: slack
processId: 900024
geometry: 104,128 3736x2032
```

A stable `get_app_state` observation succeeded with a 1920x1044 image and a
short-lived capture ID. The exact-PID semantic tree included native controls,
the menu bar, and a `document web` subtree. The AT-SPI-to-stage transform was
verified at scale 2 with zero offset against the X11 frame, and focus/identity/
geometry remained stable during capture.

This clears both prerequisites for capture-bound semantic actions on this Slack
launch: renderer accessibility and exact X11/Xwayland surface identity.

### Xwayland limitations

- This remains a one-session launch. No persistent Slack launcher was changed.
- The bounded app-state tree was partial/truncated at the requested depth/node
  budget; callers must re-observe with a scope and budget suitable for the
  intended control.
- The experiment performed read-only observation only. No Slack control was
  invoked because a harmless, explicit postcondition was not selected.
- Xwayland compatibility is not the final architecture. Native-Wayland exact
  identity still requires the compositor broker.
- Returning to a normal Slack launch removes both temporary flags and may return
  the app to native Wayland with frame-only renderer accessibility.

## VS Code Insiders baseline

VS Code Insiders already exposed a Chromium accessibility subtree without a
special launch flag. A depth-10 read-only query reached its `document web`
subtree and actionable sections. Shallower queries mostly returned native
window controls, demonstrating that traversal depth and node budgets—not only
launch configuration—determine apparent coverage.

VS Code Insiders was not restarted and no configuration was changed.

## Product consequence

Renderer accessibility setup and exact surface identity are separate gates.
The first Slack experiment cleared only renderer accessibility; the approved
Xwayland experiment cleared both for one launch. Until the compositor broker
exists, full capture-bound semantic actions require this X11/Xwayland identity
in addition to a rich AT-SPI tree.
