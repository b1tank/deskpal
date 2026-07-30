# Maintainer handoff

This document is the starting point for a new maintainer or a fresh agent
session. Read [`../AGENTS.md`](../AGENTS.md), this file,
[`plan.md`](plan.md), [`broker-protocol.md`](broker-protocol.md), and
[`gnome-broker-feasibility.md`](gnome-broker-feasibility.md) before changing the
broker path.

## Repository split and transfer boundary

Deskpal has two intentionally separate repositories:

1. **Public Deskpal** contains the MCP server, optional GNOME Shell extension,
   backend-neutral broker contract, tests, and public documentation.
2. **Private Mutter experiment** contains the local compositor feasibility
   patch and nested tests. Its access and remote must be transferred privately.
   Never publish its URL, patches, source, artifacts, or credentials through the
   public repository, and never propose or push the changes to Mutter upstream.

At handoff, public `main` is clean at `5cb2402` before this document. The private
proof's last implementation commit is `1963e6c7e`; a later local documentation
commit, `467683a35`, adds `DESKPAL-HANDOFF.md` and corrects a stale proof note.
Confirm that documentation commit reached the successor-owned private remote
before beginning implementation. Do not assume a remote transfer succeeded just
because a local URL was changed.

For ownership migration:

- update each repository's remote explicitly and verify it with `git remote -v`;
- use `git ls-remote <url> HEAD` before the first push;
- transfer branch protection, issues, releases, Actions settings, and secrets
  separately—none are encoded in Git history;
- do not commit tokens, SSH aliases, local paths, installed extension state, or
  private remote URLs; and
- verify both worktrees are clean and synchronized after the transfer.

## Shipped public state

The public repository currently provides:

- X11/Xwayland discovery, capture, shared-seat compatibility input, OCR, and
  private Xvfb sessions;
- bounded AT-SPI observation and verified semantic mutations;
- capture-bound frame settling and regional visual verification;
- a machine-wide visible-desktop control lease;
- a visual-only, caller-owned GNOME logical cursor;
- the read-only `org.deskpal.ShellBridge1` service for native-Wayland window and
  monitor metadata; and
- deterministic GNOME 42 and gated GNOME 45–50 extension artifacts.

The GNOME 42 bridge has passed nested and live acceptance. It separates native
Wayland identities from XIDs, invalidates identities on Shell restart, advances
geometry revisions, keeps duplicate titles distinct, and does not expose
capture, window management, or input. The GNOME 45–50 artifact is syntax-checked
but remains experimentally gated until live acceptance on a newer Shell.

Stock GNOME must continue reporting:

```text
nativeWaylandSurfaceControl.available = false
backgroundPixelInput.available = false
```

Do not interpret Shell enumeration, visual cursor movement, a ScreenCast
coordinate transform, or a virtual input device as covered-surface control.

## Private Mutter proof state

The private branch proves, only in a purpose-built nested fixture:

- a pointer-only secondary `wl_seat` filtered to one exact Wayland client;
- direct standard pointer enter/motion/button/frame/leave delivery to an exact
  mapped native-Wayland toplevel and generation;
- refusal for wrong client, stale generation, outside input region, hidden or
  modal target, pointer constraint, active human-seat grab, and Xwayland;
- unique operation IDs, replay refusal, pre-dispatch cancellation, revocation,
  and explicit accepted/dispatching/dispatched/completed/cancelled/unknown/
  failed/revoked states;
- a caller-bound grant with canonical client, exact surface generation,
  pointer/background capabilities, monotonic expiry, and synchronous teardown;
- a fully covered target changing its committed buffer while a same-geometry
  foreground window remains focused and topmost; and
- unchanged human-seat pointer focus, focus, stacking, workspaces, and frame
  rectangles, with no event delivered to the foreground fixture.

It does **not** yet provide authenticated transport, permission UI, protected
surface policy, broker restart identity, frame streaming, a public Deskpal
backend, ordinary-app acceptance, or Xwayland delivery. All grants and operations
are still constructed directly by the private test. Public capability therefore
remains unavailable.

## Next milestone 1: authenticated private transport

Goal: move grant creation from direct test calls behind one authenticated,
bounded private transport without broadening capability.

Required design properties:

- bind authority to an authenticated Unix/session peer and one unique live
  transport identity—not a caller-provided PID, title, app ID, environment
  variable, command line, or copied token;
- make grant handles opaque, non-transferable, capability-scoped, expiring, and
  tied to one broker instance plus exact client/surface generation;
- recheck peer, grant, surface, generation, capabilities, protected state,
  geometry, expiry, and revocation immediately before dispatch;
- revoke synchronously on peer disconnect, explicit revocation, expiry, surface
  destruction, compositor restart, session lock, or user switch;
- bound peers, grants, operations, message sizes, queues, rates, deadlines, and
  memory; and
- remain reachable only in the private nested compositor until acceptance.

Minimum tests: authenticated success; wrong peer/user; copied handle; wrong
owner; duplicate operation ID; expiry before acceptance and before dispatch;
disconnect with accepted operation; surface replacement; explicit revocation;
restart and lock generation; protected target; overflow/rate limit; cancellation
before dispatch; forced post-dispatch unknown outcome; and teardown in every
ordering.

Completion evidence: one coherent private commit, repeated
`mutter-wayland-global-filter` passes, `remote-desktop-tests` and
`service-channel` baselines, updated private handoff/status documents, and only
capability-neutral public documentation. Do not expose an MCP tool yet.

## Next milestone 2: Deskpal broker client

Goal: connect the public backend-neutral contract to the accepted private
transport while preserving stock-GNOME refusal.

Recommended public boundary:

- add a cohesive `broker_transport`/GNOME broker client module rather than
  adding transport concerns to X11, Shell bridge, or indicator code;
- reuse `csrc/broker_contract.*` as the canonical identity, capability, stable
  error, and operation-state model;
- use bounded typed parsing and explicit timeout/cancellation ownership;
- negotiate broker instance, exact surfaces, coordinate spaces, operation
  limits, side-effect guarantees, and background capability;
- keep Shell IDs, XIDs, AT-SPI paths, and broker IDs as separate backend-scoped
  identity types;
- map transport failures to the stable error taxonomy without echoing
  application-controlled content; and
- clean grants, operations, streams, descriptors, and listeners on every normal,
  timeout, cancellation, disconnect, and server-shutdown path.

Initially expose the backend only in tests or behind an explicit development
capability. `get_environment_status` must still report `backgroundUnavailable`
on stock GNOME and whenever any required guarantee is absent. Never retry an
`operationOutcomeUnknown` and never escalate to shared-seat input automatically.

Completion evidence: parser/protocol tests, malformed/oversized/replay tests,
private nested transport integration, ASan/UBSan public suites, truthful
capability output, and no public host mutation.

## Next milestone 3: capture-bound covered click

Goal: prove one end-to-end exact-surface click with independent capture and
application verification.

Required flow:

```text
trusted grant and exact surface
→ broker-owned before frame and frame ID
→ Deskpal capture ID and geometry revision
→ surface-local coordinate and unique operation ID
→ final atomic grant/identity/frame freshness check
→ independent-seat dispatch
→ broker delivery evidence
→ later exact-surface frame
→ Deskpal regional visual verification
```

Bind the operation to the complete broker instance/surface/generation tuple,
grant generation, source frame, geometry revision, coordinate space, local
coordinates, deadline, cancellation state, and `backgroundOnly` policy.

The result must distinguish broker delivery from application success and report:
operation state; whether dispatch was attempted; cancellation/unknown outcome;
source and post-action frame sequence; target fixture state; and measured human
pointer, focus, stacking, workspace, clipboard, and foreground-window input
side effects.

Acceptance uses target A fully covered by focused/topmost B. Only A changes and
its explicit region verifies. Pointer, focus, stacking, workspace, clipboard,
and B remain unchanged. Replacement, revocation, cancellation, restart, stale
frame, changed geometry, protected target, and unknown outcome must produce the
specified fail-closed result. Only after this gate passes may a development
broker capability become visible; ordinary host support still requires policy,
permission UI, hardening, and broader app evidence.

## Commands and evidence

Public repository:

```bash
npm run build
npm test
npm run test:asan
npm run test:shell-bridge-live
npm run indicator:package
```

Visible-desktop tests are separate and can disrupt the user:

```bash
npm run test:indicator-live
npm run test:live
```

Private Mutter uses the pinned container, prefix, and DESTDIR in
`DESKPAL-LOCAL-BUILD.md`. Never install it over the host compositor. Run the
focused `mutter-wayland-global-filter` test repeatedly plus
`remote-desktop-tests` and `service-channel`; preserve the documented upstream
`wayland-subsurface` grouped teardown-flake distinction.

## Known follow-up outside the three milestones

- live GNOME 45–50 extension acceptance before removing its installer gate;
- explicitly scheduled lock/unlock and suspend/resume identity tests;
- protected-surface and permission UI policy;
- ordinary GTK/Qt/Electron secondary-seat behavior;
- popup, subsurface, drag-and-drop, constraints, active grabs, and forced
  unknown-outcome evidence;
- Xwayland broker route or explicit permanent refusal;
- persistent frame streaming and overflow behavior; and
- packaging, upgrade migration, support matrix, and release ownership under the
  new repository owner.

## Prompt for a new implementation session

Use this as the opening request after both repositories are available:

> Read `AGENTS.md`, `docs/maintainer-handoff.md`, `docs/plan.md`,
> `docs/broker-protocol.md`, and `docs/gnome-broker-feasibility.md` completely.
> In the private Mutter clone also read `DESKPAL-HANDOFF.md`,
> `DESKPAL-AGENT-SEAT.md`, and `DESKPAL-LOCAL-BUILD.md`. Verify both worktrees,
> branches, remotes, and expected commits before editing. Implement only next
> milestone 1: authenticated private broker transport and its fail-closed nested
> tests. Keep all Mutter work private/local, do not change the host compositor,
> do not expose a public Deskpal capability, update handoff/status docs, run the
> focused and baseline tests, and leave one clean coherent commit.
