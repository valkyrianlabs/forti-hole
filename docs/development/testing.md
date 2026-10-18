---
title: Building and testing
description: Build forti-hole, run its test suites, and the rules for testing against a real FortiGate.
order: 110
status: published
tags:
  - development
---

# Building and testing

:::toc[On this page]{depth="2" theme="compact"}
:::

## Build

```bash
ln -sfn ../../forti-cli subprojects/forti-cli    # optional: engine co-development (else the wrap clones the pin)
ln -sfn ../../forti-api subprojects/forti-api    # optional: SDK co-development
meson setup build/dev -Dtests=enabled --buildtype=debug
meson compile -C build/dev
meson test -C build/dev --print-errorlogs
ci/smoke-test.sh build/dev/src/forti-hole
```

C++23, Meson, GCC 13+ or Apple clang. Warnings are errors in CI (`-Dwerror=true`), and Linux CI runs
everything under ASan and UBSan.

## Suites

| Test | Suite | What |
|---|---|---|
| `compiler` | unit | Names and punycode, every parser, deduplication, subtree collapse, exceptions, encodings, partitioning, determinism, budgets |
| `unit` | unit | Configuration, list loading and the cache, the write guard, Pi-hole import, 0.x migration, and the CLI against an in-memory FortiGate with the push behaviour measured on a FortiGate-60F (load delay, dropped early pushes, the push lock, the device-wide entry limit, category validation) |
| `e2e` | integration | The real binary through the real forti engine (libcurl, verified TLS, plan/apply, blob-stored bodies, audit) against forti-cli's mock FortiOS, with lists from `file://` URLs |

No test talks to a FortiGate or a list server.

## Testing against a real FortiGate

Live checks follow forti-api's rules: every object is named `forti-api-test-*` (for forti-hole: a
configuration with `"prefix": "forti-api-test-fh-"`), only feeds and DNS filter profiles are created,
nothing is attached to a production policy, everything is removed afterwards (`forti-hole remove
--yes`), and none of it runs in CI. Use a separate forti home (`FORTI_HOME`) so the plans and audit
log of a test stay apart from your own.
