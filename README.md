# forti-hole

**DNS blocklists as FortiGate threat feeds, deduplicated, budgeted and verified.**

forti-hole downloads the DNS blocklists you choose (hagezi, StevenBlack, your own lists), merges them
into one deduplicated set, and keeps it on a FortiGate as push-mode external threat feeds that the
FortiGate's DNS filter profiles block. Ad, tracker and threat blocking for every network behind the
firewall, with no resolver to run.

- **Deduplicated for real.** Every name is kept once across lists and filtering tiers; names already
  covered by a blocked parent domain are dropped; each feed line is published exactly once. The report
  shows every count, per list.
- **Fits the device, or says why not.** The compiled set is checked against the FortiGate's
  device-wide entry limit (1,000,000 on a 60F) and its memory before anything is written. hagezi TIF
  uses the largest variant that fits. Nothing is silently truncated.
- **Deltas, verified.** Only changed entries are pushed; the device is read back after every push. An
  unchanged run writes nothing.
- **Fail-static.** A failed or broken download falls back to the last good copy; a feed is never
  emptied by a network error. An interrupted sync is repaired by the next one.
- **Leaves your objects alone.** It writes only feeds and DNS filter profiles carrying its prefix and
  ownership marker, and touches a firewall policy only when you run `forti-hole attach`.

## When to use it, and when not to

forti-hole is for networks with a FortiGate whose owners do **not** want to run their own DNS server.
A real Pi-hole (or AdGuard Home) as your resolver does more: per-client query logs, instant
allow-listing from a dashboard, regex blocking, and no device-wide entry limit. If you already run
one, forti-hole adds nothing; use one or the other.

A DNS filter, forti-hole's or a Pi-hole's, only sees DNS sent to it: DNS over HTTPS, hard-coded
resolvers and VPNs bypass it unless your policies block them
([Where filtering applies](docs/guides/topologies.md)).

## Capacity

| FortiGate class (FortiOS 7.4.4+) | Domain-entry limit | Default lists with |
|---|---|---|
| Entry-level, 2 GB (40F, 60F, 61F, 70F, 80F) | 1,000,000 | hagezi TIF **mini**: 807,914 lines |
| Mid-range | 3,000,000 | TIF **medium**: 2,221,664 lines |
| High-end | 5,000,000 | TIF full |

The limit is shared by every domain feed on the device. Twelve to twenty full 131k-line feeds
(1.6–2.6 million entries) overflow a 2 GB model: FortiOS 7.4.4+ drops what does not fit, and older
firmware runs out of memory first. forti-hole's budget prevents both
([Capacity](docs/guides/capacity.md)).

## Install

```sh
brew install valkyrianlabs/tap/forti-cli valkyrianlabs/tap/forti-hole    # macOS, Linuxbrew
sudo apt install forti-hole                                              # Debian/Ubuntu amd64, apt.valkyrianlabs.com
```

You need a FortiGate on FortiOS 7.2.1 or later (7.4.4+ recommended) and a forti profile for it
(`forti configure`, from [forti-cli](https://github.com/valkyrianlabs/forti-cli)). A FortiGuard
subscription is not required. [Install](docs/getting-started/install.md) has the APT setup and the
source build.

## Quick start

```sh
forti configure                       # the FortiGate profile (forti-cli)
forti-hole setup                      # ~/.forti/hole/config.json, "enabled": false
forti-hole compile --device           # deduplication report, and whether it fits this FortiGate
forti-hole plan                       # every change a sync would make (writes nothing)
# set "enabled": true in ~/.forti/hole/config.json
forti-hole sync                       # create, push, verify
forti-hole attach standard --policy 2 # filter a firewall policy's DNS with fh-standard
systemctl --user enable --now forti-hole.timer    # twice a day (or: brew services start forti-hole)
```

Coming from forti-hole 0.x: `forti-hole migrate config config.yaml` and `forti-hole migrate feeds`
([Migrating](docs/guides/migration.md)).

## Documentation

| | |
|---|---|
| [Overview](docs/index.md) | What it does, measured on a FortiGate-60F |
| [Install](docs/getting-started/install.md), [Quick start](docs/getting-started/quick-start.md) | Getting started |
| [Lists, tiers and profiles](docs/guides/sources-and-profiles.md), [Capacity](docs/guides/capacity.md), [Where filtering applies](docs/guides/topologies.md) | How it works |
| [Safety](docs/guides/safety.md), [Scheduling](docs/guides/scheduling.md), [Migrating](docs/guides/migration.md) | Operating it |
| [Command line](docs/reference/cli.md), [Configuration](docs/reference/configuration.md), [Troubleshooting](docs/reference/troubleshooting.md) | Reference |
| [Building and testing](docs/development/testing.md) | Development |

An agent skill for operating forti-hole is in [skill/forti-hole](skill/forti-hole/SKILL.md).

## License

MIT
