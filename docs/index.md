---
title: forti-hole
description: DNS blocklists as FortiGate threat feeds, deduplicated across sources and tiers, checked against the device's entry and memory limits, pushed as deltas and verified on the device.
order: 0
status: published
tags:
  - overview
---

# forti-hole

**DNS blocklists as FortiGate threat feeds, deduplicated, budgeted and verified.**

forti-hole downloads the DNS blocklists you choose (hagezi, StevenBlack, your own lists), merges them
into one deduplicated set, and keeps that set on a FortiGate as push-mode **external threat feeds**.
FortiGate DNS filter profiles block the feeds' categories, so every DNS query that crosses a policy
with one of those profiles is filtered by your lists, with no resolver to run.

:::toc[On this page]{depth="2" theme="compact"}
:::

## When forti-hole is the right tool

:::callout[forti-hole or a real Pi-hole?]{variant="info"}
forti-hole is for networks that already have a FortiGate and do not want to run their own DNS server.
If you are willing to own DNS (a Pi-hole, AdGuard Home or Unbound box), a real resolver does more:
per-client query logs, instant allow-listing from a dashboard, regex blocking, CNAME inspection, and
no device-wide entry limit. Use one or the other: a Pi-hole behind a FortiGate already filters, and
forti-hole adds nothing to it.
:::

| You want | Use |
|---|---|
| Ad, tracker and threat blocking on a FortiGate network, nothing else to run | **forti-hole** |
| Query logs, per-client dashboards, instant allow-lists, regex rules | Pi-hole (or AdGuard Home) as your DNS server |
| Pi-hole groups kept in step with FortiGate devices | [forti-pihole](https://github.com/valkyrianlabs/forti-pihole) |

## What it does

:::steps

### Fetch

Every list is downloaded in parallel with conditional requests: an unchanged list costs a `304` and
nothing else. A download that fails, returns a web page, is truncated or shrinks by more than 20% is
rejected, and the last good copy is used instead. A failed download never empties a feed.

### Compile

Every name is parsed, canonicalized (lowercase, no trailing dot, punycode) and kept **once**, across
sources and across filtering tiers. Names already covered by a blocked parent domain are dropped.
Adblock rules keep their meaning on the FortiGate: `||example.com^` becomes `example.com` and
`*.example.com`, because a FortiOS feed entry `example.com` matches that name only.

### Budget

The compiled line count is checked against the device's **domain-entry limit** (1,000,000 on
entry-level models such as the FortiGate-60F) and against its **memory**, before anything is
written. A set that does not fit is refused, never silently truncated. hagezi TIF picks its largest
variant that fits (`auto`: medium, else mini).

### Push

Only what changed is sent: `add` and `remove` commands for the entries that differ, a full snapshot
only for a new feed or one whose device contents can no longer be trusted.

### Verify

After every push forti-hole reads the device back: the entry count must equal the compiled count,
nothing may be invalid or truncated, and sampled entries must be compiled lines. Only then is a
feed's content recorded as confirmed.
:::

## Measured on a FortiGate-60F

The default configuration (StevenBlack, hagezi pro, pro.plus, ultimate and TIF in four tiers, three
profiles), compiled on 2026-10-18 and loaded on a FortiGate-60F (FortiOS 7.4.8, 2 GB RAM):

| | |
|---|---|
| Valid entries in the five lists | 962,134 |
| Duplicates removed (within and across lists) | 495,982 |
| Covered by a blocked parent domain | 49,141 |
| Unique names published | 416,765 |
| FortiOS feed lines (TIF mini, strict encoding) | **807,914** in 11 feeds |
| TIF medium instead | 2,221,664 lines: does not fit the 1,000,000-entry limit |
| Device memory during the first load / an hour later | +1.5% / +0.2% of 2 GB |
| A sync with no upstream change | 0 writes, about 5 seconds |

See [Capacity](/guides/capacity) for the limits and how forti-hole stays inside them.

## Safety

- **It never touches what it did not create.** Feeds and DNS filter profiles are forti-hole's only
  when they carry its name prefix **and** its ownership marker; everything else is read-only, and a
  name collision stops the run. A guard under the engine refuses any other write before it is sent.
- **Policies change only when you say so** (`forti-hole attach`), never during a sync.
- **Nothing is written until `"enabled": true`**, and every write goes through the forti engine's
  plan and apply: your forti profile's mode (`read-only`, `guarded`, `autonomous`) applies, and every
  change is audited.

## Next

:::cards{columns="3"}
:::card[Install]{href="/getting-started/install"}
Homebrew, APT, or a source build.
:::
:::card[Quick start]{href="/getting-started/quick-start"}
From an empty FortiGate to verified feeds.
:::
:::card[Where filtering applies]{href="/guides/topologies"}
Which network layouts a FortiGate DNS filter can see.
:::
:::
