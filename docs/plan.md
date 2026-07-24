# Deskpal plan

## Goal

Deliver macOS-like computer use on Linux: an agent can inspect and operate an
approved app without borrowing the human cursor, keyboard focus, or foreground
window. The target is reliable behavior, not one universal low-level trick.

## Product contract

On a supported desktop, a normal action must:

- target an exact approved app, surface, and control;
- leave the human pointer, focus, and window stacking unchanged;
- work when the target is covered or on another workspace;
- bind coordinates to a fresh captured frame and verify the resulting state;
- report the route used and any human-visible side effects;
- fail clearly instead of silently escalating to disruptive input.

A colored agent cursor may show target, owner, motion, drag, and button state.
It is a visual overlay with its own `cursorId`, not the system cursor or the
input mechanism. It must never imply that an underlying shared-seat action is
independent.

## Architecture

1. **Intent router** — prefer an app API, shell, or browser-specific tool when
   the host has one; use general desktop control only for genuine GUI work.
2. **Deskpal core** — owns MCP tools, app identity, policy, OCR, action routing,
   capture freshness, cancellation, and verification.
3. **Semantic backend** — uses AT-SPI to read and operate named controls without
   pointer movement. It is the default GUI route when the tree is trustworthy.
4. **Desktop broker interface** — exposes narrow, authenticated operations for
   exact compositor surfaces: enumerate, capture, route input, and report
   state. IDs are backend-scoped and short-lived.
5. **GNOME/Mutter broker** — first production broker. It must deliver fallback
   pointer, keyboard, scroll, and drag actions to one surface without changing
   the human seat, focus, or stacking. Other desktops implement the same
   contract later.
6. **Capture pipeline** — combines compositor/PipeWire frames, compact semantic
   state, focus, scale, and a capture ID in one observation.
7. **Private sessions** — remain the fully independent path for disposable apps
   and tests. They do not replace control of apps already running on the host.

The broker must enforce app/surface permissions independently from Deskpal and
must not expose arbitrary compositor scripting.

## Action routing

Use the first route that can satisfy and verify the requested behavior:

1. direct app, shell, or browser operation;
2. semantic action or text/value mutation;
3. desktop-broker surface action;
4. capture-bound pixel action through a supported background route;
5. explicitly approved shared-seat foreground fallback;
6. structured refusal.

Every mutation returns its backend, target identity, verification result, and
whether it moved the shared pointer or changed focus, stacking, or clipboard.
Background-only requests never silently become foreground actions.

After an action, wait on accessibility events or frame stability rather than a
fixed delay where possible. Success means observed application state changed as
expected; command completion alone is not evidence.

## Immediate milestone — capture-bound agent cursor

Prove the first truthful Pi-to-desktop vertical slice before attempting broker
input: Pi observes the desktop, selects a point in that observation, and moves
its session-owned logical cursor to the corresponding stage position without
changing application state or the human seat.

The slice must:

- report indicator availability, stage and monitor geometry, scale, and the
  supported coordinate spaces;
- bind image coordinates to a recent capture ID and return the resolved stage
  coordinates rather than accepting unexplained global coordinates alone;
- create or move only cursors owned by the calling Deskpal session and remove
  all of them when that session ends;
- expose move completion or a sequence state so a later action can synchronize
  delivery with the animation; and
- report `indicatorMoved`, `inputDelivered`, and shared-pointer, focus, and
  stacking side effects separately. Indicator movement is never input proof.

Acceptance requires Pi to move its cursor to known points from both full-size
and downscaled captures within a small measured tolerance. Independent checks
must show no physical-pointer, focus, stacking, or application-state change,
and shutdown must leave no cursor behind. The initial supported geometry is one
monitor covering the full GNOME stage; other layouts must return a structured
unsupported error until per-monitor transforms are implemented.

This milestone does not click, type, inject input, implement the compositor
broker, or automatically decorate existing action tools. Its tools are the
public proof of the same internal cursor path that later verified semantic and
broker actions will drive.

## Delivery plan

### Phase 1 — semantic-first, observable behavior

- [x] Ship the native Pi extension with typed tools and persistent lifecycle.
- [x] Ship the capture-bound, session-owned agent-cursor milestone defined
      above, including truthful side-effect reporting and shutdown cleanup.
- [x] Add `get_environment_status`: capabilities, selected backends, blockers,
      shared-seat risks, and concrete setup actions.
- [x] Add unified [`get_app_state`](app-state.md): image, compact semantic state,
      focus, app/surface identity, scale transform, and capture ID.
- [ ] Expand verified semantic press, selection, value, text-range, scroll,
      menu, toggle, and expandable-control operations.
  - [x] Ship capture-bound verified [`agent_semantic_press`](semantic-actions.md)
        with logical-cursor motion and no shared-pointer fallback.
  - [x] Ship capture-bound verified `agent_semantic_set_text` without keyboard
        or clipboard fallback.
  - [x] Verify checkbox/toggle actions and idempotency through the capture-bound
        semantic press route.
  - [x] Ship capture-bound verified `agent_semantic_set_value` with fresh range
        and increment preconditions.
  - [x] Ship capture-bound verified `agent_semantic_select` with fresh direct
        child bounds and selected-state verification.
  - [x] Verify expandable controls and idempotency through advertised actions
        and the AT-SPI `expanded` state.
  - [x] Ship capture-bound atomic Unicode text-range replacement with exact
        resulting-text verification.
  - [ ] Identify a toolkit with working AT-SPI `Component.scroll_to`; GTK3 and
        GTK4 fixture probes currently fail, and no disruptive fallback is allowed.
  - [ ] Identify a trustworthy menu tree; GTK3 popover items remain non-showing
        with invalid bounds after the menu button reports open.
- [x] Test an explicit, user-approved session accessibility setup that lets
      Chromium/Electron apps expose rich AT-SPI trees; see
      [chromium-accessibility.md](chromium-accessibility.md). Never change the
      setting silently.
- [ ] Add accessibility events, stable signatures, element diffs, frame-settle
      detection, and capture-bound pixel verification.
- [ ] Add `backgroundOnly` and `foregroundAllowed` delivery policy plus
      structured `backgroundUnavailable` errors.
- [ ] Add per-app capability policy, untrusted-content marking, high-impact
      confirmation hooks, ownership status, and immediate stop.

### Phase 2 — prove non-interfering host control

- [ ] Specify the broker protocol, threat model, surface identity, capability
      negotiation, cancellation, and verification contract.
- [ ] Extract current X11 behavior behind the backend interface.
- [ ] Build a GNOME proof that captures and clicks a covered target surface
      while the human uses another focused window, with no pointer, focus, or
      stacking change.
- [ ] Extend the proof to typing, scroll, drag, menus, and transient dialogs.
- [ ] Add persistent compositor capture and a latest-frame cache linked to
      broker surface IDs.
- [x] Prototype colored, labeled logical cursors in GNOME Shell over a narrow
      D-Bus API; visually prove movement leaves the human pointer and focus unchanged.
- [ ] Add target borders, held-button states, ownership status, and immediate
      stop without touching the human cursor.

The covered-window proof is the architectural gate. Anything that cannot pass
it may remain a compatibility fallback but must not shape the primary API.

### Phase 3 — harden, broaden, retire

- [ ] Harden the GNOME broker against replacement, ambiguity, revocation,
      compositor restart, crashes, lock screen, and confused-deputy attacks.
- [ ] Add independent fixture-state, focus, stacking, pointer-leak, input-leak,
      screenshot, and trajectory evidence for every claimed route.
- [ ] Add KDE/KWin and other brokers only behind the proven contract.
- [ ] Make private sessions first-class and harden them with explicit
      filesystem, process, and network policy where required.
- [ ] Permit concurrent host control only when the broker proves targets and
      capabilities are independent.

## Legacy retirement

XTest, `xdotool`, global `/dev/uinput`, forced focus, forced stacking, and
forced XWayland enabled useful control before native brokers existed. They are
compatibility implementations, not the product design.

Retire them per operation:

1. instrument route and side effects;
2. reach equivalent verified coverage through semantic or broker control;
3. make the better route the default;
4. require explicit approval for the shared-seat fallback;
5. deprecate and remove the legacy path after supported brokers and private
   sessions cover its tested use cases.

Do not build cursor/focus save-and-restore, window hiding, Multi-Pointer X, or
larger `xdotool` sequences into parallel architectures. They may be bounded
experiments for compatibility evidence only.

## Platform boundary

Standard Wayland intentionally provides no safe API for an arbitrary client to
inject input into another client's private surface. Complete non-interference
therefore requires a trusted desktop-specific broker. Without one, Deskpal must
offer verified semantics, a private session, or an explicitly approved
shared-seat fallback and must report the limitation plainly.
