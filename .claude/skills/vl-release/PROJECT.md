<!-- vl-release:generated-local -- regenerate with `vlr install-local-skill` after changing release.toml; local edits are overwritten -->
# forti-hole: release specifics

Generated from this repository's `release.toml`. The general workflow is in `SKILL.md`;
this file says how it applies here.

## Files

| Purpose | Path |
|---|---|
| Release contract | `release.toml` |
| Staged changelog (keep current) | `.release/CHANGELOG_NEXT.md` |
| Staged release notes (keep current) | `.release/RELEASE_NOTES_NEXT.md` |
| Published release notes (do not edit) | `RELEASE_NOTES.md` |
| Published Debian changelog (do not edit) | `debian/changelog` |

## Versions

- Canonical: `VERSION`
- Kept in sync by `vlr version …`: no other files
- Tags: `vX.Y.Z` on branch `main`; GitHub release title: `vX.Y.Z — <title>`

## Release channels

- **Debian packages** (built by `vlr build-deb` from `debian/`, revision 1):
  - `forti-hole`: architecture `amd64`; must ship `usr/bin/forti-hole`, `usr/share/doc/forti-hole/copyright`, `usr/lib/systemd/user/forti-hole.service`, `usr/lib/systemd/user/forti-hole.timer`, `usr/share/forti-hole/config.example.json`, `usr/share/forti-hole/com.valkyrianlabs.forti-hole.plist`, `usr/share/forti-hole/skill/forti-hole/SKILL.md`; must never ship `usr/include/*`, `usr/lib/*.a`, `usr/lib/*.so*`, `usr/lib/*pkgconfig/*`, `usr/share/pkgconfig/*`, `usr/bin/forti`, `usr/share/man/man1/forti.1*`, `usr/lib/systemd/system/*`, `lib/systemd/system/*`, `etc/systemd/*`, `*.env`, `*/.env`, `*.p12`, `*.pfx`, `*.pem`, `*.cer`, `*.crt`, `*.key`, `*.token`
  - pre-build: `bash packaging/forti-cli-pin.sh vendor`
- **APT**: https://apt.valkyrianlabs.com (suite `stable`, components main), published by release CI with `vlr publish-deb`
- **Source archive** of the prepared tree, attached to the GitHub release
- **Homebrew**: formula template `packaging/homebrew/forti-hole.rb`, published to `valkyrianlabs/homebrew-tap` as `Formula/forti-hole.rb`

## Before considering substantial work complete

```sh
vlr check
```
