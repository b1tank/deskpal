# Contributing to Deskpal

Deskpal is experimental security-sensitive systems software. Discuss significant behavior or protocol changes in an issue before implementation.

## Development loop

1. Read `AGENTS.md`, the owning C module, and the nearest deterministic test.
2. Build with `npm run build`.
3. Run the safe nested-display suite with `npm test`.
4. Run `npm run test:asan` for capture, OCR, X11 traversal, process, memory, or lifecycle changes.
5. Run live tests only when nested Xvfb cannot prove the behavior. Live tests manipulate the visible desktop.
6. Update affected contracts and roadmap documents in the same change.

Never include private desktop content, credentials, host paths, or private Mutter source. Keep file access and process execution opt-in, preserve fail-closed targeting, and add a regression test for every bug fix.

By contributing, you agree that your contribution is licensed under the MIT License.
