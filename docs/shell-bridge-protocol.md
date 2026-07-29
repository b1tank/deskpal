# GNOME Shell bridge protocol

## Purpose

The optional Deskpal GNOME Shell extension exposes bounded, read-only native
window and monitor metadata that an ordinary X11 client cannot obtain for
native-Wayland applications. It also retains the separate visual-only logical
cursor interface documented in [`gnome-indicator.md`](gnome-indicator.md).

The Shell bridge is not the trusted compositor broker defined in
[`broker-protocol.md`](broker-protocol.md). It does not grant capture, input,
activation, movement, resize, background delivery, or permission authority.
Missing capabilities must be reported as unavailable rather than emulated with
global virtual input, focus changes, stacking changes, or window hiding.

## D-Bus endpoint

The extension owns the following endpoint on the user's session bus while it is
enabled:

```text
service:   org.deskpal.ShellBridge
path:      /org/deskpal/ShellBridge
interface: org.deskpal.ShellBridge1
```

Version 1 exports three read-only methods:

- `GetCapabilities() -> json`
- `ListWindows() -> json`
- `GetMonitorLayout() -> json`

JSON is used as an explicitly versioned compatibility envelope between GNOME
Shell JavaScript versions and the Deskpal native process. Every response is
bounded before serialization. Strings are limited to 512 Unicode characters;
the native client additionally enforces a bounded response size. A malformed,
oversized, unknown-version, or
incomplete response fails closed.

## Capability response

`GetCapabilities` returns:

```json
{
  "protocolVersion": 1,
  "backend": "gnome-shell-extension",
  "shellInstanceId": "opaque UUID",
  "coordinateSpace": "gnome-stage-logical",
  "capabilities": {
    "windowEnumeration": true,
    "monitorLayout": true,
    "windowCapture": false,
    "foregroundWindowManagement": false,
    "surfaceInput": false,
    "backgroundInput": false
  },
  "limits": {
    "maxWindows": 256,
    "maxStringCharacters": 512
  }
}
```

Capabilities describe only the currently loaded extension implementation. They
are not permissions and do not imply that a later broker operation is allowed.

## Window identity and lifecycle

`ListWindows` returns a `protocolVersion`, the current `shellInstanceId`, a
`complete` flag, and a bounded `windows` array. Each record contains:

- `surfaceId`: opaque within one Shell instance;
- `generation`: changes when the represented compositor window is replaced;
- `geometryRevision`: changes when the reported frame, workspace, monitor, or
  scale identity changes;
- display-only application metadata: title, app ID, WM class, and PID;
- frame bounds in GNOME stage-logical coordinates;
- workspace, focused, hidden, and client type state; and
- `backend: "gnome-shell-extension"`.

The complete identity is `(shellInstanceId, surfaceId, generation)`. A Deskpal
caller must never construct it from PID, title, app ID, WM class, XID, bounds,
or array position. The bridge allocates an identity from the live compositor
window object and drops it when that object is unmanaged. Restarting or
reloading GNOME Shell creates a new `shellInstanceId`, invalidating every old
reference.

`geometryRevision` is freshness metadata, not identity. A change requires a new
observation before capture-bound coordinate use. Duplicate titles and app IDs
remain distinct records and are never resolved by choosing the first or focused
window.

Desktop, override-redirect, and non-window Shell actors are excluded. If the
window limit or safe traversal cannot produce a complete result, `complete` is
false; Deskpal must not treat the projection as authoritative.

## Monitor layout

`GetMonitorLayout` reports the same `shellInstanceId`, coordinate space, stage
size, primary monitor index, and a bounded monitor array containing index,
logical bounds, scale, and primary state. Deskpal must reject transforms when
the bridge instance changes or the returned layout is incomplete.

## Trust and privacy boundary

Application titles and other metadata are untrusted application-controlled
content. Strings are bounded and must never become trusted error text or policy
identity.

The session bus authenticates a Unix user but does not establish a Deskpal
permission grant. Consequently version 1 is deliberately read-only. Future
window-management mutations require a separate caller-bound grant and
operation-state design; they must not be appended to this interface merely
because GNOME Shell exposes a convenient `Meta.Window` method.

The existing `org.deskpal.Indicator` interface remains visual only. Logical
cursor movement is not application input and is never evidence that input was
delivered.

## Compatibility and distribution

GNOME Shell 42–44 use the legacy `imports.*` extension entry point. GNOME Shell
45 and newer use ES modules. Deskpal may ship separate release artifacts, but
both must expose this exact protocol and pass the same contract fixtures.
Runtime support is advertised only for Shell versions tested with the matching
artifact; syntax and metadata checks alone are not runtime acceptance.

## Implementation status

The GNOME 42 extension exports the read-only interface. Deskpal's native client
uses a one-second read-only call, caps replies at 256 KiB, requires protocol
version 1 and a bounded Shell instance ID, and validates every capability,
window, monitor, identity, geometry, and boolean field before returning an
object to the tool layer. Malformed, oversized, unsupported-method, and
incompatible-version fixtures fail closed. Visible-desktop environment reporting
now exposes the validated bridge instance and capability object; private sessions
do not contact the host bridge. Visible-desktop window listing appends only
native-Wayland bridge records, preserving complete Shell identity and omitting
Shell-observed Xwayland duplicates; legacy control tools continue to accept only
XIDs. Deterministic release packaging now emits a runtime-accepted GNOME 42
legacy artifact and a gated, syntax-checked GNOME 45–50 ES-module artifact from
the same implementation. Modern live runtime acceptance remains pending, so its
installer path requires an explicit experimental opt-in.
