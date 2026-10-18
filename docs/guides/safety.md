---
title: Safety
description: What forti-hole writes and never writes, how it stays fail-static when lists or the device misbehave, and how an interrupted sync recovers.
order: 50
status: published
tags:
  - guides
---

# Safety

forti-hole changes a firewall unattended, so it writes as little as it can, only what it can prove is
its own, and nothing at all when in doubt.

:::toc[On this page]{depth="2" theme="compact"}
:::

## What it writes

| Object | When |
|---|---|
| `system external-resource` `<prefix>*`, with the marker `managed by forti-hole` in `comments` | Every sync that changes them: create, set category, delete when obsolete |
| Push commands (`monitor/system/external-resource/dynamic`) naming only those feeds | Every sync that changes their contents |
| `dnsfilter profile` `<prefix><profile>`, with the marker in `comment` | Created and kept current (`ftgd-dns` filters for its categories, `block-action`, `error-allow`) |
| `firewall policy <id>`: `dnsfilter-profile` and `utm-status` only | Only `forti-hole attach` / `detach`, for the policy you name |
| A profile that still lists a forti-hole 0.x category | Only `forti-hole migrate feeds --apply`: that filter entry is removed |

This is enforced twice: every planned operation is checked before planning, and a guard under the
engine refuses any other `POST`, `PUT` or `DELETE` before it is sent. An object is forti-hole's only
with the prefix **and** the marker. An object with the prefix but without the marker stops the run.

## Fail-static

- **A failed, refused or implausible download** (an error, an HTML page, a count far from what the
  list's header declares, or a list more than `sync.max_shrink_percent` smaller than last time)
  falls back to the last good copy. Without one, nothing is compiled and nothing written.
- **A set that does not fit** the entry limit or memory is not pushed (exit 9).
- **Profiles keep blocking during a change.** A sync adds new categories first and removes old ones
  only after every new feed has verified; obsolete feeds are deleted last.
- **`"enabled": false`** (as `setup` writes it) and `pause` stop all writes; `plan`, `compile`,
  `status` and `doctor` never write to the device.

## Verification and recovery

A feed's content is recorded as confirmed only after the device shows it: the content-update time
moved, the accepted count equals the compiled count, nothing is invalid or truncated, and sampled
entries are compiled lines. A sync writes a journal before its first device write and removes it after
its last state update. A run that dies part-way (power, `kill -9`, a device error) leaves the journal,
and the next run sends every feed it names in full and verifies it again. A feed whose device count
or update time no longer matches what was confirmed (a reboot, someone else's push) is also sent in
full.

FortiOS applies a push asynchronously and refuses a second push to the same feed until the first is
applied ("Unable to unlock external resource push file"). forti-hole therefore sends at most one
request per feed at a time, and waits for each to apply before the next.

## Engine safeguards

Every write is an engine plan and apply, so the forti profile's mode applies (`read-only` refuses,
`guarded` waits for `forti approve`), each apply re-reads the device first, refuses a stale plan, and is
appended to the audit log (`~/.forti/state/audit.jsonl`). Large push bodies are stored once by
content hash while a plan is pending, and deleted after it is applied.

## Secrets

The API key and certificate password stay in forti's secret store (`forti configure`). forti-hole's
own files (`~/.forti/hole/`) are 0600 in 0700 directories and never contain a secret.
