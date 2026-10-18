---
name: forti-hole
description: Operate forti-hole, which keeps DNS blocklists on a FortiGate as deduplicated, budget-checked push-mode threat feeds, on the user's behalf with its command line. Use when the user asks which lists are loaded, how many entries a FortiGate holds or can hold, whether hagezi TIF medium fits, why a domain is (not) blocked, wants a sync planned, run or verified, a DNS filter profile attached to a firewall policy, a list or exception added, or forti-hole 0.x cleaned up.
---

# forti-hole

forti-hole downloads DNS blocklists, deduplicates them across sources and **tiers**, and keeps the
result on a FortiGate as push-mode external threat feeds (`fh-<group>-<n>`). Its DNS filter profiles
(`fh-<profile>`) block the feeds' categories. A **profile** blocks a set of tiers. Lines blocked by the
same profiles share a group of feeds and one remote category.

You run it with shell commands. Its files are in `$FORTI_HOME/hole/` (default `~/.forti/hole/`):
`config.json` (the user's), `state.json`, `feeds/`, `cache/`, `journal.json`, `history.jsonl`.
Credentials live in forti's secret store: never try to read, print or copy them.

## 1. Look before you change anything

All of these are read-only on the FortiGate. Prefer `--json` when you need to parse the output.

| Command | Answers |
|---|---|
| `forti-hole doctor` | Firmware, the device's entry limit, memory and conserve threshold, free categories, name collisions, whether any policy uses a forti-hole profile, an unfinished sync |
| `forti-hole status` | Each forti-hole feed's device count against what was confirmed, attachments, the last sync |
| `forti-hole status --verify --full` | Reads every entry back and compares it with what was compiled (slow on large sets: about a minute for 800,000) |
| `forti-hole compile --device` | Per list: valid, invalid, duplicates within and across lists; totals; feeds; which TIF variant fits; the entry and memory budgets |
| `forti-hole plan` | Every feed and profile a sync would create or change, and why |

## 2. Changing things

- **`sync`** writes only when `"enabled": true` and not paused, and only what changed. The forti
  profile's mode decides: `read-only` refuses, `guarded` stops with `waiting for approval of plan <id>`.
  Tell the user to run `forti approve <id>` themselves; never approve for them.
- **`attach PROFILE --policy ID` / `detach --policy ID`** change a firewall policy. Only do this when
  the user names the policy, and say which traffic it affects. `detach` restores what was there before.
- **Lists, tiers, profiles, exceptions** are edits to `config.json` (`sources`, `tiers`, `profiles`,
  `allow`, `allow_regex`, `block`). Run `forti-hole compile --device` after editing: unknown keys and
  mistakes are reported with their location, and the budget says whether it still fits.
- **`remove --yes`** deletes every forti-hole feed and profile. Only on an explicit request.

## 3. Capacity facts to get right

- FortiOS 7.4.4+ caps domain-feed entries **device-wide**: 1,000,000 on entry-level models
  (40F/60F/61F/70F/80F), 3,000,000 mid-range, 5,000,000 high-end. Entries past it are silently
  dropped by FortiOS; forti-hole refuses such a set (exit 9) instead.
- A strict adblock rule costs two lines (`name`, `*.name`). The default lists with TIF **mini** are
  about 808,000 lines. TIF **medium** is about 2.2 million and does not fit an entry-level FortiGate.
  Do not suggest `compact` encoding just to make something fit: it can over-match unrelated names.
- forti-hole budgets memory from the lines it would add, with 15% free memory kept by default.

## 4. Things to never claim

- That a DNS filter stops DNS over HTTPS, hard-coded resolvers or VPNs. It does not.
- That forti-hole and a Pi-hole together filter more: a Pi-hole answers before the FortiGate's filter
  sees the name. Use one or the other.
- That a FortiGuard subscription is needed. It is not; profiles use `error-allow`.

## Exit statuses

0 ok · 2 usage · 3 config · 4 connection · 5 not found · 6 refused (collision, unavailable list, API
error) · 7 not enabled / approval / read-only · 8 part-way or not verified (the next sync repairs it) ·
9 does not fit the device (nothing written).
