---
title: Migrating
description: Move from forti-hole 0.x (config.yaml, forti-hole_security_level feeds) or bootstrap a configuration from a Pi-hole.
order: 70
status: published
tags:
  - guides
---

# Migrating

:::toc[On this page]{depth="2" theme="compact"}
:::

## From forti-hole 0.x

forti-hole 1.0 is a rewrite: a new configuration (`~/.forti/hole/config.json`), forti profiles for
the connection, new feed names, and deltas instead of full re-uploads.

:::steps{numbered}

### Create a forti profile

`forti configure` replaces the old `fortigate:` section (API key, certificate, port). See
[Install](/getting-started/install#create-the-forti-profile).

### Convert the old configuration

```bash
forti-hole migrate config ~/forti-hole/config.yaml            # print the new configuration
forti-hole migrate config ~/forti-hole/config.yaml --write -  # write ~/.forti/hole/config.json
```

Lists and security levels become sources and tiers (`level0`, `level1`), DNS filters become
profiles. The old file's `fortigate:` section is skipped without being read into the new file: it
holds credentials. Levels a filter set to `monitor` or `allow` are reported (forti-hole profiles
block). The old firewall-policy assignments are listed so you can `attach` them.

### Remove the old feeds

```bash
forti-hole migrate feeds                  # list forti-hole_security_level-* feeds and their categories
forti-hole migrate feeds --apply          # free their categories in the profiles that list them, delete them
```

`--prefix` matches another naming convention. Nothing but those feeds and their filter entries is
changed.

### Sync

Review `forti-hole plan`, set `"enabled": true`, run `forti-hole sync`, and `attach` the profiles.
:::

## From a Pi-hole

If you are moving filtering from a Pi-hole to the FortiGate, `import pihole` writes a configuration
from its adlists, groups and exceptions. It is a one-time bootstrap: forti-hole never talks to the
Pi-hole again.

```bash
FORTI_HOLE_PIHOLE_PASSWORD=… forti-hole import pihole --url https://pihole.example.net --write -
```

- Each enabled group with blocklists becomes a profile; lists used by the same groups form one tier.
- hagezi TIF becomes a source with variants and `auto`.
- Exact allow entries become exceptions, regex allow entries `allow_regex`, exact deny entries
  `block` entries; regex deny entries cannot be carried over and are reported.
- The password comes from `FORTI_HOLE_PIHOLE_PASSWORD`, forti's secret store (where forti-pihole
  keeps it), or a prompt. It is never written anywhere.

Running a Pi-hole as your DNS server **and** forti-hole adds nothing: a query the Pi-hole answers
never reaches the FortiGate's DNS filter for that name. Use one or the other.
