#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path


root = Path(sys.argv[1]).resolve()
module_path = root / "tools" / "vg-exp" / "vg_exp.py"
spec = importlib.util.spec_from_file_location("vg_exp", module_path)
assert spec and spec.loader
vg_exp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vg_exp)

definition = json.loads((root / "experiments" / "definitions" / "P000-phase0-probe.json").read_text())
vg_exp.validate_definition(definition)
phase_a_ids = {"E001", "E003", "E006", "E015", "E018"}
experiment_files = list((root / "experiments" / "definitions").glob("E*.json"))
experiment_ids = {json.loads(path.read_text())["id"] for path in experiment_files}
# Phase A's exact set must still be present; Phase B milestones (E002/E004/
# E007/E009/E012, per TASK-B10's approved plan) add further E*.json files
# alongside them rather than replacing them.
assert phase_a_ids <= experiment_ids
for path in experiment_files:
    definition = json.loads(path.read_text())
    vg_exp.validate_definition(definition)
    assert all(key in definition for key in ("variants", "protocol", "correctness", "judgement"))
try:
    vg_exp.validate_definition({"schema": "vg.experiment/v1"})
except ValueError:
    pass
else:
    raise AssertionError("invalid experiment definition was accepted")

for schema in (root / "schemas").rglob("*.json"):
    json.loads(schema.read_text())
