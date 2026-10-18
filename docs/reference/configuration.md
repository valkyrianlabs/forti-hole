---
title: Configuration
description: Every key of ~/.forti/hole/config.json with its default, and the files forti-hole keeps next to it.
order: 90
status: published
tags:
  - reference
---

# Configuration

`~/.forti/hole/config.json` (`$FORTI_HOME/hole/config.json`). Unknown keys are refused, with the key
and its location in the message. `forti-hole setup` writes the example below
(`share/forti-hole/config.example.json`).

:::toc[On this page]{depth="2" theme="compact"}
:::

## Top level

| Key | Default | Meaning |
|---|---|---|
| `format` | `1` | Configuration format |
| `enabled` | `false` | Nothing is written to the FortiGate until `true` |
| `profile` | `""` | The forti profile; empty: the default profile |
| `vdom` | `""` | A VDOM; empty: the profile's |
| `prefix` | `"fh-"` | Name prefix of every object forti-hole owns (2–20 characters, ending in `-`) |
| `tiers` | | Tier names, in order |
| `sources` | | The lists ([Lists, tiers and profiles](/guides/sources-and-profiles)) |
| `profiles` | | `{"name": {"tiers": [...]}}`; each is the DNS filter profile `<prefix><name>` |
| `allow`, `allow_regex`, `block` | `[]` | Exceptions and extra entries |
| `encoding` | `"strict"` | `strict` or `compact` ([Capacity](/guides/capacity#encodings)) |
| `reference` | | `{"url", "format": "tranco"}`: real names for `compact` |

## `sources[]`

| Key | Meaning |
|---|---|
| `id` | 1–48 characters of `[a-z0-9._-]`, unique |
| `tier` | One of `tiers` |
| `format` | `adblock`, `hosts`, `domains`, `wildcard`, `subtree-domains` |
| `url` | `https://…` or `file:///…` |
| `variants`, `variant`, `auto` | Instead of `url`: named URLs, the one to use (`auto` or a name), and the `auto` order |
| `enabled` | `true` |

## `budget`

| Key | Default | Meaning |
|---|---|---|
| `entry_headroom_percent` | `10` | Kept free of the device's domain-entry limit |
| `memory_headroom_percent` | `15` | Kept free of device memory after the change |
| `fallback_limit` | `1000000` | The entry limit when the device does not report one |
| `bytes_per_line` | learned | Device memory per feed line; leave unset to use the measured value |
| `on_overrun` | `"refuse"` | `refuse` (nothing written, exit 9) or `warn` |

## `feeds`, `sync`, `fetch`, `dnsfilter`

| Key | Default | Meaning |
|---|---|---|
| `feeds.max_entries` | `131072` | Lines per device feed |
| `feeds.category_min`, `category_max` | `192`, `221` | The remote categories forti-hole may use |
| `sync.max_shrink_percent` | `20` | A list this much smaller than its last good copy is rejected |
| `sync.chunk_entries` | `50000` | Entries per delta request |
| `sync.load_timeout_seconds` | `120` | Waiting for FortiOS to load a feed or apply a push |
| `sync.verify_samples`, `verify_page` | `3`, `500` | Pages read back per changed feed, and their size |
| `fetch.timeout_seconds` | `120` | Per download |
| `fetch.max_bytes` | 128 MiB | Largest list accepted |
| `fetch.parallel` | `6` | Simultaneous downloads |
| `fetch.ca_bundle` | | Extra trusted CAs (PEM); verification stays on |
| `dnsfilter.manage_profiles` | `true` | Create and maintain the DNS filter profiles |
| `dnsfilter.block_action` | `"block"` | `block` (NXDOMAIN), `redirect` (block portal) or `block-sevrfail` |
| `dnsfilter.error_allow` | `true` | `ftgd-dns options error-allow`: resolve normally when FortiGuard rating is unavailable |
| `dnsfilter.log` | `true` | Log blocked queries |

## The example

```json
{
  "format": 1,
  "enabled": false,
  "profile": "",
  "prefix": "fh-",
  "tiers": ["base", "plus", "ultimate", "threat"],
  "sources": [
    {"id": "stevenblack", "tier": "base", "format": "hosts", "url": "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts"},
    {"id": "hagezi-pro", "tier": "base", "format": "adblock", "url": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/pro.txt"},
    {"id": "hagezi-pro-plus", "tier": "plus", "format": "adblock", "url": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/pro.plus.txt"},
    {"id": "hagezi-ultimate", "tier": "ultimate", "format": "adblock", "url": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/ultimate.txt"},
    {"id": "hagezi-tif", "tier": "threat", "format": "adblock",
     "variants": {"full": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/tif.txt",
                  "medium": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/tif.medium.txt",
                  "mini": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/tif.mini.txt"},
     "variant": "auto", "auto": ["medium", "mini"]}
  ],
  "profiles": {
    "light": {"tiers": ["base", "threat"]},
    "standard": {"tiers": ["base", "plus", "threat"]},
    "strict": {"tiers": ["base", "plus", "ultimate", "threat"]}
  },
  "encoding": "strict",
  "budget": {"entry_headroom_percent": 10, "memory_headroom_percent": 15, "on_overrun": "refuse"},
  "dnsfilter": {"manage_profiles": true, "block_action": "block", "error_allow": true, "log": true}
}
```

## Files

| Path (under `~/.forti/hole/`) | What |
|---|---|
| `config.json` | Yours |
| `state.json` | Last confirmed device state per feed, categories, list cache metadata, memory measurements |
| `feeds/<feed>.txt` | Exactly what each feed was last confirmed to hold |
| `cache/<list>.list` | The last good copy of every list |
| `journal.json` | Present only while a sync writes, or after one stopped part-way |
| `history.jsonl` | One line per sync |

Every file is 0600 in a 0700 directory and written atomically. Credentials stay in forti's secret
store. Plans and the audit log are the engine's (`~/.forti/state/`).
