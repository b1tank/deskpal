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
8. Before starting the next milestone, run the maintainability gate below and
   leave the current milestone as one coherent, tested commit.

## Maintainability gate

Deskpal is long-lived systems software, not a sequence of isolated demos. Every
feature change must leave the design easier—or at least no harder—to reason
about. Before declaring a milestone complete:

- inspect the whole affected path, not only the lines needed to make the test
  pass;
- look explicitly for duplicated state, identity types, bounds, parsing,
  cleanup, retry, timeout, and error-reporting logic;
- reuse one canonical representation and one lifecycle owner instead of adding
  a parallel abstraction or a second source of truth;
- extract a cohesive module with a narrow or opaque interface when the next
  behavior would otherwise enlarge an already broad owner, duplicate logic, or
  leak test synchronization into production APIs;
- remove superseded helpers, dead branches, stale comments, obsolete plan
  items, and redundant tests in the same change that replaces them;
- keep test helpers linked to the same production implementation where
  practical; do not maintain a simplified copy that can drift;
- prefer the smallest grounded interface that supports the proven use case;
  avoid speculative frameworks, generic wrappers, and mechanical abstraction;
- make ownership and cleanup explicit for memory, processes, descriptors,
  listeners, locks, sessions, and global library state, including every error,
  timeout, cancellation, disconnect, and forced-exit path;
- preserve bounded work and fail-closed behavior while refactoring; a cleaner
  shape is not an excuse to weaken identity, privacy, arbitration, or
  verification guarantees; and
- update the owning contract, roadmap, and test-scope documentation so completed
  work, remaining gates, and known caveats do not become stale.

Do the refactor before stacking another milestone when a local proof has exposed
the right boundary. Do not schedule broad cleanup merely for aesthetics: every
restructure needs a concrete ownership, duplication, coupling, drift, safety, or
testability reason and must remain independently verifiable.

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
