#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
build_dir = Path(sys.argv[2]).resolve()
completed = subprocess.run(
    [sys.executable, str(root / "tools" / "vg-exp" / "vg_exp.py"), "phase-e", "--build-dir", str(build_dir)],
    cwd=root, text=True, capture_output=True, check=True,
)
run_dir = root / completed.stdout.strip()
summary = json.loads((run_dir / "summary.json").read_text())
assert summary["schema"] == "vg.summary/v1"
assert summary["experiment_count"] == 18
assert summary["passed"] == summary["ctest_count"] and summary["failed"] == 0
assert summary["vulkan_status"] == "compile-review-only"
assert summary["gate_experiments"] == [
    "E001", "E002", "E003", "E004", "E005", "E006", "E007", "E008", "E009",
    "E010", "E011", "E012", "E013", "E014", "E015", "E016", "E017", "E018",
]
assert summary["e004_historical_b_row"] == "experiments/definitions/E004-access-certificate.json"
assert summary["break_even_curves"] == "unmeasured"
samples = [json.loads(line) for line in (run_dir / "samples.jsonl").read_text().splitlines()]
assert {sample["experiment"] for sample in samples} == set(summary["gate_experiments"])
executed = [sample for sample in samples if sample["backend"] != "vulkan"]
assert executed and all(sample["status"] == "passed" for sample in executed)
vulkan_samples = [sample for sample in samples if sample["backend"] == "vulkan"]
assert len(vulkan_samples) == 18
assert all(sample["status"] == "compile-review-only" for sample in vulkan_samples)
resolved = json.loads((run_dir / "definition.resolved.json").read_text())
assert resolved["definition_files"]["E004"] == "E004-discovery-revisit.json"
assert resolved["schema"] == "vg.phase-e/v1"
