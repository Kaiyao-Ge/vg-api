#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
build_dir = Path(sys.argv[2]).resolve()
completed = subprocess.run(
    [sys.executable, str(root / "tools" / "vg-exp" / "vg_exp.py"), "phase-c", "--build-dir", str(build_dir)],
    cwd=root, text=True, capture_output=True, check=True,
)
run_dir = root / completed.stdout.strip()
summary = json.loads((run_dir / "summary.json").read_text())
assert summary["schema"] == "vg.summary/v1"
assert summary["experiment_count"] == 4
assert summary["passed"] == summary["ctest_count"] and summary["failed"] == 0
assert summary["vulkan_status"] == "compile-review-only"
assert summary["gate_experiments"] == ["E005", "E008", "E016"]
assert summary["gate_passed"] == 3 and summary["gate_failed"] == 0
assert summary["e013_non_blocking"] is True
samples = [json.loads(line) for line in (run_dir / "samples.jsonl").read_text().splitlines()]
assert {sample["experiment"] for sample in samples} == {"E005", "E008", "E013", "E016"}
executed = [sample for sample in samples if sample["backend"] != "vulkan"]
assert executed and all(sample["status"] == "passed" for sample in executed)
vulkan_samples = [sample for sample in samples if sample["backend"] == "vulkan"]
assert len(vulkan_samples) == 4
assert all(sample["status"] == "compile-review-only" for sample in vulkan_samples)
