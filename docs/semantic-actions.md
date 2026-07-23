# Capture-bound semantic actions

`agent_semantic_press` and `agent_semantic_set_text` are Deskpal's first
end-to-end non-pointer action routes. They connect an exact `get_app_state`
observation, the logical agent cursor, and a verified AT-SPI mutation without
falling back to XTest, uinput, forced focus, coordinate clicking, keyboard
input, or clipboard replacement.

## Contract

The caller supplies:

- a stable window `captureId` from `get_app_state`;
- a complete semantic target locator copied from that observation (`role`,
  `path`, `busName`, `objectPath`, and `processId`);
- for `agent_semantic_press`, one action advertised by that target, such as
  `click`, plus an explicit text and/or state verification selector; or
- for `agent_semantic_set_text`, the exact replacement value. AT-SPI text
  mutation supplies its own exact-value verification.

Deskpal revalidates the captured X11 identity and geometry, refreshes the exact
AT-SPI window, filters it to the captured PID, re-resolves the complete locator,
and verifies an AT-SPI-to-stage transform against the X11 frame. It then moves
the logical cursor to the fresh semantic bounds. Because toolkit object paths
can be replaced as proxies are recreated, final invoke dispatch re-resolves the
fresh node by its exact non-empty role/name and rejects ambiguity immediately
before mutation. The requested postcondition is always verified.

The result reports the `atspi` route, operation, cursor result, semantic target
and transform, underlying action result, mutation and verification state, and
whether focus changed. `sharedPointerMoved` and `clipboardChanged` are always
false for these routes.
Unknown action outcomes are returned as unknown and are never retried blindly.

## Transform verification

AT-SPI reports logical frame coordinates while X11/Xwayland and GNOME stage
coordinates may be physical pixels. On GNOME with server-side decorations, the
AT-SPI frame can include a title bar that the X11 client geometry excludes.
Deskpal therefore derives one uniform scale from the shared frame/client width,
then requires bounded left and bottom alignment and records the top-decoration
inset. Nonuniform, missing, or unaligned bounds fail before cursor movement or
application mutation.

## Current limitations

- The route supports exact X11/Xwayland windows with stable PID/class identity;
  native-Wayland-only targets require the future compositor broker.
- Capture-bound cursor movement currently requires one monitor covering the
  full GNOME stage.
- The target must expose a complete, fresh AT-SPI locator, usable bounds, a
  non-empty accessible name unique within the exact app/window scope, and the
  requested named action.
- The GNOME logical-cursor extension must be available. The tool does not run
  the semantic mutation invisibly when the indicator cannot be verified.
- Verification is mandatory. Command completion alone is never success.
- The window identity and geometry are rechecked after cursor motion, but an
  internal control can still relayout between the fresh semantic read and the
  invoke. Postcondition verification detects action failure; accessibility
  events/frame diffs are planned to tighten this interval further.
- Focus changes are measured when X11 focus is knowable. Stacking changes are
  currently reported as unknown rather than falsely claimed unchanged.
- These first slices implement invoke/press and whole-value text replacement.
  Toggle, selection, generic value, text-range, scroll, menu, and
  expandable-control routes remain future work.
