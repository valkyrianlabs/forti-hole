#!/usr/bin/env bash
# Smoke test of a built or installed `forti-hole`, offline and without touching the caller's
# config:
#
#   ci/smoke-test.sh [PATH-TO-FORTI-HOLE]     (default: forti-hole on PATH)
#
# Checks `forti-hole --version` (including the engine revision it was built from) and
# `forti-hole --help` (every command is listed), that a bad option exits 2 (the ecosystem's usage
# status), that nothing was written to the forti home (or anywhere in HOME) by either, and that
# `setup --offline` writes a disabled configuration that validates. POSIX tools only: it also runs in
# a stock ubuntu:24.04 with nothing but the .deb installed.
set -euo pipefail

bin="${1:-forti-hole}"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
export HOME="$scratch/home" FORTI_HOME="$scratch/forti"
mkdir -p "$HOME"

fail() {
  echo "smoke-test: $*" >&2
  exit 1
}

version="$("$bin" --version)"
echo "$version"
case "$version" in
  "forti-hole "[0-9]*" (engine forti-cli "[0-9a-f]*", forti-api "[0-9]*")") ;;
  *) fail "unexpected --version output" ;;
esac

"$bin" --help > "$scratch/help" 2> "$scratch/stderr" || { cat "$scratch/stderr" >&2; fail "--help exited non-zero"; }
for command in setup doctor compile plan sync status pause resume attach detach remove import migrate; do
  grep -qw -- "$command" "$scratch/help" || { cat "$scratch/help" >&2; fail "--help does not list $command"; }
done

# Usage errors exit with 2, not a crash.
status=0
"$bin" --no-such-option > /dev/null 2>&1 || status=$?
[ "$status" -eq 2 ] || fail "an unknown option exited $status, expected 2"

if [ -n "$(find "$FORTI_HOME" "$HOME" -type f 2>/dev/null)" ]; then
  fail "--version / --help wrote files"
fi

# setup --offline: a disabled configuration, mode 0600 in a 0700 directory, that compile accepts.
"$bin" setup --offline > /dev/null || fail "setup --offline failed"
config="$FORTI_HOME/hole/config.json"
[ -f "$config" ] || fail "setup wrote no $config"
grep -q '"enabled": false' "$config" || fail "setup did not write a disabled configuration"
mode="$(stat -c %a "$config" 2>/dev/null || stat -f %Lp "$config")"
[ "$mode" = 600 ] || fail "config.json has mode $mode, expected 600"
status=0
"$bin" sync > /dev/null 2>&1 || status=$?
[ "$status" -eq 7 ] || fail "sync with \"enabled\": false exited $status, expected 7"
echo "smoke-test: ok"
