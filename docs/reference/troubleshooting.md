---
title: Troubleshooting
description: What forti-hole's messages and exit statuses mean, and what to do about each.
order: 100
status: published
tags:
  - reference
---

# Troubleshooting

Start with `forti-hole doctor`, then `forti-hole status`.

:::toc[On this page]{depth="2" theme="compact"}
:::

## Nothing was written: … lines needed, … available (exit 9)

The compiled set is larger than the device's entry limit allows, or would leave too little memory.
See [Capacity](/guides/capacity). Use a smaller variant, fewer lists, or fewer tiers in a profile;
other feeds on the device count against the same limit.

## objects named fh-* exist without forti-hole's marker (exit 6)

Something on the device has forti-hole's prefix but not its marker: forti-hole will not touch it. Rename
it, or set another `prefix`.

## list … is unavailable (exit 6)

A list could not be downloaded and there is no last good copy yet (the first run). Check the URL and
network access from this machine; after one good download, failures fall back to that copy.

## waiting for approval of plan … (exit 7)

The forti profile is `guarded`. Run `forti approve <id>`, then `forti-hole sync` again. A timer needs
an `autonomous` profile.

## forti-hole is not enabled (exit 7)

Set `"enabled": true` in `~/.forti/hole/config.json` once `plan` looks right.

## plan … stopped part-way (exit 8)

A request failed after others were applied. The message names the operation and the device's
answer. Nothing is lost: the journal makes the next sync send the affected feeds in full and verify
them. "Unable to unlock external resource push file" means FortiOS was still applying an earlier push
to that feed; forti-hole waits between requests to the same feed, so it should not recur.

## NOT VERIFIED (exit 8)

The device does not hold what was pushed: a different count, invalid entries, overflow (the entry
limit), or sampled entries that were not compiled. The feed is marked unconfirmed and the next sync
sends it in full.

## the device did not report its entry limit

No domain feed exists yet (the first run) or the firmware is older than 7.4.4. The budget uses
`fallback_limit` (1,000,000) until the device reports its own.

## Domains are not blocked

forti-hole's feeds only take effect where a DNS filter profile blocking them is on the traffic's path:
check `forti-hole status` ("Attached profiles") and [Where filtering applies](/guides/topologies). The
client may also use DNS over HTTPS. `diagnose sys external-resource stats` on the FortiGate CLI
shows what FortiOS loaded.
