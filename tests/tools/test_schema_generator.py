#!/usr/bin/env python3
from __future__ import annotations
import json, subprocess, sys
from pathlib import Path
root, out = Path(sys.argv[1]), Path(sys.argv[2])
schema = root / "schemas/ir/task-root.vg.json"
subprocess.run([sys.executable, str(root / "tools/vg-schema/vg_schema.py"), str(schema), "--out-dir", str(out)], check=True)
reflection = json.loads((out / "vg_task_root.reflection.json").read_text())
assert reflection["layouts"]["TaskRecord"]["size"] == 48
assert reflection["layouts"]["TaskRecord"]["fields"][0]["offset"] == 0
assert reflection["layouts"]["TaskRecord"]["fields"][-1]["offset"] == 40
