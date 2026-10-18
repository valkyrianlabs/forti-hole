# forti-hole

`forti-hole`: DNS blocklists as FortiGate threat feeds. It downloads lists (adblock, hosts, plain,
wildcard formats), canonicalizes and deduplicates every name across sources and **tiers**, collapses
names covered by a blocked parent, encodes them for FortiOS (`name` + `*.name` for a subtree;
`*name` only in opt-in compact encoding), groups lines by the set of **profiles** that block them,
splits groups into feeds by a stable hash, and keeps them on the FortiGate as push-mode external
resources (`<prefix><group>-<n>`, one remote category per group) that its managed DNS filter profiles
(`<prefix><profile>`) block. Before writing it checks the compiled line count against the device's
domain-entry limit and memory (`core/budget`); it pushes deltas in rounds and verifies the device
after every push. C++23, Meson. It links **forti-engine** (built from forti-cli's source as a
subproject, like forti-pihole and forti-mcp) and through it forti-api (>= 1.3.0, for
`system::feed_status` / `feed_entries`).

It is **never in the traffic path**: a CLI (`setup`, `doctor`, `compile`, `plan`, `sync`, `status`,
`pause`, `resume`, `attach`, `detach`, `remove`, `import pihole`, `migrate`) that a systemd user timer
runs twice a day. The ecosystem architecture and contracts are in `../.claude/context/architecture.md`
in the parent workspace; decisions in `decisions.md` there.

```sh
ln -sfn ../../forti-cli subprojects/forti-cli   # engine co-development (else the wrap clones the pinned commit)
ln -sfn ../../forti-api subprojects/forti-api
meson setup build/dev -Dtests=enabled --buildtype=debug
meson compile -C build/dev
meson test -C build/dev --print-errorlogs        # compiler, unit (suite unit); e2e (suite integration)
ci/smoke-test.sh build/dev/src/forti-hole
```

When running the binary by hand, set `FORTI_HOME` to a temporary directory: never touch the real
`~/.forti`, its secret store or the user's units.

## Layout

| Path | What |
|---|---|
| `src/core/` | The list compiler, its own library (`fh_compiler`, no engine, no JSON): `domain` (canonical names, punycode), `parse`, `compile` (dedupe, collapse, exceptions, encoding, groups, partitions, SHA-256), `budget` (entries and memory), `sha256` |
| `src/fetch/` | libcurl: `fetch` (parallel, conditional, size-capped list downloads; https and file only) and `https` (one JSON request, for `import pihole`) |
| `src/` | `config`, `files` (the forti-hole home, atomic 0600 writes, lock, history), `state` (confirmed feeds, journal), `sources` (validation, last good copy, shrink guard), `pipeline` (variant choice on the compiled set), `gateway` (engine + feed status reads), `guard` (the write allowlist), `device` (inventory), `sync` (plan and phased execution), `manage` (attach, detach, remove, legacy feeds), `pihole_import`, `legacy` (0.x YAML), `report`, `cli` |
| `tests/core/` | The compiler on its own |
| `tests/unit/`, `tests/support/` | Everything else against an in-memory FortiGate (`fake_gateway.hpp`: FortiOS's push behaviour as measured on a 60F) and a scripted list server |
| `tests/integration/` | The real binary through the real engine against forti-cli's mock FortiOS, extended there with feeds and DNS filter tables |
| `packaging/` | systemd user units (installed, never enabled), the launchd template, `config.example.json` (must equal `example_config()`, a unit test checks it), the Homebrew formula, `forti-cli-pin.sh` |
| `release.toml`, `debian/`, `.release/` | vl-release contract, Debian packaging, staged release docs (`.claude/context/release.md`) |
| `ci/`, `.github/` | CI container, smoke test, CI, package and release workflows |
| `docs/` | The docs package (pmdocs, docs set `forti-hole`); Payload Markdown, `.claude/skills/payload-markdown*` |
| `skill/forti-hole/` | The agent skill for operating forti-hole on a user's behalf |

Files: `$FORTI_HOME/hole/config.json` (operator); `state.json`, `feeds/<feed>.txt`, `cache/<list>.list`,
`journal.json`, `history.jsonl`, `lock` (the program). Profiles and credentials are the engine's
(`forti configure`).

## Packaging

The engine pin is `subprojects/forti-cli.wrap` (a full forti-cli commit id, today v0.6.0). Changing it:
edit the `revision`, run `packaging/forti-cli-pin.sh update`, commit both. Keep `.release/*_NEXT.md`
current and run `vlr check` (`.claude/skills/vl-release/`). Release details: `.claude/context/release.md`.

## Rules

1. **The engine owns semantics; forti-api owns FortiOS.** Every FortiGate write is an engine plan and
   apply (`apply_operations`). Reads go through `Engine::read`, except feed status and entry pages,
   which use forti-api's `system::feed_status` / `feed_entries` on the engine's own client
   (`Gateway::feed_status`). Never build URLs or call the SDK for a write here. If the engine or SDK
   needs a change, make it upstream and pin the release.
2. **Write allowlist** (`guard.hpp`), enforced twice (`check_operations` before planning and
   `GuardTransport` under the engine): `system/external-resource` and `dnsfilter/profile` objects named
   `<prefix>*` carrying the marker `managed by forti-hole`; push commands naming only `<prefix>*` feeds;
   `dnsfilter/domain-filter` tables forti-hole created. Beyond that only through `WritePermits` for one
   apply: `dnsfilter-profile`/`utm-status` of a policy named in `attach`/`detach`, the `ftgd-dns` of an
   operator profile freed by `migrate feeds`, and deleting 0.x feeds in `migrate feeds --apply`. Never
   widen it without a recorded decision.
3. **Operator objects are never modified.** A prefixed object without the marker stops the run.
   Operator feeds count against the budget and their categories are never reused.
4. **Fail-static.** A failed, implausible or shrunken download uses the last good copy; without one
   nothing is written. A set over budget is refused (exit 9). Nothing is written until `enabled`, and
   nothing while paused. Profiles drop a category only after every replacement feed verified; obsolete
   feeds are deleted last. A feed is confirmed only after the device shows it; the journal makes the
   next run re-send whatever an interrupted run touched.
5. **FortiOS behaviour the code depends on** (measured on a 60F, 7.4.8): pushes to a feed that is
   still "Connect pending" are dropped; a second push to a feed before the first is applied fails
   ("Unable to unlock external resource push file"); a DNS filter profile may only list a remote
   category some feed provides; the entry limit is device-wide and reported as `accepted_limit`.
   Keep `tests/support/fake_gateway.hpp` faithful to these.
6. **No live firewall in tests or CI.** Live checks follow forti-api's rules: `"prefix":
   "forti-api-test-fh-"`, feeds and DNS filter profiles only, nothing attached to a production policy,
   removed afterwards, a separate `FORTI_HOME`, never in CI.
7. **Secrets never leak**: not in argv, output, state, history, the journal or logs. List contents and
   device data are untrusted input.
8. Match the style: 4-space indent, `snake_case`, comments that explain *why*, no framework where a
   function will do. Keep `-Wall -Wextra -Wpedantic` (warning_level=3) clean on Apple clang and
   GCC 13+ (CI builds with `-Dwerror=true`, Linux under ASan/UBSan).
