# Releasing forti-hole

forti-hole releases with vl-release (`vlr`, ValkyrianLabs convention, same as forti-mcp, forti-cli
and forti-api). The repository contract is `release.toml`; release CI is
`.github/workflows/release.yml` and only calls `vlr` commands (plus `packaging/forti-cli-pin.sh`,
below). The generic workflow is in `.claude/skills/vl-release/SKILL.md`, this repository's specifics in
`PROJECT.md` next to it (`vlr install-local-skill` regenerates it).

## What ships

| Channel | Artifact | Built by |
|---|---|---|
| APT (`apt.valkyrianlabs.com`, suite `stable`, `main`) | `forti-hole_<v>-1_amd64.deb`: `/usr/bin/forti-hole`, the systemd user units in `/usr/lib/systemd/user/` (installed, never enabled), `/usr/share/forti-hole/config.example.json`, the launchd template, the agent skill, copyright | `vlr build-deb` (`debian/`, against `libforti-api-dev >= 1.3.0`) |
| GitHub release | the .deb, `forti-hole-<v>.tar.gz` (prepared tracked tree), the Homebrew bottles, `SHA256SUMS` | `vlr source-archive`, `vlr checksums`, `vlr github-release` |
| Homebrew (`valkyrianlabs/tap`) | `Formula/forti-hole.rb`, builds from the source archive (no systemd units on macOS) | `vlr homebrew formula` / `publish` (`packaging/homebrew/forti-hole.rb`) |

The formula depends on homebrew/core formulae only. Homebrew trusts the formulae a user names in
full (`brew install valkyrianlabs/tap/forti-hole`) and refuses to load any other formula from an
untrusted third-party tap, build dependencies included and even when it pours a bottle; so
forti-api is the formula's `resource "forti-api"` (its release archive, installed into the build
tree), never a `depends_on`. The `homebrew-check` job installs with an empty trust store and no
`brew trust`, so a dependency on the tap fails there first. Bump the resource to a newer forti-api
release when the build needs one.

forti-hole installs independently, like forti-mcp (architecture decision 3): neither channel
depends on the forti-cli package or the `forti` executable, and no engine library or headers ship. The
package *recommends* forti-cli (operators create profiles with `forti configure`) and
`valkyrianlabs-archive-keyring` (decision 27: recommend, never depend).

The package never enables the timer: `debian/rules` skips `dh_installsystemduser`, and the release
workflow's clean-install test checks that no `timers.target.wants` link appears.

## Engine source

forti-hole compiles forti-cli's engine (`engine/`, Meson dependency `forti-engine`) from source, as
the subproject `subprojects/forti-cli`, exactly as forti-mcp does. **`subprojects/forti-cli.wrap` is
the single pin**: a full commit id (`revision`; today forti-cli v0.6.0, with content-addressed action bodies). Every build path uses exactly that revision:

| Build | Where the engine source comes from | Verified by |
|---|---|---|
| Development, CI | the wrap: Meson clones the pinned commit (or a co-development symlink, see AGENTS.md) | Git (commit id) |
| Debian (`vlr build-deb`) | `release.toml` `debian.pre_build` runs `packaging/forti-cli-pin.sh vendor` in the work tree first: `meson subprojects download forti-cli`, then HEAD = the pin and no local changes. A symlink there is refused. build-deb then copies the tree (without `.git`), and `dpkg-buildpackage` configures with `--wrap-mode=nodownload`: the package build itself fetches nothing | the script (commit id) |
| Homebrew | the formula's `resource "forti-cli"`: GitHub's archive of the pinned commit, staged into `subprojects/forti-cli` before `meson setup` (Meson uses an existing subproject directory as is, so `--wrap-mode=nofallback` never fetches) | Homebrew (sha256) |
| From the release source archive by hand | the wrap, at configure time | Git (commit id) |

`packaging/forti-cli-pin.sh check --download` (CI `linux`, release `build`) proves the formula's
resource is the wrap's revision and that GitHub serves the declared sha256. **Bumping the engine:**
change `revision` in the wrap, run `packaging/forti-cli-pin.sh update` (rewrites the resource url and
sha256), commit both. Pin a forti-cli release tag's commit.

The built binary says what it was built from: `forti-hole --version` prints
`forti-hole 1.0.0 (engine forti-cli 13b7dfe, forti-api 1.3.0)`. The revision is the wrap's for
package builds (an unpacked archive has no Git metadata), or the actual HEAD (`-dirty` with local
changes) when `subprojects/forti-cli` is a Git checkout. CI's clean-install and formula checks compare
it with the wrap. Why the engine is not vendored into the source archive: forti-mcp's
`.claude/context/release.md`, "Why not a source archive that vendors the engine" (the same vl-release
0.5 limits apply).

## Cutting a release

1. Keep `.release/RELEASE_NOTES_NEXT.md` and `.release/CHANGELOG_NEXT.md` current with every
   user-visible change (`vlr help staging`). `vlr status` must say `pending`.
2. On a clean, up-to-date `main`: `vlr check`, then `vlr cut patch|minor|major --push` (or
   `vlr cut 1.0.0 --push` for the rewrite's first release: the version already in `VERSION`).
3. The tag push runs `release.yml`:
   `build` (gate, prepare, engine pin check, `.deb` against `libforti-api-dev`, source archive,
   formula, checksums, contract validation, clean-install smoke test in stock `ubuntu:24.04`, APT
   dry-run plan) → `homebrew-check` (macOS 15 arm64 and Intel: formula from a local tap, built from the
   archive, `brew test`, smoke test, bottle) → `publish-apt` (Production; upload, index verification,
   install from the repository) → `github-release` → `homebrew-tap` (Production) → `finalize`
   (commits the promoted `debian/changelog` / `RELEASE_NOTES.md` to `main`); `docs` (pmdocs push)
   runs after `publish-apt`, alongside.
4. A rehearsal without publishing: Actions → Release → Run workflow, `ref: vX.Y.Z`,
   `publish: dry-run` (runs `build` and `homebrew-check` only). `package.yml` (Actions → Package) does
   the same from any branch.

`debian/changelog` and `RELEASE_NOTES.md` do not exist until the first `vlr prepare`; never create
or edit them by hand (vl-release would treat a hand-written entry as an inconsistent release).

## Secrets, variables and settings

| Name | Kind | Used by |
|---|---|---|
| `NEXUS_USER`, `NEXUS_PASSWORD` | org secrets | `publish-apt` (`vlr publish-deb`) |
| `NEXUS_APT_REPO` | org variable | `publish-apt`: Nexus APT upload URL |
| `HOMEBREW_TAP_TOKEN` | secret (org or repo) | `homebrew-tap`: push to `valkyrianlabs/homebrew-tap` |
| `RELEASE_PUBLISH_MODE` | repo variable, optional | `nexus` (the workflow default) or `disabled` |
| `VL_DOCS_ENDPOINT` | org variable | `docs` (publish), CI on `main` (dry-run sync) |
| `GITHUB_TOKEN` | automatic | `github-release`, `finalize` (`contents: write`) |

Org secrets and variables must be granted to this repository (organization settings → secrets →
repository access).

## Before the first release (checklist)

- [x] The GitHub repository `valkyrianlabs/forti-hole` exists (the 0.x project, rewritten in place).
- [ ] Environment `Production` on it (`publish-apt`, `homebrew-tap`, `docs` use it; add reviewers if
      publication should need a click).
- [ ] The org secrets/variables above are available to this repository; `HOMEBREW_TAP_TOKEN` can push
      to `valkyrianlabs/homebrew-tap`.
- [ ] The self-hosted VPS runner (labels `self-hosted, Linux, X64, ubuntu-latest-lts`) serves this
      repository (runner group access), with rootless Podman as for forti-mcp.
- [ ] The docs set `forti-hole` exists on the docs site and trusts this repository's workflows over
      GitHub OIDC. Until then CI's dry-run sync on pushes to `main` fails.
- [ ] If `main` is protected, `github-actions[bot]` may push the `finalize` commit.
- [x] `libforti-api-dev >= 1.3.0` is in the APT index and `forti-api` 1.3.0 is in the tap (published 2026-10-18).
- [ ] The wrap pins the forti-cli commit that should ship, and the formula's resource matches
      (`packaging/forti-cli-pin.sh check --download`).
- [ ] Staged release docs describe the first release; `vlr status` says `pending`.
- [ ] A dry-run of `release.yml` (workflow_dispatch, `publish: dry-run`) is green.

## Build profile

The same as forti-mcp's and forti-cli's: `buildtype=release` (-O3, tested as built), no LTO; on ELF,
non-debug builds add `-ffunction-sections -fdata-sections -Wl,--gc-sections` as *global* arguments, so
the engine compiled in the subproject gets them too. Packaging: Debian swaps dpkg's `-O2` for `-O3`,
drops Ubuntu's default `-flto=auto -ffat-lto-objects` and `-g`, sets `b_ndebug=true` and ships no
`-dbgsym` package (`debian/rules`); Homebrew passes `-Doptimization=3 -Dstrip=true`. Parallelism comes
from `ci/build-jobs` (DEB_BUILD_OPTIONS=parallel).

The subproject never builds `forti` here (`subproject_cli=false` in `meson.build`: no test drives the
CLI). The Debian build runs every test suite (all network-free) against the -O3 binary unless
`nocheck`.

## Multi-architecture (arm64): vl-release gap

The same limits as forti-mcp: vl-release 0.5 accepts one architecture per package contract and one
`.deb` per package in a release directory, so the package is amd64 only. `debian/control` says
`Architecture: any`, and `libforti-api-dev` is `Architecture: all`, so a native arm64 runner is all the
build itself needs once vl-release accepts several architectures. Homebrew bottles cover Apple silicon
and Intel.
