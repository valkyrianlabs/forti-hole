# forti-hole release notes

<!-- vl-release:entry version=1.0.0 -->
## 1.0.0 — forti-hole rebuilt: deduplicated, budgeted, verified threat feeds

_Released 2026-10-18_

forti-hole 1.0 is a ground-up rewrite. It still turns DNS blocklists into FortiGate threat feeds,
with no resolver to run, but now it proves what it does: every name is published once, the set is
checked against what the FortiGate can hold before anything is written, only changes are pushed, and
the device is read back after every push.

### What changed for you

- **Deduplication you can see.** Every list is parsed, canonicalized and merged once across lists
  and tiers; names under a blocked parent domain are dropped. `forti-hole compile` reports valid,
  invalid, duplicate (within and across lists) and published counts per list, and the feeds with
  their SHA-256.
- **Subdomains are blocked again.** An adblock rule `||example.com^` becomes `example.com` and
  `*.example.com`, because a FortiOS feed entry matches its name only. forti-hole 0.x wrote the name
  alone, and StevenBlack's hosts list was not read at all.
- **It fits the device.** The compiled set is checked against the FortiGate's device-wide entry limit
  (1,000,000 on a 60F) and its memory (15% kept free, a learned cost per line) before writing. A set
  that does not fit is refused, never silently truncated by FortiOS.
- **hagezi TIF, sized for you.** `"variant": "auto"` uses TIF medium where it fits and mini where it
  does not. On a FortiGate-60F the default lists compile to 807,914 lines with mini; medium would need
  2.2 million.
- **Deltas, verified.** Unchanged lists cost a `304` and no writes. Changes go out as add/remove
  commands, at most one request per feed at a time, and every pushed feed is read back.
- **Fail-static.** A failed, truncated or HTML download falls back to the last good copy. An
  interrupted sync is repaired by the next one.
- **Your objects are safe.** forti-hole writes only feeds and DNS filter profiles with its prefix and
  ownership marker; the 0.x option that deleted every external resource on the device is gone. Firewall
  policies change only with `forti-hole attach`.
- **The forti ecosystem.** Connection and credentials come from a forti profile (`forti configure`);
  every write is an engine plan and apply with the profile's mode, audit and drift checks.
- **New commands**: `setup`, `doctor`, `compile`, `plan`, `sync`, `status --verify`, `pause`,
  `resume`, `attach`, `detach`, `remove`, `import pihole` (a one-time bootstrap from a Pi-hole), and
  `migrate config` / `migrate feeds` for forti-hole 0.x.
- **Packages**: APT (amd64) and Homebrew, with a systemd user timer (twice a day, never enabled by the
  package) and a launchd template.

### Upgrading from 0.x

Run `forti configure`, then `forti-hole migrate config config.yaml --write -` and
`forti-hole migrate feeds --apply`. The old `config.yaml` is never read for credentials and is not
changed.
