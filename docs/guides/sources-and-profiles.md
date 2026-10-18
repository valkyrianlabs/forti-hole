---
title: Lists, tiers and profiles
description: Sources and their formats, tiers, profiles, your own lists, allow and block entries, and how forti-hole deduplicates and groups them into feeds.
order: 20
status: published
tags:
  - guides
---

# Lists, tiers and profiles

:::toc[On this page]{depth="2" theme="compact"}
:::

## The model

- A **source** is one list (a URL or a local file) in one **tier**.
- A **profile** blocks a set of tiers. Each profile is one FortiOS DNS filter profile,
  `<prefix><profile>` (`fh-standard`).
- Every name ends up blocked by a set of profiles. Lines blocked by exactly the same profiles share a
  **group** of feeds and one remote category. Every profile in the set blocks that category.

The default configuration mirrors a typical Pi-hole setup:

| Tier | Source | light | standard | strict |
|---|---|---|---|---|
| base | StevenBlack hosts, hagezi pro | ✓ | ✓ | ✓ |
| plus | hagezi pro.plus | | ✓ | ✓ |
| ultimate | hagezi ultimate | | | ✓ |
| threat | hagezi TIF (`auto`) | ✓ | ✓ | ✓ |

## Deduplication

Nothing is published twice:

1. One record per name across every source and tier. A repeat in the same list and a name another
   list already contributed are counted (`dup.within`, `dup.across` in the report) and dropped.
2. A name below a blocked subtree is dropped for every profile the parent already blocks it for
   (`a.example.com` under `||example.com^`). Profiles that still need it keep it, in a smaller group.
3. Each distinct feed line lives in exactly one feed. A feed's content hash (SHA-256) depends on
   its contents alone.

`forti-hole compile` reports every step: lines, valid, invalid, unsupported, duplicates within and
across lists, names covered by a parent, unique names, and feed lines per profile.

## Source formats

| `format` | Lines | Meaning on the FortiGate |
|---|---|---|
| `adblock` | `\|\|name^`, `@@\|\|name^` | The name and its subdomains; `@@` is an exception. A bare `name` line is exact |
| `hosts` | `0.0.0.0 name [name...]` | Exact names. Other addresses (redirects) are skipped; `localhost` and the like ignored |
| `domains` | `name` | Exact names |
| `wildcard` | `*.name`, `name` | `*.name`: the name and its subdomains; `name`: exact |
| `subtree-domains` | `name` | The name and its subdomains (hagezi's `onlydomains` lists) |

Adblock modifiers (`$third-party`, `$denyallow`...), paths, mid-name wildcards and cosmetic rules have
no meaning in a DNS feed. They are counted as unsupported and left out. `$important` is accepted.
Names are lowercased, a trailing dot is dropped, and internationalized names become punycode.
Malformed names are counted as invalid, with the first one shown.

## Your own lists

A source can be any `https://` URL or a local file:

```json
{"id": "office", "tier": "base", "format": "domains", "url": "file:///etc/forti-hole/office-block.txt"}
```

Single names go in the configuration:

```json
"block": [{"name": "evil.example", "scope": "subtree", "tier": "base"}],
"allow": [{"name": "needed.example", "scope": "exact", "profiles": ["standard"]}],
"allow_regex": [{"pattern": "^cdn[0-9]+\\.vendor\\.example$"}]
```

An exception removes the name from the feeds for its profiles (all profiles when `profiles` is left
out). FortiOS feeds cannot express "block `example.com` except `safe.example.com`": an exception
below a blocked subtree, and every regex exception, is reported by `compile` as a static exception
for the profile's domain filter.

## TIF and other variants

A source can offer variants instead of one URL:

```json
{"id": "hagezi-tif", "tier": "threat", "format": "adblock",
 "variants": {"full": "…/tif.txt", "medium": "…/tif.medium.txt", "mini": "…/tif.mini.txt"},
 "variant": "auto", "auto": ["medium", "mini"]}
```

`"variant": "auto"` keeps the first candidate whose compiled set fits the device
([Capacity](/guides/capacity#hagezi-tif-full-medium-mini)). A named variant is always used.

## Feeds

Each group is split into feeds of at most `feeds.max_entries` lines (131,072 by default), by a
stable hash of the name: adding a name changes one feed, not every feed after it. The partition count
of a group is kept between runs. Feed names are `<prefix><group>-<n>`: `fh-all-1` (every profile),
`fh-standard_strict-1`; a long group name becomes a short hex label. Remote categories (192–221)
are assigned per group and kept, and never one an operator's feed uses.
