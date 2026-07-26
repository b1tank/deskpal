# Codex control-lease release hook

Pi calls Deskpal's `release_control` tool directly when an agent run settles.
Codex CLI cannot invoke an existing MCP connection from an external hook, but
its stable `Stop` hook can request one final model continuation. Deskpal ships a
hook helper that does this only when the stopping Codex process's own Deskpal
child currently holds the real kernel lock.

## Install

Add or merge this entry in `~/.codex/hooks.json`:

```json
{
  "description": "Release Deskpal control after Codex turns.",
  "hooks": {
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "/usr/bin/python3 /home/b1tank/deskpal/scripts/codex-release-control-hook.py",
            "timeout": 3,
            "statusMessage": "Checking Deskpal control lease"
          }
        ]
      }
    ]
  }
}
```

Then open `/hooks` in Codex, review the exact command, and trust it. Restart or
resume Codex after installation if the current session does not reload hooks.
Do not modify an active Codex process merely to install this hook; wait for a
safe session boundary.

## Behavior

At `Stop`, the helper:

1. finds its Codex ancestor and descendant processes;
2. non-destructively probes the Deskpal kernel lock;
3. ignores free locks, stale metadata, and locks owned by other clients;
4. if this Codex's Deskpal child owns the lock, returns one fixed continuation
   instruction asking Codex to call `release_control`;
5. returns no continuation on the second `Stop` (`stop_hook_active`) to prevent
   loops.

The helper never kills, signals, restarts, or writes to Codex or Deskpal. The
model-visible continuation contains no application-controlled content.

## Limitations

- The model must follow the continuation and call the MCP tool. If it does not,
  the hook avoids an infinite loop and the lease remains held.
- `release_control` can refuse while an isolated session or Deskpal-held mouse
  button still depends on the lease. The continuation tells Codex to clean up
  those resources first.
- Unknown application mutation outcomes must not be retried merely because the
  lease is being released.
- The helper relies on Linux `/proc` ancestry and `flock`; it is not portable to
  other operating systems.
- The configured command uses an absolute checkout path and must be updated if
  Deskpal moves.
- Non-Codex MCP clients still need their own lifecycle integration or an
  explicit `release_control` call.
