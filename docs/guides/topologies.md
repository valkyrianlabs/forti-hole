---
title: Where filtering applies
description: Which network layouts a FortiGate DNS filter can see, where to attach forti-hole's profiles, the FortiGate DNS service, and the limits every DNS filter shares.
order: 40
status: published
tags:
  - guides
---

# Where filtering applies

A FortiGate DNS filter inspects DNS queries that **cross a firewall policy** carrying a DNS filter
profile. forti-hole maintains the feeds and the profiles; whether queries meet them depends on where
your clients send DNS.

:::toc[On this page]{depth="2" theme="compact"}
:::

## Network layouts

| Clients resolve through | Attach the profile to | Per-group profiles |
|---|---|---|
| Public or ISP resolvers (DHCP hands out 1.1.1.1, 9.9.9.9, the ISP's) | The LAN → WAN policies | Yes: one policy per group of sources (address groups, users, interfaces) |
| A resolver on another FortiGate interface (a server VLAN or DMZ) | The client → resolver policy, which keeps each client's address | Yes |
| A resolver on the same segment as the clients | Its outbound (resolver → WAN) policy only: every client gets the same profile | No |
| The FortiGate's own DNS service | The DNS service on that interface (below) | Per interface |

```bash
forti-hole attach standard --policy 2      # fh-standard on policy 2
forti-hole attach strict --policy 7
forti-hole detach --policy 7               # restore what policy 7 had before
```

`attach` sets the policy's `dnsfilter-profile` and, if the policy's security profiles were off,
`utm-status enable`. It records what was there, and `detach` restores it. Flow and proxy inspection
both work. Nothing else on the policy changes, and a sync never touches a policy.

## The FortiGate's DNS service

forti-hole's profiles are ordinary DNS filter profiles, so they can also be set on the FortiGate's DNS
service for an interface (`config system dns-server`, `set dnsfilter-profile fh-standard`). forti-hole
does not manage that setting in this release: set it in the GUI or CLI, and `forti-hole remove`
refuses while a profile is still in use there. Results reported for this setup vary between
deployments; test it on yours with a name from one of your lists.

## What every DNS filter misses

:::callout[DNS filtering is advisory against a determined client]{variant="warning"}
A DNS filter sees DNS. It does not see:

- **DNS over HTTPS and DNS over TLS** (browsers, Android's Private DNS, apps with their own resolver).
  Block TCP/853, and add a DoH blocklist (hagezi's `doh` list) as a source so the bootstrap names
  fail. Fortinet's deep inspection can redirect DoH into the DNS filter.
- **Hard-coded resolver addresses** that bypass your DHCP settings. A firewall policy that denies
  port 53 to anything but your resolvers (or a VIP that redirects it) closes that.
- **Names resolved somewhere else**: a VPN, a cached answer, a hosts file.

A Pi-hole has the same limits; it only sees what is sent to it.
:::

## Block action

forti-hole's profiles use `block`: the client gets NXDOMAIN, the fastest failure for ads and
trackers. `redirect` (the FortiOS default) answers with the block portal's address instead, which
produces certificate warnings in browsers for every HTTPS name. Set `dnsfilter.block_action` to
change it.
