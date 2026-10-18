---
title: Command line
description: Every forti-hole command and option, the JSON output and the exit statuses.
order: 80
status: published
tags:
  - reference
---

# Command line

```text
forti-hole [--config PATH] [--profile FORTI_PROFILE] [--json] [--quiet] COMMAND
```

| Option | Effect |
|---|---|
| `--config PATH` | The configuration file (default `~/.forti/hole/config.json`; the forti home is `$FORTI_HOME`, else `~/.forti`) |
| `--profile NAME` | The forti profile, overriding the configuration's `profile` |
| `--json` | One JSON document on stdout: `{"ok": true, "data": …}` or `{"ok": false, "error": {"code", "message"}}` |
| `--quiet`, `-q` | No progress lines; output only when something fails |

:::toc[On this page]{depth="2" theme="compact"}
:::

## Commands

| Command | Writes to the FortiGate | What it does |
|---|---|---|
| `setup [--force] [--offline]` | No | Writes the default configuration (disabled) and, unless `--offline`, checks the device |
| `doctor` | No | Firmware, entry limit, memory, free categories, name collisions, licence (for information), attachments, an unfinished sync |
| `compile [--offline] [--device] [--write DIR] [--lines]` | No | Fetches (or with `--offline` reads the cached copies of) every list, compiles, reports deduplication, feeds and the budget. `--device` reads the device's limit and memory; `--write` writes each feed and `report.json`; `--lines` adds every line to the JSON |
| `plan` | No | Everything a sync would do, with reasons and entry counts |
| `sync [--force-snapshot]` | Yes, if enabled | Compile, budget, create or update feeds and profiles, push, verify, clean up. `--force-snapshot` sends every feed in full |
| `status [--verify [--full]]` | No | The device's forti-hole feeds against what was confirmed, attachments, the last run. `--verify` samples entries back; `--full` reads every entry |
| `pause`, `resume` | No | Stop and restart a sync's writes |
| `attach PROFILE --policy ID` | Yes | Sets the policy's DNS filter to `<prefix>PROFILE` (and `utm-status enable` if it was off) |
| `detach --policy ID [--force]` | Yes | Restores what `attach` replaced; `--force` clears a forti-hole profile it has no record of |
| `remove --yes` | Yes | Deletes every forti-hole feed and DNS filter profile; refused while a profile is still in use |
| `import pihole --url URL [--name N] [--ca-bundle PEM] [--write PATH\|-]` | No | A configuration from a Pi-hole v6. `-` writes the default configuration path |
| `migrate config OLD.yaml [--write PATH\|-] [--force]` | No | A configuration from forti-hole 0.x |
| `migrate feeds [--prefix forti-hole_] [--apply]` | With `--apply` | forti-hole 0.x feeds: listed; with `--apply` their categories are freed and they are deleted |
| `version` | No | The version and what it was built from |

## Exit status

| Status | Meaning |
|---|---|
| 0 | Success (also: paused, nothing to do) |
| 1 | Internal error, including a guard refusal (a bug: please report it) |
| 2 | Usage |
| 3 | Configuration: missing or invalid config.json, state or forti profile |
| 4 | Connection or credentials (the FortiGate or a Pi-hole) |
| 5 | Not found (a policy, a profile that does not exist yet) |
| 6 | Refused: a name collision, an unavailable list, a FortiOS API error |
| 7 | Not enabled, a read-only profile, waiting for `forti approve`, or a stale plan |
| 8 | Part-way: a push failed or a feed did not verify; the next sync repairs it |
| 9 | The compiled set does not fit the device; nothing was written |

## JSON

`compile --json` (and the `compile` key of `plan` and `sync`) carries, among others:

```json
{
  "sources": [{"id": "hagezi-pro", "tier": "base",
               "parse": {"lines": 202311, "valid": 202297, "invalid": 0, "unsupported": 0, "declared": 202297},
               "duplicates_within": 0, "duplicates_across": 0, "unique": 202297}],
  "totals": {"valid_parsed": 962134, "duplicates_removed": 495982, "parent_subdomain_removed": 49141,
             "unique_names": 416765, "emitted_lines": 807914, "lines_per_profile": {"light": 720920}},
  "feeds": [{"name": "fh-all-1", "group": "all", "profiles": ["light", "standard", "strict"],
             "lines": 117105, "sha256": "052b6e20…"}],
  "variant_attempts": [{"source": "hagezi-tif", "variant": "medium", "lines": 2221664, "chosen": false}],
  "budget": {"lines": 807914, "fits": true,
             "entries": {"limit": 1000000, "available": 900000},
             "memory": {"used_percent": 54.7, "after_percent": 57.8, "ceiling_percent": 85.0}}
}
```

`sync --json` adds `plan` (actions per feed and profile, request counts) and `outcome` (per-feed
verification: expected, accepted, sampled, problem; deleted feeds; the memory measurement).
