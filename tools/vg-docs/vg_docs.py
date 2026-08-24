#!/usr/bin/env python3
"""Check Markdown links and JSON syntax without adding a runtime dependency."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


LINK = re.compile(r"(?<!\!)\[[^]]+\]\(([^)]+)\)")


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else Path.cwd()
    failures: list[str] = []
    warnings: list[str] = []
    for path in sorted(root.rglob("*.json")):
        if "build" in path.parts:
            continue
        try:
            json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            failures.append(f"invalid JSON: {path.relative_to(root)}: {error}")
    for path in sorted([*root.rglob("*.md"), *root.rglob("*.MD")]):
        if "build" in path.parts:
            continue
        contents = path.read_text(encoding="utf-8")
        if contents.count("```") % 2:
            failures.append(f"unclosed code fence: {path.relative_to(root)}")
        for raw_target in LINK.findall(contents):
            target = raw_target.split("#", 1)[0].strip()
            if not target or "://" in target or target.startswith("mailto:"):
                continue
            resolved = (path.parent / target).resolve()
            try:
                resolved.relative_to(root)
            except ValueError:
                warnings.append(f"external source not checked: {path.relative_to(root)} -> {target}")
                continue
            if not resolved.exists():
                failures.append(f"broken link: {path.relative_to(root)} -> {target}")
    for warning in warnings: print(f"warning: {warning}")
    for failure in failures: print(f"error: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
