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

## Engineering quality guardrail

The roadmap is cumulative: a checked box must not leave behind a second identity
model, lifecycle owner, parser, timeout policy, cleanup path, or test-only copy
for later phases to work around. At every milestone boundary:

1. review the complete affected path for duplication, stale code/docs, unclear
   ownership, over-broad modules, and test/production drift;
2. refactor immediately when the completed proof reveals a concrete reusable
   boundary or when the next slice would otherwise copy or further entangle the
   existing implementation;
3. remove the superseded path in the same change and keep one canonical source
   of truth;
4. keep interfaces narrow, state and cleanup ownership explicit, work bounded,
   and failure behavior closed; and
5. finish with deterministic tests, sanitizer coverage where required, current
   contracts/roadmap, a complete diff review, and one coherent commit before
   starting the next milestone.

Refactoring is not a periodic rewrite and abstraction is not a goal by itself.
Restructure only for an observed maintainability or reliability reason, make the
smallest defensible change, and preserve the product's identity, privacy,
arbitration, side-effect, and verification guarantees throughout.

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
- [x] Release Pi's visible-desktop control lease when an agent run settles;
      refuse release while isolated sessions or held mouse input depend on it.
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
  - [x] Verify a temporary Slack Xwayland launch with renderer accessibility,
        exact X11 identity, stable capture ID, and a verified 2x semantic transform.
- [ ] Add accessibility events, stable signatures, element diffs, frame-settle
      detection, and capture-bound pixel verification.
  - [x] Ship privacy-safe semantic revisions and bounded same-target element
        diffs through `get_app_state(previousCaptureId)`.
  - [x] Fail semantic diffs closed when either projection is incomplete.
  - [x] Extract canonical projection, revision, completeness, and diff ownership
        into `csrc/semantic_state.c` so event-driven observation can reuse it.
  - [x] Compare exact EWMH stacking order around semantic mutations when the
        window manager exposes complete before/after snapshots.
  - [x] Add normalized source-frame revisions and a reusable bounded pixel-diff
        engine with explicit tolerance and non-comparable geometry results; see
        [`frame-state.md`](frame-state.md).
  - [x] Ship capture-bound `wait_for_frame_stable` with anchor-based stability,
        exact-window revalidation, cancellation, bounded sampling/tolerance,
        transition summaries, and truthful timeout/error results.
  - [x] Ship `verify_frame_change` for explicit source-region postconditions
        against bounded retained projections, with inside/outside thresholds and
        truthful `actionAttributed: false` reporting.
  - [x] Bind one capture-preflighted X11 click and explicit regional visual
        postcondition into `click_and_verify_frame_change`, requiring visible-
        desktop `foregroundAllowed`, measuring side effects, and distinguishing
        shared-seat from private-session delivery.
  - [ ] Repeat the same bound verification contract over a trusted background
        compositor route before claiming non-interfering pixel-action control.
  - [ ] Prove bounded temporary AT-SPI listener registration, GLib dispatch,
        cancellation, exact-target filtering, and cleanup before exposing a
        semantic-change wait tool; use revision polling only if the listener
        lifecycle cannot be made reliable, and report that fallback truthfully.
    - [x] Extract an opaque begin/wait/end listener lifecycle with one shared
          semantic-window identity and registration across repeated wakeups.
          Prove bounded GLib dispatch, exact-window filtering, callback
          cancellation, timeout, accounting, deregistration, and canonical
          revision verification in private Xvfb; retain identity in stable
          captures. See [`semantic-events.md`](semantic-events.md).
    - [x] Compose an internal capture-bound loop that revalidates exact X11
          identity and geometry, re-observes after wakeups, continues past
          unchanged revisions under one registration, rejects accessible-window
          replacement, and returns the existing bounded diff or incomplete-
          projection result.
    - [x] Add bounded transport-safe MCP cancellation with queued-request
          preservation, disconnect cancellation, Pi graceful-cancel fallback,
          and listener disconnect/process-termination cleanup coverage.
    - [x] Expose `wait_for_semantic_change` with retained capture validation,
          bounded event accounting, canonical diffs, truthful timeout and
          cancellation outcomes, and no input or desktop-state mutation.
- [ ] Add `backgroundOnly` and `foregroundAllowed` delivery policy plus
      structured `backgroundUnavailable` errors.
- [ ] Add per-app capability policy, untrusted-content marking, high-impact
      confirmation hooks, ownership status, and immediate stop.

### Phase 2 — prove non-interfering host control

- [x] Specify the broker protocol, threat model, grant ownership, replacement-
      safe surface identity, capability negotiation, frame/action binding,
      cancellation/outcome states, limits, errors, and verification evidence;
      see [`broker-protocol.md`](broker-protocol.md).
- [x] Audit installed Mutter 42.9 and upstream main for an exact covered-surface
      input primitive. Window streams map coordinates, but RemoteDesktop/libei
      still use virtual devices and global scene picking; stock GNOME must report
      `backgroundUnavailable`. See
      [`gnome-broker-feasibility.md`](gnome-broker-feasibility.md).
- [x] Add backend-neutral broker surface identity, capability, stable error,
      and operation-state foundations with fail-closed transition tests; no
      compositor mutation or advertised background capability.
- [ ] Design and test a minimal privately maintained Mutter change for an
      authorized agent pointer context and direct surface-local click in a nested
      compositor; stop if Wayland/Xwayland protocol semantics or non-interference
      cannot be kept. Never submit or push this work upstream/publicly, never
      vendor it into Deskpal, and install it only to an isolated local prefix.
  - [x] Prove a caller-filtered secondary `wl_seat` visible to one exact client,
        with pointer-only capability and disconnect cleanup.
  - [x] Prove standard pointer enter/motion/press/frame/release/frame/leave/frame
        delivery to an authorized mapped XDG toplevel while the human seat's
        pointer focus remains unchanged; reject another client, stale generation,
        invalid input-region coordinates, hidden/unmapped/modal, and Xwayland
        targets.
  - [x] Add private unique operation IDs, replay rejection, accepted/dispatching/
        dispatched/completed/cancelled/unknown/failed/revoked states, idempotent
        pre-dispatch cancellation, generation recheck, revocation, input-region,
        human-grab, and pointer-constraint refusal.
  - [x] Prove in nested Mutter that a fully covered authorized XDG target changes
        its committed application buffer while a same-geometry foreground window
        remains focused/topmost; preserve frame rectangles, workspaces, human-seat
        pointer focus, and foreground fixture state.
  - [x] Bind private operations to one caller identity, the seat's canonical
        client, exact surface generation, pointer/background capabilities, and
        monotonic expiry; synchronously revoke pending operations on explicit
        revocation, expiry, surface destruction, or caller disconnect.
  - [ ] Connect those grants to authenticated transport; add protected-surface
        and restart generations, explicit popup/drag/constraint/cancellation/
        unknown-outcome evidence, clipboard/pointer-position evidence, and
        broker-stream frame verification before exposing any Deskpal capability.
- [ ] Add the optional, read-only GNOME Shell bridge defined in
      [`shell-bridge-protocol.md`](shell-bridge-protocol.md): bounded native-
      Wayland window enumeration, replacement-safe Shell-scoped identity,
      monitor layout, capability negotiation, and separate GNOME 42–44 and
      45+ artifacts. Keep the existing indicator visual-only and advertise no
      capture, window-management, or input capability.
  - [x] Add the GNOME 42 bridge service plus a bounded, versioned native D-Bus
        client/parser with malformed, oversized, and incompatible-response
        refusal tests.
  - [x] Integrate validated bridge capability and instance status into
        environment discovery without contacting the host bus from private
        sessions.
  - [x] Add native-Wayland records to visible-desktop window listing with full
        Shell-instance identity, omit Shell-observed Xwayland duplicates, and
        keep private sessions isolated from the host bus.
  - [x] Prove the GNOME 42 bridge in a private nested compositor with a native-
        Wayland fixture: enumerate it through D-Bus and Deskpal, preserve outer
        pointer/focus/stacking state, and invalidate the Shell instance on
        extension restart.
  - [x] Deterministically package the runtime-accepted GNOME 42 legacy artifact
        and a gated GNOME 45–50 ES-module test artifact from one implementation.
  - [ ] Run live acceptance on GNOME 45+ before removing its experimental
        installer gate or making a runtime support claim.
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
