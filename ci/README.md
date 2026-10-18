# CI container

forti-hole's Linux CI and release jobs follow forti-mcp's, forti-cli's and forti-api's conventions:
every step runs inside a disposable, rootless Podman container on the self-hosted VPS runner
(GitHub-hosted `ubuntu-latest` for pull requests from forks). The runner needs no host sudo, host
packages or Docker. The shared conventions are described in [payload-markdown's `ci/README.md`](https://github.com/valkyrianlabs/payload-markdown/blob/main/ci/README.md).

- `ci/run-ci` is copied **verbatim** from forti-mcp / forti-cli / forti-api / payload-markdown (it is
  repository-agnostic); update it there and copy it back.
- `ci/build-jobs` is copied from forti-mcp: the largest translation units are the engine's, which are
  the same here.
- `ci/Containerfile`: Ubuntu 24.04 (pinned by digest, GCC 13) with meson, ninja, g++, libcurl,
  GoogleTest, shellcheck, dpkg-dev, debhelper, gh, vl-release and pmdocs; the same image as forti-mcp's.
  libforti-api-dev is installed at run time by the package jobs only (development builds use the
  pinned wraps).
- `ci/smoke-test.sh`: an offline check of a built or installed `forti-hole` (`--version` with the
  engine revision, `--help` listing every subcommand, a bad option exiting 2, nothing written to the
  forti home). It needs only POSIX tools, so it also runs in a stock `ubuntu:24.04` with just the `.deb`
  installed. It never contacts a FortiGate.
- Workflows set `defaults.run.shell: bash ./ci/run-ci bash -euo pipefail {0}` on their Linux jobs.
  Clean-install checks of the `.deb` use a stock image instead:
  `./ci/run-ci --image "$UBUNTU_IMAGE" --root bash -c 'apt-get install -y ./release/…'`.
- ccache: `ci/run-ci` keeps `$cache/ccache` on the runner and mounts it at `/cache/ccache`
  (`CCACHE_DIR`, `CCACHE_MAXSIZE=8G`); Meson picks ccache up from the image, so a PR that changes
  neither the forti-cli nor the forti-api pin gets the engine from the cache. macOS uses `actions/cache`
  on `~/Library/Caches/ccache`, keyed by the two wraps.
- Memory and parallelism: `ci/run-ci` sizes the container (`CI_MEMORY` overrides it), and
  `ci/build-jobs` runs every core that memory allows, budgeting 1.2 GB per job (the largest TU with
  GCC 13 at -O3).
  Each CI job builds exactly once: pull requests build -O0 (Linux with ASan/UBSan) and run every suite
  against it; release builds are -O3 and are tested as built (package.yml, release.yml).

Build locally the way CI does (needs Podman):

```sh
./ci/run-ci bash -c 'meson setup build/ci -Dtests=enabled -Dwerror=true && meson compile -C build/ci -j 4 && meson test -C build/ci'
```
