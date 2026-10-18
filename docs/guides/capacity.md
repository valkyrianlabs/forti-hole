---
title: Capacity
description: How many entries a FortiGate holds, what they cost in memory, how forti-hole budgets both from the deduplicated set, and which hagezi TIF variant fits which model.
order: 30
status: published
tags:
  - guides
---

# Capacity

A FortiGate holds a limited number of feed entries, and each costs memory. forti-hole checks both
**before** it writes, from the deduplicated lines it would publish, never from a list's advertised
size, and refuses a set that does not fit rather than letting FortiOS drop part of it.

:::toc[On this page]{depth="2" theme="compact"}
:::

## The entry limit

Since FortiOS 7.4.4 the limit is **device-wide**, across every domain feed (forti-hole's and yours).
Fortinet documents three model classes; the device reports its own limit (`accepted_limit`) once any
domain feed exists, and forti-hole uses that figure.

| Class | Domain entries (7.4.4+) | Examples |
|---|---|---|
| Entry-level (2 GB RAM) | 1,000,000 | 40F, 60F, 61F, 70F, 80F |
| Mid-range | 3,000,000 | |
| High-end | 5,000,000 | |

Entries past the limit are not loaded, and FortiOS does not fail the push: the feed reports
`overflow`. forti-hole checks for it after every push. Before 7.4.4 there is no device-wide limit,
only 131,072 lines per feed (forti-hole's default `feeds.max_entries`); memory is then the only
ceiling, which is why too many full-size feeds pushed an older 60F into conserve mode.

:::callout[Twelve full feeds is too many for a 2 GB FortiGate]{variant="warning"}
Twelve to twenty feeds of 131,072 lines are 1.6 to 2.6 million entries. On 7.4.4 and later an
entry-level FortiGate loads one million and silently drops the rest; on older firmware the memory runs
out first. forti-hole's budget makes either outcome impossible by default.
:::

### What forti-hole allows

```text
available = limit − entries in feeds forti-hole does not own − entry_headroom_percent of the limit
```

`entry_headroom_percent` defaults to 10. When the device has not reported its limit yet (no domain
feed exists, or FortiOS before 7.4.4) `budget.fallback_limit` (1,000,000) stands in.

## Memory

FortiOS keeps feed entries in memory, and conserve mode starts at the
`memory-use-threshold-red` setting (88% by default). forti-hole reads the device's total and used
memory (`monitor/system/performance/status`) and predicts the change:

```text
predicted used = used now + (lines after − lines on the device now) × bytes per line
```

The prediction must leave `memory_headroom_percent` (default 15) free and stay 3 points under the red
threshold. Shrinking is always allowed. It also warns when the last 24 hours' memory **peak** plus
the change would reach the red threshold.

**Bytes per line** starts at a conservative 400 and is then learned from the device: after a push
that adds at least 50,000 lines, forti-hole compares memory before and after and keeps the larger of
its estimate and the measurement (it comes down only slowly). Measured on a FortiGate-60F
(FortiOS 7.4.8):

| Load | Memory during the apply | Settled |
|---|---|---|
| 73,633 lines (StevenBlack) | +2 MB | +3 MB |
| 448,503 lines | +66 MB (≈175 bytes per line pushed) | +4 MB |
| 807,914 lines (the default set) | +30 MB | +4 MB |

The cost is mostly transient, while FortiOS applies a push; settled, a large set costs little.
forti-hole budgets for the transient peak.

## hagezi TIF: full, medium, mini

hagezi's Threat Intelligence Feeds come in three sizes. Their compiled cost depends on what the other
lists already cover, so forti-hole decides on the compiled set: with `"variant": "auto"` it compiles
each candidate in `auto` order (medium, then mini) together with every other source and keeps the
first that fits both budgets. Measured on 2026-10-18 with the default lists:

| Variant | TIF entries | Whole set, strict encoding | Entry-level (1,000,000) | Mid-range (3,000,000) |
|---|---|---|---|---|
| full | 2,033,060 | ≈4.5 million | no | no |
| medium | 939,721 | 2,221,664 | no | yes |
| mini | 229,609 | 807,914 | **yes** | yes |

On a 2 GB FortiGate the default configuration therefore uses **mini**. On a mid-range model `auto`
picks medium; set `"variant": "full"` only on a high-end one. `forti-hole compile --device` shows the
choice and the figures for your device.

:::callout[Is there a lossless way to fit medium on a 60F?]{variant="note"}
No. forti-hole already removes every duplicate and every name covered by a blocked parent. A strict
FortiOS entry matches a name exactly, so each adblock rule needs two lines (`name` and `*.name`).
`*name` would need one, but it also matches any name merely ending in it (`*ads.com` blocks
`uploads.com`). Even at one line per name medium needs about 1.2 million entries: over the limit
before any headroom.
:::

## Encodings

| Encoding | An adblock rule `\|\|example.com^` becomes | Lines per rule |
|---|---|---|
| `strict` (default) | `example.com`, `*.example.com` | 2 |
| `compact` | `*example.com`, unless a name in your reference list would also match, then strict | 1 |

`compact` needs `reference`, a list of real names (a Tranco top-1M CSV, from a URL or a local file),
and checks every candidate against it. A name outside that list can still be over-matched, so
`compact` is opt-in. Hosts-file and plain-domain entries are exact names: one line either way.

## When the set does not fit

A sync with a set that fits neither budget writes nothing, exits **9** and says why:

```text
Nothing was written: 807914 lines needed, 700000 available of the device's 1000000-entry limit (0 in other feeds, 30% kept free)
```

Choose a smaller variant or fewer lists, or move a tier to fewer profiles. `budget.on_overrun:
"warn"` pushes anyway; FortiOS then drops what does not fit and `status` reports it.
