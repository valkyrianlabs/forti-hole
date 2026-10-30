<!--
vl-release staged changelog: package-facing changes for the NEXT release.
Maintained continuously while working; `vlr prepare` renders it into debian/changelog
and resets this file to this template.

Format: one "- " bullet per change (concise, technical). Indent continuation lines.
Optional "## Section" headings group bullets; if used, every bullet must be under one.
One level of nested "  - " detail bullets is allowed. Consolidate; don't paste commit logs.
-->
- Homebrew formula: forti-api is a resource (the v1.3.0 release archive, installed into the build
  tree for pkg-config) instead of a build dependency on valkyrianlabs/tap/forti-api, so the formula
  depends on homebrew/core only and installs from an untrusted tap by its full name.
- CI: the Homebrew check installs the formula with an empty trust store and no `brew tap` or
  `brew trust`.
