# Visual frame signatures and pixel diffs

Deskpal's X11 capture path can produce a normalized source-resolution BGRA
frame before PNG encoding or downscaling. Depth-24 padding is normalized to
opaque alpha; genuine depth-32 alpha is preserved. Unsupported pixel layouts
fail instead of being interpreted as four-byte pixels.

`frame_state` provides two reusable operations:

- an opaque FNV-1a frame revision for equality checks; and
- a bounded comparison of two same-sized frames, including changed pixel count,
  changed fraction, maximum channel delta, and the smallest changed rectangle.

The revision is an informational, non-cryptographic equality hint. It is not an
authorization token or a privacy-preserving proof against an attacker. Raw
pixels, rather than encoded PNG bytes, enter the revision so PNG compression and
metadata cannot create false changes. Width and height are included.

A channel tolerance from 0 to 255 may be used when comparing frames. A pixel is
changed when any BGRA channel differs by more than that tolerance. Frames with
different dimensions are reported as non-comparable rather than silently
rescaled.

`get_app_state` and X11-backed screenshots report `frameRevision` when the raw
source frame was available. Fallback captures that only produce a PNG report
`frameRevisionAvailable: false`. Stable app-state captures retain the source
frame revision for later capture-bound frame settling and visual verification.

`wait_for_frame_stable` accepts a retained stable `get_app_state` capture and
samples its normalized source pixels under one absolute deadline. It validates
exact X11 identity and geometry before capture, around every sample, and before
success or timeout. Stability is measured against the anchor frame at the start
of the current stable period—not merely against the previous sample—so slow
per-sample drift cannot be misclassified as settled. Stability means unchanged
for the requested observation window; an animation whose quiet period is longer
than `stableMs` can still settle between transitions, so callers must choose a
duration appropriate to the application.

Callers choose a required stable duration, sample interval, and channel
tolerance within bounded schema limits. Results report samples, transitions
detected after waiting began, stable duration, exact base/final revisions,
changed-from-capture state, and bounded summaries of the last and largest
transitions. A difference already present in the first sample sets
`changedFromCapture` without inventing an observed transition. Timeout is a truthful
unsettled result; cancellation is interrupted; changed geometry, unsupported
pixels, capture failure, and non-comparable frames fail closed. The tool never
delivers input or changes focus, stacking, pointer, or clipboard state.

This milestone does not yet use frame settling to claim that a pixel action
succeeded. Pixel verification must additionally bind an action to before/after
captures, define the expected changed region or visual postcondition, and report
background-route side effects.
