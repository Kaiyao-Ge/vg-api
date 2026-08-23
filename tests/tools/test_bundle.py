#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path


root = Path(sys.argv[1]).resolve()
build_dir = Path(sys.argv[2]).resolve()
completed = subprocess.run(
    [sys.executable, str(root / "tools" / "vg-exp" / "vg_exp.py"), "probe", "--build-dir", str(build_dir)],
    cwd=root, text=True, capture_output=True, check=True,
)
run_dir = root / completed.stdout.strip()
manifest = json.loads((run_dir / "manifest.json").read_text())
required = {"environment.json", "build.json", "definition.resolved.json", "stdout.log", "stderr.log",
            "samples.jsonl", "summary.json", "summary.csv", "report.md"}
assert required <= set(manifest["files"])
assert "manifest.json" not in manifest["files"]
assert str(root) not in (run_dir / "environment.json").read_text()
for relative, expected_hash in manifest["files"].items():
    actual = "sha256:" + hashlib.sha256((run_dir / relative).read_bytes()).hexdigest()
    assert actual == expected_hash
