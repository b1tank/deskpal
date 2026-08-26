# Security policy

Deskpal controls desktop applications and can optionally read files or launch processes. Treat it as security-sensitive local software.

## Reporting a vulnerability

Use GitHub private vulnerability reporting for this repository. Do not include vulnerability details, credentials, screenshots containing private data, or desktop content in a public issue.

Include the affected commit, Linux distribution, desktop session, reproduction steps, expected impact, and whether the issue crosses an app, session, control-lock, accessibility, filesystem, or process-execution boundary.

## Current security boundary

Deskpal is an experimental alpha, not a security sandbox. Xvfb sessions isolate desktop input and display routing, but applications still run as the same OS user unless separately sandboxed. Visible-desktop compatibility routes may use the shared seat. File reads and process launch remain disabled unless explicitly enabled.

The private Mutter research fork is not distributed with Deskpal and provides no capability in the public build. Native-Wayland background control remains unavailable unless a separately reviewed trusted compositor broker is installed.
