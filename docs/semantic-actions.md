# Capture-bound semantic actions

`agent_semantic_press`, `agent_semantic_set_text`,
`agent_semantic_set_value`, `agent_semantic_select`, and
`agent_semantic_replace_text_range` are Deskpal's first end-to-end non-pointer
action routes. They connect an exact `get_app_state`
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
  mutation supplies its own exact-value verification; or
- for `agent_semantic_set_value`, one finite value within the freshly observed
  minimum/maximum and aligned to its minimum increment. Numeric verification is
  exact within a `1e-6` tolerance; or
- for `agent_semantic_select`, one direct child index within the freshly
  observed AT-SPI selection container. Verification checks that exact child is
  selected; exclusivity is guaranteed only when the control itself is
  single-select; or
- for `agent_semantic_replace_text_range`, ordered Unicode character offsets
  and replacement text. Deskpal reads the full bounded value, computes one
  resulting string, rechecks that the source text is unchanged immediately
  before dispatch, performs one AT-SPI set operation to avoid partial
  delete/insert outcomes, and verifies the exact result without echoing the
  observed or derived full text. Text beyond the
  bounded 4096-character read/offset limit is rejected. Because the safe atomic
  implementation writes one resulting plain-text value, it does not preserve
  rich-text spans, caret position, or selection; rich-text editors are not yet
  supported by this route.

Deskpal revalidates the captured X11 identity and geometry, refreshes the exact
AT-SPI window, filters it to the captured PID, re-resolves the complete locator,
and verifies an AT-SPI-to-stage transform against the X11 frame. It then moves
the logical cursor to the fresh semantic bounds. Because toolkit object paths
can be replaced as proxies are recreated, final invoke dispatch re-resolves the
fresh node by its exact non-empty role/name and rejects ambiguity immediately
before mutation. The requested postcondition is always verified.

The result reports the `atspi` route, operation, cursor result, semantic target
and transform, underlying action result, mutation and verification state, and
whether focus or X11 stacking changed. `sharedPointerMoved` and
`clipboardChanged` are always false for these routes. Stacking is compared as
exact before/after `_NET_CLIENT_LIST_STACKING` order when the window manager
publishes a complete snapshot; otherwise it remains explicitly unknown.
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
- Focus changes are measured when X11 focus is knowable. Stacking preservation
  is proven only when complete before/after EWMH lists are available; otherwise
  `stackingKnown` is false and `stackingChanged` remains null.
- These first slices implement invoke/press, verified checkbox toggles through
  press, whole-value text replacement, numeric/range value mutation, and direct
  child selection, Unicode text-range replacement, and expandable controls
  through advertised actions plus the `expanded` state. Scroll and menu-specific
  routes remain blocked as described below.
- Semantic `Component.scroll_to` was probed against deterministic GTK3 and GTK4
  scrolled fixtures on this development desktop. Both reported failure or an
  unknown outcome and did not make the target showing, so Deskpal does not
  advertise a scroll route or fall back to wheel, keyboard, or scrollbar input.
  A supported target toolkit must be identified before this operation can ship.
- GTK3 menu-button/popover probing could verify the menu button's `checked`
  state after opening, but the exposed menu item remained `showing: false` with
  invalid offscreen bounds. Deskpal therefore does not advertise menu-item
  navigation or bypass its showing/bounds preconditions. Native detached and
  popover menu coverage remains blocked pending a trustworthy semantic tree.
