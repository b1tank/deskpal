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

### Slack limitations

- The flag was applied for this launch only. No desktop file, autostart entry,
  wrapper, or profile setting was modified.
- Slack still runs with `--ozone-platform=wayland`. Its semantic tree is rich,
  but Deskpal cannot yet bind it to exact X11/Xwayland image identity for
  `get_app_state` and capture-bound cursor actions.
- Relaunching with Xwayland as well as forced renderer accessibility would be a
  separate user-visible experiment requiring explicit approval.
- Deep Chromium trees can exceed bounded depth/node budgets. Partial/truncated
  metadata must be honored; callers should narrow exact application/window
  scopes and request only the depth they need.
- Accessible names and all optional text/attributes are application-controlled,
  potentially sensitive, and untrusted.

To return to the normal launch configuration, fully quit Slack and launch it
without the flag.

## VS Code Insiders baseline

VS Code Insiders already exposed a Chromium accessibility subtree without a
special launch flag. A depth-10 read-only query reached its `document web`
subtree and actionable sections. Shallower queries mostly returned native
window controls, demonstrating that traversal depth and node budgets—not only
launch configuration—determine apparent coverage.

VS Code Insiders was not restarted and no configuration was changed.

## Product consequence

Renderer accessibility setup and exact surface identity are separate gates.
The Slack experiment clears the renderer-accessibility gate but not native
Wayland capture identity. Until the compositor broker exists, full
capture-bound semantic actions require an exact X11/Xwayland window in addition
to a rich AT-SPI tree.
