# GNOME/Mutter broker feasibility audit

## Decision

Stock Mutter does not expose a compositor-enforced API that delivers pointer
buttons directly to one covered, unfocused surface while preserving the human
pointer, focus, stacking, and workspace.

Deskpal must therefore report `backgroundUnavailable` for stock-GNOME pixel
input. The existing GNOME Shell indicator must remain input-free. The next input
feasibility route is a narrowly reviewed, privately maintained local Mutter
change/plugin implementing the broker contract, not a larger Shell extension or
portal/virtual-pointer wrapper. It must never be submitted or pushed upstream.

This conclusion does not block semantic actions, private Xvfb sessions, or the
explicit shared-seat `foregroundAllowed` compatibility route.

## Audit scope

The source-level audit covered:

- the development system: GNOME Shell 42.9 and Ubuntu Mutter 42.9;
- upstream Mutter tag/archive `42.9`;
- upstream Mutter `main` at commit
  `52924b84de06c4ce01551449c2dc2d8d74ea754c` (2026-07-23); and
- installed Mutter/Clutter exported symbols and introspection data.

The Brave search integration was unavailable because this environment has no
API key, so the evidence below comes from the upstream source trees and installed
artifacts rather than search-result summaries.

## Findings

### Exact window capture exists, but is a separate capability

Mutter's private ScreenCast API supports `RecordWindow` with a compositor window
ID. In 42.9:

- `data/dbus-interfaces/org.gnome.Mutter.ScreenCast.xml` documents
  `RecordWindow` and the `window-id` property;
- `meta-screen-cast-session.c::handle_record_window` resolves that ID to a
  `MetaWindow`, or uses the focused window only when no ID was supplied;
- `MetaScreenCastWindowStream` retains that exact `MetaWindow`; and
- the stream closes when the window becomes unmanaged.

The window stream paints the compositor's window actor rather than taking a
normal desktop screenshot, so it is the relevant starting point for covered
surface capture. It is still a private, version-unstable API requiring trusted
permission/session integration and PipeWire consumption. This audit does not
mark persistent covered-window capture accepted; it only identifies a plausible
read-only primitive for the next spike.

### RemoteDesktop absolute coordinates still become global pointer motion

In Mutter 42.9, `NotifyPointerMotionAbsolute` accepts a ScreenCast stream path,
but the stream is used only to transform stream-local coordinates into absolute
stage coordinates. `meta-remote-desktop-session.c` then calls:

```text
clutter_virtual_input_device_notify_absolute_motion(..., abs_x, abs_y)
```

`NotifyPointerButton` has no stream, window, surface, or generation parameter. It
only calls:

```text
clutter_virtual_input_device_notify_button(...)
```

The current upstream implementation remains structurally the same. Its libei/EIS
path can expose a standalone window viewport and transform window-local
coordinates, but `meta-eis-client.c::handle_motion_absolute` still sends the
transformed position to a `ClutterVirtualInputDevice`; button handling uses the
same virtual device and carries no target surface.

A stream-local coordinate mapping is therefore not surface-targeted input. It is
a convenient way to move the logical/global remote pointer to the corresponding
stage location.

### Event delivery is selected by compositor picking, not by the stream

Mutter 42.9 Clutter dispatch obtains the event actor with
`clutter_stage_get_event_actor`. Pointer motion/button processing emits to the
actor associated with the virtual input device at the stage coordinates.

The Wayland pointer implementation then repicks its current surface from that
actor. In 42.9, `meta-wayland-pointer.c::repick_for_event` uses
`clutter_stage_get_device_actor`. Current upstream uses the virtual pointer
sprite's current picked actor, which has the same topmost-scene routing effect.

Consequently, if surface A is covered by B at the transformed coordinates, the
virtual pointer targets B. A window ScreenCast stream does not override picking
for the following button event.

### Shell virtual devices cannot satisfy the contract

GNOME Shell can reach Clutter virtual input APIs through Mutter introspection,
and the installed libraries export virtual pointer creation/motion/button
symbols. Those devices enter the same seat/event routing described above.
Adding such methods to `org.deskpal.Indicator` would create another global input
injector; it would not produce an independent surface cursor or covered-surface
delivery.

### No stock direct-surface button operation was found

Neither the 42.9 nor audited current RemoteDesktop interface contains a pointer
button method taking a ScreenCast stream, `MetaWindow`, Wayland surface, surface
generation, or mapping ID. The current libei window viewport changes coordinate
mapping, not button destination. No supported Mutter/Meta API was found that
bypasses scene picking and delivers a complete pointer sequence directly to an
arbitrary covered client surface.

## Capability result

| Capability | Stock Mutter 42.9 | Audited upstream main | Deskpal result |
|---|---|---|---|
| Exact compositor window identity | Internal/private | Internal/private | Spike required |
| Window actor ScreenCast stream | Yes, private API | Yes, refactored API | Read-only spike candidate |
| Window-local input coordinates | Transform to stage | Window EIS viewport transforms to stage | Not background input |
| Button bound to window stream | No | No | `backgroundUnavailable` |
| Covered/unfocused direct click | No supported primitive | No supported primitive found | `backgroundUnavailable` |
| Human pointer independence | No for virtual pointer route | No for virtual pointer route | Contract not met |
| Focus/stacking preservation guarantee | No | No | Contract not met |

## Why common workarounds are rejected

- **Move a virtual pointer over the target:** scene picking reaches the covering
  surface and uses the logical seat.
- **Temporarily raise or focus the target:** directly violates the product
  contract, even if state is later restored.
- **Hide the covering window:** mutates unrelated application state and creates
  race/flicker failure modes.
- **Emit a Clutter event directly at a window actor:** actor event handling is
  not equivalent to Wayland/Xwayland client protocol delivery, bypasses pointer
  focus/grab/serial semantics, and is not a supported client-input API.
- **XSendEvent:** applies only to X11, many clients ignore synthetic events, and
  it does not solve native Wayland or the trusted cross-backend contract.
- **Portal RemoteDesktop or libei:** permission improves, but delivery still uses
  compositor virtual devices and global picking.

## Mutter change feasibility requirements

A reviewed Mutter proof would need a new internal operation that accepts an
authorized, generation-checked target and surface-local coordinates, then
implements client protocol semantics without changing the physical/logical
human pointer.

At minimum it must address:

- an agent-specific pointer context or seat distinct from the human seat;
- Wayland `wl_pointer` enter, motion, button, frame, serial, and leave semantics;
- subsurface/input-region picking within the authorized toplevel;
- popups, modal relationships, grabs, pointer constraints, drag-and-drop, and
  surface destruction;
- equivalent Xwayland delivery without trusting client-provided identity;
- lock screen, protected surfaces, revocation, and permission UI;
- operation IDs, cancellation, unknown outcomes, limits, and caller cleanup;
- no focus, activation, stacking, workspace, clipboard, or human-pointer side
  effects; and
- post-action frame sequence evidence usable by Deskpal's existing settling and
  regional verification.

A proof may initially support one pointer, one button, one toplevel, no active
grab, and a dedicated nested compositor. Unsupported states must return
`backgroundUnavailable` before dispatch.

## Private implementation progress

The private local Mutter branch now proves two prerequisites without changing
or advertising any public Deskpal capability:

- a pointer-only secondary `wl_seat` filtered to one exact Wayland client, hidden
  from another client, and removed on disconnect; and
- direct standard Wayland pointer enter/motion/button/frame/leave delivery to an
  authorized mapped XDG toplevel with generation and input-region checks while
  the ordinary human seat's pointer focus remains unchanged.

The nested test rejects a second client's toplevel, stale generation,
out-of-region coordinates, pointer constraints, and active human-seat implicit
grabs. A private operation manager adds unique IDs, replay rejection,
pre-dispatch cancellation, generation recheck, revocation, and explicit
accepted/dispatching/dispatched/completed/cancelled/unknown/failed/revoked states.

The private nested proof now maps two equal-sized XDG toplevels at the same frame
rectangle, activates and raises the foreground client, and dispatches the
independent-seat operation to the fully covered target. Only the target commits
a changed buffer after receiving the pointer sequence. Focus, top-of-stack
window, both frame rectangles, both workspaces, human-seat pointer focus, and the
foreground fixture's state remain unchanged across dispatch. The operation layer
also covers unique IDs, replay rejection, pre-dispatch cancellation, replacement
recheck, revocation, and fail-closed states. A private grant layer now binds
operations to one caller identity, the seat's canonical client, exact surface
generation, bounded pointer/background capabilities, and monotonic expiry. It
rechecks at acceptance and dispatch, and synchronously revokes pending operations
on expiry, explicit revocation, surface destruction, or caller disconnect.

This remains test-only. The caller identity is injected by the test rather than
an authenticated peer, and there is no grant transport or permission UI,
protected-surface classifier, complete popup/drag/cancellation/unknown-outcome
evidence, independent physical pointer/clipboard measurement, broker-stream
before/after frames, or Xwayland route. `backgroundUnavailable` therefore remains
the only truthful public capability.

## Next steps

1. Keep `get_environment_status` truthful: stock GNOME has no background pixel
   input backend.
2. Build the read-only ScreenCast window-stream spike separately; do not confuse
   capture success with input feasibility.
3. Create the minimal Mutter change in a private local clone outside Deskpal.
   Build/install only to an isolated prefix or container; do not vendor the patch
   here, alter the active host compositor, or prepare any upstream submission.
4. Design an independent agent pointer context and direct authorized-surface
   click in that private clone.
5. Run it only in a nested Mutter test session with two fixture clients: A
   covered, B focused and receiving human input.
6. Require the complete covered-window acceptance gate from
   [`broker-protocol.md`](broker-protocol.md) before exposing any host tool.
7. If the patch cannot preserve client protocol semantics and all stated side
   effects, stop and retain `backgroundUnavailable` rather than adding a
   compatibility workaround.

## Reproduction references

Upstream source locations audited:

- Mutter 42.9:
  - `data/dbus-interfaces/org.gnome.Mutter.RemoteDesktop.xml`
  - `data/dbus-interfaces/org.gnome.Mutter.ScreenCast.xml`
  - `src/backends/meta-remote-desktop-session.c`
  - `src/backends/meta-screen-cast-session.c`
  - `src/backends/meta-screen-cast-window-stream.c`
  - `src/wayland/meta-wayland-pointer.c`
  - `clutter/clutter/clutter-main.c`
- Mutter upstream main commit above:
  - `src/backends/meta-remote-desktop-session.c`
  - `src/backends/meta-eis-client.c`
  - `src/backends/meta-stream-window.c`
  - `src/wayland/meta-wayland-pointer.c`
  - `clutter/clutter/clutter-main.c`

The source archives/clones were inspected under `/tmp` and are not project
inputs or vendored dependencies.
