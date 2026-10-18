---
title: Scheduling
description: Run forti-hole's sync twice a day with the systemd user timer, launchd or Homebrew services, and what an unattended run needs.
order: 60
status: published
tags:
  - guides
---

# Scheduling

hagezi rebuilds its lists several times a day; twice a day keeps the feeds current without load on
the list servers. A run with nothing new upstream costs a few conditional requests and status reads,
and writes nothing.

:::toc[On this page]{depth="2" theme="compact"}
:::

## What an unattended run needs

- `"enabled": true` in `~/.forti/hole/config.json`.
- A forti profile in **autonomous** mode. A `guarded` profile makes each sync wait for
  `forti approve`, and the timer run exits 7.
- Credentials the unit can read: a file-stored key (`forti configure` on Linux), or the macOS
  Keychain for a launchd agent in your session.

## systemd (Linux)

The package installs a user service and timer and never enables them:

```bash
systemctl --user enable --now forti-hole.timer
systemctl --user list-timers forti-hole.timer
journalctl --user -u forti-hole.service
loginctl enable-linger "$USER"      # keep the timer running while you are logged out
```

The timer starts at 04:00 and 16:00 with a random delay of up to 45 minutes, and catches up a missed
run after the machine was off. Exit 7 (not enabled, paused, waiting for approval) is not a unit
failure.

## launchd (macOS)

```bash
cp "$(brew --prefix)/share/forti-hole/com.valkyrianlabs.forti-hole.plist" ~/Library/LaunchAgents/
launchctl bootstrap "gui/$(id -u)" ~/Library/LaunchAgents/com.valkyrianlabs.forti-hole.plist
```

Or, with Homebrew on either system: `brew services start forti-hole` (every 12 hours).

## History

Every sync appends one line to `~/.forti/hole/history.jsonl`: when, the line count, the variants
chosen, the result, the exit status and the engine plan ids. `forti-hole status` shows the last one.
