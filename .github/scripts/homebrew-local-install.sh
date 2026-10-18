#!/usr/bin/env bash
# Install the release formula from a throwaway local tap, pointing it at the locally built source
# archive (file://), then run `brew test` and the smoke test. Proves the formula builds from source and
# works before it ever reaches valkyrianlabs/homebrew-tap. Expects `vlr` on PATH; run it in a
# disposable checkout: `vlr prepare` rewrites the release documents (bumping the version first when
# unreleased).
#
# The build dependency forti-api comes from the published tap (valkyrianlabs/tap), exactly as it
# will for users; meson, ninja and pkgconf from homebrew/core; the engine source from the formula's
# forti-cli resource (GitHub's archive of the pinned forti-cli commit).
set -euo pipefail

bash packaging/forti-cli-pin.sh check

phase="$(vlr status --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["phase"])')"
if [ "$phase" = unbumped ]; then vlr version bump minor >/dev/null; fi
vlr prepare >/dev/null
vlr source-archive
formula="$(vlr homebrew formula --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["path"])')"
version="$(vlr version show)"
archive="$PWD/release/forti-hole-${version}.tar.gz"

export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
brew tap valkyrianlabs/tap
# Recent Homebrew refuses formulae from third-party taps until they are trusted; forti-hole depends on
# valkyrianlabs/tap/forti-api, and the throwaway tap below holds the formula under test.
trust() { if brew commands --quiet | grep -qx trust; then brew trust "$1"; fi; }
trust valkyrianlabs/tap

tap="forti-hole-ci/local"
brew untap "$tap" >/dev/null 2>&1 || true
brew tap-new --no-git "$tap"
trust "$tap"
tap_formula="$(brew --repository "$tap")/Formula/forti-hole.rb"
trap 'brew uninstall --formula "$tap/forti-hole" >/dev/null 2>&1 || true; brew untap "$tap" >/dev/null 2>&1 || true' EXIT

# Style-check the exact formula that will be published, as a tap formula.
cp "$formula" "$tap_formula"
brew style "$tap/forti-hole"

# Then install it from the locally built archive as a bottle build and run its test block.
sed -E "s#^  url \".*\"#  url \"file://${archive}\"#" "$formula" > "$tap_formula"
brew install --build-bottle "$tap/forti-hole"
brew test "$tap/forti-hole"
installed="$(brew --prefix)/bin/forti-hole"
# The installed binary reports the engine revision the wrap pins.
pinned="$(sed -n 's/^revision *= *//p' subprojects/forti-cli.wrap | cut -c1-7)"
"$installed" --version | grep -F "(engine forti-cli $pinned, forti-api "
ci/smoke-test.sh "$installed"

# The bottle: a release asset of this version, which the published formula names (root_url), so
# users pour it instead of compiling. brew writes name--version...; the asset is the JSON's filename.
mkdir -p bottle
(
  cd bottle
  brew bottle --json --root-url "https://github.com/valkyrianlabs/forti-hole/releases/download/v${version}" "$tap/forti-hole"
  python3 - <<'PY'
import json, pathlib
for path in pathlib.Path(".").glob("*.bottle.json"):
    for entry in json.loads(path.read_text()).values():
        for info in entry["bottle"]["tags"].values():
            if info["local_filename"] != info["filename"]:
                pathlib.Path(info["local_filename"]).rename(info["filename"])
            print("bottle:", info["filename"], info["sha256"])
PY
)
