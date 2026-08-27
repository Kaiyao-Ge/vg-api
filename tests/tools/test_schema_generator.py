#!/usr/bin/env python3
from __future__ import annotations
import json, subprocess, sys
from pathlib import Path
root, out = Path(sys.argv[1]), Path(sys.argv[2])
schema = root / "schemas/ir/task-root.vg.json"
subprocess.run([sys.executable, str(root / "tools/vg-schema/vg_schema.py"), str(schema), "--out-dir", str(out)], check=True)
header = (out / "vg_task_root.h").read_text()
assert "VG_SCHEMA_STATIC_ASSERT" in header
assert "_Static_assert(sizeof" not in header
reflection = json.loads((out / "vg_task_root.reflection.json").read_text())
assert reflection["layouts"]["TaskRecord"]["size"] == 48
assert reflection["layouts"]["TaskRecord"]["fields"][0]["offset"] == 0
assert reflection["layouts"]["TaskRecord"]["fields"][-1]["offset"] == 40

scene_schema = root / "schemas/ir/scene-root-raster.vg.json"
subprocess.run([sys.executable, str(root / "tools/vg-schema/vg_schema.py"), str(scene_schema), "--out-dir", str(out)], check=True)
scene_header = (out / "vg_scene_root.h").read_text()
public_header = (root / "include/vg/vg_scene_root.h").read_text()
assert scene_header == public_header, "public SceneRoot header is stale; regenerate it from scene-root-raster.vg.json"
scene_reflection = json.loads((out / "vg_scene_root.reflection.json").read_text())
assert scene_reflection["layouts"]["SceneRootRaster"]["size"] == 88
assert scene_reflection["relocations"] == [{"path": ["SceneRootRaster", "material", "albedo"], "kind": "facet", "offset": 80}]
