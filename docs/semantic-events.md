# Capture-bound semantic change events

Deskpal exposes `wait_for_semantic_change` as a bounded, capture-bound AT-SPI
wait. It builds on an internal listener and orchestration path shared with the
private integration probe.

## Proven in private Xvfb

The opaque `semantic_events` module owns one temporary AT-SPI `object`
listener. Its `begin` / `wait` / `end` lifecycle:

- registers once and remains active across multiple wake/re-observe waits;
- pumps the default GLib context without a second thread mutating MCP output;
- filters callbacks to one expected process ID, bus name, and accessible window
  object path by walking bounded source ancestry;
- retains no accessible names, text, attributes, raw event names, or event-source
  paths; only the caller-provided target identity and bounded hashes persist;
- counts at most 256 callbacks and 64 in-memory event fingerprints;
- reports relevant, irrelevant, coalesced, dropped, and overflow counts and fails
  closed if accounting overflows its safety bound;
- accepts a cancellation callback and checks it while waiting;
- uses one absolute monotonic deadline across repeated waits (the proof limits
  callers to 1–5000 ms); and
- attempts explicit deregistration and release on every caller cleanup path;
  failed deregistration retains callback storage rather than risking use-after-free.

Listener lifecycle, filtering, and AT-SPI timeout restoration no longer live in
the large accessibility traversal/action module. `SemanticWindowIdentity` is the
single shared PID/bus/object-path representation used by snapshots, retained
captures, listeners, and tests. `semantic_change` owns capture-bound validation,
re-observation, revision comparison, and bounded diff construction. The
production binary and private probe link the same internal
`semantic-observation` library rather than recompiling separate copies of its
sources.

The private `accessibility-event-probe` test helper proves relevant GTK state
events, same-process mismatched-window rejection, callback cancellation, timeout,
successful deregistration, and a canonical revision change after a checked-state
event. The probe keeps one registration while it ignores wakeups that do not
change the canonical revision. It runs only against the nested accessibility
fixture and is not installed.
Stable app-state captures retain the bounded semantic window bus/object identity
and original projection bounds used by the capture-bound wait loop.

## Capture-bound wait contract

The internal `semantic_change` orchestrator composes the listener with a
retained stable capture. Required callbacks validate exact X11 identity and
geometry before registration, after every wakeup, and after canonical
re-observation. The loop continues past unchanged revisions under one absolute
deadline, rejects changed accessible-window identity, and returns the existing
bounded diff or incomplete-projection result. The private Xvfb probe exercises
this path with the production X11 resolver, asserts a comparable changed diff,
and proves that geometry replacement fails closed while the listener is active.
An event alone is never treated as a semantic change.

`wait_for_semantic_change` accepts a retained stable `get_app_state` capture and
a 1–5000 ms timeout. Changed results include the existing bounded semantic diff;
timeout is a successful unchanged outcome; cancellation is an interrupted tool
result. All outcomes report listener accounting and pointer, input, focus,
stacking, and clipboard non-mutation.

The MCP input transport uses bounded raw-fd framing so a synchronous handler can
consume only a matching `notifications/cancelled` message while preserving other
complete requests for the main loop. Client disconnect is treated as
cancellation, SIGPIPE is ignored so cleanup can unwind, and Pi sends graceful
cancellation before a bounded process-termination fallback. Private tests cover
matching cancellation, queued-request preservation, stdin disconnect with
listener deregistration, and forced helper termination.

If listener delivery is unavailable, Deskpal fails rather than silently claiming
event-driven behavior or falling back to polling.
