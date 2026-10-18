#!/usr/bin/env bash
# The engine source for package builds: forti-cli at exactly the revision subprojects/forti-cli.wrap
# pins. The wrap is the single source of truth; this script keeps the package builds on it.
#
#   packaging/forti-cli-pin.sh vendor             clone forti-cli at the pinned revision into
#                                                 subprojects/forti-cli (through the wrap) and verify
#                                                 it; `vlr build-deb` runs this first (release.toml
#                                                 debian.pre_build)
#   packaging/forti-cli-pin.sh check [--download] the Homebrew formula's forti-cli resource names the
#                                                 pinned revision; --download also verifies its sha256
#   packaging/forti-cli-pin.sh update             point the formula's resource at the pinned revision
#                                                 (downloads the archive to compute its sha256)
#
# Run from anywhere; paths are relative to the repository root. MESON overrides the meson executable.
# After changing the wrap's revision: `update`, then commit both files.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
wrap="$root/subprojects/forti-cli.wrap"
formula="$root/packaging/homebrew/forti-hole.rb"
dir="$root/subprojects/forti-cli"

die() {
  echo "forti-cli-pin: $*" >&2
  exit 1
}

wrap_value() { # KEY
  sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$wrap" | tr -d '[:space:]'
}

revision="$(wrap_value revision)"
git_url="$(wrap_value url)"
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || die "$wrap: revision must be a full 40-character commit id, got '$revision'"
[[ "$git_url" =~ ^https://github\.com/[^/]+/[^/]+\.git$ ]] || die "$wrap: url must be a https://github.com/OWNER/REPO.git URL"
# GitHub's archive of exactly that commit (its sha256 is stable; Homebrew relies on that for every
# github.com archive url).
archive_url="${git_url%.git}/archive/$revision.tar.gz"

sha256_of_stdin() {
  if command -v sha256sum >/dev/null; then sha256sum | cut -d' ' -f1; else shasum -a 256 | cut -d' ' -f1; fi
}

# The url and sha256 inside the formula's `resource "forti-cli" do ... end` block.
resource_value() { # KEY
  sed -n '/^  resource "forti-cli" do$/,/^  end$/p' "$formula" | sed -n "s/^    $1 \"\(.*\)\"$/\1/p"
}

vendor() {
  if [ -L "$dir" ]; then
    die "subprojects/forti-cli is a symlink (a co-development checkout); package builds use only the pinned revision $revision. Remove it (or build in a clean clone)."
  fi
  if [ ! -e "$dir" ]; then
    (cd "$root" && "${MESON:-meson}" subprojects download forti-cli)
  fi
  [ -e "$dir/.git" ] || die "subprojects/forti-cli exists but is not the wrap's Git clone; remove it"
  local head
  head="$(git -C "$dir" rev-parse HEAD)"
  [ "$head" = "$revision" ] || die "subprojects/forti-cli is at $head, the wrap pins $revision; remove it and re-run"
  # Untracked files are ignored: Meson keeps its own bookkeeping file (.meson-subproject-wrap-hash.txt) there.
  [ -z "$(git -C "$dir" status --porcelain --untracked-files=no)" ] || die "subprojects/forti-cli has local changes"
  if [ ! -f "$dir/meson.build" ] || [ ! -d "$dir/engine" ]; then die "subprojects/forti-cli has no engine (meson.build, engine/)"; fi
  echo "forti-cli-pin: subprojects/forti-cli is forti-cli $revision"
}

check() {
  local url sha
  url="$(resource_value url)"
  sha="$(resource_value sha256)"
  if [ -z "$url" ] || [ -z "$sha" ]; then die "$formula has no resource \"forti-cli\" block with url and sha256"; fi
  [ "$url" = "$archive_url" ] || die "the formula's forti-cli resource is $url; the wrap pins $archive_url (run: packaging/forti-cli-pin.sh update)"
  if [ "${1:-}" = --download ]; then
    local actual
    actual="$(curl -fsSL --retry 3 "$archive_url" | sha256_of_stdin)"
    [ "$actual" = "$sha" ] || die "$archive_url has sha256 $actual; the formula declares $sha"
  fi
  echo "forti-cli-pin: the formula's forti-cli resource is the pinned revision $revision"
}

update() {
  local sha tmp
  sha="$(curl -fsSL --retry 3 "$archive_url" | sha256_of_stdin)"
  tmp="$(mktemp)"
  sed -e '/^  resource "forti-cli" do$/,/^  end$/{' \
    -e "s#^    url \".*\"\$#    url \"$archive_url\"#" \
    -e "s#^    sha256 \".*\"\$#    sha256 \"$sha\"#" \
    -e '}' "$formula" > "$tmp"
  cat "$tmp" > "$formula"
  rm -f "$tmp"
  check
}

case "${1:-}" in
  vendor) vendor ;;
  check) check "${2:-}" ;;
  update) update ;;
  *) sed -n '2,15p' "$0" >&2; exit 2 ;;
esac
