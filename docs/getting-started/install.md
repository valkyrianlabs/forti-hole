---
title: Install
description: Install forti-hole with Homebrew or APT, or build it from source, and prepare the forti profile it uses to reach the FortiGate.
order: 10
status: published
tags:
  - getting-started
---

# Install

forti-hole is one executable, `forti-hole`. libcurl is its only runtime dependency; the forti command
engine is compiled in. Install it with Homebrew or APT, or build it from source.

:::toc[On this page]{depth="2" theme="compact"}
:::

## Prerequisites

| You need | Why |
|---|---|
| A FortiGate on **FortiOS 7.2.1 or later** (7.4.4 or later recommended) | Push-mode feeds arrived in 7.2.1; from 7.4.4 the device reports its domain-entry limit, which forti-hole budgets against |
| A **forti profile** for it | forti-hole reaches the FortiGate through the forti engine with the profile, API key and certificate that `forti configure` (from [forti-cli](https://github.com/valkyrianlabs/forti-cli)) stores in the forti home (`~/.forti`) |
| An API administrator allowed to write `system external-resource` and `dnsfilter profile` | The feeds and DNS filter profiles forti-hole manages. `attach` also needs write access to firewall policies |

A FortiGuard web-filtering subscription is **not** required: forti-hole's feeds are remote categories
on the device itself. Its DNS filter profiles set `error-allow`, so a lapsed subscription never stops
name resolution.

## Install the package

:::tabs{
  default="homebrew"
  theme="glass"
  tabTheme="muted"
}

:::tab[Homebrew (macOS, Linux)]{value="homebrew"}
```bash
brew install valkyrianlabs/tap/forti-cli valkyrianlabs/tap/forti-hole
```

Recent Homebrew versions ask you to trust a third-party tap first (`brew trust valkyrianlabs/tap`).
`forti-cli` provides `forti configure`; forti-hole does not need it at run time.

The example configuration and the launchd template are in `$(brew --prefix)/share/forti-hole/`.
`brew services start forti-hole` runs a sync every 12 hours ([Scheduling](/guides/scheduling)).
:::

:::tab[Debian and Ubuntu (APT)]{value="apt"}
`forti-hole` is published to the ValkyrianLabs APT repository (amd64):

```bash
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSL https://apt.valkyrianlabs.com/pubkey.gpg \
  -o /etc/apt/keyrings/valkyrianlabs.gpg
sudo chmod 0644 /etc/apt/keyrings/valkyrianlabs.gpg

echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/valkyrianlabs.gpg] https://apt.valkyrianlabs.com stable main" | \
  sudo tee /etc/apt/sources.list.d/valkyrianlabs.list > /dev/null

sudo apt update
sudo apt install -y forti-hole
```

The package recommends `forti-cli` and `valkyrianlabs-archive-keyring`, so APT installs both by
default. Signing key fingerprint: `494D 71A1 EEDD F87B D48E  5048 6D64 CB7B DFC3 0EC6`.

| Path | What |
|---|---|
| `/usr/bin/forti-hole` | The program |
| `/usr/lib/systemd/user/forti-hole.service` | One `sync --quiet` |
| `/usr/lib/systemd/user/forti-hole.timer` | Starts it twice a day, once you enable it |
| `/usr/share/forti-hole/config.example.json` | What `forti-hole setup` writes |
| `/usr/share/forti-hole/skill/forti-hole/SKILL.md` | The agent skill |

The package never enables the timer.
:::

:::tab[From source]{value="source"}
```bash
# Ubuntu: sudo apt install meson ninja-build g++ pkgconf libcurl4-openssl-dev libforti-api-dev [libgtest-dev]
# macOS:  brew install meson ninja valkyrianlabs/tap/forti-api [googletest]
git clone https://github.com/valkyrianlabs/forti-hole.git
cd forti-hole
meson setup build
meson compile -C build
meson install -C build --skip-subprojects
```

The forti engine is compiled from forti-cli's source at the revision `subprojects/forti-cli.wrap`
pins; Meson fetches it at configure time.
:::
:::

## Create the forti profile

```bash
forti configure
```

`forti configure` asks for the FortiGate's address, the API key and, if your API user has a
trusted-host certificate, the client certificate (`forti cert create` makes one). It stores the key in
the macOS Keychain or a 0600 file, bound to that device, and sets the profile's **mode**:

| Mode | forti-hole's writes |
|---|---|
| `read-only` (the default) | Refused: `plan` and `compile` work, `sync` exits 7 |
| `guarded` | Wait for your approval of each plan: `forti approve <id>`, then `forti-hole sync` again |
| `autonomous` | Applied directly; every safeguard still applies |

An unattended timer needs `autonomous`. Check the profile with `forti discover capabilities`.

## Next

[Quick start](/getting-started/quick-start): from an empty FortiGate to verified feeds.
