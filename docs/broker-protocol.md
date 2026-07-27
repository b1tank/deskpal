# Trusted desktop broker contract

## Purpose

Deskpal's north-star fallback is an operation on one approved compositor
surface that does not borrow the human pointer, keyboard focus, or window
stacking. Standard Wayland does not give ordinary applications this authority.
The broker is therefore a trusted desktop component, not another compatibility
input injector.

This document defines the backend-neutral contract and the security gate for a
GNOME/Mutter implementation. It does not claim that GNOME currently exposes all
required implementation primitives.

## Non-goals

The broker is not:

- a general compositor scripting API;
- a global virtual mouse or keyboard;
- an arbitrary screenshot service;
- a way to bypass lock-screen, protected-content, or application permissions;
- the existing `org.deskpal.Indicator` extension; or
- permission to silently fall back to XTest, uinput, portal RemoteDesktop,
  activation, raising, workspace switching, or window hiding.

The indicator extension must remain input-free. Its visual cursor can display a
broker operation, but it is not evidence that delivery occurred.

## Trust boundary and threat model

### Trusted components

- The compositor-side broker authenticates callers, identifies surfaces,
  enforces grants, captures frames, and performs any surface-targeted delivery.
- Deskpal translates MCP intent into narrow broker requests and independently
  verifies application state.
- The desktop's user-facing permission UI creates, narrows, and revokes grants.

Deskpal cannot grant itself broker authority. A process command line, ambient
environment variable, caller-provided PID, X11 property, accessible name, or MCP
approval is not a compositor grant.

### Untrusted inputs

Treat all of the following as untrusted:

- application titles, app IDs, PIDs, accessibility data, pixels, and window
  metadata;
- MCP arguments and capture IDs;
- stale broker IDs copied from previous sessions;
- another local process on the session bus;
- a compromised controlled application attempting to impersonate another app;
- late replies, duplicated requests, and events delivered during replacement;
- paths, labels, or strings displayed by the logical cursor.

### Threats that must fail closed

- surface destruction and ID reuse;
- a process replacing its window between capture and action;
- duplicate titles or app IDs;
- caller disconnect, compositor restart, session lock, user switch, suspend, or
  permission revocation;
- coordinates from a different frame, scale, transform, or surface generation;
- action replay after an unknown outcome;
- capture or input directed at lock-screen, password, protected-media, system
  permission, or broker-owned UI;
- one caller using another caller's grant, stream, operation, or cursor ID;
- concurrent operations whose independence has not been proven;
- a request for background-only delivery when only shared-seat delivery exists.

## Identity model

Every identity is broker-scoped and opaque. Deskpal must not construct one from
PID, title, app ID, XID, pointer coordinates, or accessibility paths.

A surface reference contains:

- `brokerInstanceId`: changes whenever the broker/compositor restarts;
- `surfaceId`: opaque random or monotonic ID unique within that instance;
- `generation`: increases whenever the underlying compositor surface is
  replaced or its security identity changes;
- `application`: broker-observed app ID and process credentials for display and
  policy only, never as the primary key;
- `capabilities`: currently granted operations for this exact surface;
- `coordinateSpace`: explicit surface-local logical or physical space;
- `geometryRevision`: changes when size, scale, transform, monitor, workspace,
  or decoration mapping changes; and
- `protected`: whether capture/input is forbidden.

A reference is valid only as the complete tuple
`(brokerInstanceId, surfaceId, generation)`. Geometry and frame revisions add
freshness requirements; they do not replace identity.

Xwayland XIDs and native-Wayland handles may be reported as diagnostic backend
metadata, but they are never portable broker IDs.

## Grants and ownership

A grant is created through trusted desktop UI and is bound to:

- the caller's authenticated OS/session identity and unique transport peer;
- one exact application or surface selection;
- explicit capabilities such as `observe`, `capture`, `pointer`, `keyboard`,
  `scroll`, or `drag`;
- whether covered/background operation is allowed;
- expiry and idle timeout;
- session lock and user-presence policy; and
- a revocation generation.

Grant handles are opaque, non-transferable, short-lived, and removed on caller
disconnect. Delegation requires a new trusted grant; forwarding a token is not
delegation. The broker rechecks the grant at dispatch and completion, not only
when the handle is created.

High-impact system surfaces and protected roles are denied even when an
application-level grant exists unless a future, separately reviewed policy says
otherwise.

## Capability negotiation

Before selecting a route, Deskpal requests a structured capability report. At a
minimum it distinguishes:

- exact surface enumeration;
- covered-surface capture;
- persistent frame stream;
- surface-local pointer motion and button delivery;
- surface-local keyboard, scroll, and drag delivery;
- background delivery without focus or stacking changes;
- cancellation before dispatch and after dispatch;
- operation completion evidence;
- lock-screen/protected-content enforcement; and
- supported coordinate spaces and scale transforms.

A capability is not boolean marketing metadata. It includes version, limits,
side-effect guarantees, unsupported states, and whether the claim is enforced
by the compositor. Deskpal returns `backgroundUnavailable` when the required
claim is absent.

## Observation contract

### Surface enumeration

Enumeration returns only surfaces visible to the caller's grant. It is bounded,
does not expose pixels, and includes replacement-safe identity. Ambiguous
selection never chooses the focused or first surface as a fallback.

### Frame streams

Large frames travel through a broker-owned bounded stream (for example a
PipeWire node or sealed descriptor), not JSON or unbounded D-Bus byte arrays.
The stream is bound to the grant and exact surface generation.

Each frame has:

- `frameId` and monotonic `sequence`;
- surface identity and `geometryRevision`;
- source width, height, scale, transform, and pixel format;
- monotonic capture time;
- completeness/protected-content status; and
- an explicit dropped-frame/overflow indication.

Deskpal may compute local frame revisions and bounded projections, but a local
hash is not a broker authorization token.

## Action contract

A pointer operation includes:

- grant and complete surface identity;
- `operationId`, unique per caller;
- required capability and `backgroundOnly` policy;
- source `frameId`, `geometryRevision`, and coordinate space;
- surface-local coordinates and bounded action details;
- dispatch deadline and cancellation state; and
- optional expected visual region/postcondition used by Deskpal verification.

The broker atomically re-resolves identity, grant, geometry, and frame freshness
immediately before dispatch. It must deliver to the specified surface rather
than hit-testing the global desktop at the human pointer position.

For `backgroundOnly`, success requires all of the following:

- no human-pointer movement;
- no keyboard-focus change;
- no activation or stacking change;
- no workspace switch;
- no clipboard mutation; and
- no global shared-seat event visible to another surface.

If the compositor cannot meet every guarantee, the broker returns
`backgroundUnavailable` before dispatch. Deskpal may offer a separate,
explicitly approved foreground compatibility operation, but never retries or
escalates automatically.

## Operation state and outcomes

Operations are idempotently addressed by `operationId` and move through:

1. `accepted` — syntax and grant checked, no input dispatched;
2. `dispatching` — final identity/freshness check in progress;
3. `dispatched` — compositor accepted delivery for the exact surface;
4. `completed` — broker-side completion evidence available;
5. `cancelledBeforeDispatch`;
6. `outcomeUnknown` — dispatch may have occurred, final evidence unavailable;
7. `failedBeforeDispatch`; or
8. `revoked`.

The broker never maps `accepted` or command completion to application success.
Deskpal reports broker delivery separately from semantic or frame verification.
An unknown outcome is never blindly retried.

Cancellation is scoped by caller and operation ID. Cancellation after dispatch
may produce `outcomeUnknown`; it cannot claim the application was untouched.

## Required result evidence

Every action result includes:

- complete target identity and operation ID;
- selected backend and capability version;
- grant generation;
- source frame and geometry revisions;
- whether dispatch was attempted and accepted;
- cancellation and unknown-outcome state;
- shared pointer, focus, stacking, workspace, and clipboard side effects;
- post-action frame sequence when available; and
- reasons/limits when any side effect is unknown.

Application success still requires a fresh semantic postcondition or bounded
frame verification. Broker evidence and application verification remain
separate fields.

## Error taxonomy

The stable backend-neutral errors are:

- `brokerUnavailable`
- `capabilityUnavailable`
- `backgroundUnavailable`
- `permissionRequired`
- `permissionDenied`
- `grantExpired`
- `grantRevoked`
- `surfaceNotFound`
- `surfaceAmbiguous`
- `surfaceReplaced`
- `surfaceProtected`
- `geometryChanged`
- `frameStale`
- `coordinateSpaceUnsupported`
- `operationCancelled`
- `operationOutcomeUnknown`
- `brokerRestarted`
- `sessionLocked`
- `rateLimited`
- `overflow`
- `internalError`

Errors state whether dispatch occurred, whether retry with a fresh observation is
safe, and whether user interaction is required. Application-controlled strings
are not echoed into trusted error messages.

## Resource and abuse limits

The broker sets explicit bounds for grants, surfaces, streams, frame dimensions,
frame rate, queued operations, event backlog, action rate, memory, and operation
time. Overflow revokes or fails the affected operation; it does not silently drop
identity, permission, or completion checks.

Streams and operations are cleaned on normal completion, cancellation, caller
disconnect, compositor restart, and grant revocation. No caller can clear another
caller's resources.

## GNOME implementation gate

The current GNOME Shell indicator extension is suitable only for click-through
visual overlays. Shell JavaScript APIs that create a virtual pointer still route
through the logical seat/global hit testing and therefore do not prove exact
covered-surface delivery.

Before extending any D-Bus API, a GNOME feasibility spike must identify a
compositor-enforced primitive that can:

1. target one `Meta.Window`/surface generation directly;
2. deliver while that surface is covered and unfocused;
3. leave pointer, focus, stacking, and workspace unchanged; and
4. expose completion/cancellation evidence.

The source audit in
[`gnome-broker-feasibility.md`](gnome-broker-feasibility.md) found no such stock
primitive in installed Mutter 42.9 or audited upstream main. Stock GNOME must
therefore report `backgroundUnavailable`; the remaining honest implementation
option is a reviewed, privately maintained local Mutter patch/plugin. It must
never be submitted or pushed upstream/publicly and must be built and installed
only in an isolated local environment. A larger Shell extension, synthetic
global events, temporary raising, focus restoration, or window hiding does not
satisfy this contract.

## Acceptance gates

### Read-only identity and capture gate

- exact authorized surface is enumerated without title fallback;
- capture works while covered and on another workspace;
- replacement invalidates the old generation;
- protected and revoked surfaces fail closed;
- frame stream cleanup survives disconnect and compositor restart.

### Covered-window action gate

With target A completely covered while the human actively uses window B:

- a capture-bound click reaches A at the expected local point;
- A's independent fixture state and post-action frame change verify;
- the human pointer position is unchanged;
- focus remains on B;
- stacking and workspace remain unchanged;
- B receives no input;
- cancellation, replacement, revocation, and broker restart produce the stated
  outcomes; and
- independent evidence is collected from broker result, X11/Wayland focus and
  stacking state where applicable, and both fixture applications.

This gate must pass on a real nested compositor or dedicated test session before
the route is advertised. A live manual demo alone is not acceptance.

## Staged implementation

1. Implement backend-neutral C identity, capability, grant, operation, and error
   types with parser/state-machine tests; no compositor mutation.
2. Create the Mutter work only in a private local clone outside the public
   Deskpal repository. Use a local prefix/container and nested compositor; never
   modify the active host compositor or prepare an upstream submission.
3. Build a read-only GNOME spike for exact authorized surface identity and
   persistent covered-surface capture.
4. Resolve the GNOME direct-delivery feasibility gate. Record
   `backgroundUnavailable` if no supported primitive exists.
5. Only after the primitive is proven, implement one capture-bound covered click
   with cancellation and the evidence contract above.
6. Reuse Deskpal's existing frame settling and explicit regional verification.
7. Harden grants, lock/revocation, protected surfaces, rate limits, crash
   cleanup, and confused-deputy cases before broader input capabilities.
