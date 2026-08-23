#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
build_dir = Path(sys.argv[2]).resolve()
completed = subprocess.run(
    [sys.executable, str(root / "tools" / "vg-exp" / "vg_exp.py"), "benchmark", "--build-dir", str(build_dir)],
    cwd=root, text=True, capture_output=True, check=True,
)
run_dir = root / completed.stdout.strip()
summary = json.loads((run_dir / "summary.json").read_text())
assert summary["schema"] == "vg.summary/v1"
assert summary["experiment"] == "E010"
assert summary["ctest"] == "vertical-slice.metal.tier2-nodes"
assert summary["evidence_grade"] == "P0"
assert summary["status"] == "ok"
assert summary["gpu_ns"] is None
assert summary["break_even_curves"] == "unmeasured"
assert isinstance(summary["host_wall_clock_s"], float)
assert summary["host_wall_clock_s"] >= 0.0
samples = [json.loads(line) for line in (run_dir / "samples.jsonl").read_text().splitlines()]
assert len(samples) == 1
assert samples[0]["status"] == "passed"
assert samples[0]["parameters"]["evidence_grade"] == "P0"
assert "gpu_ns" not in samples[0]["metrics"]
resolved = json.loads((run_dir / "definition.resolved.json").read_text())
assert resolved["schema"] == "vg.benchmark/v1"
assert resolved["evidence_grade"] == "P0"
