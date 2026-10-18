#!/usr/bin/env python3
"""Add a `bottle do` block to a rendered formula, from the JSON files `brew bottle --json` wrote.

    homebrew-bottle-block.py release/homebrew/forti-hole.rb bottle/*.bottle.json

The bottles are release assets of the same GitHub release (root_url). vl-release renders the formula
from packaging/homebrew/ and publishes it as it is; its own checks read only the top-level url and
sha256, which the block (indented one level deeper) leaves alone.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def block(paths: list[Path]) -> str:
    root_url = None
    rebuild = 0
    entries: list[tuple[str, str, str]] = []   # (cellar, tag, sha256)
    for path in paths:
        for entry in json.loads(path.read_text()).values():
            bottle = entry["bottle"]
            if root_url not in (None, bottle["root_url"]):
                raise SystemExit(f"{path}: root_url {bottle['root_url']} differs from {root_url}")
            root_url = bottle["root_url"]
            rebuild = max(rebuild, int(bottle.get("rebuild", 0)))
            for tag, info in sorted(bottle["tags"].items()):
                # brew writes the relocatable cellars as "any" / "any_skip_relocation" (symbols in the
                # formula); anything else is a Cellar path, a string.
                cellar = str(info.get("cellar", bottle.get("cellar", "any"))).lstrip(":")
                cellar = f":{cellar}" if cellar in ("any", "any_skip_relocation") else f'"{cellar}"'
                entries.append((cellar, tag, info["sha256"]))
    if not entries:
        raise SystemExit("no bottles in the JSON files")
    # brew style (FormulaAudit/BottleTagIndentation, BottleDigestIndentation) wants the tags and the
    # digests in aligned columns once there is more than one bottle, as `brew bottle --merge` writes;
    # FormulaAudit/BottleOrder wants the ARM bottles first.
    entries.sort(key=lambda e: (not e[1].startswith("arm64_"), e[1]))
    cellar_width = max(len(f"cellar: {c},") for c, _, _ in entries)
    tag_width = max(len(f"{t}:") for _, t, _ in entries)
    lines = [f'    sha256 {f"cellar: {c},":<{cellar_width}} {f"{t}:":<{tag_width}} "{d}"' for c, t, d in entries]
    head = [f'    root_url "{root_url}"'] + ([f"    rebuild {rebuild}"] if rebuild else [])
    return "  bottle do\n" + "\n".join(head + lines) + "\n  end\n"


def main() -> int:
    formula = Path(sys.argv[1])
    text = formula.read_text()
    if re.search(r"^  bottle do$", text, re.M):
        raise SystemExit(f"{formula} already has a bottle block")
    anchor = re.search(r'^  head ".*\n', text, re.M) or re.search(r'^  license ".*\n', text, re.M)
    if not anchor:
        raise SystemExit(f"{formula}: no head or license line to put the bottle block after")
    text = text[: anchor.end()] + "\n" + block([Path(p) for p in sys.argv[2:]]) + text[anchor.end():]
    formula.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
