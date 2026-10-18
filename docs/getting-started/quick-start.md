---
title: Quick start
description: Set up forti-hole, see what your lists compile to and whether they fit the device, sync, verify, and attach a DNS filter profile to a firewall policy.
order: 20
status: published
tags:
  - getting-started
---

# Quick start

From a FortiGate with a forti profile to blocklists the device verifiably holds. Nothing is written
to the FortiGate until step 4.

:::toc[On this page]{depth="2" theme="compact"}
:::

:::steps{numbered}

### Write the configuration

```bash
forti-hole setup
```

`setup` checks the device and writes `~/.forti/hole/config.json` with the default lists, three
profiles and `"enabled": false`. `--profile NAME` picks a forti profile other than the default one;
`--offline` skips the device check.

### See what the lists compile to

```bash
forti-hole compile --device
```

`compile` downloads every list, deduplicates them and reports, per source, how many entries were
valid, invalid, duplicated within the list and already contributed by another list, then the totals:
unique names, FortiOS feed lines and the feeds they go into. `--device` also reads the FortiGate's
entry limit and memory and says whether the set fits:

```text
Variant choice
  hagezi-tif medium: 2,221,664 lines, does not fit: 2221664 lines needed, 900000 available ...
  hagezi-tif mini: 807,914 lines, fits  <- chosen

Budget
  entries: 807,914 of 900,000 available (1,000,000 limit reported by the device, 0 in other feeds, 10% headroom) -> fits
  memory:  54.7% used now, 57.8% predicted (+63 MB at 175 bytes/line), ceiling 85.0% -> fits
```

`--write DIR` writes every feed as a text file and the report as JSON, for inspection.

### Review the plan

```bash
forti-hole plan
```

Every feed and DNS filter profile a sync would create or change, with the reason and the number of
entries sent. `plan` writes nothing to the FortiGate.

### Enable and sync

Set `"enabled": true` in `~/.forti/hole/config.json`, then:

```bash
forti-hole sync
```

The first sync creates the feeds (`fh-all-1`, `fh-light-1`, ...) and the DNS filter profiles
(`fh-light`, `fh-standard`, `fh-strict`), waits for FortiOS to load the new feeds, pushes them, and
verifies each on the device. Later syncs send only what changed upstream.

### Check what the device holds

```bash
forti-hole status --verify --full
```

Reads every entry back from the device and compares it with what was compiled. Without `--full`,
`--verify` samples pages; without `--verify`, `status` compares counts and update times.

### Attach a profile

The feeds filter nothing until a DNS filter profile that blocks them is on a firewall policy that DNS
queries cross ([Where filtering applies](/guides/topologies)):

```bash
forti-hole attach standard --policy 2
```

This sets the policy's DNS filter to `fh-standard` (and turns its security profiles on if they were
off). `forti-hole detach --policy 2` puts back what was there before.

### Keep it current

```bash
systemctl --user enable --now forti-hole.timer     # Linux
brew services start forti-hole                     # Homebrew
```

Twice a day; see [Scheduling](/guides/scheduling).
:::

## If something looks wrong

`forti-hole doctor` checks the firmware, the entry limit, memory, free categories, name collisions,
whether any policy uses a forti-hole profile, and whether the last sync finished.
[Troubleshooting](/reference/troubleshooting) explains every exit status.
