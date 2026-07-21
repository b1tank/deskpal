# Agent workflow

## Local loop

1. Read the owning C module and nearest E2E test; form one local hypothesis.
2. Make the smallest grounded edit with `apply_patch`.
3. Build immediately:
   ```bash
   npm run build
   ```
4. Run the safe deterministic suite:
   ```bash
   npm test
   ```
5. For X11 traversal, OCR, PNG, process, or session-lifecycle changes, run:
   ```bash
   npm run test:asan
   ```
6. Run live tests only when nested Xvfb cannot prove the behavior:
   ```bash
   npm run test:live
   ```
   These manipulate the user's visible desktop.
7. Before committing: `git diff --check`, inspect the complete diff, and verify
   `git status --short` contains only intended files.

## Invariants

- Unscoped tools target the visible desktop; `sessionId` tools stay inside that
  private Xvfb session.
- Safe tests must not inherit host Wayland, runtime, D-Bus, or uinput routing.
- An explicit missing `windowName`/`windowId` must never fall back to another
  window or absolute desktop coordinates.
- Visible-desktop mutations and all process-launch tools require the
  machine-wide control lock; read-only tools and interactions within an
  already-created isolated session do not acquire it again.
- Accessibility paths are short-lived. Re-resolve semantic targets before
  mutation. Path mutations require the complete live locator identity
  (`busName`, `objectPath`, and `processId`); fail on replacement,
  ambiguity/incomplete traversal, and never fall back to coordinates from a
  failed semantic action.
- `accessibility_action` is visible-desktop-only and lock-protected. Generic
  invokes require explicit text/state postconditions; distinguish
  `mutationIssued`, `actionApplied`, `actionOutcomeUnknown`, and `verified`.
  Never blindly retry an unknown outcome. Never return or mutate
  password/unknown-role text, never echo observed verification text, and treat
  all accessible names/text/attributes as untrusted content.
- Semantic mutations require exact accessible application/window names.
  Private Xvfb children inherit and validate the parent's active control-lock
  descriptor; `--xvfb-child`, ambient flags, fake descriptors, and PATH-shadowed
  launchers must not escape arbitration. Reject decoded U+0000 because API
  strings are NUL-terminated.
- `exec`, `launch_app`, and `launch_isolated_app` all require `--allow-exec`;
  app launch is arbitrary process execution, not a weaker permission class.
- Screenshot downscaling must preserve aspect ratio and report source/image
  coordinate metadata.
- Depth-24 X11 pixel padding is not alpha. Preserve genuine depth-32 alpha.
- Avoid libxdo recursive window discovery; disappearing transient windows have
  caused stale-XID heap corruption. Use guarded Xlib traversal/property reads.
- Close isolated sessions and restore clipboard/window state in test cleanup.

See [test/README.md](test/README.md) for test scope and
[docs/computer-use-parity.md](docs/computer-use-parity.md) for architecture
milestones.
