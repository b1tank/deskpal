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
states, actions, and bounds are included, while free-form text and attributes
require explicit `includeText` or `includeAttributes` opt-in. All semantic
content is application-controlled and untrusted.

## Output

A successful observation returns one image plus structured metadata:

- `target`: backend (`x11`), window ID, exact title, class, PID, and geometry;
- `focus`: active-window IDs and target-focused state before and after capture;
- `image`: source and returned dimensions and image-to-source scale;
- `transform`: window-local source pixels to desktop/stage coordinates;
- `captureId`: a short-lived ID bound to the exact target identity and geometry;
- `semantic`: bounded AT-SPI state plus availability, completion, truncation,
  and query-error metadata; and
- `consistency`: whether identity, geometry, and focus stayed stable during the
  observation, with `retryRecommended` when they did not.

AT-SPI failure does not discard an otherwise valid visual observation. It
returns `semantic.available: false` or partial completion metadata. Semantic
bounds are reported in their own accessibility coordinate space and are never
silently treated as image pixels.

## Freshness and failure rules

Deskpal resolves and snapshots the target before capture, then re-resolves and
revalidates it afterward.

- If the window disappears, becomes non-viewable, changes PID/class, or the
  requested name becomes ambiguous, the call fails closed and issues no
  reusable capture ID.
- If geometry or focus changes while identity remains valid, the image may be
  returned for inspection, but `consistency.stable` is false,
  `retryRecommended` is true, and no reusable capture ID is issued.
- Stable observations register the capture ID with target identity, geometry,
  source/image dimensions, and creation time.
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
point through this transform.

The first implementation supports one monitor covering the full GNOME stage.
Other layouts return a structured unsupported result until per-monitor capture
transforms are available.

## Side effects

`get_app_state` is read-only. Its result reports that it did not move the shared
pointer, deliver input, change focus or stacking, or modify the clipboard. A
subsequent action must report its own actual delivery route and side effects;
the observation and logical cursor are not evidence that input was delivered.
