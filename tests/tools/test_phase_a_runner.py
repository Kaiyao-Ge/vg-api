#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
build_dir = Path(sys.argv[2]).resolve()
completed = subprocess.run(
    [sys.executable, str(root / "tools" / "vg-exp" / "vg_exp.py"), "phase-a", "--build-dir", str(build_dir)],
    cwd=root, text=True, capture_output=True, check=True,
)
run_dir = root / completed.stdout.strip()
summary = json.loads((run_dir / "summary.json").read_text())
assert summary["schema"] == "vg.summary/v1"
assert summary["passed"] == 5 and summary["failed"] == 0
assert summary["native_adapter"] == "deferred"
samples = [json.loads(line) for line in (run_dir / "samples.jsonl").read_text().splitlines()]
assert {sample["experiment"] for sample in samples} == {"E001", "E003", "E006", "E015", "E018"}
assert all(sample["status"] == "passed" for sample in samples)
