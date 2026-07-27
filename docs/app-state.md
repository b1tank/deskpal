# Unified app-state observation contract

`get_app_state` is Deskpal's capture-bound observation API for one exact
X11/Xwayland application window. It combines a rendered image, backend-scoped
window identity, focus and geometry, coordinate transforms, and bounded AT-SPI
state without activating, raising, resizing, or otherwise mutating the target.

Native Wayland surfaces remain unsupported until a trusted compositor broker
can enumerate and identify them. Deskpal never substitutes the active window or
a full-screen capture when exact resolution fails.

## Input

Callers must provide exactly one target:

- `windowId`: a live X11 window ID; or
- `windowName`: an exact, case-sensitive title that resolves to one live window.

Duplicate exact titles are ambiguous and fail closed. There is no implicit
active-window mode.

Image defaults are `maxWidth: 1920` and `maxHeight: 1080`. Set either dimension
to `0` to leave that axis unconstrained; set both to `0` for full resolution.
Semantic defaults are bounded and privacy-preserving: roles, accessible names,
states, actions, and bounds are included. Offscreen nodes, free-form text, and
attributes require explicit `includeOffscreen`, `includeText`, or
`includeAttributes` opt-in. All semantic
content is application-controlled and untrusted.

An optional `previousCaptureId` requests a bounded semantic diff. The base must
be a retained stable app-state capture; desktop screenshots cannot be diff
bases. Unknown or expired IDs fail explicitly. Captures for a different exact
X11 window identity return `sameTarget: false` without comparing elements.

## Output

A successful observation returns one image plus structured metadata:

- `target`: backend (`x11`), window ID, exact title, class, PID, and geometry;
- `focus`: active-window IDs and target-focused state before and after capture;
- `image`: source and returned dimensions and image-to-source scale;
- `transform`: window-local source pixels to desktop/stage coordinates;
- `captureId`: a short-lived ID bound to the exact target identity and geometry;
- `frameRevision`: an optional opaque revision of the normalized source pixels,
  accompanied by `frameRevisionAvailable`;
- `semantic`: bounded AT-SPI state plus availability, completion, truncation,
  and query-error metadata;
- `semanticRevision`: an opaque informational structural revision; and
- optional `semanticDiff`: same-target added, removed, and updated elements; and
- `consistency`: whether identity, geometry, and focus stayed stable during the
  observation, with `retryRecommended` when they did not.

AT-SPI failure does not discard an otherwise valid visual observation. It
returns `semantic.available: false` or partial completion metadata. Semantic
bounds are reported in their own accessibility coordinate space and are never
silently treated as image pixels.

## Semantic revisions and diffs

The canonical semantic projection uses complete live locator identity plus role
and path as its element key. It includes states, actions, bounds, numeric value,
and selection metadata. Accessible names, free-form text, and attributes never
enter the revision or diff, even when separately returned by explicit opt-in.
The reusable projection, revision, completeness, snapshot ownership, and bounded
comparison rules live in `csrc/semantic_state.c`; future event-driven observation
must use that module rather than maintaining a second semantic representation.

Revisions are non-cryptographic FNV-1a equality hints only. They are not
security digests or authorization tokens and never replace fresh complete-
locator resolution before mutation. Diff lists are
bounded. A diff is `comparable: true` only when both retained projections are
complete. Otherwise it returns
`reason: base_or_current_projection_incomplete` and omits added/removed/updated
lists; callers must re-observe rather than treating omitted nodes as removals.

## Freshness and failure rules

Deskpal resolves and snapshots the target before capture, then re-resolves and
revalidates it afterward.

- If the window disappears, becomes non-viewable, changes PID/class, or the
  requested name becomes ambiguous, the call fails closed and issues no
  reusable capture ID.
- If geometry or focus changes while identity remains valid, or focus cannot be
  determined before and after capture, the image may be returned for inspection,
  but `consistency.stable` is false,
  `retryRecommended` is true, and no reusable capture ID is issued.
- Stable observations register the capture ID with target identity, geometry,
  source/image dimensions, creation time, optional source-frame revision, a
  bounded visual projection when raw pixels are available, and the semantic
  window bus/object identity when a complete root locator is available.
- Unknown, stale, evicted, replaced, or geometry-mismatched capture IDs fail
  before any later mutation.

For a stable window capture, image point `(ix, iy)` resolves to desktop/stage
coordinates using the recorded frame:

```text
sourceX = ix * sourceWidth / imageWidth
sourceY = iy * sourceHeight / imageHeight
stageX  = windowX + sourceX
stageY  = windowY + sourceY
```

A stable window `captureId` can be passed directly to `agent_cursor_move`; that
tool revalidates the recorded identity and geometry before resolving the image
point through this transform. When the capture retained an exact semantic root,
it can also be passed to `wait_for_semantic_change`, which preserves the original
semantic depth/node/offscreen bounds and returns only after a canonical revision
change, timeout, or cancellation. Captures with `frameRevisionAvailable: true`
can be passed to `wait_for_frame_stable` for bounded, cancellable source-pixel
settling under exact identity and geometry revalidation, or to
`verify_frame_change` with an explicit source-pixel region. The latter verifies
a visual postcondition but does not attribute it to a particular action.

The first implementation supports one monitor covering the full GNOME stage.
Other layouts return a structured unsupported result until per-monitor capture
transforms are available.

## Side effects

`get_app_state` is read-only. Its result reports that it did not move the shared
pointer, deliver input, change focus or stacking, or modify the clipboard. A
subsequent action must report its own actual delivery route and side effects;
the observation and logical cursor are not evidence that input was delivered.
