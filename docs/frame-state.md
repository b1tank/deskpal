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

This milestone does not yet claim that a frame is settled, wait on frames, or
verify a pixel action. The next slice must define a bounded sampling lifecycle,
exact-window revalidation, stability duration, cancellation, tolerance policy,
and truthful timeout/non-comparable results before exposing such a claim.
